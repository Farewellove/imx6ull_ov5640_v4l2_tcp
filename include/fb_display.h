/*
 * fb_display.h - Linux Framebuffer 显示模块 API
 *
 * 提供:
 *   - 打开 /dev/fb0
 *   - 获取屏幕参数 (可变/固定)
 *   - mmap 显存
 *   - 将 V4L2 采集的 YUYV/RGB565 帧转换为 XRGB8888 写入 framebuffer
 *   - 居中显示 (画面 < 屏幕) 或裁剪 (画面 > 屏幕)
 *
 * 注意: 当前只支持 32bpp framebuffer (XRGB8888, 如 red.offset=16
 *       green.offset=8 blue.offset=0), 其他位深会初始化失败。
 */

#ifndef FB_DISPLAY_H
#define FB_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

/* framebuffer 显示上下文 */
struct fb_display {
    int fd;                 /* /dev/fb0 文件描述符 */
    void *fb_mmap;          /* mmap 映射的显存地址 */
    size_t fb_size;         /* 显存总大小 (bytes) */
    uint32_t screen_width;  /* 屏幕宽度 (像素) */
    uint32_t screen_height; /* 屏幕高度 (像素) */
    uint32_t screen_bpp;    /* 屏幕位深 (bits per pixel) */
    uint32_t line_length;   /* 一行字节数 (fix.line_length) */
    uint32_t red_offset;    /* var.red.offset   - R 通道在像素字中的位偏移 */
    uint32_t green_offset;  /* var.green.offset - G 通道位偏移 */
    uint32_t blue_offset;   /* var.blue.offset  - B 通道位偏移 */
};

/*
 * 初始化 framebuffer:
 *   1. open /dev/fb0
 *   2. ioctl FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO
 *   3. 检查 bits_per_pixel == 32 (只支持 XRGB8888 类格式)
 *   4. mmap 显存
 *
 * 参数:
 *   fb  - 上下文指针
 *   dev - 设备路径, 默认 "/dev/fb0"
 *
 * 返回: 成功返回 0, 失败返回 -1.
 */
int fb_display_init(struct fb_display *fb, const char *dev);

/*
 * 将一帧图像显示到 LCD:
 *
 *   图像 < 屏幕 → 居中显示 (四周留黑)
 *   图像 > 屏幕 → 居中裁剪 (截取中间区域)
 *
 * 支持的图像格式 (均转换为 XRGB8888 后写入):
 *   YUYV   (V4L2_PIX_FMT_YUYV)   → BT.601 定点转换
 *   RGB565 (V4L2_PIX_FMT_RGB565) → 解包 + 位扩展转换
 *   其他格式返回 -1, 不写显存
 *
 * 参数:
 *   fb         - 上下文指针
 *   data       - 帧数据
 *   width      - 图像宽度 (像素)
 *   height     - 图像高度 (像素)
 *   src_stride - 源图像行步长 (字节), 传 V4L2 的 ctx.bytesperline,
 *                避免驱动行尾 padding 导致错行; 传 0 时按 width*2 兜底
 *   pixfmt     - 像素格式 (V4L2_PIX_FMT_YUYV 或 V4L2_PIX_FMT_RGB565)
 *
 * 返回: 成功返回 0, 失败返回 -1.
 */
int fb_display_show_frame(struct fb_display *fb, const void *data,
                          uint32_t width, uint32_t height,
                          uint32_t src_stride, uint32_t pixfmt);

/*
 * 关闭 framebuffer:
 *   munmap + close
 */
void fb_display_deinit(struct fb_display *fb);

#endif /* FB_DISPLAY_H */
