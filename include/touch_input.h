/*
 * touch_input.h - Linux Input (evdev) 多点触摸读取模块
 *
 * 目标设备: GT911 电容触摸屏, 通过内核 input 子系统以 **MT Protocol B** 上报。
 * 驱动侧事实 (已审计, 本项目不改驱动):
 *   kernel_drivers/gt911_ts/lcd.c
 *     - idev->name = "GT911 Touchscreen", bustype = BUS_I2C
 *     - input_mt_init_slots(5, INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED)
 *     - ABS_MT_POSITION_X ∈ [0, 1024], ABS_MT_POSITION_Y ∈ [0, 600]
 *     - 同时开启 pointer emulation (ABS_X/ABS_Y + BTN_TOUCH)
 *     - 未上报 ABS_MT_PRESSURE / ABS_MT_ORIENTATION
 *
 * 因此本模块只依赖 Protocol B 的最小可靠子集:
 *     ABS_MT_SLOT / ABS_MT_TRACKING_ID / ABS_MT_POSITION_X,Y / SYN_REPORT
 *
 * 设计要点:
 *   1. **一帧 = 一次 SYN_REPORT**。同一帧内各 slot 坐标是一致的, 所以手势
 *      判定只能在 SYN_REPORT 时提交, 不能看到半个帧就下结论。
 *   2. **释放只认 tracking_id = -1**, 不依赖 BTN_TOUCH (后者是单点兼容语义,
 *      多点场景下不可靠)。
 *   3. **SYN_DROPPED 必须自己防**。它不由驱动产生, 而是内核 input 核心在
 *      事件缓冲溢出时补发; 一旦发生当前帧就不完整。这里采取保守策略:
 *      清空全部 slot + 上报一个"空帧"给上层, 让手势层终止当前会话, 避免
 *      幽灵触点一直挂在屏幕上。
 *   4. **支持离线回放** (touch_open_replay)。回放走的是**同一套状态机**,
 *      不是另写一个解析器 —— 否则离线测试就证明不了真实路径的正确性。
 *      回放时用日志里的时间戳作为逻辑时间, 使"时长阈值/冷却"这类判断
 *      在离线环境下与真实设备行为一致。
 */

#ifndef TOUCH_INPUT_H
#define TOUCH_INPUT_H

#include <stdint.h>
#include <stdio.h>

/* 最多跟踪的 slot 数 (GT911 驱动只申请了 5 个, 留一倍余量) */
#define TOUCH_MAX_SLOTS 10

/*
 * 幽灵触点过滤阈值 (X/Y 事件各计一次, GT911 ~10ms/帧 → 30 ≈ 150ms)。
 * 实测: 双指捏合时 GT911 会持续多报一个位置纹丝不动的第三触点
 * (46 帧里 27 帧为多指), 使 n>=3, 双指缩放永远无法触发。
 * 连续静止超过该阈值的触点从帧里剔除; 一恢复移动立即重新计入。
 */
#define TOUCH_GHOST_STILL_FRAMES 30

/* 一帧触摸数据 (一次 SYN_REPORT 的结果) */
struct touch_frame {
    int      n;                     /* 有效触点数 (0 = 所有手指已抬起) */
    int32_t  x[TOUCH_MAX_SLOTS];    /* LCD 坐标 (已按 ABS 范围映射并裁剪) */
    int32_t  y[TOUCH_MAX_SLOTS];
    int      id[TOUCH_MAX_SLOTS];   /* ABS_MT_TRACKING_ID */
    uint64_t t_us;                  /* 逻辑时间 (单调微秒) */
    int      syn_dropped;           /* 1 = 本帧由 SYN_DROPPED 触发, 状态已清空 */

    /*
     * 静止过滤前的原始触点数, 以及"生涯内发生过位移"的触点数。
     * 三指复位必须用这两个值判定, 不能用过滤后的 n: 三指按住不动时
     * 第三指会被静止过滤剔除 (n 变成 2), 复位永远触发不了。而幽灵
     * 触点从出现到消失坐标从不变化 (moved=0), 用 n_moved 即可区分。
     */
    int      n_raw;                 /* 过滤前的有效触点数 (含静止幽灵) */
    int      n_moved;               /* 生涯内移动过的触点数 */
};

struct touch_ctx {
    /* 事件源: fd >= 0 为真实设备; 否则为回放文件 */
    int      fd;
    FILE    *replay_fp;
    int      replay_mode;
    char     dev_name[64];

    /* MT Protocol B 状态机 */
    int      cur_slot;
    struct touch_slot_raw {
        int     id;        /* -1 = 未使用 */
        int32_t rx, ry;    /* 原始坐标 (未映射) */
        int     seen;      /* 生涯内是否收到过坐标 (新触点首个坐标不计静止) */
        int     still_run; /* 连续静止事件计数 (X/Y 各计一次), 幽灵过滤用 */
        int     moved;     /* 生涯内坐标是否变化过 (区分真实手指与幽灵) */
    } slot[TOUCH_MAX_SLOTS];

    /* 坐标范围与映射 */
    int32_t  abs_x_min, abs_x_max;
    int32_t  abs_y_min, abs_y_max;
    uint32_t lcd_w, lcd_h;
    int      swap_xy;
    int      invert_x, invert_y;

    /* 统计 */
    uint64_t ev_total;
    uint64_t frames;
    uint64_t syn_dropped_cnt;
    uint64_t frames_overflow;   /* 同帧触点数超过 TOUCH_MAX_SLOTS 的次数 */
    uint64_t replay_bad_lines;
    uint64_t replay_events;
};

/*
 * 打开真实的触摸设备。
 * dev 为 NULL 时自动扫描 /dev/input/event0..event9, 找第一个支持
 * ABS_MT_POSITION_X/Y 的设备 (设备节点编号在不同板子上不固定)。
 * lcd_w/lcd_h 用于把 ABS 原始坐标映射到屏幕像素坐标。
 * 返回 0 成功, -1 失败。
 */
int touch_open(struct touch_ctx *t, const char *dev,
               uint32_t lcd_w, uint32_t lcd_h);

/*
 * 打开一份合成的 touch 事件日志用于离线回放。
 * 日志格式 (与 GT911 项目 tools/mt_event_sim.c 的 gen 模式一致):
 *     t_us type code value
 * '#" 开头为注释, 空行忽略。
 * 返回 0 成功, -1 失败。
 */
int touch_open_replay(struct touch_ctx *t, const char *path,
                      uint32_t lcd_w, uint32_t lcd_h);

/* 可选的方向调整 (不同装配方向可能需要) */
void touch_set_orientation(struct touch_ctx *t, int swap_xy,
                           int invert_x, int invert_y);

/*
 * 读取下一帧触摸数据。
 * 返回:  1 = 拿到一帧 (写入 out)
 *        0 = 目前没有新帧 (真实设备无事件 / 回放已到文件尾)
 *       -1 = 读取错误
 *
 * 注意: 真实设备与回放模式一致, 每凑齐一个 SYN_REPORT 就返回一帧 ——
 * 手势层的"连续 N 帧确认 / 比例阈值"依赖逐帧时序; 积压由调用方的
 * guard 上限兜住 (触摸只是控制信号, 落后就丢)。
 */
int touch_read_frame(struct touch_ctx *t, struct touch_frame *out);

/* 打印设备/范围/统计信息 (便于上板排查坐标映射) */
void touch_print_info(const struct touch_ctx *t);

/* 打印累计统计 (回放或退出时调用) */
void touch_print_stats(const struct touch_ctx *t);

void touch_close(struct touch_ctx *t);

#endif /* TOUCH_INPUT_H */
