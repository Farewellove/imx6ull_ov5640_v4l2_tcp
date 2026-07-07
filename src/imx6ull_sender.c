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

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
	(void)sig;
	g_running = 0;
	fprintf(stderr, "\n[INFO] Received signal, exiting...\n");
}

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

/* YUV order name -> internal FOURCC (for overriding S_FMT result) */
static uint32_t yuv_order_to_pixfmt(const char *name)
{
	if (strcasecmp(name, "yuyv") == 0) return V4L2_PIX_FMT_YUYV;
	if (strcasecmp(name, "uyvy") == 0) return V4L2_PIX_FMT_UYVY;
	if (strcasecmp(name, "yvyu") == 0) return PIX_FMT_YVYU;
	if (strcasecmp(name, "vyuy") == 0) return PIX_FMT_VYUY;
	return 0;
}

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

	/* 严格检查: S_FMT 返回的格式必须可接受 */
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

	/* 如果指定格式但驱动返回不同 -> 警告，可选退出 */
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

	/* YUV order 覆盖: 仅影响 frame_header 中的 pixfmt 标签 */
	if (yuv_override && pixfmt_is_yuv422(ctx.pixfmt)) {
		printf("[INFO]   yuv-order override: %s -> %s (header only)\n",
		       pixfmt_to_string(ctx.pixfmt),
		       pixfmt_to_string(yuv_override));
	}

	if (enable_lcd && !pixfmt_is_displayable(ctx.pixfmt))
		printf("[WARN] LCD display may not support pixfmt=%s\n",
		       pixfmt_to_string(ctx.pixfmt));
	printf("[INFO] ========================================\n");

	/* 2. 连接 TCP */
	sockfd = tcp_client_connect(server_ip, port);
	if (sockfd < 0) {
		fprintf(stderr, "[ERROR] TCP connect failed\n");
		v4l2_deinit(&ctx);
		return 1;
	}

	/* 3. LCD (可选) */
	if (enable_lcd) {
		if (fb_display_init(&fb, "/dev/fb0") < 0) {
			fprintf(stderr, "[WARN] LCD init failed, "
			        "continuing without display\n");
			enable_lcd = 0;
		}
	}

	/* 4. 采集 + 发送 */
	printf("[INFO] Starting capture loop (Ctrl+C to stop)...\n");

	while (g_running && (max_frames == 0 || (int)frame_id < max_frames)) {
		void *data = NULL;
		size_t size = 0;
		int buf_idx = -1;
		uint64_t timestamp = 0;
		struct frame_header hdr;
		uint32_t header_pixfmt;

		ret = v4l2_get_frame(&ctx, &data, &size, &buf_idx,
		                     &timestamp, 2000);
		if (ret < 0) {
			if (ret == -ETIMEDOUT || ret == -EAGAIN) continue;
			if (ret == -EINTR) break;
			fprintf(stderr, "[ERROR] v4l2_get_frame failed (ret=%d)\n",
			        ret);
			break;
		}

		/* LCD 显示 (YUV 自动转 RGB565) */
		if (enable_lcd)
			fb_display_show_frame(&fb, data, ctx.width, ctx.height,
			                      ctx.pixfmt);

		/* 帧头: 使用 yuv_override (如果设置) 或 ctx.pixfmt */
		header_pixfmt = (yuv_override && pixfmt_is_yuv422(ctx.pixfmt))
		                ? yuv_override : ctx.pixfmt;

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
		v4l2_put_frame(&ctx, buf_idx);
	}

	printf("[INFO] Sent %u frames total\n", frame_id);

	/* 5. 清理 */
	if (sockfd >= 0) close(sockfd);
	if (enable_lcd) fb_display_deinit(&fb);
	v4l2_deinit(&ctx);
	printf("[INFO] Done.\n");
	return 0;
}
