/*
 * fb_display.c - Linux Framebuffer 显示模块实现
 *
 * 直接操作 /dev/fb0, 不使用 Qt/OpenCV/SDL.
 * 支持 YUYV→XRGB8888 (BT.601 定点转换) 和 RGB565→XRGB8888
 * (解包 + 位扩展) 后写入显存.
 *
 * 目标 framebuffer 必须是 32bpp (XRGB8888 类), 每像素 4 字节;
 * 行步长必须使用 fix.line_length, 不能假设等于 width * 4.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/videodev2.h>

#include "fb_display.h"

/* ================================================================
 * fb_display_init - 打开 framebuffer 并 mmap
 * ================================================================ */
int fb_display_init(struct fb_display *fb, const char *dev)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;

    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;

    if (dev == NULL)
        dev = "/dev/fb0";

    /* 1. 打开设备 */
    fb->fd = open(dev, O_RDWR);
    if (fb->fd < 0) {
        perror("open /dev/fb0");
        fprintf(stderr, "[ERROR] Failed to open %s\n", dev);
        goto fail;
    }
    printf("[FB] Opened %s (fd=%d)\n", dev, fb->fd);

    /* 2. 获取可变信息 (分辨率, bpp, 通道位偏移) */
    memset(&var, 0, sizeof(var));
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &var) < 0) {
        perror("FBIOGET_VSCREENINFO");
        fprintf(stderr, "[ERROR] Failed to get var screeninfo\n");
        goto fail;
    }

    /* 3. 获取固定信息 (line_length, smem_len) */
    memset(&fix, 0, sizeof(fix));
    if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        perror("FBIOGET_FSCREENINFO");
        fprintf(stderr, "[ERROR] Failed to get fix screeninfo\n");
        goto fail;
    }

    fb->screen_width  = var.xres;
    fb->screen_height = var.yres;
    fb->screen_bpp    = var.bits_per_pixel;
    fb->line_length   = fix.line_length;
    fb->red_offset    = var.red.offset;
    fb->green_offset  = var.green.offset;
    fb->blue_offset   = var.blue.offset;

    printf("[FB] Screen: %ux%u, bpp=%u, line_length=%u\n",
           fb->screen_width, fb->screen_height,
           fb->screen_bpp, fb->line_length);
    printf("[FB] red:   offset=%u length=%u\n",
           var.red.offset, var.red.length);
    printf("[FB] green: offset=%u length=%u\n",
           var.green.offset, var.green.length);
    printf("[FB] blue:  offset=%u length=%u\n",
           var.blue.offset, var.blue.length);

    /* 只支持 32bpp XRGB8888 类格式, 其他位深 (如 16bpp RGB565 屏) 直接失败,
     * 避免再用错误的每像素字节数写显存 */
    if (fb->screen_bpp != 32) {
        fprintf(stderr,
                "[ERROR] Unsupported framebuffer bpp=%u "
                "(only 32bpp XRGB8888 supported)\n",
                fb->screen_bpp);
        goto fail;
    }

    /* 4. mmap 显存 */
    fb->fb_size = fix.smem_len;
    fb->fb_mmap = mmap(NULL, fb->fb_size,
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED, fb->fd, 0);
    if (fb->fb_mmap == MAP_FAILED) {
        perror("mmap framebuffer");
        fprintf(stderr, "[ERROR] mmap framebuffer failed (size=%zu)\n",
                fb->fb_size);
        fb->fb_mmap = NULL;
        goto fail;
    }
    printf("[FB] mmap framebuffer: addr=%p, size=%zu\n",
           fb->fb_mmap, fb->fb_size);

    /* 清屏 (全黑) */
    memset(fb->fb_mmap, 0x00, fb->fb_size);

    return 0;

fail:
    if (fb->fd >= 0) {
        close(fb->fd);
        fb->fd = -1;
    }
    return -1;
}

/* ================================================================
 * YUYV → RGB888 单像素转换 (BT.601 有限范围, 定点运算)
 *
 * YUYV 4:2:2 格式: [Y0, U, Y1, V] 4 字节 = 2 个像素
 *   像素0: Y0, U, V
 *   像素1: Y1, U, V
 *
 * 公式:
 *   C = Y - 16   (C < 0 时取 0)
 *   D = U - 128
 *   E = V - 128
 *   R = (298*C + 409*E + 128) >> 8
 *   G = (298*C - 100*D - 208*E + 128) >> 8
 *   B = (298*C + 516*D + 128) >> 8
 *   R/G/B clamp 到 [0, 255]
 * ================================================================ */
static inline void yuyv_to_rgb888(int y, int u, int v,
                                  int *r, int *g, int *b)
{
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;

    if (c < 0)
        c = 0;

    *r = (298 * c + 409 * e + 128) >> 8;
    *g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    *b = (298 * c + 516 * d + 128) >> 8;

    /* clamp to [0, 255] */
    if (*r < 0) *r = 0; if (*r > 255) *r = 255;
    if (*g < 0) *g = 0; if (*g > 255) *g = 255;
    if (*b < 0) *b = 0; if (*b > 255) *b = 255;
}

/* ================================================================
 * 按 fb 的通道位偏移把 RGB888 打包成 32 位像素值.
 * 本环境 red.offset=16 green.offset=8 blue.offset=0 → 0x00RRGGBB
 * ================================================================ */
static inline uint32_t fb_pack_rgb888(const struct fb_display *fb,
                                      int r, int g, int b)
{
    return ((uint32_t)r << fb->red_offset) |
           ((uint32_t)g << fb->green_offset) |
           ((uint32_t)b << fb->blue_offset);
}

/* ================================================================
 * 将一帧 RGB565 图像的指定区域转换为 XRGB8888 写入 framebuffer
 *
 * src        - 源图像数据
 * src_stride - 源行步长 (字节)
 * src_w      - 源图像宽度 (像素)
 * src_x, src_y, copy_w, copy_h - 源中要拷贝的区域
 * fb         - framebuffer 上下文
 * dst_x, dst_y - 目标起始坐标
 * ================================================================ */
static void fb_copy_rgb565_to_xrgb8888(struct fb_display *fb,
                                       const uint8_t *src,
                                       uint32_t src_stride,
                                       uint32_t src_x, uint32_t src_y,
                                       uint32_t copy_w, uint32_t copy_h,
                                       uint32_t dst_x, uint32_t dst_y)
{
    uint8_t *fb_base = (uint8_t *)fb->fb_mmap;
    uint32_t row;

    for (row = 0; row < copy_h; row++) {
        const uint16_t *src_row =
            (const uint16_t *)(src + (src_y + row) * src_stride);
        /* 行首地址必须用 line_length, 不能用 screen_width */
        uint32_t *dst_line =
            (uint32_t *)(fb_base + (dst_y + row) * fb->line_length);
        uint32_t col;

        for (col = 0; col < copy_w; col++) {
            uint16_t px = src_row[src_x + col];
            int r5 = (px >> 11) & 0x1f;
            int g6 = (px >> 5)  & 0x3f;
            int b5 =  px        & 0x1f;

            /* 5/6 bit → 8 bit 位扩展 */
            int r = (r5 << 3) | (r5 >> 2);
            int g = (g6 << 2) | (g6 >> 4);
            int b = (b5 << 3) | (b5 >> 2);

            dst_line[dst_x + col] = fb_pack_rgb888(fb, r, g, b);
        }
    }
}

/* ================================================================
 * 将一帧 YUYV 图像转换为 XRGB8888 写入 framebuffer 指定位置
 *
 * yuyv_data  - YUYV 数据
 * src_stride - 源行步长 (字节, 用 V4L2 bytesperline, 含行尾 padding)
 * src_w      - 图像宽高 (像素)
 * src_x, src_y, copy_w, copy_h - 源中要拷贝/转换的区域
 * fb         - framebuffer 上下文
 * dst_x, dst_y - 目标起始坐标
 * ================================================================ */
static void fb_copy_yuyv_to_xrgb8888(struct fb_display *fb,
                                     const uint8_t *yuyv_data,
                                     uint32_t src_stride,
                                     uint32_t src_x, uint32_t src_y,
                                     uint32_t copy_w, uint32_t copy_h,
                                     uint32_t dst_x, uint32_t dst_y)
{
    uint8_t *fb_base = (uint8_t *)fb->fb_mmap;
    uint32_t row;

    /* 对齐 src_x 到偶数, 因为 YUYV 每 4 字节 = 2 像素,
     * 奇数起点会导致 U/V 错位 */
    uint32_t aligned_x = src_x & ~1U;
    uint32_t delta = src_x - aligned_x; /* 0 或 1 */

    /* 源起点向左对齐后, 目标位置同步右移 delta,
     * 保证画面裁剪位置不变 (不会整体左移 1 像素);
     * 若超出屏幕右边界, 缩减拷贝宽度防止越界 */
    uint32_t out_x = dst_x + delta;
    if (out_x + copy_w > fb->screen_width)
        copy_w = fb->screen_width - out_x;

    for (row = 0; row < copy_h; row++) {
        const uint8_t *src_row =
            yuyv_data + (src_y + row) * src_stride;
        /* 行首地址必须用 line_length, 不能用 screen_width */
        uint32_t *dst_line =
            (uint32_t *)(fb_base + (dst_y + row) * fb->line_length);
        uint32_t col;

        for (col = 0; col < copy_w; col++) {
            uint32_t px = aligned_x + col;
            /* YUYV: [Y0, U, Y1, V] 每 2 像素一组 */
            uint32_t pair_off = (px / 2) * 4;
            uint8_t  y0 = src_row[pair_off + 0];
            uint8_t  u  = src_row[pair_off + 1];
            uint8_t  y1 = src_row[pair_off + 2];
            uint8_t  v  = src_row[pair_off + 3];
            int y = (px & 1) ? y1 : y0;
            int r, g, b;

            yuyv_to_rgb888(y, u, v, &r, &g, &b);
            dst_line[out_x + col] = fb_pack_rgb888(fb, r, g, b);
        }
    }
}

/* ================================================================
 * fb_display_show_frame - 将一帧显示到 LCD
 * ================================================================ */
int fb_display_show_frame(struct fb_display *fb, const void *data,
                          uint32_t width, uint32_t height,
                          uint32_t src_stride, uint32_t pixfmt)
{
    const uint8_t *src = (const uint8_t *)data;
    uint32_t scr_w = fb->screen_width;
    uint32_t scr_h = fb->screen_height;
    uint32_t copy_w, copy_h;
    uint32_t src_x, src_y, dst_x, dst_y;

    if (fb->fd < 0 || fb->fb_mmap == NULL) {
        fprintf(stderr, "[FB] Not initialized\n");
        return -1;
    }

    /* 行步长兜底: 调用者没传 bytesperline 时按紧凑行宽 */
    if (src_stride == 0)
        src_stride = width * 2;

    /* ============================================================
     * 计算显示区域:
     *
     *   图像 <= 屏幕 → 居中显示
     *   图像 >  屏幕 → 居中裁剪
     * ============================================================ */
    if (width <= scr_w) {
        copy_w = width;
        src_x  = 0;
        dst_x  = (scr_w - width) / 2;
    } else {
        copy_w = scr_w;
        src_x  = (width - scr_w) / 2;
        dst_x  = 0;
    }

    if (height <= scr_h) {
        copy_h = height;
        src_y  = 0;
        dst_y  = (scr_h - height) / 2;
    } else {
        copy_h = scr_h;
        src_y  = (height - scr_h) / 2;
        dst_y  = 0;
    }

    /* ============================================================
     * 按格式转换后写入 (输出统一为 32bpp XRGB8888)
     * ============================================================ */
    if (pixfmt == V4L2_PIX_FMT_YUYV) {
        fb_copy_yuyv_to_xrgb8888(fb, src, src_stride,
                                 src_x, src_y, copy_w, copy_h,
                                 dst_x, dst_y);
    } else if (pixfmt == V4L2_PIX_FMT_RGB565) {
        fb_copy_rgb565_to_xrgb8888(fb, src, src_stride,
                                   src_x, src_y, copy_w, copy_h,
                                   dst_x, dst_y);
    } else {
        /* 不支持的格式, 忽略 (MJPEG 等需要解码, 不在本模块范围内) */
        fprintf(stderr,
                "[FB] Unsupported pixel format 0x%08X, skip display\n",
                pixfmt);
        return -1;
    }

    return 0;
}

/* ================================================================
 * fb_display_deinit - 关闭 framebuffer
 * ================================================================ */
void fb_display_deinit(struct fb_display *fb)
{
    if (fb->fb_mmap != NULL && fb->fb_mmap != MAP_FAILED) {
        /* 清屏 */
        memset(fb->fb_mmap, 0x00, fb->fb_size);
        if (munmap(fb->fb_mmap, fb->fb_size) < 0)
            perror("munmap framebuffer");
        else
            printf("[FB] munmap done\n");
        fb->fb_mmap = NULL;
    }

    if (fb->fd >= 0) {
        if (close(fb->fd) < 0)
            perror("close /dev/fb0");
        printf("[FB] Device closed\n");
        fb->fd = -1;
    }
}
