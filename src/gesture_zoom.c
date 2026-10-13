/*
 * gesture_zoom.c - 双指手势 → 数字缩放视图 实现
 *
 * 详细交互与防抖设计见 include/gesture_zoom.h
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "gesture_zoom.h"
#include "metrics.h"

/* ================================================================
 * 初始化 / 复位
 * ================================================================ */
void zoom_state_init(struct zoom_state *z, uint32_t img_w, uint32_t img_h,
                     uint32_t lcd_w, uint32_t lcd_h,
                     double min_zoom, double max_zoom)
{
    if (z == NULL)
        return;

    memset(z, 0, sizeof(*z));

    z->img_w = img_w;
    z->img_h = img_h;
    z->lcd_w = lcd_w;
    z->lcd_h = lcd_h;

    z->min_zoom = (min_zoom > 0.0) ? min_zoom : 1.0;
    z->max_zoom = (max_zoom > 0.0) ? max_zoom : 2.0;
    if (z->max_zoom < z->min_zoom)
        z->max_zoom = z->min_zoom;

    zoom_state_reset_view(z);
}

void zoom_state_reset_view(struct zoom_state *z)
{
    if (z == NULL)
        return;

    fb_view_reset(&z->view, z->img_w, z->img_h, z->lcd_w, z->lcd_h);
    z->view.zoom = z->min_zoom > 1.0 ? z->min_zoom : 1.0;
    fb_view_clamp(&z->view, z->img_w, z->img_h, z->lcd_w, z->lcd_h);
}

int zoom_state_set_zoom(struct zoom_state *z, double zoom)
{
    if (z == NULL || !(zoom > 0.0))
        return -1;

    if (zoom < z->min_zoom)
        zoom = z->min_zoom;
    if (zoom > z->max_zoom)
        zoom = z->max_zoom;

    z->view.zoom = zoom;
    /* 重新居中: 基准测试要的是"同一中心、不同倍率"的干净对比 */
    z->view.view_x0 = ((double)z->img_w - (double)z->lcd_w / zoom) / 2.0;
    z->view.view_y0 = ((double)z->img_h - (double)z->lcd_h / zoom) / 2.0;
    fb_view_clamp(&z->view, z->img_w, z->img_h, z->lcd_w, z->lcd_h);
    return 0;
}

int zoom_state_is_fit(const struct zoom_state *z)
{
    if (z == NULL)
        return 0;
    return fb_view_is_fit(&z->view, z->img_w, z->img_h, z->lcd_w, z->lcd_h);
}

/* ================================================================
 * 每帧更新
 * ================================================================ */
static void zoom_session_abort(struct zoom_state *z)
{
    z->have_prev = 0;
    z->base_dist = 0.0;
    z->confirm_zoom = 0;
    z->confirm_dir = 0;
}

static void zoom_record_time(struct zoom_state *z, uint64_t t0)
{
    double dt = (double)(metrics_now_us() - t0);

    z->last_update_us = dt;
    z->sum_update_us += dt;
    if (dt > z->max_update_us)
        z->max_update_us = dt;
}

int zoom_state_update(struct zoom_state *z, const struct touch_frame *tf)
{
    uint64_t t0;
    int changed = 0;
    double cx, cy, ddx, ddy, dist;

    if (z == NULL || tf == NULL)
        return -1;

    t0 = metrics_now_us();
    z->frames_total++;

    /* ---- SYN_DROPPED: 当前帧不完整, 终止会话但保留视图 ---- */
    if (tf->syn_dropped) {
        z->aborted_syn_dropped++;
        z->frames_0f++;
        zoom_session_abort(z);
        zoom_record_time(z, t0);
        return 0;
    }

    /* ---- 三指: 复位到 1.0x 居中 (持续时长确认, 一次按压只执行一次) ----
     *
     * 用 n_raw (静止过滤前) 判定: 三指按住不动时第三指会被静止过滤
     * 剔除, 过滤后的 n 只有 2, 复位永远触发不了。
     * 同时要求 n_moved >= 3: 幽灵触点坐标从不变 (moved=0), 这样捏合
     * 时混入的第三幽灵点不会误触发复位, 而真实手指落下必有位移。
     */
    if (tf->n_raw >= ZOOM_RESET_FINGERS) {
        z->frames_multi++;

        if (tf->n_moved >= ZOOM_RESET_FINGERS) {
            if (z->multi_start_us == 0)
                z->multi_start_us = tf->t_us;

            if (!z->reset_latched &&
                (tf->t_us - z->multi_start_us) >= ZOOM_RESET_HOLD_US) {
                z->reset_latched = 1;
                zoom_state_reset_view(z);
                z->resets++;
                changed = 1;
                z->view_changed_frames++;
                zoom_session_abort(z);  /* 复位后重建基准 */
            }
        }

        /* 未确认复位的多指帧不终止双指会话, 双指中心用本帧前两个触点
         * 近似更新, 保持平移的连续性 (基准距离不动) */
        if (z->have_prev && tf->n >= 2) {
            z->prev_cx = ((double)tf->x[0] + (double)tf->x[1]) / 2.0;
            z->prev_cy = ((double)tf->y[0] + (double)tf->y[1]) / 2.0;
        }
        zoom_record_time(z, t0);
        return changed;
    }
    z->reset_latched = 0;
    z->multi_start_us = 0;

    /* ---- 非双指: 结束双指会话 (视图保持不变) ---- */
    if (tf->n != 2) {
        if (tf->n == 0)
            z->frames_0f++;
        else if (tf->n == 1)
            z->frames_1f++;
        else
            z->frames_multi++;
        zoom_session_abort(z);
        zoom_record_time(z, t0);
        return 0;
    }

    z->frames_2f++;

    cx = ((double)tf->x[0] + (double)tf->x[1]) / 2.0;
    cy = ((double)tf->y[0] + (double)tf->y[1]) / 2.0;
    ddx = (double)tf->x[1] - (double)tf->x[0];
    ddy = (double)tf->y[1] - (double)tf->y[0];
    dist = sqrt(ddx * ddx + ddy * ddy);

    /* 本次双指会话的建立帧: 只记基准, 不做任何变换 */
    if (!z->have_prev) {
        z->have_prev = 1;
        z->prev_cx = cx;
        z->prev_cy = cy;
        z->base_dist = dist;
        z->confirm_zoom = 0;
        z->confirm_dir = 0;
        zoom_record_time(z, t0);
        return 0;
    }

    /* ---- 平移: 双指中心移动 → 视图跟随 ----
     *
     * src_x = view_x0 + dst_x / zoom
     * 要让手指下方的内容跟着手指走, 手指右移 dx 时 view_x0 必须减小
     * dx/zoom。这样固定屏幕位置上的 src_x 变小, 画面整体向右移动。
     */
    {
        double pan_dx = cx - z->prev_cx;
        double pan_dy = cy - z->prev_cy;

        if (pan_dx != 0.0 || pan_dy != 0.0) {
            z->view.view_x0 -= pan_dx / z->view.zoom;
            z->view.view_y0 -= pan_dy / z->view.zoom;
            if (pan_dx * pan_dx + pan_dy * pan_dy >= 1.0)
                z->pan_frames++;
            changed = 1;
        }
    }

    /* ---- 缩放: 比例阈值 + 连续确认 ----
     *
     * 两指距离太近时比例噪声极大 (分母小), 因此基准距离与当前距离
     * 都必须不小于 ZOOM_MIN_BASE_DIST 才做比例判断。
     */
    if (z->base_dist >= ZOOM_MIN_BASE_DIST && dist >= ZOOM_MIN_BASE_DIST) {
        double ratio = dist / z->base_dist;
        double hi = 1.0 + (double)ZOOM_PINCH_RATIO_NUM / ZOOM_PINCH_RATIO_DEN;
        double lo = 1.0 - (double)ZOOM_PINCH_RATIO_NUM / ZOOM_PINCH_RATIO_DEN;
        int dir = 0;

        if (ratio > hi)
            dir = 1;        /* 张开 → 放大 */
        else if (ratio < lo)
            dir = -1;       /* 合拢 → 缩小 */

        if (dir == 0) {
            z->confirm_zoom = 0;
            z->confirm_dir = 0;
        } else {
            if (dir == z->confirm_dir) {
                z->confirm_zoom++;
            } else {
                z->confirm_dir = dir;
                z->confirm_zoom = 1;
            }

            if (z->confirm_zoom >= ZOOM_CONFIRM_FRAMES) {
                /*
                 * 保持"双指中心下方的源像素"不动:
                 *   anchor = view0 + c / zoom_old
                 *   view0' = anchor - c / zoom_new
                 * 这样缩放围绕手指中心进行, 而不是围绕屏幕中心,
                 * 观感上不会一边放大一边画面乱跑。
                 */
                double anchor_x = z->view.view_x0 + cx / z->view.zoom;
                double anchor_y = z->view.view_y0 + cy / z->view.zoom;
                double nz = z->view.zoom * ratio;

                if (nz < z->min_zoom)
                    nz = z->min_zoom;
                if (nz > z->max_zoom)
                    nz = z->max_zoom;

                /* 已经顶到边界时不再改 zoom, 但要重设基准, 否则用户
                 * 继续张开手指会一直累积 confirm, 一松手就跳变 */
                z->view.zoom = nz;
                z->view.view_x0 = anchor_x - cx / nz;
                z->view.view_y0 = anchor_y - cy / nz;

                z->base_dist = dist;        /* 重设基准, 防误差累积 */
                z->confirm_zoom = 0;
                z->confirm_dir = 0;

                if (dir > 0)
                    z->pinch_out++;
                else
                    z->pinch_in++;
                changed = 1;
            }
        }
    }

    /*
     * 边界钳制 —— 连续缩放/平移过程中保证视图始终合法:
     *   - 放大到顶时 view_x0 会被夹在 [0, img_w - lcd_w/zoom]
     *   - 图像比屏幕小时强制居中
     * 不做这一步就可能算出越界的 view, 渲染时露出空白或画面跑飞。
     */
    fb_view_clamp(&z->view, z->img_w, z->img_h, z->lcd_w, z->lcd_h);

    z->prev_cx = cx;
    z->prev_cy = cy;

    if (changed)
        z->view_changed_frames++;

    zoom_record_time(z, t0);
    return changed;
}

/* ================================================================
 * 打印
 * ================================================================ */
void zoom_state_print_info(const struct zoom_state *z)
{
    if (z == NULL)
        return;
    printf("[ZOOM] image %ux%u -> LCD %ux%u\n",
           z->img_w, z->img_h, z->lcd_w, z->lcd_h);
    printf("[ZOOM] zoom range : %.2fx .. %.2fx\n", z->min_zoom, z->max_zoom);
    printf("[ZOOM] initial    : zoom=%.2f view=(%.1f, %.1f) %s\n",
           z->view.zoom, z->view.view_x0, z->view.view_y0,
           zoom_state_is_fit(z) ? "[fit: 走无缩放快速路径]" : "[zoomed]");
    printf("[ZOOM] gestures   : 双指张开/合拢=缩放, 双指平移=拖动, "
           "三指=复位\n");
}

void zoom_state_print_stats(const struct zoom_state *z)
{
    uint64_t n;

    if (z == NULL)
        return;

    printf("\n---- 手势统计 ----\n");
    printf("  frames_total      : %llu\n",
           (unsigned long long)z->frames_total);
    printf("  0指/1指/2指/多指   : %llu / %llu / %llu / %llu\n",
           (unsigned long long)z->frames_0f,
           (unsigned long long)z->frames_1f,
           (unsigned long long)z->frames_2f,
           (unsigned long long)z->frames_multi);
    printf("  pinch_out(放大)    : %llu\n",
           (unsigned long long)z->pinch_out);
    printf("  pinch_in(缩小)     : %llu\n",
           (unsigned long long)z->pinch_in);
    printf("  pan_frames(平移)   : %llu\n",
           (unsigned long long)z->pan_frames);
    printf("  resets(三指复位)   : %llu\n",
           (unsigned long long)z->resets);
    printf("  aborted(SYN_DROP)  : %llu\n",
           (unsigned long long)z->aborted_syn_dropped);
    printf("  view_changed_frames: %llu  (需要按缩放路径重绘的帧)\n",
           (unsigned long long)z->view_changed_frames);
    printf("  final view         : zoom=%.3f view=(%.2f, %.2f)\n",
           z->view.zoom, z->view.view_x0, z->view.view_y0);

    n = z->frames_total;
    if (n > 0) {
        printf("  gesture_update_us  : mean=%.2f  max=%.2f  (n=%llu)\n",
               z->sum_update_us / (double)n, z->max_update_us,
               (unsigned long long)n);
    }
    printf("--------------------\n");
}
