/*
 * fb_display.h - Linux Framebuffer 显示模块 API
 *
 * 提供:
 *   - 打开 /dev/fb0 并读取真实几何 (分辨率 / bits_per_pixel / line_length /
 *     各通道 offset 与 length)
 *   - mmap 显存
 *   - 将 V4L2 采集的 YUYV / RGB565 帧写入 framebuffer
 *   - 居中显示 (画面 < 屏幕) 或裁剪 (画面 > 屏幕)
 *   - **数字缩放显示** (双指手势驱动, 最近邻插值, 见 fb_display_show_frame_zoomed)
 *
 * 位深支持: **16bpp (RGB565) 与 32bpp (XRGB/ARGB)**, 不再硬编码 32bpp。
 * 像素打包一律按 `var.red/green/blue.offset + length` 计算, 因此不假设固定的
 * 通道排布 (有的屏 R 在低位, 有的在高位)。
 *
 * 行步长一律使用 `fix.line_length`, 不假设等于 `width * bytes_per_pixel`。
 */

#ifndef FB_DISPLAY_H
#define FB_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

/* framebuffer 显示上下文 */
struct fb_display {
    int fd;                 /* /dev/fb0 文件描述符; 合成 framebuffer 测试时可为 -1 */
    void *fb_mmap;          /* mmap 映射的显存地址 (或测试用堆缓冲) */
    size_t fb_size;         /* 显存总大小 (bytes) */
    uint32_t screen_width;  /* 屏幕宽度 (像素) */
    uint32_t screen_height; /* 屏幕高度 (像素) */
    uint32_t screen_bpp;    /* 屏幕位深 (bits per pixel), 支持 16 / 32 */
    uint32_t bytes_per_pixel; /* screen_bpp / 8, 用于行内寻址 */
    uint32_t line_length;   /* 一行字节数 (fix.line_length) */
    uint32_t red_offset;    /* var.red.offset   - R 通道在像素字中的位偏移 */
    uint32_t green_offset;  /* var.green.offset - G 通道位偏移 */
    uint32_t blue_offset;   /* var.blue.offset  - B 通道位偏移 */
    uint32_t red_len;       /* var.red.length   - R 通道位宽 (5 或 8) */
    uint32_t green_len;     /* var.green.length */
    uint32_t blue_len;      /* var.blue.length */
    uint32_t transp_offset; /* var.transp.offset (32bpp 时用于补齐高位) */
    uint32_t transp_len;    /* var.transp.length */
    int      swap_rb;       /* RGB565 输入时是否交换 R/B (默认 0 = 不交换) */
};

/*
 * 显示视图 (数字缩放 + 平移)。
 *
 * 坐标约定: dst 是 LCD 像素坐标, src 是**源图像**像素坐标, 映射为
 *
 *     src_x = view_x0 + dst_x / zoom
 *     src_y = view_y0 + dst_y / zoom
 *
 * 也就是说 (view_x0, view_y0) 是"显示在 LCD 左上角 (0,0) 处的源像素坐标"。
 * 它可以是负数 —— 图像比屏幕小时靠居中显示, 左边界自然落在负坐标上。
 *
 * zoom = 1.0 且 view 取 fb_view_reset() 的初值时, 渲染结果与
 * fb_display_show_frame() 完全一致 (居中 + 裁剪), 因此默认路径不受影响。
 */
struct fb_view {
    double zoom;    /* 缩放倍率, 1.0 起 */
    double view_x0; /* LCD(0,0) 对应的源 x 坐标 (可为负) */
    double view_y0; /* LCD(0,0) 对应的源 y 坐标 (可为负) */
};

/*
 * 初始化 framebuffer:
 *   1. open /dev/fb0
 *   2. ioctl FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO
 *   3. 检查 bits_per_pixel 是否为 16 或 32
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
 * 设置 RGB565 输入是否交换 R/B 通道。
 *
 * 背景: i.MX6ULL 的 CSI 通路在输出 RGB565 时存在 R/B 通道交换, 即驱动报
 * "RGB565" 但数据实际按 BGR 排列。此时若板端 LCD 直接按 RGB565 解包,
 * 画面会整体偏蓝绿 (而 PC 端用 BGR565 解码才是正常色)。
 *
 * 默认 0 = 保持项目原有行为 (不交换)。只影响 RGB565 路径; YUYV 路径是
 * 真正的 YUV 转换, 与 R/B 顺序无关。
 */
void fb_display_set_swap_rb(struct fb_display *fb, int enable);

/*
 * 将一帧图像显示到 LCD (无缩放, 保持原有行为):
 *
 *   图像 < 屏幕 → 居中显示 (四周留黑)
 *   图像 > 屏幕 → 居中裁剪 (截取中间区域)
 *
 * 参数:
 *   fb         - 上下文指针
 *   data       - 帧数据
 *   width      - 图像宽度 (像素)
 *   height     - 图像高度 (像素)
 *   src_stride - 源图像行步长 (字节), 传 V4L2 的 ctx.bytesperline;
 *                传 0 时按 width*2 兜底
 *   pixfmt     - 像素格式 (V4L2_PIX_FMT_YUYV 或 V4L2_PIX_FMT_RGB565)
 *
 * 返回: 成功返回 0, 失败返回 -1.
 */
int fb_display_show_frame(struct fb_display *fb, const void *data,
                          uint32_t width, uint32_t height,
                          uint32_t src_stride, uint32_t pixfmt);

/* ----------------------------------------------------------------
 * 数字缩放显示
 * ---------------------------------------------------------------- */

/*
 * 把 view 复位到"居中且不缩放":
 *   zoom = 1.0
 *   view_x0 = (img_w - lcd_w) / 2   (图像小于屏幕时为负, 即左右留黑)
 *   view_y0 = (img_h - lcd_h) / 2
 *
 * 该初值下 fb_display_show_frame_zoomed() 与 fb_display_show_frame() 等价。
 */
void fb_view_reset(struct fb_view *v, uint32_t img_w, uint32_t img_h,
                   uint32_t lcd_w, uint32_t lcd_h);

/*
 * 把 view 限制在合理范围内:
 *   - 若可见源区域比图像小 (放大状态), 则不允许露出图像以外的空白,
 *     即 view_x0 ∈ [0, img_w - lcd_w/zoom]
 *   - 若可见源区域比图像大 (图像比屏幕小), 则强制居中
 *
 * 触摸缩放/平移后必须调用本函数, 否则会出现画面跑飞或边缘露出空白。
 */
void fb_view_clamp(struct fb_view *v, uint32_t img_w, uint32_t img_h,
                   uint32_t lcd_w, uint32_t lcd_h);

/* view 是否等价于"居中且不缩放" (用于决定走原始快速路径) */
int fb_view_is_fit(const struct fb_view *v, uint32_t img_w, uint32_t img_h,
                   uint32_t lcd_w, uint32_t lcd_h);

/*
 * 按 view 做最近邻插值的缩放显示。
 *
 * 实现要点 (为什么不是"逐目标像素换算") :
 *   逐目标像素做浮点反算 + 每像素格式转换, 在 i.MX6ULL 这种没有 NEON 加速
 *   路径的平台上太贵。这里改为:
 *     1. 预计算 dst_x → src_x 的整数映射表 (每帧一次, O(lcd_w))
 *     2. 逐行只反算一次 src_y, 并把该源行**整行解码成已打包的 fb 像素**
 *        放到行缓存里 (源行没变就复用缓存 —— 放大时同一源行会被多个
 *        目标行用到, 这是主要的省算力点)
 *     3. 内层只做 "查表 + 取缓存 + 写显存"
 *   越界的映射统一指向行缓存末尾的哨兵像素 (黑色), 因此**不存在越界读**。
 *
 * 参数:
 *   fb         - 上下文指针 (只要求 fb_mmap 有效; fd 可为 -1, 便于离线测试)
 *   data       - 源图像数据 (只读, 绝不被写入)
 *   width/height/src_stride/pixfmt - 同 fb_display_show_frame
 *   view       - 缩放/平移视图
 *
 * 返回: 0 成功; -1 参数或显存无效。
 * 注意: 不会向源图像缓冲区写入任何数据 (符合"不向 DMA 采集缓冲区写入"约束)。
 */
int fb_display_show_frame_zoomed(struct fb_display *fb, const void *data,
                                 uint32_t width, uint32_t height,
                                 uint32_t src_stride, uint32_t pixfmt,
                                 const struct fb_view *view);

/*
 * 关闭 framebuffer:
 *   munmap + close
 */
void fb_display_deinit(struct fb_display *fb);

#endif /* FB_DISPLAY_H */
