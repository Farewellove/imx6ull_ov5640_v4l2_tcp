/*
 * imx6ull_sender.c - i.MX6ULL 板端采集发送程序
 *
 * 默认采集格式: YUYV (V4L2_PIX_FMT_YUYV)
 * 使用 S_FMT 驱动实际返回的 pixfmt 构造帧头 (ctx.pixfmt)
 *
 * 用法:
 *   ./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -p 5000
 *   ./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -p 5000 --pixfmt yuyv
 *   ./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -p 5000 --yuv-order uyvy
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>
#include <arpa/inet.h>

#include "frame_proto.h"
#include "v4l2_capture.h"
#include "tcp_socket.h"
#include "fb_display.h"

/*
 * 采集循环的运行标志。
 * 必须是 volatile sig_atomic_t: 在信号处理函数里被改写, 主循环里被读,
 * 用 sig_atomic_t 保证读写是单指令、不会被中断撕裂, volatile 阻止
 * 编译器把它优化进寄存器 (否则循环可能永远看不到变化)。
 */
static volatile sig_atomic_t g_running = 1;

/*
 * SIGINT/SIGTERM 处理: 只置标志位就返回, 不在信号上下文里做清理。
 * 真正的清理 (关 socket、STREAMOFF、munmap) 交给主循环退出后按顺序做
 * —— 信号处理函数里能安全调用的函数非常有限, 在那里做清理容易出问题。
 */
static void signal_handler(int sig)
{
	(void)sig;
	g_running = 0;
	fprintf(stderr, "\n[INFO] Received signal, exiting...\n");
}

/* 命令行格式名 → FOURCC (-f / --pixfmt)。返回 0 表示无法识别 */
static uint32_t name_to_pixfmt(const char *name)
{
	if (strcasecmp(name, "RGB565")  == 0) return V4L2_PIX_FMT_RGB565;
	if (strcasecmp(name, "RGB565X") == 0) return V4L2_PIX_FMT_RGB565X;
	if (strcasecmp(name, "BGR565")  == 0) return PIX_FMT_BGR565;
	if (strcasecmp(name, "BGR565X") == 0) return PIX_FMT_BGR565X;
	if (strcasecmp(name, "YUYV")    == 0) return V4L2_PIX_FMT_YUYV;
	if (strcasecmp(name, "UYVY")    == 0) return V4L2_PIX_FMT_UYVY;
	if (strcasecmp(name, "YVYU")    == 0) return PIX_FMT_YVYU;
	if (strcasecmp(name, "VYUY")    == 0) return PIX_FMT_VYUY;
	if (strcasecmp(name, "MJPEG")   == 0) return V4L2_PIX_FMT_MJPEG;
	return 0;
}

/*
 * YUV 字节序名 → 内部 FOURCC (--yuv-order)。
 * 用于覆盖 S_FMT 的返回值: 当驱动报告 YUYV 但实际字节排列不对时,
 * 用它改掉帧头里声明的 pixfmt, 让 PC 端按正确的顺序解码。
 * 注意这**只改帧头标签**, 板端不做任何字节重排 (数据原样发出)。
 */
static uint32_t yuv_order_to_pixfmt(const char *name)
{
	if (strcasecmp(name, "yuyv") == 0) return V4L2_PIX_FMT_YUYV;
	if (strcasecmp(name, "uyvy") == 0) return V4L2_PIX_FMT_UYVY;
	if (strcasecmp(name, "yvyu") == 0) return PIX_FMT_YVYU;
	if (strcasecmp(name, "vyuy") == 0) return PIX_FMT_VYUY;
	return 0;
}

/*
 * 发送帧头: 先拷到局部结构里逐字段 htonl, 再一次性 send_all。
 *
 * 为什么要拷一份而不是就地改 hdr: hdr 是主循环里的变量, 就地转成网络序
 * 会让后续打印/复用拿到错的值。用局部结构既不破坏调用者数据, 也保证
 * 发出的 40 字节里所有多字节字段都是大端。
 *
 * timestamp_us 是 64 位, 而 htonl 只管 32 位: 拆成高 32 位和低 32 位
 * 分别转换, 再拼回一个 uint64_t。
 *
 * 返回 send_all 的结果 (0 成功 / -1 失败)。
 */
static int send_frame_header(int sockfd, const struct frame_header *hdr)
{
	struct frame_header net_hdr;

	net_hdr.magic        = htonl(hdr->magic);
	net_hdr.version      = htonl(hdr->version);
	net_hdr.header_size  = htonl(hdr->header_size);
	net_hdr.width        = htonl(hdr->width);
	net_hdr.height       = htonl(hdr->height);
	net_hdr.pixfmt       = htonl(hdr->pixfmt);
	net_hdr.size         = htonl(hdr->size);
	net_hdr.frame_id     = htonl(hdr->frame_id);
	net_hdr.timestamp_us =
		(((uint64_t)htonl((uint32_t)(hdr->timestamp_us >> 32))) << 32)
		| htonl((uint32_t)(hdr->timestamp_us & 0xffffffff));

	return send_all(sockfd, &net_hdr, sizeof(net_hdr));
}

enum { OPT_PIXFMT = 256, OPT_YUV_ORDER };

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Options:\n"
		"  -d <dev>          Video device, default /dev/video1\n"
		"  -s <ip>           Server IP address (required)\n"
		"  -p <port>         TCP port, default 5000\n"
		"  -w <width>        Image width, default 640\n"
		"  -h <height>       Image height, default 480\n"
		"  -f <fmt>          Pixel format: YUYV(default), UYVY, RGB565, MJPEG\n"
		"  --pixfmt <fmt>    Same as -f\n"
		"  --yuv-order <o>   YUV byte order: yuyv(default), uyvy, yvyu, vyuy\n"
		"  -n <frames>       Number of frames, 0=unlimited (default 0)\n"
		"  -l                Enable LCD display via /dev/fb0\n"
		"\n"
		"Examples:\n"
		"  %s -d /dev/video1 -s 192.168.1.66 -p 5000\n"
		"  %s -d /dev/video1 -s 192.168.1.66 -p 5000 --yuv-order uyvy\n",
		prog, prog, prog);
}

int main(int argc, char *argv[])
{
	const char *devname    = "/dev/video1";
	const char *server_ip  = NULL;
	int port               = 5000;
	uint32_t req_width     = 640;
	uint32_t req_height    = 480;
	uint32_t req_pixfmt    = V4L2_PIX_FMT_YUYV;  /* 默认 YUYV */
	uint32_t yuv_override  = 0;    /* --yuv-order 覆盖 */
	int max_frames         = 0;
	int enable_lcd         = 0;
	int opt, opt_index;

	struct v4l2_context ctx;
	struct fb_display fb;
	int sockfd = -1;
	uint32_t frame_id = 0;
	int ret;

	static const struct option long_options[] = {
		{"pixfmt",    required_argument, NULL, OPT_PIXFMT},
		{"yuv-order", required_argument, NULL, OPT_YUV_ORDER},
		{NULL, 0, NULL, 0}
	};

	while ((opt = getopt_long(argc, argv, "d:s:p:w:h:f:n:l",
	                          long_options, &opt_index)) != -1) {
		switch (opt) {
		case 'd': devname = optarg; break;
		case 's': server_ip = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 'w': req_width = (uint32_t)atoi(optarg); break;
		case 'h': req_height = (uint32_t)atoi(optarg); break;
		case 'f':
		case OPT_PIXFMT:
			req_pixfmt = name_to_pixfmt(optarg);
			if (req_pixfmt == 0) {
				fprintf(stderr, "[ERROR] Unknown format: %s\n"
				        "Supported: YUYV, UYVY, RGB565, MJPEG\n",
				        optarg);
				return 1;
			}
			break;
		case OPT_YUV_ORDER:
			yuv_override = yuv_order_to_pixfmt(optarg);
			if (yuv_override == 0) {
				fprintf(stderr, "[ERROR] Unknown yuv order: %s\n"
				        "Supported: yuyv, uyvy, yvyu, vyuy\n",
				        optarg);
				return 1;
			}
			printf("[INFO] YUV byte order override: %s\n", optarg);
			break;
		case 'n': max_frames = atoi(optarg); break;
		case 'l': enable_lcd = 1; break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (server_ip == NULL) {
		fprintf(stderr, "[ERROR] Server IP (-s) is required\n\n");
		usage(argv[0]);
		return 1;
	}

	printf("================================================================\n");
	printf("  i.MX6ULL OV5640 V4L2 TCP Sender\n");
	printf("================================================================\n");
	printf("  Device    : %s\n", devname);
	printf("  Server    : %s:%d\n", server_ip, port);
	printf("  Requested : %s %ux%u (0x%08X)\n",
	       pixfmt_to_string(req_pixfmt), req_width, req_height, req_pixfmt);
	if (yuv_override)
		printf("  YUV Order : %s\n", pixfmt_to_string(yuv_override));
	printf("================================================================\n\n");

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* 1. V4L2 初始化 (S_FMT 后 ctx.pixfmt = 驱动实际格式) */
	printf("[INFO] Initializing V4L2 capture...\n");
	ret = v4l2_init(&ctx, devname, req_width, req_height, req_pixfmt);
	if (ret < 0) {
		fprintf(stderr, "[ERROR] V4L2 init failed\n");
		return 1;
	}

	/*
	 * 把 S_FMT 的实际返回结果打出来 —— 这是帧头里 pixfmt/width/height
	 * 的唯一来源, 后面所有环节 (PC 端解码、LCD 转换) 都以它为准。
	 */
	printf("[INFO] ========================================\n");
	printf("[INFO] === S_FMT result (frame_header source) ===\n");
	printf("[INFO]   accepted pixfmt = %s (0x%08X / '%.4s')\n",
	       pixfmt_to_string(ctx.pixfmt), ctx.pixfmt, (char *)&ctx.pixfmt);
	printf("[INFO]   requested       = %s (0x%08X)\n",
	       pixfmt_to_string(req_pixfmt), req_pixfmt);
	printf("[INFO]   width           = %u\n", ctx.width);
	printf("[INFO]   height          = %u\n", ctx.height);
	printf("[INFO]   bytesperline    = %u\n", ctx.bytesperline);
	printf("[INFO]   sizeimage       = %u\n", ctx.sizeimage);

	/*
	 * 严格检查: S_FMT 必须原样接受请求的格式, 否则直接退出。
	 *
	 * 注意这里和 v4l2_capture.c 里的策略不同 —— 那边把格式被替换当作
	 * 警告 (只是个采集库, 由调用者决定怎么办); 这边是最终应用, 格式
	 * 被悄悄换掉会导致 PC 端按错误的解码器解释数据 (典型症状是颜色
	 * 全错或画面撕裂), 与其发出去一堆花屏, 不如启动时就报错退出,
	 * 让用户显式换个支持的格式。
	 */
	if (ctx.pixfmt != req_pixfmt) {
		fprintf(stderr,
			"[ERROR] S_FMT returned %s (0x%08X), but %s (0x%08X) was "
			"requested.\n"
			"        Driver does not support the requested format!\n",
			pixfmt_to_string(ctx.pixfmt), ctx.pixfmt,
			pixfmt_to_string(req_pixfmt), req_pixfmt);
		v4l2_deinit(&ctx);
		return 1;
	}

	/*
	 * YUV order 覆盖: 仅影响 frame_header 中的 pixfmt 标签。
	 * 只在驱动确实返回了 YUV422 时才有意义 (对 RGB565 用 --yuv-order
	 * 没道理), 所以这里用 pixfmt_is_yuv422 把无效组合忽略掉。
	 */
	if (yuv_override && pixfmt_is_yuv422(ctx.pixfmt)) {
		printf("[INFO]   yuv-order override: %s -> %s (header only)\n",
		       pixfmt_to_string(ctx.pixfmt),
		       pixfmt_to_string(yuv_override));
	}

	/* LCD 只认 YUYV/RGB565, 其他格式仍能发出去, 只是本地不显示 */
	if (enable_lcd && !pixfmt_is_displayable(ctx.pixfmt))
		printf("[WARN] LCD display may not support pixfmt=%s\n",
		       pixfmt_to_string(ctx.pixfmt));
	printf("[INFO] ========================================\n");

	/*
	 * 2. 连接 TCP
	 * 放在 V4L2 初始化之后: 先确认摄像头能正常工作, 再去占用对方的
	 * 连接。失败要回收已申请的 V4L2 资源, 否则驱动侧缓冲区一直占着。
	 */
	sockfd = tcp_client_connect(server_ip, port);
	if (sockfd < 0) {
		fprintf(stderr, "[ERROR] TCP connect failed\n");
		v4l2_deinit(&ctx);
		return 1;
	}

	/*
	 * 3. LCD (可选, -l)
	 * 本地显示失败不算致命: 把 enable_lcd 置 0 后继续跑, 图传功能
	 * 不受影响 —— 板子接不接屏都不该影响发图, 这样调试时能少一个
	 * 变量。后续循环里所有 LCD 调用都由 enable_lcd 守着。
	 */
	if (enable_lcd) {
		if (fb_display_init(&fb, "/dev/fb0") < 0) {
			fprintf(stderr, "[WARN] LCD init failed, "
			        "continuing without display\n");
			enable_lcd = 0;
		}
	}

	/*
	 * 4. 采集 + 发送主循环
	 *
	 * 每轮处理一帧: DQBUF 取帧 → (可选) 本地显示 → 发帧头 → 发数据
	 * → QBUF 归还缓冲。三个缓冲区轮转, 所以发送期间驱动还能继续往
	 * 另一个缓冲写, 采集不会因为网络发送而停顿。
	 *
	 * max_frames == 0 表示不限帧数, 一直发到 Ctrl+C (改 g_running)。
	 */
	printf("[INFO] Starting capture loop (Ctrl+C to stop)...\n");

	while (g_running && (max_frames == 0 || (int)frame_id < max_frames)) {
		void *data = NULL;
		size_t size = 0;
		int buf_idx = -1;
		uint64_t timestamp = 0;
		struct frame_header hdr;
		uint32_t header_pixfmt;

		/* 超时 2000ms: 给足驱动的出帧间隔, 又不至于无限期卡死 */
		ret = v4l2_get_frame(&ctx, &data, &size, &buf_idx,
		                     &timestamp, 2000);
		if (ret < 0) {
			/* 超时/暂无数据不是错误, 直接再等下一帧 */
			if (ret == -ETIMEDOUT || ret == -EAGAIN) continue;
			/* 被信号打断 -> 用户按了 Ctrl+C, 正常退出 */
			if (ret == -EINTR) break;
			fprintf(stderr, "[ERROR] v4l2_get_frame failed (ret=%d)\n",
			        ret);
			break;
		}

		/* LCD 显示 (YUYV/RGB565 转 XRGB8888, 用驱动实际
		 * bytesperline 作源行步长, 避免行尾 padding 错行) */
		if (enable_lcd)
			fb_display_show_frame(&fb, data, ctx.width, ctx.height,
			                      ctx.bytesperline, ctx.pixfmt);

		/* 帧头: 使用 yuv_override (如果设置) 或 ctx.pixfmt */
		header_pixfmt = (yuv_override && pixfmt_is_yuv422(ctx.pixfmt))
		                ? yuv_override : ctx.pixfmt;

		/*
		 * 组装帧头。width/height/pixfmt 全部取 S_FMT 的实际结果,
		 * size 取 DQBUF 报的 bytesused (不是 ctx.sizeimage ——
		 * MJPEG 这类变长格式每帧大小都不同, 只能以后者为准)。
		 */
		memset(&hdr, 0, sizeof(hdr));
		hdr.magic        = FRAME_MAGIC;
		hdr.version      = FRAME_VERSION;
		hdr.header_size  = sizeof(hdr);
		hdr.width        = ctx.width;
		hdr.height       = ctx.height;
		hdr.pixfmt       = header_pixfmt;
		hdr.size         = (uint32_t)size;
		hdr.frame_id     = frame_id;
		hdr.timestamp_us = timestamp;

		/*
		 * 发送失败 (对端断开等) 必须先把 buffer 还给驱动再退出,
		 * 否则 v4l2_deinit 的 STREAMOFF 时该 buffer 仍处于已取出
		 * 状态。这里选择发完即停, 不做重连 —— 断线后帧号会不连续,
		 * 继续发只会让 PC 端收到混乱的数据。
		 */
		if (send_frame_header(sockfd, &hdr) < 0) {
			fprintf(stderr, "[ERROR] send header failed (id=%u)\n",
			        frame_id);
			v4l2_put_frame(&ctx, buf_idx);
			break;
		}
		if (send_all(sockfd, data, size) < 0) {
			fprintf(stderr, "[ERROR] send data failed (id=%u)\n",
			        frame_id);
			v4l2_put_frame(&ctx, buf_idx);
			break;
		}

		printf("[FRAME %u] hdr=%s, %ux%u, %zuB, ts=%lu us\n",
		       frame_id, pixfmt_to_string(header_pixfmt),
		       ctx.width, ctx.height,
		       size, (unsigned long)timestamp);

		frame_id++;
		/* 用完立刻归还, 保持驱动侧始终有 buffer 可写 */
		v4l2_put_frame(&ctx, buf_idx);
	}

	printf("[INFO] Sent %u frames total\n", frame_id);

	/* 5. 清理: 顺序与初始化相反 (socket → LCD → V4L2) */
	if (sockfd >= 0) close(sockfd);
	if (enable_lcd) fb_display_deinit(&fb);
	v4l2_deinit(&ctx);
	printf("[INFO] Done.\n");
	return 0;
}
