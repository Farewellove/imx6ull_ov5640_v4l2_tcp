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
#include <arpa/inet.h>
#include <getopt.h>

#include "frame_proto.h"
#include "tcp_socket.h"

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

/*
 * 收 40 字节帧头并转成本机字节序。
 * 先整体 recv_all 到临时结构 n, 逐个字段 ntohl —— 不直接往 hdr 上读,
 * 是为了避免"读了一半"时 hdr 处于半新半旧的状态。
 * timestamp_us 是 64 位, 拆成高低两个 32 位分别 ntohl 再拼回去。
 * 返回 -1 表示对端断开或出错。
 */
static int recv_frame_header(int fd, struct frame_header *hdr)
{
	struct frame_header n; int ret;
	ret=recv_all(fd,&n,sizeof(n)); if(ret<0) return -1;
	hdr->magic=ntohl(n.magic); hdr->version=ntohl(n.version); hdr->header_size=ntohl(n.header_size);
	hdr->width=ntohl(n.width); hdr->height=ntohl(n.height); hdr->pixfmt=ntohl(n.pixfmt);
	hdr->size=ntohl(n.size); hdr->frame_id=ntohl(n.frame_id);
	hdr->timestamp_us=(((uint64_t)ntohl((uint32_t)(n.timestamp_us>>32)))<<32)|ntohl((uint32_t)(n.timestamp_us&0xffffffff));
	return 0;
}

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
#ifdef ENABLE_SDL_DISPLAY
		"  -d                    Enable SDL2 display\n"
		"  --display-format <f>  Force decode: rgb565/rgb565x/bgr565/bgr565x/yuyv/uyvy/yvyu/vyuy\n"
		"  --swap-rb             Shortcut for --display-format bgr565\n"
		"  --yuv-order <o>       YUV byte order: yuyv/uyvy/yvyu/vyuy\n"
		"  --force-<fmt>         Legacy alias for --display-format\n"
#endif
		"\nExamples:\n"
		"  %s -p 5000 -o ./frames -d                       # auto-detect\n"
#ifdef ENABLE_SDL_DISPLAY
		"  %s -p 5000 -d --display-format yuyv             # force YUYV\n"
		"  %s -p 5000 -d --yuv-order uyvy                  # YUYV hdr, UYVY decode\n"
#endif
		, p, p
#ifdef ENABLE_SDL_DISPLAY
		, p, p
#endif
		);
}

#ifdef ENABLE_SDL_DISPLAY
/* 长选项的 val 从 256 起, 避开 getopt 已占用的 ASCII 短选项返回值 */
enum { OPT_DISPLAY_FMT=256, OPT_SWAP_RB, OPT_YUV_ORDER,
       OPT_F_RGB565, OPT_F_RGB565X, OPT_F_BGR565, OPT_F_BGR565X, OPT_F_YUYV, OPT_F_UYVY };
/* --force-xxx 是早期版本的写法, 保留下来兼容旧脚本, 效果等同 --display-format */
static const struct option lopts[]={
	{"display-format",1,NULL,OPT_DISPLAY_FMT},{"swap-rb",0,NULL,OPT_SWAP_RB},
	{"yuv-order",1,NULL,OPT_YUV_ORDER},
	{"force-rgb565",0,NULL,OPT_F_RGB565},{"force-rgb565x",0,NULL,OPT_F_RGB565X},
	{"force-bgr565",0,NULL,OPT_F_BGR565},{"force-bgr565x",0,NULL,OPT_F_BGR565X},
	{"force-yuyv",0,NULL,OPT_F_YUYV},{"force-uyvy",0,NULL,OPT_F_UYVY},{0,0,0,0}};
#endif

int main(int argc, char *argv[])
{
	int port=5000, enable_display=0, opt, oi, lfd=-1, cfd=-1;
	const char *outdir="./frames";
	/* fbuf/fcap: 帧数据缓冲及其当前容量, 收帧时按需 realloc 复用 */
	uint8_t *fbuf=NULL; uint32_t fcap=0;

#ifdef ENABLE_SDL_DISPLAY
	struct sdl_display disp; int dinited=0;
	/* 显式复位全局显示状态 (防重复调用 main 时残留) */
	memset(&disp,0,sizeof(disp));
	g_disp_override=0; g_disp_override_tag="auto(hdr)"; g_disp_first_frame=1; g_yuv_order=0;

	while((opt=getopt_long(argc,argv,"p:o:d",lopts,&oi))!=-1){
#else
	while((opt=getopt(argc,argv,"p:o:"))!=-1){
#endif
		switch(opt){
		case 'p': port=atoi(optarg); break;
		case 'o': outdir=optarg; break;
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
#ifdef ENABLE_SDL_DISPLAY
	printf("  Display : %s\n",enable_display?"SDL2":"disabled");
	printf("  Decoder : %s\n",g_disp_override?pixfmt_to_string(g_disp_override):"auto (from header)");
	if(g_yuv_order) printf("  YUV Ord : %s\n",pixfmt_to_string(g_yuv_order));
#endif
	printf("================================================================\n\n");

	signal(SIGINT,signal_handler); signal(SIGTERM,signal_handler);

	/* 输出目录不存在就建; 已存在 (EEXIST) 不算错 */
	if(mkdir(outdir,0755)<0&&errno!=EEXIST){perror("mkdir");return 1;}

	lfd=tcp_server_listen(port,1); if(lfd<0) return 1;
	printf("[INFO] Waiting for connection...\n");
	/* 只服务一个客户端 (accept 一次就不管了), 单帧流场景够用;
	 * backlog=1 也说明这里没打算支持多客户端 */
	cfd=accept(lfd,NULL,NULL); if(cfd<0){if(errno!=EINTR)perror("accept");close(lfd);return 1;}
	printf("[INFO] Client connected (fd=%d)\n",cfd);

	/*
	 * 收帧主循环: 一帧 = 帧头(40B) + 数据(hdr.size B)。
	 * 任何一步失败 (对端断开 / magic 不对 / 分配失败) 都直接 break 退出,
	 * 因为帧边界一旦错位, 后续收到的字节流就再也对不齐了,
	 * 继续读只会得到一堆垃圾。
	 */
	while(g_running){
		struct frame_header hdr; int ret; char fn[512], pfn[512]; uint32_t epix;

		ret=recv_frame_header(cfd,&hdr); if(ret<0) break;
		/* magic 校验: 对不齐时立刻发现, 不用等解码出错 */
		if(hdr.magic!=FRAME_MAGIC){fprintf(stderr,"[ERROR] Bad magic\n");break;}
		/* 缓冲区按需增长, 只增不减 (帧大小通常固定, realloc 只发生一次) */
		if(hdr.size>fcap){uint8_t *p=realloc(fbuf,hdr.size); if(!p)break; fbuf=p; fcap=hdr.size;}
		ret=recv_all(cfd,fbuf,hdr.size); if(ret<0) break;

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

		/* 原始数据先落盘: 扩展名按帧头声明的格式取, 方便事后辨认 */
		snprintf(fn,sizeof(fn),"%s/frame_%06u.%s",outdir,hdr.frame_id,pixfmt_to_ext(hdr.pixfmt));
		save_raw_frame(fn,fbuf,hdr.size);
		printf("[FRAME %u] %ux%u %uB hdr=%s",hdr.frame_id,hdr.width,hdr.height,hdr.size,pixfmt_to_string(hdr.pixfmt));
#ifdef ENABLE_SDL_DISPLAY
		/* 解码格式与帧头不一致时才多打一段, 免得每行都一样长 */
		if(epix!=hdr.pixfmt||g_disp_override!=0||g_yuv_order) printf(" disp=%s",pixfmt_to_string(epix));
#endif
		printf(" ts=%lu us\n",(unsigned long)hdr.timestamp_us);

		/* 能解码的格式额外存一份 PPM (用 epix 解码, 与屏幕所见一致) */
		if(pixfmt_is_displayable(epix)){
			snprintf(pfn,sizeof(pfn),"%s/frame_%06u.ppm",outdir,hdr.frame_id);
			save_frame_as_ppm(pfn,fbuf,hdr.size,hdr.width,hdr.height,epix);
			printf("  -> PPM: %s (as %s)\n",pfn,pixfmt_to_string(epix));
		}
	}

	printf("[INFO] Cleaning up...\n");
#ifdef ENABLE_SDL_DISPLAY
	if(dinited) sdl_display_deinit(&disp);
#endif
	free(fbuf); if(cfd>=0) close(cfd); if(lfd>=0) close(lfd);
	printf("[INFO] Done.\n");
	return 0;
}
