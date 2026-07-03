/*
 * fb_display.h - Linux Framebuffer 显示模块 API
 *
 * 提供:
 *   - 打开 /dev/fb0
 *   - 获取屏幕参数 (可变/固定)
 *   - mmap 显存
 *   - 将 V4L2 采集的 RGB565/YUYV 帧写入 framebuffer
 *   - 居中显示 (画面 < 屏幕) 或裁剪 (画面 > 屏幕)
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
    uint32_t line_length;   /* 一行字节数 (fix.smem_len / var.yres_virtual 或 fix.line_length) */
};

/*
 * 初始化 framebuffer:
 *   1. open /dev/fb0
 *   2. ioctl FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO
 *   3. mmap 显存
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
 * 支持的图像格式:
 *   RGB565 (V4L2_PIX_FMT_RGB565) → 直接拷贝
 *   YUYV   (V4L2_PIX_FMT_YUYV)   → 转为 RGB565 后写入
 *
 * 参数:
 *   fb      - 上下文指针
 *   data    - 帧数据
 *   width   - 图像宽度
 *   height  - 图像高度
 *   pixfmt  - 像素格式 (V4L2_PIX_FMT_RGB565 或 V4L2_PIX_FMT_YUYV)
 *
 * 返回: 成功返回 0, 失败返回 -1.
 */
int fb_display_show_frame(struct fb_display *fb, const void *data,
                          uint32_t width, uint32_t height, uint32_t pixfmt);

/*
 * 关闭 framebuffer:
 *   munmap + close
 */
void fb_display_deinit(struct fb_display *fb);

#endif /* FB_DISPLAY_H */
