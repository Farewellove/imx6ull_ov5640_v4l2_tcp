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
 * 颜色转换: 各种格式 → RGB888 (一行)
 * 位扩展: r8=(r5<<3)|(r5>>2), g8=(g6<<2)|(g6>>4), b8=(b5<<3)|(b5>>2)
 * ================================================================ */

static void rgb565_le_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for (x=0;x<w;x++) { uint16_t p=src[x*2]|(src[x*2+1]<<8);
  uint8_t r5=(p>>11)&0x1f,g6=(p>>5)&0x3f,b5=p&0x1f;
  *rgb++=(r5<<3)|(r5>>2); *rgb++=(g6<<2)|(g6>>4); *rgb++=(b5<<3)|(b5>>2); }}

static void rgb565_be_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for (x=0;x<w;x++) { uint16_t p=(src[x*2]<<8)|src[x*2+1];
  uint8_t r5=(p>>11)&0x1f,g6=(p>>5)&0x3f,b5=p&0x1f;
  *rgb++=(r5<<3)|(r5>>2); *rgb++=(g6<<2)|(g6>>4); *rgb++=(b5<<3)|(b5>>2); }}

static void bgr565_le_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for (x=0;x<w;x++) { uint16_t p=src[x*2]|(src[x*2+1]<<8);
  uint8_t b5=(p>>11)&0x1f,g6=(p>>5)&0x3f,r5=p&0x1f;
  *rgb++=(r5<<3)|(r5>>2); *rgb++=(g6<<2)|(g6>>4); *rgb++=(b5<<3)|(b5>>2); }}

static void bgr565_be_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for (x=0;x<w;x++) { uint16_t p=(src[x*2]<<8)|src[x*2+1];
  uint8_t b5=(p>>11)&0x1f,g6=(p>>5)&0x3f,r5=p&0x1f;
  *rgb++=(r5<<3)|(r5>>2); *rgb++=(g6<<2)|(g6>>4); *rgb++=(b5<<3)|(b5>>2); }}

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

static void yuyv_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for(x=0;x<w;x++) { uint32_t o=(x/2)*4; int y=src[o+((x&1)?2:0)],u=src[o+1],v=src[o+3]; yuv_to_rgb(y,u,v,rgb); rgb+=3; }}

static void uyvy_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for(x=0;x<w;x++) { uint32_t o=(x/2)*4; int u=src[o+0],y=src[o+((x&1)?3:1)],v=src[o+2]; yuv_to_rgb(y,u,v,rgb); rgb+=3; }}

static void yvyu_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for(x=0;x<w;x++) { uint32_t o=(x/2)*4; int y=src[o+((x&1)?2:0)],v=src[o+1],u=src[o+3]; yuv_to_rgb(y,u,v,rgb); rgb+=3; }}

static void vyuy_to_rgb888_line(const uint8_t *src, uint8_t *rgb, uint32_t w)
{ uint32_t x; for(x=0;x<w;x++) { uint32_t o=(x/2)*4; int v=src[o+0],y=src[o+((x&1)?3:1)],u=src[o+2]; yuv_to_rgb(y,u,v,rgb); rgb+=3; }}

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

/* PPM 保存 — 与 SDL 显示使用相同解码器 */
static int save_frame_as_ppm(const char *filename, const uint8_t *data,
                             uint32_t size, uint32_t width, uint32_t height,
                             uint32_t pixfmt)
{
	FILE *fp; uint32_t y; uint8_t *rgb_line; (void)size;
	fp=fopen(filename,"wb"); if(!fp) return -1;
	fprintf(fp,"P6\n%u %u\n255\n",width,height);
	rgb_line=malloc(width*3); if(!rgb_line){fclose(fp);return -1;}
	for(y=0;y<height;y++){convert_line_to_rgb888(data+y*width*2,rgb_line,width,pixfmt); fwrite(rgb_line,width*3,1,fp);}
	free(rgb_line); fclose(fp); return 0;
}

static int save_raw_frame(const char *filename, const uint8_t *data, uint32_t size)
{ FILE *fp=fopen(filename,"wb"); if(!fp)return -1; fwrite(data,1,size,fp); fclose(fp); return 0; }

/* ================================================================
 * SDL2 显示
 * ================================================================ */
#ifdef ENABLE_SDL_DISPLAY

struct sdl_display { SDL_Window *window; SDL_Renderer *renderer; SDL_Texture *texture; int width,height; };

/* 0 = auto-detect from header (默认) */
static uint32_t    g_disp_override     = 0;
static const char *g_disp_override_tag = "auto(hdr)";
static int         g_disp_first_frame  = 1;
static uint32_t    g_yuv_order         = 0;  /* --yuv-order override */

static void sdl_update_title(struct sdl_display *d, uint32_t hdr_pixfmt, uint32_t disp_pixfmt)
{ char t[256]; snprintf(t,sizeof(t),"OV5640 | hdr=%s | disp=%s",pixfmt_to_name(hdr_pixfmt),pixfmt_to_name(disp_pixfmt)); SDL_SetWindowTitle(d->window,t); }

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

	/* 事件后重取 */
	if(g_disp_override!=0) disp_pixfmt=g_disp_override;
	else if(g_yuv_order && pixfmt_is_yuv422(hdr_pixfmt)) disp_pixfmt=g_yuv_order;
	else disp_pixfmt=hdr_pixfmt;

	if(!pixfmt_is_displayable(disp_pixfmt)){SDL_SetRenderDrawColor(d->renderer,0,0,0,255);SDL_RenderClear(d->renderer);SDL_RenderPresent(d->renderer);return 0;}

	rgb_line=malloc(d->width*3); if(!rgb_line) return 0;
	for(y=0;y<(uint32_t)d->height;y++){convert_line_to_rgb888(data+y*d->width*2,rgb_line,d->width,disp_pixfmt); SDL_UpdateTexture(d->texture,&(SDL_Rect){0,(int)y,d->width,1},rgb_line,d->width*3);}
	free(rgb_line);
	SDL_RenderClear(d->renderer); SDL_RenderCopy(d->renderer,d->texture,NULL,NULL); SDL_RenderPresent(d->renderer);
	return 0;
}

static void sdl_display_deinit(struct sdl_display *d)
{ if(d->texture) SDL_DestroyTexture(d->texture); if(d->renderer) SDL_DestroyRenderer(d->renderer); if(d->window) SDL_DestroyWindow(d->window); SDL_Quit(); }
#endif

/* ================================================================
 * 网络
 * ================================================================ */
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

/* parse --display-format / --yuv-order */
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
enum { OPT_DISPLAY_FMT=256, OPT_SWAP_RB, OPT_YUV_ORDER,
       OPT_F_RGB565, OPT_F_RGB565X, OPT_F_BGR565, OPT_F_BGR565X, OPT_F_YUYV, OPT_F_UYVY };
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
	uint8_t *fbuf=NULL; uint32_t fcap=0;

#ifdef ENABLE_SDL_DISPLAY
	struct sdl_display disp; int dinited=0;
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
	if(mkdir(outdir,0755)<0&&errno!=EEXIST){perror("mkdir");return 1;}

	lfd=tcp_server_listen(port,1); if(lfd<0) return 1;
	printf("[INFO] Waiting for connection...\n");
	cfd=accept(lfd,NULL,NULL); if(cfd<0){if(errno!=EINTR)perror("accept");close(lfd);return 1;}
	printf("[INFO] Client connected (fd=%d)\n",cfd);

	while(g_running){
		struct frame_header hdr; int ret; char fn[512], pfn[512]; uint32_t epix;

		ret=recv_frame_header(cfd,&hdr); if(ret<0) break;
		if(hdr.magic!=FRAME_MAGIC){fprintf(stderr,"[ERROR] Bad magic\n");break;}
		if(hdr.size>fcap){uint8_t *p=realloc(fbuf,hdr.size); if(!p)break; fbuf=p; fcap=hdr.size;}
		ret=recv_all(cfd,fbuf,hdr.size); if(ret<0) break;

		/* effective pixfmt: override > yuv-order > auto */
#ifdef ENABLE_SDL_DISPLAY
		if(g_disp_override!=0) epix=g_disp_override;
		else if(g_yuv_order && pixfmt_is_yuv422(hdr.pixfmt)) epix=g_yuv_order;
		else epix=hdr.pixfmt;
#else
		epix=hdr.pixfmt;
#endif

#ifdef ENABLE_SDL_DISPLAY
		if(enable_display&&!dinited){if(sdl_display_init(&disp,(int)hdr.width,(int)hdr.height)<0)enable_display=0;else dinited=1;}
		if(enable_display&&dinited){if(sdl_display_show_frame(&disp,fbuf,hdr.size,hdr.pixfmt)<0) g_running=0;}
#endif

		snprintf(fn,sizeof(fn),"%s/frame_%06u.%s",outdir,hdr.frame_id,pixfmt_to_ext(hdr.pixfmt));
		save_raw_frame(fn,fbuf,hdr.size);
		printf("[FRAME %u] %ux%u %uB hdr=%s",hdr.frame_id,hdr.width,hdr.height,hdr.size,pixfmt_to_string(hdr.pixfmt));
#ifdef ENABLE_SDL_DISPLAY
		if(epix!=hdr.pixfmt||g_disp_override!=0||g_yuv_order) printf(" disp=%s",pixfmt_to_string(epix));
#endif
		printf(" ts=%lu us\n",(unsigned long)hdr.timestamp_us);

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
