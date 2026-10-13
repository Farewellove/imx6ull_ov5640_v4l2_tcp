/*
 * touch_input.c - Linux Input (evdev) 多点触摸读取实现
 *
 * 只依赖 MT Protocol B 的最小可靠子集, 不修改 GT911 内核驱动。
 * 详细设计说明见 include/touch_input.h。
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "touch_input.h"
#include "metrics.h"

/* 老内核头文件里未必有 SYN_DROPPED 的宏, 按 input 子系统约定补一个 */
#ifndef SYN_DROPPED
#define SYN_DROPPED 3
#endif

/*
 * 为什么不做批量读缓冲:
 * 触摸只在一帧上产生约 10~20 个事件, 按 15fps 计也就是每秒几百次 read(),
 * 相比 61 万像素的绘制完全不值一提。用"一次 read 取一个事件"能让
 * 状态机代码保持最简, 少一份容易写错的环形缓冲。
 */

/* 用户态没有内核的 test_bit(); 位图查询自己实现, 避免引入 linux/bitops.h */
static int bit_is_set(const unsigned long *bits, int bit)
{
    unsigned int per = (unsigned int)(8 * sizeof(unsigned long));

    return (int)((bits[bit / per] >> (bit % per)) & 1UL);
}

/* ================================================================
 * 内部状态
 * ================================================================ */
static void touch_reset_state(struct touch_ctx *t)
{
    int i;

    for (i = 0; i < TOUCH_MAX_SLOTS; i++) {
        t->slot[i].id = -1;
        t->slot[i].rx = 0;
        t->slot[i].ry = 0;
        t->slot[i].seen = 0;
    }
    t->cur_slot = 0;
}

static void touch_init_common(struct touch_ctx *t, uint32_t lcd_w, uint32_t lcd_h)
{
    memset(t, 0, sizeof(*t));
    t->fd = -1;
    t->replay_fp = NULL;
    t->replay_mode = 0;

    /* 驱动上报的范围写死成 1024x600 作为兜底; 打开真实设备时会用
     * EVIOCGABS 覆盖成实际值。 */
    t->abs_x_min = 0; t->abs_x_max = 1023;
    t->abs_y_min = 0; t->abs_y_max = 599;

    t->lcd_w = lcd_w ? lcd_w : 1024;
    t->lcd_h = lcd_h ? lcd_h : 600;
    t->swap_xy = 0;
    t->invert_x = 0;
    t->invert_y = 0;

    touch_reset_state(t);
}

/* ================================================================
 * 坐标映射: ABS 原始值 → LCD 像素
 *
 * 用 (max - min + 1) 作为跨度: 因为 ABS 上限是**闭区间**, 0..1023 共 1024 个
 * 取值, 对应 LCD 的 1024 列。若写成 (max-min) 会让最后一列永远取不到。
 * ================================================================ */
static int32_t touch_map_axis(int32_t raw, int32_t amin, int32_t amax,
                              uint32_t lcd)
{
    int64_t span = (int64_t)amax - (int64_t)amin + 1;
    int64_t v;

    if (lcd == 0)
        return 0;
    if (span <= 0)
        return 0;

    v = ((int64_t)(raw - amin) * (int64_t)lcd) / span;

    if (v < 0)
        v = 0;
    if (v >= (int64_t)lcd)
        v = (int64_t)lcd - 1;
    return (int32_t)v;
}

static int32_t touch_map_x(const struct touch_ctx *t, int32_t raw)
{
    int32_t v = touch_map_axis(raw, t->abs_x_min, t->abs_x_max, t->lcd_w);
    return t->invert_x ? (int32_t)t->lcd_w - 1 - v : v;
}

static int32_t touch_map_y(const struct touch_ctx *t, int32_t raw)
{
    int32_t v = touch_map_axis(raw, t->abs_y_min, t->abs_y_max, t->lcd_h);
    return t->invert_y ? (int32_t)t->lcd_h - 1 - v : v;
}

void touch_set_orientation(struct touch_ctx *t, int swap_xy,
                           int invert_x, int invert_y)
{
    t->swap_xy  = swap_xy ? 1 : 0;
    t->invert_x = invert_x ? 1 : 0;
    t->invert_y = invert_y ? 1 : 0;
}

/* ================================================================
 * 事件源: 真实设备 (批量 read) / 回放文件 (逐行解析)
 *
 * 返回 1 = 取到一个事件, 0 = 暂时没有/已到末尾, -1 = 错误。
 * ================================================================ */
static int touch_next_event(struct touch_ctx *t, struct input_event *ev)
{
    if (t->replay_mode) {
        char line[256];

        while (fgets(line, sizeof(line), t->replay_fp) != NULL) {
            char *p = line;
            unsigned long long tus;
            int type, code, value;

            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
                continue;   /* 注释 / 空行 */

            if (sscanf(p, "%llu %d %d %d", &tus, &type, &code, &value) != 4) {
                t->replay_bad_lines++;
                continue;
            }

            /* 把日志里的逻辑时间放回 ev->time, 上层据此做时长/冷却判断,
             * 使离线回放的时序语义与真实设备一致 */
            ev->time.tv_sec  = (long)(tus / 1000000ULL);
            ev->time.tv_usec = (long)(tus % 1000000ULL);
            ev->type  = (uint16_t)type;
            ev->code  = (uint16_t)code;
            ev->value = value;
            t->replay_events++;
            return 1;
        }
        return 0;   /* EOF */
    }

    /* 真实设备: 一次 read 取一批。O_NONBLOCK 下无事件返回 EAGAIN。 */
    {
        ssize_t n = read(t->fd, ev, sizeof(struct input_event));

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            if (errno == EINTR)
                return 0;
            perror("read(/dev/input)");
            return -1;
        }
        if (n == 0)
            return 0;
        if ((size_t)n < sizeof(struct input_event)) {
            /* 半个事件: 丢弃, 下次读剩下的 —— 不能按错误处理,
             * 否则一次不完全读就把触摸功能整个关掉 */
            return 0;
        }
        return 1;
    }
}

/* 事件逻辑时间: 回放用日志时间, 真实设备用当前单调时钟 */
static uint64_t touch_event_time_us(const struct touch_ctx *t,
                                   const struct input_event *ev)
{
    if (t->replay_mode)
        return (uint64_t)ev->time.tv_sec * 1000000ULL
               + (uint64_t)ev->time.tv_usec;
    return metrics_now_us();
}

/* 把当前 slot 状态导出成一帧 */
static void touch_emit_frame(struct touch_ctx *t, struct touch_frame *out,
                             uint64_t t_us, int dropped)
{
    int i, n = 0;

    memset(out, 0, sizeof(*out));
    out->t_us = t_us;
    out->syn_dropped = dropped;

    if (!dropped) {
        for (i = 0; i < TOUCH_MAX_SLOTS; i++) {
            int32_t lx, ly;

            if (t->slot[i].id < 0)
                continue;
            if (n >= TOUCH_MAX_SLOTS) {
                t->frames_overflow++;
                break;
            }

            lx = touch_map_x(t, t->slot[i].rx);
            ly = touch_map_y(t, t->slot[i].ry);
            if (t->swap_xy) {
                int32_t tmp = lx;
                lx = ly;
                ly = tmp;
            }

            out->id[n] = t->slot[i].id;
            out->x[n]  = lx;
            out->y[n]  = ly;
            n++;
        }
    }

    out->n = n;
    t->frames++;
}

/*
 * 处理一个事件。
 * 返回 1 = 产生了一帧 (已写入 out), 0 = 还需要继续读, -1 = 错误。
 */
static int touch_apply_event(struct touch_ctx *t, const struct input_event *ev,
                             struct touch_frame *out)
{
    t->ev_total++;

    if (ev->type == EV_SYN) {
        if (ev->code == SYN_REPORT) {
            touch_emit_frame(t, out, touch_event_time_us(t, ev), 0);
            return 1;
        }

        if (ev->code == SYN_DROPPED) {
            /*
             * 内核 input 缓冲溢出, 当前帧不完整。
             * 保守策略: 清空全部 slot, 并上报一个"空帧 + dropped"让上层
             * 终止当前手势会话 —— 否则不完整的坐标会留下幽灵触点。
             */
            t->syn_dropped_cnt++;
            touch_reset_state(t);
            touch_emit_frame(t, out, touch_event_time_us(t, ev), 1);
            return 1;
        }
        return 0;
    }

    if (ev->type != EV_ABS)
        return 0;   /* EV_KEY(BTN_TOUCH) / 其他一律忽略: 释放只认 tracking_id */

    switch (ev->code) {
    case ABS_MT_SLOT:
        if (ev->value >= 0 && ev->value < TOUCH_MAX_SLOTS)
            t->cur_slot = ev->value;
        /* 越界 slot 不改变 cur_slot, 后续事件会被丢到已知 slot 上 —— 比
         * 越界写数组安全; 驱动实际只申请了 5 个 slot, 不会走到这里 */
        return 0;

    case ABS_MT_TRACKING_ID:
        t->slot[t->cur_slot].id = ev->value;   /* -1 = 抬起 */
        if (ev->value < 0) {
            t->slot[t->cur_slot].seen = 0;
        } else {
            t->slot[t->cur_slot].seen = 0;     /* 等坐标 */
        }
        return 0;

    case ABS_MT_POSITION_X:
        t->slot[t->cur_slot].rx = ev->value;
        t->slot[t->cur_slot].seen = 1;
        return 0;

    case ABS_MT_POSITION_Y:
        t->slot[t->cur_slot].ry = ev->value;
        t->slot[t->cur_slot].seen = 1;
        return 0;

    default:
        /* ABS_MT_TOUCH_MAJOR / ABS_X / ABS_Y(单点兼容) 等: 本模块用不到 */
        return 0;
    }
}

/* ================================================================
 * 打开真实设备
 * ================================================================ */
static int touch_read_abs_range(int fd, int code, int32_t *amin, int32_t *amax)
{
    struct input_absinfo ai;

    memset(&ai, 0, sizeof(ai));
    if (ioctl(fd, EVIOCGABS(code), &ai) < 0)
        return -1;
    *amin = ai.minimum;
    *amax = ai.maximum;
    return 0;
}

/* 该设备是否支持多点触摸 (检查 EV_ABS 位图) */
static int touch_dev_has_mt(int fd)
{
    unsigned long absbits[(ABS_MAX / (8 * sizeof(long))) + 1];

    memset(absbits, 0, sizeof(absbits));
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0)
        return 0;
    return bit_is_set(absbits, ABS_MT_POSITION_X) &&
           bit_is_set(absbits, ABS_MT_POSITION_Y);
}

static int touch_try_open_path(struct touch_ctx *t, const char *path,
                               int verbose)
{
    int fd;
    char name[64];
    int32_t ax0, ax1, ay0, ay1;

    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (verbose)
            perror(path);
        return -1;
    }

    if (!touch_dev_has_mt(fd)) {
        if (verbose)
            printf("[TOUCH] %s: no MT axes, skip\n", path);
        close(fd);
        return -1;
    }

    memset(name, 0, sizeof(name));
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
        snprintf(name, sizeof(name), "(unknown)");

    if (touch_read_abs_range(fd, ABS_MT_POSITION_X, &ax0, &ax1) == 0) {
        t->abs_x_min = ax0;
        t->abs_x_max = ax1;
    }
    if (touch_read_abs_range(fd, ABS_MT_POSITION_Y, &ay0, &ay1) == 0) {
        t->abs_y_min = ay0;
        t->abs_y_max = ay1;
    }

    t->fd = fd;
    snprintf(t->dev_name, sizeof(t->dev_name), "%s", name);
    printf("[TOUCH] opened %s: name=\"%s\"\n", path, name);
    printf("[TOUCH] abs range: X[%d..%d] Y[%d..%d] -> LCD %ux%u\n",
           t->abs_x_min, t->abs_x_max, t->abs_y_min, t->abs_y_max,
           t->lcd_w, t->lcd_h);
    return 0;
}

int touch_open(struct touch_ctx *t, const char *dev,
               uint32_t lcd_w, uint32_t lcd_h)
{
    touch_init_common(t, lcd_w, lcd_h);

    if (dev != NULL) {
        if (touch_try_open_path(t, dev, 1) == 0)
            return 0;
        fprintf(stderr, "[TOUCH] cannot use %s\n", dev);
        return -1;
    }

    /* 自动扫描: 设备节点编号在不同板子上不固定, 不要硬编码 event1 */
    {
        int i;
        char path[64];

        for (i = 0; i < 10; i++) {
            snprintf(path, sizeof(path), "/dev/input/event%d", i);
            if (touch_try_open_path(t, path, 0) == 0)
                return 0;
        }
    }

    fprintf(stderr, "[TOUCH] no MT touchscreen found in /dev/input/event0..9\n");
    t->fd = -1;
    return -1;
}

/* ================================================================
 * 打开离线回放
 * ================================================================ */
int touch_open_replay(struct touch_ctx *t, const char *path,
                      uint32_t lcd_w, uint32_t lcd_h)
{
    touch_init_common(t, lcd_w, lcd_h);

    if (path == NULL)
        return -1;
    t->replay_fp = fopen(path, "r");
    if (t->replay_fp == NULL) {
        perror(path);
        return -1;
    }
    t->replay_mode = 1;
    snprintf(t->dev_name, sizeof(t->dev_name), "replay:%s", path);
    printf("[TOUCH] replay mode: %s (LCD %ux%u)\n", path, t->lcd_w, t->lcd_h);
    return 0;
}

/* ================================================================
 * 读取一帧
 * ================================================================ */
int touch_read_frame(struct touch_ctx *t, struct touch_frame *out)
{
    struct input_event ev;
    int ret, last_frame_valid = 0;
    struct touch_frame last;

    if (t == NULL || out == NULL)
        return -1;

    memset(&last, 0, sizeof(last));

    for (;;) {
        ret = touch_next_event(t, &ev);
        if (ret < 0)
            return -1;

        if (ret == 0) {
            /*
             * 事件源暂时/永久空了。
             *
             * 两种模式必须区别对待 —— 这一点曾经写错并导致回放只处理
             * 了 1 帧 (整份日志被一次 read 吞掉, 手势时序全丢):
             *   - 真实设备: 触摸只是控制信号, 落后的事件不需要逐帧补,
             *     所以返回本批的**最后一帧** (排空式)
             *   - 回放: 每一帧都必须交给手势层, 否则"连续 2 帧确认"、
             *     冷却、比例阈值这些时序逻辑根本无从生效
             */
            if (last_frame_valid && !t->replay_mode) {
                *out = last;
                return 1;
            }
            return 0;
        }

        ret = touch_apply_event(t, &ev, &last);
        if (ret < 0)
            return -1;

        if (ret == 1) {
            last_frame_valid = 1;
            if (t->replay_mode) {
                /* 回放模式: 每凑齐一帧就返回, 保住逐帧时序 */
                *out = last;
                return 1;
            }
        }
    }
}

/* ================================================================
 * 信息与统计
 * ================================================================ */
void touch_print_info(const struct touch_ctx *t)
{
    if (t == NULL)
        return;
    printf("[TOUCH] source      : %s\n", t->dev_name);
    printf("[TOUCH] mode        : %s\n",
           t->replay_mode ? "replay" : "live evdev");
    printf("[TOUCH] abs X range : [%d..%d]  (%d 个取值)\n",
           t->abs_x_min, t->abs_x_max, t->abs_x_max - t->abs_x_min + 1);
    printf("[TOUCH] abs Y range : [%d..%d]  (%d 个取值)\n",
           t->abs_y_min, t->abs_y_max, t->abs_y_max - t->abs_y_min + 1);
    printf("[TOUCH] mapped to   : LCD %ux%u\n", t->lcd_w, t->lcd_h);
    printf("[TOUCH] orientation : swap_xy=%d invert_x=%d invert_y=%d\n",
           t->swap_xy, t->invert_x, t->invert_y);
}

void touch_print_stats(const struct touch_ctx *t)
{
    if (t == NULL)
        return;
    printf("[TOUCH] events=%llu frames=%llu syn_dropped=%llu overflow=%llu\n",
           (unsigned long long)t->ev_total,
           (unsigned long long)t->frames,
           (unsigned long long)t->syn_dropped_cnt,
           (unsigned long long)t->frames_overflow);
    if (t->replay_mode)
        printf("[TOUCH] replay: events=%llu bad_lines=%llu\n",
               (unsigned long long)t->replay_events,
               (unsigned long long)t->replay_bad_lines);
}

void touch_close(struct touch_ctx *t)
{
    if (t == NULL)
        return;
    if (t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
    if (t->replay_fp != NULL) {
        fclose(t->replay_fp);
        t->replay_fp = NULL;
    }
}
