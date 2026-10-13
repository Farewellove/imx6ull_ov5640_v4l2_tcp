/*
 * fb_display.c - Linux Framebuffer 显示模块实现
 *
 * 直接操作 framebuffer, 不使用 Qt/OpenCV/SDL.
 *
 * 支持:
 *   - 目标位深 16bpp (RGB565) 与 32bpp (XRGB/ARGB), 按 var.red/green/blue
 *     的 offset+length 通用打包, 不假设固定通道排布
 *   - 源格式 YUYV → RGB (BT.601 定点转换) 与 RGB565 → RGB (解包 + 位扩展)
 *   - 无缩放的居中/裁剪显示 (fb_display_show_frame)
 *   - 数字缩放的最近邻显示 (fb_display_show_frame_zoomed)
 *
 * 两条必须遵守的约束:
 *   1) 行步长一律用 fix.line_length, 绝不假设 == width * bytes_per_pixel
 *   2) **只读**源图像缓冲区, 绝不向其写入 (源是 V4L2 DMA 采集缓冲区, 写完
 *      一旦 QBUF 归还驱动就会被覆写, 且会破坏帧内容)
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

/* 缩放倍率的硬边界 (手势层另有 1.0–2.0 的软范围; 这里只是防跑飞的兜底) */
#define FB_ZOOM_MIN   1.0
#define FB_ZOOM_MAX   8.0

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

    /* 2. 可变信息 (分辨率, bpp, 通道位偏移与位宽) */
    memset(&var, 0, sizeof(var));
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &var) < 0) {
        perror("FBIOGET_VSCREENINFO");
        fprintf(stderr, "[ERROR] Failed to get var screeninfo\n");
        goto fail;
    }

    /* 3. 固定信息 (line_length, smem_len) */
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
    fb->red_len       = var.red.length;
    fb->green_len     = var.green.length;
    fb->blue_len      = var.blue.length;
    fb->transp_offset = var.transp.offset;
    fb->transp_len    = var.transp.length;
    fb->swap_rb       = 0;   /* 由 fb_display_set_swap_rb() 设置 */

    printf("[FB] Screen: %ux%u, bpp=%u, line_length=%u\n",
           fb->screen_width, fb->screen_height,
           fb->screen_bpp, fb->line_length);
    printf("[FB] red:   offset=%u length=%u\n",
           var.red.offset, var.red.length);
    printf("[FB] green: offset=%u length=%u\n",
           var.green.offset, var.green.length);
    printf("[FB] blue:  offset=%u length=%u\n",
           var.blue.offset, var.blue.length);

    /*
     * 只支持 16bpp (RGB565) 与 32bpp。
     *
     * 之前的实现只认 32bpp, 遇到 16bpp 屏直接初始化失败 —— 而 i.MX6ULL
     * 上常见的 1024x600 7 寸屏就是 16bpp。数字缩放功能必须能在这类屏上
     * 工作, 所以这里放开到 16/32, 并统一按 var 里的 offset/length 打包。
     */
    if (fb->screen_bpp != 16 && fb->screen_bpp != 32) {
        fprintf(stderr,
                "[ERROR] Unsupported framebuffer bpp=%u (only 16/32 supported)\n",
                fb->screen_bpp);
        goto fail;
    }
    fb->bytes_per_pixel = fb->screen_bpp / 8;

    /* 行步长必须能容纳一整行, 否则 write 会错行 */
    if (fb->line_length < fb->screen_width * fb->bytes_per_pixel) {
        fprintf(stderr,
                "[ERROR] line_length=%u too small for width=%u x %u bytes\n",
                fb->line_length, fb->screen_width, fb->bytes_per_pixel);
        goto fail;
    }

    /* 4. mmap 显存 */
    fb->fb_size = fix.smem_len;
    if (fb->fb_size < (size_t)fb->line_length * fb->screen_height) {
        fprintf(stderr,
                "[ERROR] smem_len=%zu smaller than one screen (%u x %u)\n",
                fb->fb_size, fb->line_length, fb->screen_height);
        goto fail;
    }

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
 * YUYV → RGB 单像素转换 (BT.601 有限范围, 定点运算)
 *
 * YUYV 4:2:2: [Y0, U, Y1, V] 4 字节 = 2 个像素, 每组的 U/V 被两个像素共用。
 *
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

    /* 每个 if 只保护紧邻的一条语句, 拆成多行写是为了避免现代编译器
     * 报 -Wmisleading-indentation。逻辑与原来完全一致。 */
    if (*r < 0) *r = 0;
    if (*r > 255) *r = 255;
    if (*g < 0) *g = 0;
    if (*g > 255) *g = 255;
    if (*b < 0) *b = 0;
    if (*b > 255) *b = 255;
}

/* ================================================================
 * RGB888 → 目标 framebuffer 像素值
 *
 * 通用实现: 按 var 里各通道的 length 右移截断, 再按 offset 就位。
 * 这样 16bpp (R5/G6/B5) 与 32bpp (R8/G8/B8) 共用一套代码, 也不假设
 * 通道顺序 (某些屏 R 在低位)。
 * ================================================================ */
static inline uint32_t fb_pack_rgb(const struct fb_display *fb,
                                   int r, int g, int b)
{
    uint32_t px = 0;

    if (fb->red_len)
        px |= (uint32_t)(r >> (8 - (int)fb->red_len)) << fb->red_offset;
    if (fb->green_len)
        px |= (uint32_t)(g >> (8 - (int)fb->green_len)) << fb->green_offset;
    if (fb->blue_len)
        px |= (uint32_t)(b >> (8 - (int)fb->blue_len)) << fb->blue_offset;

    /* 32bpp 且带透明通道时把高位补满, 避免 ARGB 屏上画出来是全透明 */
    if (fb->transp_len)
        px |= (uint32_t)((1u << fb->transp_len) - 1u) << fb->transp_offset;

    return px;
}

/* ================================================================
 * 源数据 → RGB888 的单行解码辅助
 *
 * 统一收口"源格式 → 8/8/8"这一步, 避免每种绘制路径各写一份转换。
 * ================================================================ */

/* 解出源行第 x 个像素的 RGB888 (调用方保证 x 在 [0,width) 内) */
static inline void src_pixel_rgb(const struct fb_display *fb,
                                 uint32_t pixfmt, const uint8_t *src_row,
                                 uint32_t x, int *r, int *g, int *b)
{
    if (pixfmt == V4L2_PIX_FMT_YUYV) {
        /* YUYV: [Y0,U,Y1,V], 每 2 像素一组 */
        uint32_t off = (x / 2) * 4;
        uint8_t y0 = src_row[off + 0];
        uint8_t u  = src_row[off + 1];
        uint8_t y1 = src_row[off + 2];
        uint8_t v  = src_row[off + 3];
        int y = (x & 1) ? (int)y1 : (int)y0;

        yuyv_to_rgb888(y, u, v, r, g, b);
    } else {
        /* RGB565 (小端 16bit 字), swap_rb 时把 R/B 对调 */
        const uint16_t *s = (const uint16_t *)src_row;
        uint16_t p = s[x];
        int r5, g6, b5;

        if (fb->swap_rb) {
            b5 = (p >> 11) & 0x1f;
            g6 = (p >> 5)  & 0x3f;
            r5 =  p        & 0x1f;
        } else {
            r5 = (p >> 11) & 0x1f;
            g6 = (p >> 5)  & 0x3f;
            b5 =  p        & 0x1f;
        }
        /* 5/6 bit → 8 bit 位扩展 (把高位补到低位, 满量程对齐) */
        *r = (r5 << 3) | (r5 >> 2);
        *g = (g6 << 2) | (g6 >> 4);
        *b = (b5 << 3) | (b5 >> 2);
    }
}

/*
 * 把源行 src_row 的 [src_x, src_x+n) 转换并写入目标行 dst_row 的
 * [dst_x, dst_x+n)。
 *
 * bpp 与格式的分支都在循环外, 内层循环保持干净 —— 这段是所有绘制路径
 * 的最热代码, i.MX6ULL 上没有 NEON 加速路径, 每像素多一个分支都要还回来。
 */
static void fb_convert_span(const struct fb_display *fb, uint32_t pixfmt,
                            const uint8_t *src_row, uint32_t src_x,
                            uint8_t *dst_row, uint32_t dst_x, uint32_t n)
{
    uint32_t i;

    if (fb->screen_bpp == 16) {
        uint16_t *d = (uint16_t *)dst_row;
        for (i = 0; i < n; i++) {
            int r, g, b;
            src_pixel_rgb(fb, pixfmt, src_row, src_x + i, &r, &g, &b);
            d[dst_x + i] = (uint16_t)fb_pack_rgb(fb, r, g, b);
        }
    } else {
        uint32_t *d = (uint32_t *)dst_row;
        for (i = 0; i < n; i++) {
            int r, g, b;
            src_pixel_rgb(fb, pixfmt, src_row, src_x + i, &r, &g, &b);
            d[dst_x + i] = fb_pack_rgb(fb, r, g, b);
        }
    }
}

/* 把源行整行解码成"已按目标格式打包"的像素数组 (缩放渲染的行缓存用) */
static void fb_decode_row_packed(const struct fb_display *fb, uint32_t pixfmt,
                                 const uint8_t *src_row, uint32_t w,
                                 uint32_t *px)
{
    uint32_t x;

    for (x = 0; x < w; x++) {
        int r, g, b;
        src_pixel_rgb(fb, pixfmt, src_row, x, &r, &g, &b);
        px[x] = fb_pack_rgb(fb, r, g, b);
    }
}

/* 按位深把显存中一行填成黑色 */
static void fb_fill_row_black(const struct fb_display *fb, uint8_t *dst_row,
                              uint32_t n)
{
    memset(dst_row, 0x00, (size_t)n * fb->bytes_per_pixel);
}

/* ================================================================
 * fb_display_set_swap_rb - 运行时开关: RGB565 输入是否交换 R/B
 * ================================================================ */
void fb_display_set_swap_rb(struct fb_display *fb, int enable)
{
    fb->swap_rb = enable ? 1 : 0;
}

/* ================================================================
 * 无缩放的区域 blit (居中 / 裁剪路径)
 *
 * 语义与改造前完全一致, 包括 YUYV 的偶数对齐处理:
 *   YUYV 每 4 字节 = 2 像素, 若 src_x 是奇数, U/V 会错位,
 *   因此把源起点左移到偶数, 目标位置同步右移, 保证裁剪位置不变。
 * ================================================================ */
static void fb_blit_region(const struct fb_display *fb, const char *src,
                           uint32_t src_stride, uint32_t pixfmt,
                           uint32_t src_x, uint32_t src_y,
                           uint32_t copy_w, uint32_t copy_h,
                           uint32_t dst_x, uint32_t dst_y)
{
    uint8_t *base = (uint8_t *)fb->fb_mmap;
    uint32_t aligned_x = src_x;
    uint32_t out_x = dst_x;
    uint32_t row;

    if (pixfmt == V4L2_PIX_FMT_YUYV) {
        uint32_t a = src_x & ~1u;
        uint32_t delta = src_x - a;

        aligned_x = a;
        out_x = dst_x + delta;
        if (out_x + copy_w > fb->screen_width) {
            if (fb->screen_width <= out_x)
                return;
            copy_w = fb->screen_width - out_x;
        }
    }

    for (row = 0; row < copy_h; row++) {
        const uint8_t *srow = (const uint8_t *)src + (src_y + row) * src_stride;
        /* 行首地址必须用 line_length, 不能用 screen_width */
        uint8_t *drow = base + (size_t)(dst_y + row) * fb->line_length;

        fb_convert_span(fb, pixfmt, srow, aligned_x, drow, out_x, copy_w);
    }
}

/* ================================================================
 * fb_display_show_frame - 居中显示 / 居中裁剪 (无缩放)
 * ================================================================ */
int fb_display_show_frame(struct fb_display *fb, const void *data,
                          uint32_t width, uint32_t height,
                          uint32_t src_stride, uint32_t pixfmt)
{
    const char *src = (const char *)data;
    uint32_t scr_w, scr_h, copy_w, copy_h, src_x, src_y, dst_x, dst_y;

    if (fb == NULL || fb->fb_mmap == NULL) {
        fprintf(stderr, "[FB] Not initialized\n");
        return -1;
    }
    if (data == NULL || width == 0 || height == 0)
        return -1;
    if (pixfmt != V4L2_PIX_FMT_YUYV && pixfmt != V4L2_PIX_FMT_RGB565) {
        fprintf(stderr,
                "[FB] Unsupported pixel format 0x%08X, skip display\n",
                pixfmt);
        return -1;
    }

    scr_w = fb->screen_width;
    scr_h = fb->screen_height;

    /* 行步长兜底: 调用者没传 bytesperline 时按紧凑行宽 */
    if (src_stride == 0)
        src_stride = width * 2;

    /* 图像 <= 屏幕 → 居中显示; 图像 > 屏幕 → 居中裁剪 */
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

    fb_blit_region(fb, src, src_stride, pixfmt,
                   src_x, src_y, copy_w, copy_h, dst_x, dst_y);
    return 0;
}

/* ================================================================
 * 视图辅助
 * ================================================================ */
void fb_view_reset(struct fb_view *v, uint32_t img_w, uint32_t img_h,
                   uint32_t lcd_w, uint32_t lcd_h)
{
    if (v == NULL)
        return;
    v->zoom = 1.0;
    /* 居中: 图像比屏幕小时 view_x0 为负, 于是左右留黑 */
    v->view_x0 = ((double)img_w - (double)lcd_w) / 2.0;
    v->view_y0 = ((double)img_h - (double)lcd_h) / 2.0;
}

void fb_view_clamp(struct fb_view *v, uint32_t img_w, uint32_t img_h,
                   uint32_t lcd_w, uint32_t lcd_h)
{
    double span_w, span_h;

    if (v == NULL)
        return;

    if (!(v->zoom > 0.0))
        v->zoom = 1.0;
    if (v->zoom < FB_ZOOM_MIN)
        v->zoom = FB_ZOOM_MIN;
    if (v->zoom > FB_ZOOM_MAX)
        v->zoom = FB_ZOOM_MAX;

    span_w = (double)lcd_w / v->zoom;   /* 一屏能看到的源宽 (源像素) */
    span_h = (double)lcd_h / v->zoom;

    if (span_w <= (double)img_w) {
        /* 可见区域比图像小: 不允许露出图像以外的空白 */
        if (v->view_x0 < 0.0)
            v->view_x0 = 0.0;
        if (v->view_x0 + span_w > (double)img_w)
            v->view_x0 = (double)img_w - span_w;
    } else {
        /* 图像比可见区域还小: 只能居中, 左右留黑 */
        v->view_x0 = ((double)img_w - span_w) / 2.0;
    }

    if (span_h <= (double)img_h) {
        if (v->view_y0 < 0.0)
            v->view_y0 = 0.0;
        if (v->view_y0 + span_h > (double)img_h)
            v->view_y0 = (double)img_h - span_h;
    } else {
        v->view_y0 = ((double)img_h - span_h) / 2.0;
    }
}

int fb_view_is_fit(const struct fb_view *v, uint32_t img_w, uint32_t img_h,
                   uint32_t lcd_w, uint32_t lcd_h)
{
    struct fb_view fit;

    if (v == NULL)
        return 0;
    fb_view_reset(&fit, img_w, img_h, lcd_w, lcd_h);

    /* 浮点比较留一个很小的容差, 避免累积误差导致误判 */
    if (v->zoom < fit.zoom - 1e-9 || v->zoom > fit.zoom + 1e-9)
        return 0;
    if (v->view_x0 < fit.view_x0 - 0.5 || v->view_x0 > fit.view_x0 + 0.5)
        return 0;
    if (v->view_y0 < fit.view_y0 - 0.5 || v->view_y0 > fit.view_y0 + 0.5)
        return 0;
    return 1;
}

/* ================================================================
 * fb_display_show_frame_zoomed - 最近邻数字缩放显示
 *
 * 算法 (为什么这么写, 见头文件说明):
 *   map_x[dx] : dst 列 → 源列 (越界记为哨兵下标 width)
 *   rowcache  : 当前源行的"已打包像素"整行缓存, 末位是黑色哨兵
 *   逐行只反算一次 src_y; 源行未变则复用 rowcache
 *
 * 越界安全:
 *   - 源列越界 → 取哨兵 (width), 上界受 rowcache 容量 (width+1) 保护
 *   - 源行越界 → 整行填空黑, 不访问源数据
 *   - 目标写入范围是 [0, lcd_w) / [0, lcd_h), 由 line_length 与 map 共同保证
 *   - 源缓冲区自始至终**只读**
 * ================================================================ */
int fb_display_show_frame_zoomed(struct fb_display *fb, const void *data,
                                 uint32_t width, uint32_t height,
                                 uint32_t src_stride, uint32_t pixfmt,
                                 const struct fb_view *view)
{
    const uint8_t *src = (const uint8_t *)data;
    uint32_t lcd_w, lcd_h, dy, dx;
    uint32_t *map_x = NULL;
    uint32_t *rowcache = NULL;
    int32_t cached_src_y = -1;
    double zoom;
    int rc = -1;

    /* 注意: 这里只要求 fb_mmap 有效, 不要求 fd —— 这样离线测试可以把
     * fb_mmap 指向堆缓冲, 用同一份生产代码做渲染基准测试。 */
    if (fb == NULL || fb->fb_mmap == NULL) {
        fprintf(stderr, "[FB] Not initialized\n");
        return -1;
    }
    if (data == NULL || view == NULL || width == 0 || height == 0)
        return -1;
    if (pixfmt != V4L2_PIX_FMT_YUYV && pixfmt != V4L2_PIX_FMT_RGB565) {
        fprintf(stderr,
                "[FB] Unsupported pixel format 0x%08X for zoomed display\n",
                pixfmt);
        return -1;
    }

    if (src_stride == 0)
        src_stride = width * 2;

    zoom = view->zoom;
    if (!(zoom > 0.0))
        zoom = 1.0;
    if (zoom < FB_ZOOM_MIN)
        zoom = FB_ZOOM_MIN;
    if (zoom > FB_ZOOM_MAX)
        zoom = FB_ZOOM_MAX;

    lcd_w = fb->screen_width;
    lcd_h = fb->screen_height;
    if (lcd_w == 0 || lcd_h == 0)
        return -1;

    map_x    = (uint32_t *)malloc(sizeof(uint32_t) * lcd_w);
    rowcache = (uint32_t *)malloc(sizeof(uint32_t) * ((size_t)width + 1));
    if (map_x == NULL || rowcache == NULL) {
        fprintf(stderr, "[FB] out of memory for zoom buffers\n");
        goto out;
    }

    /* dst 列 → 源列。越界统一指向哨兵下标 width。 */
    for (dx = 0; dx < lcd_w; dx++) {
        double sx = view->view_x0 + (double)dx / zoom;

        if (sx < 0.0) {
            map_x[dx] = width;
            continue;
        }
        {
            uint32_t ix = (uint32_t)sx;     /* sx >= 0, 直接截断即 floor */
            map_x[dx] = (ix < width) ? ix : width;
        }
    }

    /* 哨兵 = 黑 */
    rowcache[width] = fb_pack_rgb(fb, 0, 0, 0);

    for (dy = 0; dy < lcd_h; dy++) {
        double sy = view->view_y0 + (double)dy / zoom;
        uint8_t *drow = (uint8_t *)fb->fb_mmap
                        + (size_t)dy * fb->line_length;
        int32_t iy;

        if (sy < 0.0) {
            fb_fill_row_black(fb, drow, lcd_w);
            continue;
        }
        iy = (int32_t)sy;
        if (iy >= (int32_t)height) {
            fb_fill_row_black(fb, drow, lcd_w);
            continue;
        }

        if (iy != cached_src_y) {
            const uint8_t *srow = src + (size_t)iy * src_stride;
            fb_decode_row_packed(fb, pixfmt, srow, width, rowcache);
            cached_src_y = iy;
        }

        if (fb->screen_bpp == 16) {
            uint16_t *d = (uint16_t *)drow;
            for (dx = 0; dx < lcd_w; dx++)
                d[dx] = (uint16_t)rowcache[map_x[dx]];
        } else {
            uint32_t *d = (uint32_t *)drow;
            for (dx = 0; dx < lcd_w; dx++)
                d[dx] = rowcache[map_x[dx]];
        }
    }

    rc = 0;

out:
    free(map_x);
    free(rowcache);
    return rc;
}

/* ================================================================
 * fb_display_deinit - 关闭 framebuffer
 * ================================================================ */
void fb_display_deinit(struct fb_display *fb)
{
    if (fb == NULL)
        return;

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
