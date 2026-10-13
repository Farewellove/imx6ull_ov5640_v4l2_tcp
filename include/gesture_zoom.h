/*
 * gesture_zoom.h - 双指手势 → 数字缩放视图
 *
 * 输入: touch_input 解析出的一帧触点 (一次 SYN_REPORT 的结果)
 * 输出: struct fb_view (zoom + view_x0/view_y0), 直接喂给
 *       fb_display_show_frame_zoomed()
 *
 * 交互定义:
 *   双指张开  → 放大 (zoom 增大, 范围由 min/max_zoom 限定)
 *   双指合拢  → 缩小
 *   双指平移  → 画面跟随双指中心移动 (相当于拖动可见区域)
 *   三指同时按→ 复位到 1.0x 且居中 (提供一条"退回原状"的退路)
 *
 * 双指中心即缩放中心: 缩放时保持"双指中心下方的源像素"不动, 因此不会出现
 * "一边放大一边画面乱跑"的观感。
 *
 * 防误触发设计 (与 GT911 项目 user_app/touch_sensor_scope.c 的手势引擎保持
 * 一致的阈值, 便于两个项目互相印证):
 *   1. 比例阈值 12%: 距离变化不到 12% 不判定为缩放
 *   2. 连续 2 帧确认: 越过阈值的帧数不够不生效 (消除单帧抖动)
 *   3. 基准距离下限 40px: 两指靠得太近时比例噪声极大, 直接不做比例判断
 *   4. 基准重设: 每次真正缩放后把当前距离设为新基准, 避免误差累积
 *   5. 复位锁定: 三指复位在一次按压中只执行一次
 *   6. 非双指帧一律结束当前双指会话 (have_prev = 0), 避免手指数量变化时
 *      用错误的"上一帧中心"算出巨大位移
 *   7. SYN_DROPPED: 直接终止会话, 视图保持不动, 并计数
 *
 * 视图状态在手指抬起后**保持不变** (用户设定的缩放不会自己跳回去),
 * 只有三指手势或命令行才复位。
 */

#ifndef GESTURE_ZOOM_H
#define GESTURE_ZOOM_H

#include <stdint.h>

#include "touch_input.h"
#include "fb_display.h"

/* 比例阈值 12% (= 12/100) */
#define ZOOM_PINCH_RATIO_NUM   12
#define ZOOM_PINCH_RATIO_DEN   100
/* 连续确认帧数 */
#define ZOOM_CONFIRM_FRAMES    2
/* 双指基准距离下限 (LCD 像素) */
#define ZOOM_MIN_BASE_DIST     40.0
/* 触发复位所需的手指数 */
#define ZOOM_RESET_FINGERS     3
/*
 * 三指复位需要的"三指同时存在"持续时长。
 * 不用帧数: 真实设备一次读取会返回一批积压帧, 这些帧的时间戳几乎相同,
 * 帧数不等于时长 (4 帧可能只跨 1ms)。用帧自带的时间戳判时长才准确。
 *
 * 判定用 n_raw (静止过滤前的触点数) + n_moved (生涯内移动过的触点数):
 *   - 三指按住不动时第三指会被静止过滤剔除, n 变成 2 → 复位永远不触发;
 *   - 幽灵触点坐标从不变 (moved=0), 要求 n_moved>=3 即可把它排除。
 */
#define ZOOM_RESET_HOLD_US  120000   /* 120 ms */

struct zoom_state {
    /* 视图 (对外直接可用) */
    struct fb_view view;
    double   min_zoom, max_zoom;
    uint32_t img_w, img_h;      /* 源图像尺寸 */
    uint32_t lcd_w, lcd_h;      /* 屏幕尺寸 */

    /* 双指会话状态 */
    int      have_prev;         /* 上一帧双指中心是否有效 */
    double   prev_cx, prev_cy;  /* 上一帧双指中心 (LCD 坐标) */
    double   base_dist;         /* 本次会话的基准距离 */
    int      confirm_zoom;      /* 连续越过阈值的帧数 */
    int      confirm_dir;       /* +1 = 张开, -1 = 合拢, 0 = 未定 */
    int      reset_latched;     /* 三指复位本次按压是否已执行 */
    uint64_t multi_start_us;    /* 本轮"三指同时存在"的起始时间 */

    /* 统计 */
    uint64_t frames_total;
    uint64_t frames_0f, frames_1f, frames_2f, frames_multi;
    uint64_t pinch_out, pinch_in, pan_frames, resets;
    uint64_t aborted_syn_dropped;
    uint64_t view_changed_frames;   /* 视图发生变化 (需要重绘缩放) 的帧数 */
    double   sum_update_us;         /* 本模块每帧处理耗时累计 */
    double   max_update_us;
    double   last_update_us;        /* 最近一帧处理耗时 */
};

/*
 * 初始化。min_zoom/max_zoom 传 0 时使用 1.0 / 2.0。
 * 初始视图 = 1.0x 居中 (等价于 fb_display_show_frame 的显示效果)。
 */
void zoom_state_init(struct zoom_state *z, uint32_t img_w, uint32_t img_h,
                     uint32_t lcd_w, uint32_t lcd_h,
                     double min_zoom, double max_zoom);

/* 视图复位到 1.0x 居中 */
void zoom_state_reset_view(struct zoom_state *z);

/*
 * 直接设置缩放倍率并把视图重新居中 (供 --zoom-fixed 这类基准测试使用,
 * 不经过手势)。
 * 返回 0 成功, -1 参数错误。
 */
int zoom_state_set_zoom(struct zoom_state *z, double zoom);

/*
 * 用一帧触点数据更新视图。
 * 返回: 1 = 本帧视图发生了变化 (需要按缩放路径重绘)
 *       0 = 无变化
 *      -1 = 参数错误
 *
 * 本函数内部会调用 fb_view_clamp(), 保证 view 始终在合法范围内 ——
 * 这是"连续缩放不出现越界/跑飞"的关键保障点。
 */
int zoom_state_update(struct zoom_state *z, const struct touch_frame *tf);

/* 当前视图是否等价于"1.0x 居中" (可直接走无缩放的快速路径) */
int zoom_state_is_fit(const struct zoom_state *z);

void zoom_state_print_info(const struct zoom_state *z);
void zoom_state_print_stats(const struct zoom_state *z);

#endif /* GESTURE_ZOOM_H */
