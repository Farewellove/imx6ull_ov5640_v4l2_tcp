/*
 * pc_receiver.c - PC 端接收保存 + SDL2 实时显示
 *
 * 默认: 根据 frame_header.pixfmt 自动选择解码方式。
 * 支持 --display-format / --yuv-order / --swap-rb 覆盖。
 *
 * 用法:
 *   ./pc_receiver -p 5000 -o ./frames -d                  # auto-detect
 *   ./pc_receiver -p 5000 -d --display-format yuyv         # force YUYV
 *   ./pc_receiver -p 5000 -d --yuv-order uyvy              # YUYV header, UYVY decode
 *
 * SDL keys: 1=RGB565 2=RGB565X 3=BGR565 4=BGR565X 5=YUYV 6=UYVY 7=YVYU 8=VYUY
 *           0=auto(hdr) ESC/Q=Quit
 *
 * ------------------------------------------------------------------
 * 程序结构
 * ------------------------------------------------------------------
 *   tcp_server_listen  监听端口
 *     └─ accept        只接受一个客户端, 收发都在单线程里轮流做
 *          └─ 收帧循环 {
 *                 recv_frame_header    收 40 字节帧头 + ntohl 转回本机序
 *                 recv_all             按 hdr.size 收完整帧数据
 *                 sdl_display_show_frame   可选: 转 RGB888 推给 SDL 纹理
 *                 save_raw_frame       原始数据落盘 frame_XXXXXX.<ext>
 *                 save_frame_as_ppm    能解码时另存 PPM, 便于肉眼比对颜色
 *             }
 *
 * ------------------------------------------------------------------
 * 解码格式的优先级
 * ------------------------------------------------------------------
 *   1. --display-format / --swap-rb / --force-* / SDL 数字键  → g_disp_override
 *   2. --yuv-order (仅当帧头声明的是 YUV422)                   → g_yuv_order
 *   3. 帧头 hdr.pixfmt 原样使用 (auto-detect)
 *
 * 优先级每次收帧都重新算一遍, 所以 SDL 窗口里按键切换是立即生效的
 * (这是调"颜色不对"问题的主要手段)。
 *
 * 重要: 这些覆盖只影响 PC 端**如何解释**数据, 不改变落盘的 raw 数据。
 *       因此同一份 raw 文件事后可以换不同参数重新解码验证。
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <poll.h>
#include <arpa/inet.h>
#include <getopt.h>

#include "frame_proto.h"
#include "tcp_socket.h"
#include "metrics.h"

#ifdef ENABLE_SDL_DISPLAY
#include <SDL2/SDL.h>
#endif

static volatile sig_atomic_t g_running = 1;
static void signal_handler(int sig) { (void)sig; g_running = 0; }

/* ================================================================
 * 颜色转换: 各种格式 → RGB888 (逐行转换)
 *
 * 统一产出 RGB888 (每像素 3 字节, R/G/B 顺序), 这样 SDL 纹理和
 * PPM 保存可以共用同一套解码器 —— 屏幕上看到的和存下来的必然一致,
 * 不会出现"显示对但文件错"这种干扰排查的情况。
 *
 * 为什么要支持这么多种 565 变体:
 *   i.MX6ULL 的 CSI 通路存在 R/B 通道交换, 驱动报 RGB565 但实际
 *   数据是 BGR565。所以除了标准 RGB565(LE/BE), 还有两个内部自定义
 *   FOURCC (PIX_FMT_BGR565/BGR565X) 仅用于在 PC 端标定 R/B 顺序,
 *   不参与任何 V4L2 ioctl。
 *
 * 位扩展 (5/6 bit → 8 bit):
 *   r8=(r5<<3)|(r5>>2), g8=(g6<<2)|(g6>>4), b8=(b5<<3)|(b5>>2)
 *   即把高位补到低位上, 使 0x1F → 0xFF、0x3F → 0xFF 满量程对齐,
 *   比单纯左移更不容易出现"白色发灰"。
 * ================================================================ */

/* RGB565 小端: 内存低字节在前, 16 位字内 [15:11]=R [10:5]=G [4:0]=B */
static void rgb565_le_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for (x=0;x<w;x++) { uint16_t p=src[x*2]|(src[x*2+1]<<8);
  uint8_t r5=(p>>11)&0x1f,g6=(p>>5)&0x3f,b5=p&0x1f;
  *rgb++=(r5<<3)|(r5>>2); *rgb++=(g6<<2)|(g6>>4); *rgb++=(b5<<3)|(b5>>2); }}

/* RGB565 大端: 高字节在前 (V4L2 里叫 RGB565X), 常见于传感器直接输出 */
static void rgb565_be_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for (x=0;x<w;x++) { uint16_t p=(src[x*2]<<8)|src[x*2+1];
  uint8_t r5=(p>>11)&0x1f,g6=(p>>5)&0x3f,b5=p&0x1f;
  *rgb++=(r5<<3)|(r5>>2); *rgb++=(g6<<2)|(g6>>4); *rgb++=(b5<<3)|(b5>>2); }}

/* BGR565 小端: 与上面唯一的区别是 [15:11] 当 B、[4:0] 当 R (R/B 交换) */
static void bgr565_le_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for (x=0;x<w;x++) { uint16_t p=src[x*2]|(src[x*2+1]<<8);
  uint8_t b5=(p>>11)&0x1f,g6=(p>>5)&0x3f,r5=p&0x1f;
  *rgb++=(r5<<3)|(r5>>2); *rgb++=(g6<<2)|(g6>>4); *rgb++=(b5<<3)|(b5>>2); }}

/* BGR565 大端: R/B 交换 + 高字节在前 */
static void bgr565_be_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for (x=0;x<w;x++) { uint16_t p=(src[x*2]<<8)|src[x*2+1];
  uint8_t b5=(p>>11)&0x1f,g6=(p>>5)&0x3f,r5=p&0x1f;
  *rgb++=(r5<<3)|(r5>>2); *rgb++=(g6<<2)|(g6>>4); *rgb++=(b5<<3)|(b5>>2); }}

/* ================================================================
 * YUV → RGB888 单像素 (BT.601 有限范围, 定点整数运算)
 *
 *   C = Y - 16    (去掉 16 的黑电平, 负值按 0 处理)
 *   D = U - 128   (色度归零成有符号)
 *   E = V - 128
 *   R = (298*C + 409*E + 128) >> 8
 *   G = (298*C - 100*D - 208*E + 128) >> 8
 *   B = (298*C + 516*D + 128) >> 8
 *
 * 系数是标准值左移 8 位后的整数近似 (如 1.402*256 ≈ 359, 1.772*256 ≈ 454),
 * +128 是四舍五入补偿, >>8 移回来。结果 clamp 到 [0,255]。
 * ================================================================ */
static void yuv_to_rgb(int y, int u, int v, uint8_t *rgb)
{
	int c=y-16, d=u-128, e=v-128;
	int r=(298*c+409*e+128)>>8;
	int g=(298*c-100*d-208*e+128)>>8;
	int b=(298*c+516*d+128)>>8;
	if (r < 0) r = 0;
	if (r > 255) r = 255;
	if (g < 0) g = 0;
	if (g > 255) g = 255;
	if (b < 0) b = 0;
	if (b > 255) b = 255;
	rgb[0] = (uint8_t)r;
	rgb[1] = (uint8_t)g;
	rgb[2] = (uint8_t)b;
}

/*
 * 下面 4 个函数对应 YUV 4:2:2 的四种字节排布。共同点:
 *   - 每 4 字节 = 2 个像素, 一组内 Y 各一个、U/V 共用 (这就是 4:2:2)
 *   - 组内偏移 o=(x/2)*4, 组内第 1/2 像素靠 (x&1) 选 Y
 * 区别只在 U/V/Y 这 4 个字节的先后顺序。
 *
 * YUYV: [Y0 U Y1 V]   UYVY: [U Y0 V Y1]
 * YVYU: [Y0 V Y1 U]   VYUY: [V Y0 U Y1]
 */

/* YUYV: Y 在 0/2, U 在 1, V 在 3 */
static void yuyv_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for(x=0;x<w;x++) { uint32_t o=(x/2)*4; int y=src[o+((x&1)?2:0)],u=src[o+1],v=src[o+3]; yuv_to_rgb(y,u,v,rgb); rgb+=3; }}

/* UYVY: U 在前, Y 在 1/3, V 在 2 */
static void uyvy_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for(x=0;x<w;x++) { uint32_t o=(x/2)*4; int u=src[o+0],y=src[o+((x&1)?3:1)],v=src[o+2]; yuv_to_rgb(y,u,v,rgb); rgb+=3; }}

/* YVYU: Y 在 0/2, V 在 1, U 在 3 (U/V 与 YUYV 对调) */
static void yvyu_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for(x=0;x<w;x++) { uint32_t o=(x/2)*4; int y=src[o+((x&1)?2:0)],v=src[o+1],u=src[o+3]; yuv_to_rgb(y,u,v,rgb); rgb+=3; }}

/* VYUY: V 在前, Y 在 1/3, U 在 2 */
static void vyuy_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for(x=0;x<w;x++) { uint32_t o=(x/2)*4; int v=src[o+0],y=src[o+((x&1)?3:1)],u=src[o+2]; yuv_to_rgb(y,u,v,rgb); rgb+=3; }}

/*
 * 按 pixfmt 分派到对应的逐行解码器。
 * 未知格式填灰色 (0x80) 而不是留黑, 这样"格式选错"在屏幕上一眼可见,
 * 不会和"没收到帧"混淆。
 */
static void convert_line_to_rgb888(const uint8_t *src, uint8_t *rgb,
                                   uint32_t width, uint32_t pixfmt)
{
	switch (pixfmt) {
	case V4L2_PIX_FMT_RGB565:  rgb565_le_to_rgb888_line(src,rgb,width); break;
	case V4L2_PIX_FMT_RGB565X: rgb565_be_to_rgb888_line(src,rgb,width); break;
	case PIX_FMT_BGR565:       bgr565_le_to_rgb888_line(src,rgb,width); break;
	case PIX_FMT_BGR565X:      bgr565_be_to_rgb888_line(src,rgb,width); break;
	case V4L2_PIX_FMT_YUYV:    yuyv_to_rgb888_line(src,rgb,width); break;
	case V4L2_PIX_FMT_UYVY:    uyvy_to_rgb888_line(src,rgb,width); break;
	case PIX_FMT_YVYU:         yvyu_to_rgb888_line(src,rgb,width); break;
	case PIX_FMT_VYUY:         vyuy_to_rgb888_line(src,rgb,width); break;
	default: memset(rgb,0x80,width*3); break;
	}
}

/*
 * 存成 PPM (P6 二进制格式) —— 与 SDL 显示共用 convert_line_to_rgb888,
 * 所以 PPM 里的颜色 == 屏幕上看到的颜色。
 * PPM 几乎所有图片查看器都能直接打开, 排查颜色问题比看 raw 方便得多。
 *
 * 注意: 这里按 width*2 当行步长, 即假定帧数据是紧凑排列的。
 *       PC 端收到的 raw 是板端按 sizeimage 连续发的, 对于本项目
 *       用的 640x480 YUYV/RGB565 这个前提成立; 若驱动返回的
 *       bytesperline != width*2 (行尾带 padding), 这里会错行。
 */
static int save_frame_as_ppm(const char *filename, const uint8_t *data,
                             uint32_t size, uint32_t width, uint32_t height,
                             uint32_t pixfmt)
{
	FILE *fp; uint32_t y; uint8_t *rgb_line; (void)size;
	fp=fopen(filename,"wb"); if(!fp) return -1;
	fprintf(fp,"P6\n%u %u\n255\n",width,height);   /* P6 = 二进制 RGB, 最大分量 255 */
	rgb_line=malloc(width*3); if(!rgb_line){fclose(fp);return -1;}
	for(y=0;y<height;y++){convert_line_to_rgb888(data+y*width*2,rgb_line,width,pixfmt); fwrite(rgb_line,width*3,1,fp);}
	free(rgb_line); fclose(fp); return 0;
}

/*
 * 原样落盘 —— 不做任何转换, 逐字节保存 hdr.size 个字节。
 * 这是最关键的排查素材: 解码参数事后可以随便换, 但原始数据一旦
 * 被转换过就回不去了, 所以永远先存 raw。
 * 扩展名由 pixfmt_to_ext() 决定 (.rgb565/.yuv/.mjpg/.raw), 只影响
 * 文件名, 方便一眼看出这帧大概是什么。
 */
static int save_raw_frame(const char *filename, const uint8_t *data, uint32_t size)
{ FILE *fp=fopen(filename,"wb"); if(!fp)return -1; fwrite(data,1,size,fp); fclose(fp); return 0; }

/* ================================================================
 * SDL2 显示
 *
 * 用 SDL 纹理做显示, 一帧的解码路径是:
 *   raw 帧数据 → convert_line_to_rgb888 → RGB24 行缓冲
 *              → SDL_UpdateTexture (逐行) → RenderCopy → Present
 *
 * 纹理格式固定 SDL_PIXELFORMAT_RGB24, 与 convert_line_to_rgb888 的
 * 输出对齐, 中间不再做任何格式转换。
 *
 * 只有定义了 ENABLE_SDL_DISPLAY 才会编进来 (Makefile 里 PC 端有,
 * 板端 ARM 端没有 —— 板子上跑的是 /dev/fb0, 不需要 SDL)。
 * ================================================================ */
#ifdef ENABLE_SDL_DISPLAY

struct sdl_display { SDL_Window *window; SDL_Renderer *renderer; SDL_Texture *texture; int width,height; };

/*
 * 显示格式的运行时状态 (都是全局的, 因为要在 SDL 事件回调路径上改)。
 * 任意时刻 disp_pixfmt = override ?: (yuv_order 且帧头是 YUV422) ?: hdr
 */
static uint32_t    g_disp_override     = 0;              /* 0 = auto-detect from header (默认) */
static const char *g_disp_override_tag = "auto(hdr)";    /* 当前 override 来自哪个开关, 只用于打印 */
static int         g_disp_first_frame  = 1;              /* 首帧打印一次实际使用的解码器 */
static uint32_t    g_yuv_order         = 0;              /* --yuv-order override */

/* 窗口标题实时显示 hdr/disp 两个格式, 切格式时不用回头看终端 */
static void sdl_update_title(struct sdl_display *d, uint32_t hdr_pixfmt, uint32_t disp_pixfmt)
{ char t[256]; snprintf(t,sizeof(t),"OV5640 | hdr=%s | disp=%s",pixfmt_to_name(hdr_pixfmt),pixfmt_to_name(disp_pixfmt)); SDL_SetWindowTitle(d->window,t); }

/*
 * 初始化 SDL 显示。
 * 失败时逐层回滚 (texture → renderer → window → SDL_Quit), 不留半初始化状态。
 * 纹理尺寸按首帧的 width/height 定, 后续帧尺寸变化不会重建纹理。
 */
static int sdl_display_init(struct sdl_display *d, int w, int h)
{
	memset(d,0,sizeof(*d));
	if(SDL_Init(SDL_INIT_VIDEO)<0){fprintf(stderr,"[SDL] Init failed: %s\n",SDL_GetError());return -1;}
	d->window=SDL_CreateWindow("OV5640 Live View",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,w,h,SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
	if(!d->window){SDL_Quit();return -1;}
	d->renderer=SDL_CreateRenderer(d->window,-1,SDL_RENDERER_ACCELERATED);
	if(!d->renderer){SDL_DestroyWindow(d->window);SDL_Quit();return -1;}
	d->texture=SDL_CreateTexture(d->renderer,SDL_PIXELFORMAT_RGB24,SDL_TEXTUREACCESS_STREAMING,w,h);
	if(!d->texture){SDL_DestroyRenderer(d->renderer);SDL_DestroyWindow(d->window);SDL_Quit();return -1;}
	d->width=w; d->height=h;
	SDL_SetRenderDrawColor(d->renderer,0,0,0,255); SDL_RenderClear(d->renderer); SDL_RenderPresent(d->renderer);
	printf("[SDL] Display %dx%d, auto-detect from header\n",w,h);
	printf("[SDL] Keys: 1=RGB565 2=RGB565X 3=BGR565 4=BGR565X 5=YUYV 6=UYVY 7=YVYU 8=VYUY 0=auto ESC=Quit\n");
	return 0;
}

/*
 * 处理 SDL 事件队列。
 * 数字键 1~8 = 手动指定解码格式, 0 = 回到按帧头自动判断, ESC/Q = 退出。
 * 返回 -1 表示要求主循环退出 (关窗口或按键)。
 *
 * 这几个键是查"颜色不对"的核心手段: 保持数据流不断, 逐个试解码器,
 * 哪个颜色正常就说明板端实际输出的是那个格式。
 */
static int sdl_handle_events(void)
{
	SDL_Event ev;
	while(SDL_PollEvent(&ev)){
		if(ev.type==SDL_QUIT) return -1;
		if(ev.type==SDL_KEYDOWN){
			switch(ev.key.keysym.sym){
			case SDLK_ESCAPE: case SDLK_q: return -1;
			case SDLK_1: g_disp_override=V4L2_PIX_FMT_RGB565; g_disp_override_tag="key-1"; printf("[SDL] >>> RGB565_LE\n"); break;
			case SDLK_2: g_disp_override=V4L2_PIX_FMT_RGB565X; g_disp_override_tag="key-2"; printf("[SDL] >>> RGB565X_BE\n"); break;
			case SDLK_3: g_disp_override=PIX_FMT_BGR565; g_disp_override_tag="key-3"; printf("[SDL] >>> BGR565_LE\n"); break;
			case SDLK_4: g_disp_override=PIX_FMT_BGR565X; g_disp_override_tag="key-4"; printf("[SDL] >>> BGR565X_BE\n"); break;
			case SDLK_5: g_disp_override=V4L2_PIX_FMT_YUYV; g_disp_override_tag="key-5"; printf("[SDL] >>> YUYV\n"); break;
			case SDLK_6: g_disp_override=V4L2_PIX_FMT_UYVY; g_disp_override_tag="key-6"; printf("[SDL] >>> UYVY\n"); break;
			case SDLK_7: g_disp_override=PIX_FMT_YVYU; g_disp_override_tag="key-7"; printf("[SDL] >>> YVYU\n"); break;
			case SDLK_8: g_disp_override=PIX_FMT_VYUY; g_disp_override_tag="key-8"; printf("[SDL] >>> VYUY\n"); break;
			case SDLK_0: g_disp_override=0; g_disp_override_tag="key-0"; printf("[SDL] >>> AUTO (header)\n"); break;
			default: break;
			}
		}
	}
	return 0;
}

/*
 * 显示一帧。hdr_pixfmt 只作参考 (标题显示 / auto 时使用),
 * 真正用哪个解码器由上面的优先级决定。
 *
 * 注意这里算了两遍 disp_pixfmt: 事件处理之前算一次是为了让标题栏
 * 反映"上一帧"的状态, 事件处理之后再算一次才是本帧真正要用的 ——
 * 因为 sdl_handle_events() 里可能刚按了切换键。
 * 返回 -1 表示要退出。
 */
static int sdl_display_show_frame(struct sdl_display *d, const uint8_t *data,
                                  uint32_t size, uint32_t hdr_pixfmt)
{
	uint32_t y, disp_pixfmt; uint8_t *rgb_line; (void)size;

	/* 确定显示 pixfmt */
	if (g_disp_override != 0)
		disp_pixfmt = g_disp_override;
	else if (g_yuv_order && pixfmt_is_yuv422(hdr_pixfmt))
		disp_pixfmt = g_yuv_order;   /* --yuv-order 覆盖 */
	else
		disp_pixfmt = hdr_pixfmt;    /* auto-detect */

	if(g_disp_first_frame){printf("[SDL] hdr=%s -> disp=%s (%s)\n",pixfmt_to_string(hdr_pixfmt),pixfmt_to_string(disp_pixfmt),g_disp_override_tag); g_disp_first_frame=0;}

	sdl_update_title(d, hdr_pixfmt, disp_pixfmt);
	if(sdl_handle_events()<0) return -1;

	/* 事件后重取: 上面那次按键可能已经改了 g_disp_override */
	if(g_disp_override!=0) disp_pixfmt=g_disp_override;
	else if(g_yuv_order && pixfmt_is_yuv422(hdr_pixfmt)) disp_pixfmt=g_yuv_order;
	else disp_pixfmt=hdr_pixfmt;

	/* 解码不了的格式 (如 MJPEG) 就刷黑屏, 不写脏纹理 */
	if(!pixfmt_is_displayable(disp_pixfmt)){SDL_SetRenderDrawColor(d->renderer,0,0,0,255);SDL_RenderClear(d->renderer);SDL_RenderPresent(d->renderer);return 0;}

	/* 逐行解码 + 更新纹理 (按行更新, 省一块整帧的临时缓冲) */
	rgb_line=malloc(d->width*3); if(!rgb_line) return 0;
	for(y=0;y<(uint32_t)d->height;y++){convert_line_to_rgb888(data+y*d->width*2,rgb_line,d->width,disp_pixfmt); SDL_UpdateTexture(d->texture,&(SDL_Rect){0,(int)y,d->width,1},rgb_line,d->width*3);}
	free(rgb_line);
	SDL_RenderClear(d->renderer); SDL_RenderCopy(d->renderer,d->texture,NULL,NULL); SDL_RenderPresent(d->renderer);
	return 0;
}

/* 逆序释放, 与 sdl_display_init 的创建顺序相反 */
static void sdl_display_deinit(struct sdl_display *d)
{ if(d->texture) SDL_DestroyTexture(d->texture); if(d->renderer) SDL_DestroyRenderer(d->renderer); if(d->window) SDL_DestroyWindow(d->window); SDL_Quit(); }
#endif

/* ================================================================
 * 网络
 * ================================================================ */

/* 命令行格式名 → FOURCC, 用于 --display-format / --yuv-order。失败返回 -1 */
static int parse_fmt_name(const char *n)
{
	if(strcasecmp(n,"rgb565")==0) return V4L2_PIX_FMT_RGB565;
	if(strcasecmp(n,"rgb565x")==0) return V4L2_PIX_FMT_RGB565X;
	if(strcasecmp(n,"bgr565")==0) return PIX_FMT_BGR565;
	if(strcasecmp(n,"bgr565x")==0) return PIX_FMT_BGR565X;
	if(strcasecmp(n,"yuyv")==0) return V4L2_PIX_FMT_YUYV;
	if(strcasecmp(n,"uyvy")==0) return V4L2_PIX_FMT_UYVY;
	if(strcasecmp(n,"yvyu")==0) return PIX_FMT_YVYU;
	if(strcasecmp(n,"vyuy")==0) return PIX_FMT_VYUY;
	return -1;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -p <port>             Listen port (default 5000)\n"
		"  -o <dir>              Output dir (default ./frames)\n"
		"  -n <frames>           Exit after N complete frames (0=unlimited)\n"
		"  --max-frames <n>      Same as -n\n"
		"  --stats-dir <dir>     Enable stats: network_stats.csv /\n"
		"                        process_usage.csv / summary.csv in <dir>\n"
		"  --stats-interval <s>  Periodic resource sample interval (default 1)\n"
		"  --no-save             Do not write frame files (removes disk I/O)\n"
		"  --quiet               Suppress per-frame stdout\n"
		"  --accept-timeout <s>  Give up if no client connects within <s> sec\n"
		"                        (0=block forever, default). Essential for\n"
		"                        automated tests so nothing hangs on accept().\n"
		"  --io-timeout <s>      SO_RCVTIMEO/SO_SNDTIMEO on the data socket\n"
		"                        so a stalled peer cannot block forever\n"
#ifdef ENABLE_SDL_DISPLAY
		"  -d                    Enable SDL2 display\n"
		"  --display-format <f>  Force decode: rgb565/rgb565x/bgr565/bgr565x/yuyv/uyvy/yvyu/vyuy\n"
		"  --swap-rb             Shortcut for --display-format bgr565\n"
		"  --yuv-order <o>       YUV byte order: yuyv/uyvy/yvyu/vyuy\n"
		"  --force-<fmt>         Legacy alias for --display-format\n"
#endif
		"\nExamples:\n"
		"  %s -p 5000 -o ./frames -d                       # auto-detect\n"
		"  %s -p 5000 --no-save -n 300 --quiet --stats-dir ./bench/rx\n"
#ifdef ENABLE_SDL_DISPLAY
		"  %s -p 5000 -d --display-format yuyv             # force YUYV\n"
		"  %s -p 5000 -d --yuv-order uyvy                  # YUYV hdr, UYVY decode\n"
#endif
		, p, p, p
#ifdef ENABLE_SDL_DISPLAY
		, p, p
#endif
		);
}

/* 长选项的 val 从 256 起, 避开 getopt 已占用的 ASCII 短选项返回值。
 * 统计相关选项在所有构建下都存在; SDL 相关选项只在 ENABLE_SDL_DISPLAY
 * 下存在 (板端不编译 SDL, PC 端才需要)。 */
enum {
	OPT_DISPLAY_FMT = 256, OPT_SWAP_RB, OPT_YUV_ORDER,
	OPT_F_RGB565, OPT_F_RGB565X, OPT_F_BGR565, OPT_F_BGR565X,
	OPT_F_YUYV, OPT_F_UYVY,
	OPT_STATS_DIR, OPT_STATS_INTERVAL, OPT_NO_SAVE, OPT_QUIET,
	OPT_MAX_FRAMES, OPT_ACCEPT_TIMEOUT, OPT_IO_TIMEOUT
};

/* --force-xxx 是早期版本的写法, 保留下来兼容旧脚本, 效果等同 --display-format */
static const struct option lopts[] = {
	{"stats-dir",      1, NULL, OPT_STATS_DIR},
	{"stats-interval", 1, NULL, OPT_STATS_INTERVAL},
	{"no-save",        0, NULL, OPT_NO_SAVE},
	{"quiet",          0, NULL, OPT_QUIET},
	{"max-frames",     1, NULL, OPT_MAX_FRAMES},
	{"accept-timeout", 1, NULL, OPT_ACCEPT_TIMEOUT},
	{"io-timeout",     1, NULL, OPT_IO_TIMEOUT},
#ifdef ENABLE_SDL_DISPLAY
	{"display-format",1,NULL,OPT_DISPLAY_FMT},{"swap-rb",0,NULL,OPT_SWAP_RB},
	{"yuv-order",1,NULL,OPT_YUV_ORDER},
	{"force-rgb565",0,NULL,OPT_F_RGB565},{"force-rgb565x",0,NULL,OPT_F_RGB565X},
	{"force-bgr565",0,NULL,OPT_F_BGR565},{"force-bgr565x",0,NULL,OPT_F_BGR565X},
	{"force-yuyv",0,NULL,OPT_F_YUYV},{"force-uyvy",0,NULL,OPT_F_UYVY},
#endif
	{0,0,0,0}
};

#ifdef ENABLE_SDL_DISPLAY
#define PC_OPTSTR "p:o:n:d"
#else
#define PC_OPTSTR "p:o:n:"
#endif

/* ================================================================
 * PC 端接收统计 (--stats-dir 开启后才工作)
 * ================================================================ */

/* 单帧最大允许字节数。协议里 size 是 32 位, 但一次收帧不可能真的
 * 有几 GB; 给一个上限可以避免用畸形帧头把接收端 realloc 到爆。 */
#define RX_MAX_FRAME_BYTES  (64U * 1024U * 1024U)

struct rx_bench {
	int      enabled;
	int      quiet;
	int      no_save;         /* --no-save: 不落盘, 消除磁盘 I/O 干扰 */
	double   interval_s;
	char     dir[256];
	FILE    *fp_frames;
	FILE    *fp_proc;

	uint64_t t_start_ns;
	uint64_t next_sample_ns;

	/* 完整性计数 */
	uint64_t frames_ok;       /* 完成帧头+数据重组的帧数 */
	uint64_t frames_written;
	uint64_t bad_magic;
	uint64_t bad_version;
	uint64_t bad_hdr_size;
	uint64_t bad_size;        /* size 越界 */
	uint64_t bad_geometry;    /* 宽/高不合理 */
	uint64_t session_end;     /* 对端断开/读错误 */
	uint64_t frame_id_gap_events;
	uint64_t frame_id_gap_total;
	int      prev_id_valid;
	uint32_t prev_id;
	uint32_t first_id;
	uint32_t last_id;
	uint64_t payload_bytes;   /* 有效图像数据字节 */
	uint64_t header_bytes;
	uint64_t reassembled_bytes; /* 包含被拒绝帧在内, 实际从 socket 读到的数据字节 */
	struct io_stats hdr_io;
	struct io_stats data_io;

	/* 直方图 (微秒) */
	struct metrics_hist h_wait;       /* R2 - R1_pre (等待首字节+重组) */
	struct metrics_hist h_reasm;      /* R2 - R1     (帧头完成→数据完成) */
	struct metrics_hist h_sdl;        /* R3 - R2     (提交显示) */
	struct metrics_hist h_interval;   /* 相邻帧 R2 间隔 */

	uint64_t prev_r2_ns;
	int      prev_r2_valid;

	struct metrics_proc_tracker ptrack;
	double   cpu_sum;
	uint64_t cpu_samples;
	uint64_t rss_max_kb;
};

static struct rx_bench g_rx;

static int rx_bench_start(struct rx_bench *b)
{
	char path[320];

	if (!b->enabled)
		return 0;
	if (b->dir[0] == '\0')
		snprintf(b->dir, sizeof(b->dir), ".");

	if (mkdir(b->dir, 0755) < 0 && errno != EEXIST) {
		perror("mkdir stats dir");
		return -1;
	}

	snprintf(path, sizeof(path), "%s/network_stats.csv", b->dir);
	b->fp_frames = metrics_csv_open(path,
		"frame_index,frame_id,gap,width,height,pixfmt,size,"
		"hdr_recv_us,payload_reasm_us,reasm_us,sdl_us,r1pre_ns,r1_ns,"
		"r2_ns,r3_ns,payload_bytes,saved");
	if (b->fp_frames == NULL)
		return -1;

	snprintf(path, sizeof(path), "%s/process_usage.csv", b->dir);
	b->fp_proc = metrics_csv_open(path,
		"elapsed_s,cpu_percent,rss_kb,vsz_kb,threads,"
		"frames_ok,fps_interval,payload_bytes");
	if (b->fp_proc == NULL)
		return -1;

	metrics_proc_tracker_init(&b->ptrack);
	b->t_start_ns = metrics_now_ns();
	b->next_sample_ns = b->t_start_ns + (uint64_t)(b->interval_s * 1e9);

	/* 立刻建立 CPU 基准: 否则短时收帧 (< 一个采样间隔) 完全没有
	 * CPU/RSS 数据; rx_bench_finish 里再补采一次就完整了 */
	{
		struct metrics_proc_usage pu0;
		if (metrics_proc_tracker_sample(&b->ptrack, getpid(),
		                                &pu0) == 0 && pu0.valid)
			b->rss_max_kb = pu0.rss_kb;
	}
	return 0;
}

static void rx_bench_periodic(struct rx_bench *b, uint64_t now_ns)
{
	struct metrics_proc_usage pu;
	double elapsed, fps_int;

	if (!b->enabled || b->fp_proc == NULL)
		return;
	if (now_ns < b->next_sample_ns)
		return;

	elapsed = (double)(now_ns - b->t_start_ns) / 1e9;
	fps_int = 0.0;   /* 逐秒窗口的 fps 由分析脚本按行差算, 这里留 0 */

	if (metrics_proc_tracker_sample(&b->ptrack, getpid(), &pu) == 0 && pu.valid) {
		b->cpu_sum += pu.cpu_percent;
		b->cpu_samples++;
		if (pu.rss_kb > b->rss_max_kb)
			b->rss_max_kb = pu.rss_kb;
		fprintf(b->fp_proc, "%.3f,%.1f,%llu,%llu,%llu,%llu,%.2f,%llu\n",
		        elapsed, pu.cpu_percent,
		        (unsigned long long)pu.rss_kb,
		        (unsigned long long)pu.vsz_kb,
		        (unsigned long long)pu.threads,
		        (unsigned long long)b->frames_ok, fps_int,
		        (unsigned long long)b->payload_bytes);
	}
	b->next_sample_ns += (uint64_t)(b->interval_s * 1e9);
}

static void rx_bench_finish(struct rx_bench *b, uint64_t t_end_ns)
{
	struct metrics_summary s;
	double elapsed, mbps;
	FILE *fp;
	char path[320];

	if (!b->enabled)
		return;

	elapsed = (double)(t_end_ns - b->t_start_ns) / 1e9;
	mbps = (elapsed > 0.0) ? (double)b->payload_bytes * 8.0 / elapsed / 1e6
	                       : 0.0;

	/* 结束补采 (与 rx_bench_start 的基准配对), 保证任何时长都有 CPU/RSS */
	{
		struct metrics_proc_usage pu;
		if (metrics_proc_tracker_sample(&b->ptrack, getpid(), &pu) == 0 &&
		    pu.valid) {
			if (pu.interval_sec > 0.0) {
				b->cpu_sum += pu.cpu_percent;
				b->cpu_samples++;
			}
			if (pu.rss_kb > b->rss_max_kb)
				b->rss_max_kb = pu.rss_kb;
		}
	}

	printf("\n================ PC RX BENCHMARK SUMMARY ================\n");
	printf("elapsed_s        : %.3f\n", elapsed);
	printf("frames_ok        : %llu (protocol-level complete frames)\n",
	       (unsigned long long)b->frames_ok);
	printf("frames_written   : %llu\n",
	       (unsigned long long)b->frames_written);
	printf("frame_id         : first=%u last=%u\n", b->first_id, b->last_id);
	printf("frame_id_gap_ev  : %llu\n",
	       (unsigned long long)b->frame_id_gap_events);
	printf("frame_id_gap_fr  : %llu\n",
	       (unsigned long long)b->frame_id_gap_total);
	printf("payload_bytes    : %llu\n",
	       (unsigned long long)b->payload_bytes);
	printf("header_bytes     : %llu\n",
	       (unsigned long long)b->header_bytes);
	printf("payload_mbps     : %.3f (payload only, excl. header)\n", mbps);
	printf("--- error counters (malformed / disconnect) ---\n");
	printf("bad_magic        : %llu\n", (unsigned long long)b->bad_magic);
	printf("bad_version      : %llu\n", (unsigned long long)b->bad_version);
	printf("bad_hdr_size     : %llu\n", (unsigned long long)b->bad_hdr_size);
	printf("bad_size         : %llu\n", (unsigned long long)b->bad_size);
	printf("bad_geometry     : %llu\n", (unsigned long long)b->bad_geometry);
	printf("session_end      : %llu\n", (unsigned long long)b->session_end);
	printf("--- recv short-read evidence ---\n");
	printf("hdr_recv_calls   : %llu (partial %llu)\n",
	       (unsigned long long)b->hdr_io.calls,
	       (unsigned long long)b->hdr_io.partial);
	printf("data_recv_calls  : %llu (partial %llu)\n",
	       (unsigned long long)b->data_io.calls,
	       (unsigned long long)b->data_io.partial);
	printf("recv_bytes_total : %llu\n",
	       (unsigned long long)b->reassembled_bytes);
	printf("recv_timeouts    : %llu (SO_RCVTIMEO expired; 0 if unset)\n",
	       (unsigned long long)(b->hdr_io.timeouts + b->data_io.timeouts));
	if (b->cpu_samples > 0)
		printf("cpu_percent_avg  : %.1f (%% of one core, %llu samples)\n",
		       b->cpu_sum / (double)b->cpu_samples,
		       (unsigned long long)b->cpu_samples);
	printf("rss_max_kb       : %llu\n",
	       (unsigned long long)b->rss_max_kb);
	if (b->frames_ok > 0) {
		if (metrics_summary(&b->h_interval, &s) == 0)
			metrics_print_summary("frame_interval", &s);
		if (metrics_summary(&b->h_reasm, &s) == 0)
			metrics_print_summary("reasm(R2-R1)", &s);
		if (metrics_summary(&b->h_wait, &s) == 0)
			metrics_print_summary("wait+reasm", &s);
		if (b->h_sdl.n > 0 && metrics_summary(&b->h_sdl, &s) == 0)
			metrics_print_summary("sdl(R3-R2)", &s);
	} else {
		printf("WARN             : no complete frame, percentiles NOT available\n");
	}
	printf("NOTE             : R1..R3 are PC-local monotonic clocks; they are\n");
	printf("                   NOT comparable with board clocks. No end-to-end\n");
	printf("                   latency is claimed here.\n");
	printf("========================================================\n\n");

	snprintf(path, sizeof(path), "%s/summary.csv", b->dir);
	fp = metrics_csv_open(path,
		"elapsed_s,frames_ok,frames_written,payload_bytes,header_bytes,"
		"payload_mbps,bad_magic,bad_version,bad_hdr_size,bad_size,"
		"bad_geometry,session_end,frame_id_gap_events,frame_id_gap_frames,"
		"hdr_recv_calls,hdr_recv_partial,data_recv_calls,data_recv_partial,"
		"cpu_percent_avg,rss_max_kb,"
		"interval_mean_us,interval_p95_us,interval_p99_us,interval_max_us,"
		"reasm_mean_us,reasm_p99_us,wait_p99_us,sdl_mean_us,"
		"recv_timeouts,avg_data_recv_calls_per_frame");
	if (fp != NULL) {
		struct metrics_summary si, sr, sw, sd;
		int oki = (metrics_summary(&b->h_interval, &si) == 0);
		int okr = (metrics_summary(&b->h_reasm, &sr) == 0);
		int okw = (metrics_summary(&b->h_wait, &sw) == 0);
		int okd = (metrics_summary(&b->h_sdl, &sd) == 0);
		fprintf(fp,
			"%.3f,%llu,%llu,%llu,%llu,%.3f,%llu,%llu,%llu,%llu,%llu,"
			"%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.1f,%llu,"
			"%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
			"%llu,%.1f\n",
			elapsed,
			(unsigned long long)b->frames_ok,
			(unsigned long long)b->frames_written,
			(unsigned long long)b->payload_bytes,
			(unsigned long long)b->header_bytes,
			mbps,
			(unsigned long long)b->bad_magic,
			(unsigned long long)b->bad_version,
			(unsigned long long)b->bad_hdr_size,
			(unsigned long long)b->bad_size,
			(unsigned long long)b->bad_geometry,
			(unsigned long long)b->session_end,
			(unsigned long long)b->frame_id_gap_events,
			(unsigned long long)b->frame_id_gap_total,
			(unsigned long long)b->hdr_io.calls,
			(unsigned long long)b->hdr_io.partial,
			(unsigned long long)b->data_io.calls,
			(unsigned long long)b->data_io.partial,
			b->cpu_samples > 0
			  ? b->cpu_sum / (double)b->cpu_samples : -1.0,
			(unsigned long long)b->rss_max_kb,
			oki ? si.mean_us : -1.0, oki ? si.p95_us : -1.0,
			oki ? si.p99_us : -1.0, oki ? si.max_us : -1.0,
			okr ? sr.mean_us : -1.0, okr ? sr.p99_us : -1.0,
			okw ? sw.p99_us : -1.0,
			okd ? sd.mean_us : -1.0,
			(unsigned long long)(b->hdr_io.timeouts + b->data_io.timeouts),
			b->frames_ok > 0
			  ? (double)b->data_io.calls / (double)b->frames_ok : -1.0);
		fclose(fp);
		printf("[RX-BENCH] summary written: %s\n", path);
	}

	if (b->fp_frames) { fclose(b->fp_frames); b->fp_frames = NULL; }
	if (b->fp_proc)   { fclose(b->fp_proc);   b->fp_proc = NULL; }
	metrics_hist_free(&b->h_wait);
	metrics_hist_free(&b->h_reasm);
	metrics_hist_free(&b->h_sdl);
	metrics_hist_free(&b->h_interval);
}

/*
 * 帧头合法性校验 (增强项, 不改变正常帧的处理路径)。
 *
 * 原实现只查 magic。这里补上 version / header_size / size / 宽高 的检查,
 * 目的是让"畸形帧"能被分类计数, 而不是靠 realloc 一个巨大的 size 把
 * 进程拖死。任何一项不通过都返回 -1, 由调用者决定是否终止会话。
 *
 * 返回: 0 = 通过; -1 = 不合法 (rc 输出具体原因, 供统计)。
 */
static int rx_validate_header(const struct frame_header *hdr, const char **rc)
{
	uint64_t expected;

	if (hdr->magic != FRAME_MAGIC) { *rc = "bad_magic"; return -1; }
	if (hdr->version != FRAME_VERSION) { *rc = "bad_version"; return -1; }
	if (hdr->header_size != sizeof(*hdr)) { *rc = "bad_hdr_size"; return -1; }
	if (hdr->size == 0 || hdr->size > RX_MAX_FRAME_BYTES) {
		*rc = "bad_size"; return -1;
	}
	if (hdr->width == 0 || hdr->height == 0 ||
	    hdr->width > 8192 || hdr->height > 8192) {
		*rc = "bad_geometry"; return -1;
	}
	/* 每像素至少 1 字节 (MJPEG 例外, 压缩后可以更小),
	 * 所以只对未压缩格式做"数据量至少覆盖一行"的弱检查 */
	if (hdr->pixfmt != V4L2_PIX_FMT_MJPEG) {
		expected = (uint64_t)hdr->width * 2ULL;
		if (expected > 0 && hdr->size < expected) {
			*rc = "bad_size"; return -1;
		}
	}
	*rc = "ok";
	return 0;
}

/*
 * 收 40 字节帧头并转成本机字节序 (带分片统计)。
 * 返回 -1 表示对端断开或出错。
 */
/*
 * 收 40 字节帧头并转成本机字节序 (带分片统计)。
 * 先整体 recv_all 到临时结构 n, 逐个字段 ntohl —— 不直接往 hdr 上读,
 * 是为了避免"读了一半"时 hdr 处于半新半旧的状态。
 * timestamp_us 是 64 位, 拆成高低两个 32 位分别 ntohl 再拼回去。
 * 返回 -1 表示对端断开或出错。
 */
static int recv_frame_header_stat(int fd, struct frame_header *hdr,
                                  struct io_stats *st)
{
	struct frame_header n; int ret;
	ret = recv_all_stat(fd, &n, sizeof(n), st); if(ret<0) return -1;
	/* 反序列化统一走 frame_proto.h 的 frame_header_ntoh (与发送端共用) */
	frame_header_ntoh(&n, hdr);
	return 0;
}

int main(int argc, char *argv[])
{
	int port=5000, enable_display=0, opt, oi, lfd=-1, cfd=-1;
	int max_frames=0, accept_timeout_s=0, io_timeout_s=0;
	const char *outdir="./frames";
	/* fbuf/fcap: 帧数据缓冲及其当前容量, 收帧时按需 realloc 复用 */
	uint8_t *fbuf=NULL; uint32_t fcap=0;

	/*
	 * stdout 行缓冲: 重定向到文件/管道时默认是块缓冲, 日志迟迟不落盘,
	 * 自动化脚本会"看起来卡死"。行缓冲让每行立即可见。
	 */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* 统计默认关闭 (g_rx 全零 = 原行为) */
	memset(&g_rx,0,sizeof(g_rx));
	g_rx.interval_s=1.0;

#ifdef ENABLE_SDL_DISPLAY
	struct sdl_display disp; int dinited=0;
	/* 显式复位全局显示状态 (防重复调用 main 时残留) */
	memset(&disp,0,sizeof(disp));
	g_disp_override=0; g_disp_override_tag="auto(hdr)"; g_disp_first_frame=1; g_yuv_order=0;
#endif

	while((opt=getopt_long(argc,argv,PC_OPTSTR,lopts,&oi))!=-1){
		switch(opt){
		case 'p': port=atoi(optarg); break;
		case 'o': outdir=optarg; break;
		case 'n': max_frames=atoi(optarg); break;
		case OPT_MAX_FRAMES: max_frames=atoi(optarg); break;
		case OPT_STATS_DIR:
			g_rx.enabled=1;
			snprintf(g_rx.dir,sizeof(g_rx.dir),"%s",optarg);
			break;
		case OPT_STATS_INTERVAL:
			g_rx.interval_s=atof(optarg);
			if(g_rx.interval_s<=0.0) g_rx.interval_s=1.0;
			break;
		case OPT_NO_SAVE: g_rx.no_save=1; break;
		case OPT_QUIET: g_rx.quiet=1; break;
		case OPT_ACCEPT_TIMEOUT:
			accept_timeout_s=atoi(optarg);
			if(accept_timeout_s<0) accept_timeout_s=0;
			break;
		case OPT_IO_TIMEOUT:
			io_timeout_s=atoi(optarg);
			if(io_timeout_s<0) io_timeout_s=0;
			break;
#ifdef ENABLE_SDL_DISPLAY
		case 'd': enable_display=1; break;
		case OPT_DISPLAY_FMT:{int f=parse_fmt_name(optarg); if(f<0){fprintf(stderr,"[ERROR] Unknown fmt: %s\n",optarg);return 1;} g_disp_override=(uint32_t)f; g_disp_override_tag="--display-format"; printf("[INFO] Display: %s\n",pixfmt_to_string(g_disp_override)); break;}
		case OPT_SWAP_RB: g_disp_override=PIX_FMT_BGR565; g_disp_override_tag="--swap-rb"; printf("[INFO] Swap R/B: BGR565_LE\n"); break;
		case OPT_YUV_ORDER:{int f=parse_fmt_name(optarg); if(f<0||!pixfmt_is_yuv422((uint32_t)f)){fprintf(stderr,"[ERROR] Bad yuv order: %s (use yuyv/uyvy/yvyu/vyuy)\n",optarg);return 1;} g_yuv_order=(uint32_t)f; printf("[INFO] YUV order: %s\n",pixfmt_to_string(g_yuv_order)); break;}
		case OPT_F_RGB565: g_disp_override=V4L2_PIX_FMT_RGB565; g_disp_override_tag="--force"; break;
		case OPT_F_RGB565X: g_disp_override=V4L2_PIX_FMT_RGB565X; g_disp_override_tag="--force"; break;
		case OPT_F_BGR565: g_disp_override=PIX_FMT_BGR565; g_disp_override_tag="--force"; break;
		case OPT_F_BGR565X: g_disp_override=PIX_FMT_BGR565X; g_disp_override_tag="--force"; break;
		case OPT_F_YUYV: g_disp_override=V4L2_PIX_FMT_YUYV; g_disp_override_tag="--force"; break;
		case OPT_F_UYVY: g_disp_override=V4L2_PIX_FMT_UYVY; g_disp_override_tag="--force"; break;
#endif
		default: usage(argv[0]); return 1;
		}
	}

	printf("================================================================\n  PC Frame Receiver\n================================================================\n");
	printf("  Port    : %d\n  Output  : %s\n",port,outdir);
	if(max_frames>0) printf("  MaxFrm  : %d\n",max_frames);
	if(g_rx.enabled)
		printf("  Stats   : %s (save=%s, quiet=%d)\n",
		       g_rx.dir, g_rx.no_save?"off":"on", g_rx.quiet);
#ifdef ENABLE_SDL_DISPLAY
	printf("  Display : %s\n",enable_display?"SDL2":"disabled");
	printf("  Decoder : %s\n",g_disp_override?pixfmt_to_string(g_disp_override):"auto (from header)");
	if(g_yuv_order) printf("  YUV Ord : %s\n",pixfmt_to_string(g_yuv_order));
#endif
	printf("================================================================\n\n");

	signal(SIGINT,signal_handler); signal(SIGTERM,signal_handler);

	/* 输出目录不存在就建; 已存在 (EEXIST) 不算错。
	 * --no-save 时不落盘, 也就不需要这个目录。 */
	if(!g_rx.no_save && mkdir(outdir,0755)<0&&errno!=EEXIST){perror("mkdir");return 1;}

	lfd=tcp_server_listen(port,1); if(lfd<0) return 1;
	printf("[INFO] Waiting for connection...\n");
	if(accept_timeout_s>0) printf("[INFO] accept timeout: %ds\n",accept_timeout_s);

	/*
	 * 带超时的 accept。
	 *
	 * 默认 accept() 是无限阻塞的: 没人连就一直挂着。对交互使用没问题,
	 * 但在自动化测试里"客户端没能连上"会让整个脚本永远等下去 (这正是
	 * 一次真实事故的原因)。--accept-timeout 让接收端超时后自己退出并
	 * 返回非 0, 脚本就能明确区分"没连上"和"收到了畸形帧"。
	 */
	if(accept_timeout_s>0){
		struct pollfd pf; int pr;
		pf.fd=lfd; pf.events=POLLIN; pf.revents=0;
		pr=poll(&pf,1,accept_timeout_s*1000);
		if(pr<=0){
			fprintf(stderr,"[ERROR] accept timeout after %ds, "
			        "no client connected (pr=%d)\n",accept_timeout_s,pr);
			close(lfd); return 1;
		}
	}

	/* 只服务一个客户端 (accept 一次就不管了), 单帧流场景够用;
	 * backlog=1 也说明这里没打算支持多客户端 */
	cfd=accept(lfd,NULL,NULL); if(cfd<0){if(errno!=EINTR)perror("accept");close(lfd);return 1;}
	printf("[INFO] Client connected (fd=%d)\n",cfd);

	/* 对端挂起时的兜底: 设了 --io-timeout 后 recv/send 到期返回 EAGAIN,
	 * 接收循环据此退出, 不会永久挂住 */
	if(io_timeout_s>0){
		tcp_set_timeouts(cfd,io_timeout_s,io_timeout_s);
		printf("[INFO] socket io timeout: %ds\n",io_timeout_s);
	}

	if(rx_bench_start(&g_rx)<0){
		fprintf(stderr,"[ERROR] stats init failed\n");
		close(cfd); close(lfd); return 1;
	}

	/*
	 * 收帧主循环: 一帧 = 帧头(40B) + 数据(hdr.size B)。
	 * 任何一步失败 (对端断开 / 校验不过 / 分配失败) 都直接 break 退出,
	 * 因为帧边界一旦错位, 后续收到的字节流就再也对不齐了,
	 * 继续读只会得到一堆垃圾。
	 *
	 * 时间轴 (PC 本地 CLOCK_MONOTONIC):
	 *   r1pre : 开始等该帧第一个字节 (帧头读之前)
	 *   r1    : 帧头 40 字节完整收到
	 *   r2    : hdr.size 字节图像数据完整重组
	 *   r3    : SDL 纹理提交 + Present 返回
	 * 只报 PC 内部差值 (r2-r1, r3-r2); 与板端时钟不可比。
	 */
	while(g_running && (max_frames==0 || g_rx.frames_ok < (uint64_t)max_frames)){
		struct frame_header hdr; int ret; char fn[512], pfn[512]; uint32_t epix;
		uint64_t r1pre, r1, r2, r3, hdr_us, reasm_us;
		int64_t id_gap = 0;
		const char *rc = "ok";
		int saved = 0;

		r1pre = metrics_now_ns();
		ret = recv_frame_header_stat(cfd,&hdr,&g_rx.hdr_io);
		if(ret<0){ g_rx.session_end++; break; }
		r1 = metrics_now_ns();
		hdr_us = (r1 - r1pre) / 1000ULL;

		/* 帧头校验 (magic/version/header_size/size/宽高), 分类计数 */
		if(rx_validate_header(&hdr,&rc) < 0){
			if(!strcmp(rc,"bad_magic"))         g_rx.bad_magic++;
			else if(!strcmp(rc,"bad_version"))  g_rx.bad_version++;
			else if(!strcmp(rc,"bad_hdr_size")) g_rx.bad_hdr_size++;
			else if(!strcmp(rc,"bad_geometry")) g_rx.bad_geometry++;
			else                                g_rx.bad_size++;
			fprintf(stderr,"[ERROR] header rejected: %s "
			        "(magic=0x%08X ver=%u hsz=%u %ux%u size=%u)\n",
			        rc, hdr.magic, hdr.version, hdr.header_size,
			        hdr.width, hdr.height, hdr.size);
			break;   /* 帧边界已不可信, 终止会话 */
		}

		/* 缓冲区按需增长, 只增不减 (帧大小通常固定, realloc 只发生一次) */
		if(hdr.size>fcap){uint8_t *p=realloc(fbuf,hdr.size); if(!p)break; fbuf=p; fcap=hdr.size;}
		ret = recv_all_stat(cfd,fbuf,hdr.size,&g_rx.data_io); if(ret<0){ g_rx.session_end++; break; }
		r2 = metrics_now_ns();
		reasm_us = (r2 - r1) / 1000ULL;

		/* frame_id 连续性 (发送端只在整帧成功后才 +1, 所以缺口 =
		 * 发送端未完成整帧协议发送的帧, 不能等同于硬件丢帧) */
		if(g_rx.prev_id_valid){
			id_gap = (int64_t)hdr.frame_id - (int64_t)(g_rx.prev_id + 1);
			if(id_gap != 0){
				g_rx.frame_id_gap_events++;
				if(id_gap > 0)
					g_rx.frame_id_gap_total += (uint64_t)id_gap;
			}
		} else {
			g_rx.first_id = hdr.frame_id;
			g_rx.prev_id_valid = 1;
			id_gap = 0;
		}
		g_rx.prev_id = hdr.frame_id;
		g_rx.last_id = hdr.frame_id;

		g_rx.frames_ok++;
		g_rx.payload_bytes += hdr.size;
		g_rx.header_bytes += sizeof(hdr);

		if(g_rx.prev_r2_valid)
			metrics_hist_add(&g_rx.h_interval,(r2-g_rx.prev_r2_ns)/1000ULL);
		g_rx.prev_r2_ns = r2; g_rx.prev_r2_valid = 1;
		metrics_hist_add(&g_rx.h_wait,(r2-r1pre)/1000ULL);
		metrics_hist_add(&g_rx.h_reasm,reasm_us);

		/* effective pixfmt: override > yuv-order > auto */
		/* 注意 epix 用的是 hdr.pixfmt 而不是 SDL 内部的 override 状态,
		 * 因为落盘 PPM 也要遵循同一套优先级 */
#ifdef ENABLE_SDL_DISPLAY
		if(g_disp_override!=0) epix=g_disp_override;
		else if(g_yuv_order && pixfmt_is_yuv422(hdr.pixfmt)) epix=g_yuv_order;
		else epix=hdr.pixfmt;
#else
		epix=hdr.pixfmt;
#endif

		/* SDL 窗口按首帧尺寸创建; 建失败就关掉显示, 采集和落盘继续 */
#ifdef ENABLE_SDL_DISPLAY
		if(enable_display&&!dinited){if(sdl_display_init(&disp,(int)hdr.width,(int)hdr.height)<0)enable_display=0;else dinited=1;}
		if(enable_display&&dinited){if(sdl_display_show_frame(&disp,fbuf,hdr.size,hdr.pixfmt)<0) g_running=0;}
#endif
		r3 = metrics_now_ns();
#ifdef ENABLE_SDL_DISPLAY
		if(enable_display&&dinited)
			metrics_hist_add(&g_rx.h_sdl,(r3-r2)/1000ULL);
#endif

		/* 原始数据先落盘: 扩展名按帧头声明的格式取, 方便事后辨认。
		 * --no-save 时跳过, 避免磁盘 I/O 干扰吞吐测量。 */
		if(!g_rx.no_save){
			snprintf(fn,sizeof(fn),"%s/frame_%06u.%s",outdir,hdr.frame_id,pixfmt_to_ext(hdr.pixfmt));
			if(save_raw_frame(fn,fbuf,hdr.size)==0){ saved=1; g_rx.frames_written++; }
		}

		if(!g_rx.quiet){
			printf("[FRAME %u] %ux%u %uB hdr=%s",hdr.frame_id,hdr.width,hdr.height,hdr.size,pixfmt_to_string(hdr.pixfmt));
#ifdef ENABLE_SDL_DISPLAY
			/* 解码格式与帧头不一致时才多打一段, 免得每行都一样长 */
			if(epix!=hdr.pixfmt||g_disp_override!=0||g_yuv_order) printf(" disp=%s",pixfmt_to_string(epix));
#endif
			printf(" ts=%lu us\n",(unsigned long)hdr.timestamp_us);
		}

		/* 能解码的格式额外存一份 PPM (用 epix 解码, 与屏幕所见一致) */
		if(!g_rx.no_save && pixfmt_is_displayable(epix)){
			snprintf(pfn,sizeof(pfn),"%s/frame_%06u.ppm",outdir,hdr.frame_id);
			save_frame_as_ppm(pfn,fbuf,hdr.size,hdr.width,hdr.height,epix);
			if(!g_rx.quiet) printf("  -> PPM: %s (as %s)\n",pfn,pixfmt_to_string(epix));
		}

		/* 逐帧明细 CSV */
		if(g_rx.enabled && g_rx.fp_frames != NULL){
			fprintf(g_rx.fp_frames,
				"%llu,%u,%lld,%u,%u,0x%08X,%u,%llu,%llu,%llu,%llu,"
				"%llu,%llu,%llu,%llu,%u,%d\n",
				(unsigned long long)(g_rx.frames_ok-1), hdr.frame_id,
				(long long)id_gap,
				hdr.width, hdr.height, hdr.pixfmt, hdr.size,
				(unsigned long long)hdr_us,
				(unsigned long long)((r2 - r1pre)/1000ULL),
				(unsigned long long)reasm_us,
				(unsigned long long)((r3-r2)/1000ULL),
				(unsigned long long)r1pre,
				(unsigned long long)r1,
				(unsigned long long)r2,
				(unsigned long long)r3,
				hdr.size, saved);
		}

		rx_bench_periodic(&g_rx, r3);
	}

	printf("[INFO] Cleaning up...\n");
#ifdef ENABLE_SDL_DISPLAY
	if(dinited) sdl_display_deinit(&disp);
#endif
	rx_bench_finish(&g_rx, metrics_now_ns());
	free(fbuf); if(cfd>=0) close(cfd); if(lfd>=0) close(lfd);
	printf("[INFO] Done.\n");
	return 0;
}
