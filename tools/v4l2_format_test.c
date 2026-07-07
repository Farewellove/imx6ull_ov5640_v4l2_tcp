/*
 * tools/v4l2_format_test.c — V4L2 格式协商测试工具
 *
 * 测试驱动对指定格式的支持情况: ENUM_FMT → TRY_FMT → S_FMT → G_FMT
 *
 * 用法:
 *   ./v4l2_format_test /dev/video1 640 480 rgb565
 *   ./v4l2_format_test /dev/video1 640 480 yuyv
 *   ./v4l2_format_test /dev/video1 640 480 uyvy
 *
 * 编译: make arm-tools
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

/* ---- FOURCC fallback ---- */
#ifndef V4L2_PIX_FMT_RGB565
#define V4L2_PIX_FMT_RGB565   v4l2_fourcc('R','G','B','P')
#endif
#ifndef V4L2_PIX_FMT_RGB565X
#define V4L2_PIX_FMT_RGB565X  v4l2_fourcc('B','G','R','P')
#endif
#ifndef V4L2_PIX_FMT_UYVY
#define V4L2_PIX_FMT_UYVY     v4l2_fourcc('U','Y','V','Y')
#endif

#define PIX_FMT_BGR565  0x40000001
#define PIX_FMT_BGR565X 0x40000002

/* ENUM_FMT 最多收集数 */
#define MAX_ENUM_FMTS  32

/* ---- 辅助 ---- */
static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;
	for (;;) {
		ret = ioctl(fd, request, arg);
		if (ret == -1 && errno == EINTR)
			continue;
		return ret;
	}
}

static void fourcc_str(uint32_t pixfmt, char *hex, char *fcc)
{
	sprintf(hex, "0x%08X", pixfmt);
	fcc[0] = (char)( pixfmt        & 0xff);
	fcc[1] = (char)((pixfmt >>  8) & 0xff);
	fcc[2] = (char)((pixfmt >> 16) & 0xff);
	fcc[3] = (char)((pixfmt >> 24) & 0xff);
	fcc[4] = '\0';
	for (int i = 0; i < 4; i++)
		if (fcc[i] < 32 || fcc[i] > 126) fcc[i] = '?';
}

static void print_fourcc(const char *label, uint32_t pixfmt)
{
	char hex[16], fcc[8];
	fourcc_str(pixfmt, hex, fcc);
	printf("  %-18s %s (%.4s)\n", label, hex, fcc);
}

static void print_fmt_details(const char *stage, const struct v4l2_pix_format *pix)
{
	printf("--- %s ---\n", stage);
	print_fourcc("pixelformat", pix->pixelformat);
	printf("  width             %u\n", pix->width);
	printf("  height            %u\n", pix->height);
	printf("  bytesperline      %u\n", pix->bytesperline);
	printf("  sizeimage         %u\n", pix->sizeimage);
	printf("\n");
}

static uint32_t name_to_fourcc(const char *name)
{
	if (strcasecmp(name, "rgb565")  == 0) return V4L2_PIX_FMT_RGB565;
	if (strcasecmp(name, "rgb565x") == 0) return V4L2_PIX_FMT_RGB565X;
	if (strcasecmp(name, "bgr565")  == 0) return PIX_FMT_BGR565;
	if (strcasecmp(name, "bgr565x") == 0) return PIX_FMT_BGR565X;
	if (strcasecmp(name, "yuyv")    == 0) return V4L2_PIX_FMT_YUYV;
	if (strcasecmp(name, "uyvy")    == 0) return V4L2_PIX_FMT_UYVY;
	if (strcasecmp(name, "mjpeg")   == 0) return V4L2_PIX_FMT_MJPEG;
	return 0;
}

int main(int argc, char *argv[])
{
	const char *devname;
	int width, height;
	uint32_t req_pixfmt;
	int fd;
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	uint32_t try_pixfmt = 0, s_pixfmt = 0, g_pixfmt = 0;
	int g_fmt_ok = 0, g_fmt_valid = 0;
	int in_enum = 0;                       /* 请求格式是否在 ENUM_FMT 中 */
	uint32_t enum_fmts[MAX_ENUM_FMTS];
	int enum_count = 0;

	if (argc < 5) {
		fprintf(stderr,
			"Usage: %s <dev> <width> <height> <fmt>\n"
			"\n"
			"  fmt: rgb565 | rgb565x | bgr565 | bgr565x | yuyv | uyvy\n"
			"\n"
			"Examples:\n"
			"  %s /dev/video1 640 480 rgb565\n"
			"  %s /dev/video1 640 480 yuyv\n",
			argv[0], argv[0], argv[0]);
		return 1;
	}

	devname    = argv[1];
	width      = atoi(argv[2]);
	height     = atoi(argv[3]);
	req_pixfmt = name_to_fourcc(argv[4]);

	if (req_pixfmt == 0) {
		fprintf(stderr, "[ERROR] Unknown format: %s\n", argv[4]);
		return 1;
	}

	fd = open(devname, O_RDWR | O_NONBLOCK, 0);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	printf("================================================================\n");
	printf("  V4L2 Format Test Tool\n");
	printf("================================================================\n");
	printf("  Device    : %s\n", devname);
	printf("  Request   : %ux%u %s\n", width, height, argv[4]);
	print_fourcc("  Req FOURCC", req_pixfmt);
	printf("================================================================\n\n");

	/* ---- VIDIOC_QUERYCAP ---- */
	memset(&cap, 0, sizeof(cap));
	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		perror("VIDIOC_QUERYCAP");
		close(fd);
		return 1;
	}
	printf("Driver : %s\n", cap.driver);
	printf("Card   : %s\n\n", cap.card);

	/* ---- VIDIOC_ENUM_FMT: 收集驱动真正枚举的格式列表 ---- */
	{
		struct v4l2_fmtdesc fmtdesc;
		printf("--- VIDIOC_ENUM_FMT (driver declared formats) ---\n");
		memset(&fmtdesc, 0, sizeof(fmtdesc));
		fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		for (fmtdesc.index = 0; fmtdesc.index < MAX_ENUM_FMTS; fmtdesc.index++) {
			if (xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) != 0)
				break;
			char hex[16], fcc[8];
			fourcc_str(fmtdesc.pixelformat, hex, fcc);
			printf("  [%u] %s (%.4s)  %s\n",
			       fmtdesc.index, hex, fcc, fmtdesc.description);
			enum_fmts[enum_count++] = fmtdesc.pixelformat;
		}
		printf("  Total: %d format(s) enumerated\n\n", enum_count);

		/* 检查请求格式是否在枚举列表中 */
		for (int i = 0; i < enum_count; i++) {
			if (enum_fmts[i] == req_pixfmt) {
				in_enum = 1;
				break;
			}
		}
		if (!in_enum) {
			printf("  *** WARNING: requested pixfmt '%s' is NOT listed "
			       "by ENUM_FMT.\n"
			       "      S_FMT/TRY_FMT acceptance may be unreliable.\n"
			       "      Sensor/CSI may NOT actually output this format.\n\n",
			       argv[4]);
		}
	}

	/* ---- VIDIOC_TRY_FMT ---- */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width       = (uint32_t)width;
	fmt.fmt.pix.height      = (uint32_t)height;
	fmt.fmt.pix.pixelformat = req_pixfmt;
	fmt.fmt.pix.field       = V4L2_FIELD_ANY;

	if (xioctl(fd, VIDIOC_TRY_FMT, &fmt) < 0) {
		perror("VIDIOC_TRY_FMT");
		fprintf(stderr, "[ERROR] TRY_FMT failed\n");
		close(fd);
		return 1;
	}
	try_pixfmt = fmt.fmt.pix.pixelformat;
	print_fmt_details("VIDIOC_TRY_FMT (driver would accept)", &fmt.fmt.pix);

	/* ---- VIDIOC_S_FMT (真正设置, 这是当前驱动最可靠的格式来源) ---- */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width       = (uint32_t)width;
	fmt.fmt.pix.height      = (uint32_t)height;
	fmt.fmt.pix.pixelformat = req_pixfmt;
	fmt.fmt.pix.field       = V4L2_FIELD_ANY;

	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("VIDIOC_S_FMT");
		fprintf(stderr, "[ERROR] S_FMT failed\n");
		close(fd);
		return 1;
	}
	s_pixfmt = fmt.fmt.pix.pixelformat;
	print_fmt_details("VIDIOC_S_FMT (driver accepted)", &fmt.fmt.pix);

	/* ---- VIDIOC_G_FMT (仅作参考, 当前 mx6s-csi 驱动可能返回无效值) ---- */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
		printf("--- VIDIOC_G_FMT FAILED: errno=%d (%s) ---\n"
		       "    G_FMT is not reliable on this driver; use S_FMT result.\n\n",
		       errno, strerror(errno));
	} else {
		g_fmt_ok = 1;
		g_pixfmt = fmt.fmt.pix.pixelformat;

		/* 检查 G_FMT 返回值是否有效 */
		if (g_pixfmt == 0 || fmt.fmt.pix.bytesperline == 0) {
			printf("--- VIDIOC_G_FMT returned INVALID/UNINITIALIZED format ---\n");
			print_fourcc("pixelformat", g_pixfmt);
			printf("  bytesperline      %u\n", fmt.fmt.pix.bytesperline);
			printf("  sizeimage         %u\n", fmt.fmt.pix.sizeimage);
			printf("  *** WARNING: G_FMT returned invalid/uninitialized format.\n"
			       "      This driver's G_FMT is unreliable.\n"
			       "      Use S_FMT result as the actual format.\n\n");
		} else {
			g_fmt_valid = 1;
			print_fmt_details("VIDIOC_G_FMT (final state)", &fmt.fmt.pix);
		}
	}

	/* ---- SUMMARY ---- */
	printf("================================================================\n");
	printf("  SUMMARY\n");
	printf("================================================================\n");
	print_fourcc("requested",        req_pixfmt);
	print_fourcc("TRY_FMT",          try_pixfmt);
	print_fourcc("S_FMT (RELIABLE)", s_pixfmt);
	if (g_fmt_ok && g_fmt_valid)
		print_fourcc("G_FMT",        g_pixfmt);
	else if (g_fmt_ok)
		printf("  G_FMT              INVALID (pixfmt=0 or bytesperline=0)\n");
	else
		printf("  G_FMT              FAILED (ioctl error)\n");
	printf("\n");

	/* ---- 判断 ---- */
	if (!in_enum && s_pixfmt == req_pixfmt)
		printf("[!] '%s' not in ENUM_FMT but S_FMT accepted it.\n"
		       "    Driver format validation is weak; sensor may NOT\n"
		       "    actually output this format.\n\n",
		       argv[4]);

	if (try_pixfmt != req_pixfmt)
		printf("[!] TRY_FMT changed pixfmt: driver does NOT support '%s' "
		       "natively\n", argv[4]);

	if (s_pixfmt != req_pixfmt)
		printf("[!] S_FMT changed pixfmt: driver substituted a different "
		       "format\n");

	if (!g_fmt_ok || !g_fmt_valid) {
		printf("[!] G_FMT invalid or not implemented correctly by driver.\n"
		       "    Use S_FMT result for this driver.\n"
		       "    Do NOT rely on G_FMT for format decisions.\n");
	} else if (g_pixfmt != s_pixfmt) {
		printf("[!] G_FMT != S_FMT: format may have changed after S_FMT\n");
	} else {
		printf("[OK] G_FMT matches S_FMT: format negotiation stable\n");
	}

	/* ---- BGR565 提示 ---- */
	if (s_pixfmt == V4L2_PIX_FMT_RGB565 && !in_enum)
		printf("\n[NOTE] S_FMT reports RGB565 but '%s' not in ENUM_FMT.\n"
		       "       RGB565 data confirmed by PC-side testing.\n", argv[4]);
	else if (s_pixfmt == V4L2_PIX_FMT_RGB565)
		printf("\n[NOTE] S_FMT reports RGB565. PC-side tests show BGR565_LE "
		       "decoder\n"
		       "       gives correct colors (i.MX6ULL CSI swaps R/B).\n"
		       "       Use: pc_receiver -d --display-format bgr565\n");

	printf("================================================================\n");

	close(fd);
	return 0;
}
