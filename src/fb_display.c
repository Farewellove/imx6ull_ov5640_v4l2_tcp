/*
 * fb_display.c - Linux Framebuffer 显示模块实现
 *
 * 直接操作 /dev/fb0, 不使用 Qt/OpenCV/SDL.
 * 支持 RGB565 直接写入和 YUYV→RGB565 转换.
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

    /* 2. 获取可变信息 (分辨率, bpp) */
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

    printf("[FB] Screen: %ux%u, bpp=%u, line_length=%u\n",
           fb->screen_width, fb->screen_height,
           fb->screen_bpp, fb->line_length);

    /* 4. mmap 显存 */
    fb->fb_size = fix.smem_len;
    fb->fb_mmap = mmap(NULL, fb->fb_size,
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED, fb->fd, 0);
    if (fb->fb_mmap == MAP_FAILED) {
        perror("mmap framebuffer");
        fprintf(stderr, "[ERROR] mmap framebuffer failed (size=%zu)\n",
                fb->fb_size);
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
 * YUYV → RGB565 单像素转换 (定点运算)
 *
 * YUYV 4:2:2 格式: [Y0, U, Y1, V] 4 字节 = 2 个像素
 *   像素0: Y0, U, V
 *   像素1: Y1, U, V
 *
 * ITU-R BT.601 转换公式 (定点):
 *   R = (298 * (Y-16) + 409 * (V-128) + 128) >> 8
 *   G = (298 * (Y-16) - 100 * (U-128) - 208 * (V-128) + 128) >> 8
 *   B = (298 * (Y-16) + 516 * (U-128) + 128) >> 8
 * ================================================================ */
static inline uint16_t yuyv_to_rgb565(int y, int u, int v)
{
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;

    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;

    /* clamp to [0, 255] */
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;

    /* pack RGB565 */
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* ================================================================
 * 将一帧 RGB565 图像的指定区域拷贝到 framebuffer 的指定位置
 *
 * src       - 源图像数据
 * src_w, src_h - 源尺寸
 * src_x, src_y, src_copy_w, src_copy_h - 源中要拷贝的区域
 * fb        - framebuffer 上下文
 * dst_x, dst_y - 目标起始坐标
 * ================================================================ */
static void fb_copy_rgb565(struct fb_display *fb,
                           const uint8_t *src,
                           uint32_t src_w,
                           uint32_t src_x, uint32_t src_y,
                           uint32_t copy_w, uint32_t copy_h,
                           uint32_t dst_x, uint32_t dst_y)
{
    uint16_t *dst_base = (uint16_t *)fb->fb_mmap;
    const uint16_t *src_base = (const uint16_t *)src;
    uint32_t row;

    for (row = 0; row < copy_h; row++) {
        const uint16_t *src_row = src_base + (src_y + row) * src_w + src_x;
        uint16_t *dst_row = dst_base + (dst_y + row) * fb->screen_width + dst_x;
        memcpy(dst_row, src_row, copy_w * sizeof(uint16_t));
    }
}

/* ================================================================
 * 将一帧 YUYV 图像转为 RGB565 写入 framebuffer 指定位置
 *
 * yuyv_data - YUYV 数据
 * src_w, src_h - 图像宽高
 * src_x, src_y, copy_w, copy_h - 源中要拷贝/转换的区域
 * fb - framebuffer
 * dst_x, dst_y - 目标起始坐标
 * ================================================================ */
static void fb_copy_yuyv_to_rgb565(struct fb_display *fb,
                                   const uint8_t *yuyv_data,
                                   uint32_t src_w,
                                   uint32_t src_x, uint32_t src_y,
                                   uint32_t copy_w, uint32_t copy_h,
                                   uint32_t dst_x, uint32_t dst_y)
{
    uint16_t *dst_base = (uint16_t *)fb->fb_mmap;
    const uint8_t *src_base = yuyv_data;
    uint32_t row;

    /* 对齐 src_x 到偶数, 因为 YUYV 每 4 字节 = 2 像素 */
    uint32_t aligned_x = src_x & ~1U;

    for (row = 0; row < copy_h; row++) {
        const uint8_t *src_row = src_base + (src_y + row) * src_w * 2;
        uint16_t *dst_row = dst_base + (dst_y + row) * fb->screen_width + dst_x;
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
            dst_row[col] = yuyv_to_rgb565(y, u, v);
        }
    }
}

/* ================================================================
 * fb_display_show_frame - 将一帧显示到 LCD
 * ================================================================ */
int fb_display_show_frame(struct fb_display *fb, const void *data,
                          uint32_t width, uint32_t height, uint32_t pixfmt)
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
     * 按格式写入
     * ============================================================ */
    if (pixfmt == V4L2_PIX_FMT_RGB565) {
        fb_copy_rgb565(fb, src, width,
                       src_x, src_y, copy_w, copy_h, dst_x, dst_y);
    } else if (pixfmt == V4L2_PIX_FMT_YUYV) {
        fb_copy_yuyv_to_rgb565(fb, src, width,
                               src_x, src_y, copy_w, copy_h, dst_x, dst_y);
    } else {
        /* 不支持的格式, 忽略 (MJPEG 等需要解码, 不在本模块范围内) */
        fprintf(stderr, "[FB] Unsupported pixel format 0x%08X, skip display\n",
                pixfmt);
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
