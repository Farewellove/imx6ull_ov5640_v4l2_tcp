/*
 * tools/v4l2_info.c — 独立 V4L2 摄像头能力查询工具
 *
 * 用法: ./v4l2_info /dev/video1
 *
 * 打印:
 *   VIDIOC_QUERYCAP  (含 device_caps 判断)
 *   VIDIOC_G_FMT     (含 FOURCC string+hex, bytesperline, sizeimage, colorspace, field)
 *   VIDIOC_G_PARM
 *   VIDIOC_ENUM_FMT + ENUM_FRAMESIZES + ENUM_FRAMEINTERVALS
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

/* ================================================================
 * 辅助宏 / 函数
 * ================================================================ */

/* 如果内核头文件缺少某些 FOURCC, 自行定义 */
#ifndef V4L2_PIX_FMT_RGB565
#define V4L2_PIX_FMT_RGB565   v4l2_fourcc('R','G','B','P')
#endif
#ifndef V4L2_PIX_FMT_RGB565X
#define V4L2_PIX_FMT_RGB565X  v4l2_fourcc('B','G','R','P')
#endif
#ifndef V4L2_PIX_FMT_UYVY
#define V4L2_PIX_FMT_UYVY     v4l2_fourcc('U','Y','V','Y')
#endif

#ifndef V4L2_CAP_DEVICE_CAPS
#define V4L2_CAP_DEVICE_CAPS  0x80000000
#endif

/* 老内核 (4.1.15) 可能缺少部分 colorspace 定义 */
#ifndef V4L2_COLORSPACE_DEFAULT
#define V4L2_COLORSPACE_DEFAULT    0
#endif
#ifndef V4L2_COLORSPACE_SMPTE170M
#define V4L2_COLORSPACE_SMPTE170M  1
#endif
#ifndef V4L2_COLORSPACE_SMPTE240M
#define V4L2_COLORSPACE_SMPTE240M  2
#endif
#ifndef V4L2_COLORSPACE_REC709
#define V4L2_COLORSPACE_REC709     3
#endif
#ifndef V4L2_COLORSPACE_BT878
#define V4L2_COLORSPACE_BT878      4
#endif
#ifndef V4L2_COLORSPACE_470_SYSTEM_M
#define V4L2_COLORSPACE_470_SYSTEM_M  5
#endif
#ifndef V4L2_COLORSPACE_470_SYSTEM_BG
#define V4L2_COLORSPACE_470_SYSTEM_BG 6
#endif
#ifndef V4L2_COLORSPACE_JPEG
#define V4L2_COLORSPACE_JPEG       7
#endif
#ifndef V4L2_COLORSPACE_SRGB
#define V4L2_COLORSPACE_SRGB       8
#endif

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

/* FOURCC → 人类可读: 同时返回 hex 字符串和 FOURCC 字符 */
static void fourcc_to_str(uint32_t pixfmt, char *hex, char *fourcc)
{
	sprintf(hex, "0x%08X", pixfmt);
	fourcc[0] = (char)( pixfmt        & 0xff);
	fourcc[1] = (char)((pixfmt >>  8) & 0xff);
	fourcc[2] = (char)((pixfmt >> 16) & 0xff);
	fourcc[3] = (char)((pixfmt >> 24) & 0xff);
	fourcc[4] = '\0';
	/* 确保可打印 */
	for (int i = 0; i < 4; i++)
		if (fourcc[i] < 32 || fourcc[i] > 126)
			fourcc[i] = '?';
}

static const char *field_to_str(uint32_t field)
{
	switch (field) {
	case V4L2_FIELD_ANY:      return "ANY";
	case V4L2_FIELD_NONE:     return "NONE";
	case V4L2_FIELD_TOP:      return "TOP";
	case V4L2_FIELD_BOTTOM:   return "BOTTOM";
	case V4L2_FIELD_INTERLACED: return "INTERLACED";
	default:                  return "UNKNOWN";
	}
}

static const char *colorspace_to_str(uint32_t cs)
{
	switch (cs) {
	case V4L2_COLORSPACE_DEFAULT:   return "DEFAULT";
	case V4L2_COLORSPACE_SMPTE170M: return "SMPTE170M";
	case V4L2_COLORSPACE_SMPTE240M: return "SMPTE240M";
	case V4L2_COLORSPACE_REC709:    return "REC709";
	case V4L2_COLORSPACE_BT878:     return "BT878";
	case V4L2_COLORSPACE_470_SYSTEM_M:  return "470_SYSTEM_M";
	case V4L2_COLORSPACE_470_SYSTEM_BG: return "470_SYSTEM_BG";
	case V4L2_COLORSPACE_JPEG:      return "JPEG";
	case V4L2_COLORSPACE_SRGB:      return "SRGB";
	default: return "?";
	}
}

/* 打印 capabilities 位掩码 */
static void print_caps(uint32_t caps, const char *prefix)
{
	printf("%s  0x%08X\n", prefix, caps);
	if (caps & V4L2_CAP_VIDEO_CAPTURE)        printf("%s    VIDEO_CAPTURE\n", prefix);
	if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) printf("%s    VIDEO_CAPTURE_MPLANE\n", prefix);
	if (caps & V4L2_CAP_VIDEO_OUTPUT)         printf("%s    VIDEO_OUTPUT\n", prefix);
	if (caps & V4L2_CAP_VIDEO_OUTPUT_MPLANE)  printf("%s    VIDEO_OUTPUT_MPLANE\n", prefix);
	if (caps & V4L2_CAP_STREAMING)            printf("%s    STREAMING\n", prefix);
	if (caps & V4L2_CAP_READWRITE)            printf("%s    READWRITE\n", prefix);
	if (caps & V4L2_CAP_TIMEPERFRAME)         printf("%s    TIMEPERFRAME\n", prefix);
	if (caps & V4L2_CAP_DEVICE_CAPS)          printf("%s    DEVICE_CAPS\n", prefix);
}

int main(int argc, char *argv[])
{
	const char *devname;
	int fd;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <video_device>\n", argv[0]);
		fprintf(stderr, "Example: %s /dev/video1\n", argv[0]);
		return 1;
	}
	devname = argv[1];

	fd = open(devname, O_RDWR | O_NONBLOCK, 0);
	if (fd < 0) {
		perror("open");
		fprintf(stderr, "[ERROR] Failed to open %s\n", devname);
		return 1;
	}

	printf("================================================================\n");
	printf("  V4L2 Camera Information Tool\n");
	printf("  Device: %s\n", devname);
	printf("================================================================\n\n");

	/* ---- VIDIOC_QUERYCAP ---- */
	{
		struct v4l2_capability cap;
		memset(&cap, 0, sizeof(cap));
		if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
			perror("VIDIOC_QUERYCAP");
			goto out;
		}

		printf("--- VIDIOC_QUERYCAP ---\n");
		printf("  Driver      : %s\n", cap.driver);
		printf("  Card        : %s\n", cap.card);
		printf("  Bus Info    : %s\n", cap.bus_info);
		printf("  Version     : %u.%u.%u\n",
		       (cap.version >> 16) & 0xff,
		       (cap.version >> 8) & 0xff,
		       cap.version & 0xff);

		print_caps(cap.capabilities, "  Capabilities:");

		if (cap.capabilities & V4L2_CAP_DEVICE_CAPS) {
			print_caps(cap.device_caps, "  Device Caps:");
		} else {
			printf("  NOTE: device_caps not supported (kernel < 3.3?), "
			       "value may be invalid\n");
		}
		printf("\n");
	}

	/* ---- VIDIOC_G_FMT ---- */
	{
		struct v4l2_format fmt;
		char hex[16], fourcc[8];

		memset(&fmt, 0, sizeof(fmt));
		fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
			perror("VIDIOC_G_FMT");
		} else {
			fourcc_to_str(fmt.fmt.pix.pixelformat, hex, fourcc);

			printf("--- VIDIOC_G_FMT ---\n");
			printf("  Width        : %u\n", fmt.fmt.pix.width);
			printf("  Height       : %u\n", fmt.fmt.pix.height);
			printf("  Pixel Format : %s (%.4s)\n", hex, fourcc);
			printf("  Field        : %s (%u)\n",
			       field_to_str(fmt.fmt.pix.field),
			       fmt.fmt.pix.field);
			printf("  BytesPerLine : %u\n", fmt.fmt.pix.bytesperline);
			printf("  SizeImage    : %u\n", fmt.fmt.pix.sizeimage);
			printf("  ColorSpace   : %s (%u)\n",
			       colorspace_to_str(fmt.fmt.pix.colorspace),
			       fmt.fmt.pix.colorspace);
			printf("\n");
		}
	}

	/* ---- VIDIOC_G_PARM ---- */
	{
		struct v4l2_streamparm parm;
		memset(&parm, 0, sizeof(parm));
		parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (xioctl(fd, VIDIOC_G_PARM, &parm) < 0) {
			perror("VIDIOC_G_PARM");
		} else {
			printf("--- VIDIOC_G_PARM ---\n");
			printf("  Capability    : 0x%08X\n",
			       parm.parm.capture.capability);
			if (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)
				printf("    TIMEPERFRAME supported\n");
			printf("  CaptureMode   : %u\n",
			       parm.parm.capture.capturemode);
			if (parm.parm.capture.timeperframe.denominator > 0)
				printf("  TimePerFrame  : %u/%u (%.2f fps)\n",
				       parm.parm.capture.timeperframe.numerator,
				       parm.parm.capture.timeperframe.denominator,
				       (double)parm.parm.capture.timeperframe.denominator /
				       parm.parm.capture.timeperframe.numerator);
			else
				printf("  TimePerFrame  : %u/%u\n",
				       parm.parm.capture.timeperframe.numerator,
				       parm.parm.capture.timeperframe.denominator);
			printf("  ReadBuffers   : %u\n",
			       parm.parm.capture.readbuffers);
			printf("\n");
		}
	}

	/* ---- VIDIOC_ENUM_FMT ---- */
	{
		struct v4l2_fmtdesc fmtdesc;
		char hex[16], fourcc[8];

		printf("--- VIDIOC_ENUM_FMT ---\n");
		memset(&fmtdesc, 0, sizeof(fmtdesc));
		fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		for (fmtdesc.index = 0; ; fmtdesc.index++) {
			if (xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) != 0)
				break;

			fourcc_to_str(fmtdesc.pixelformat, hex, fourcc);
			printf("  [%u] %s (%.4s) %s\n",
			       fmtdesc.index, hex, fourcc,
			       fmtdesc.description);

			/* ENUM_FRAMESIZES */
			{
				struct v4l2_frmsizeenum fsize;
				memset(&fsize, 0, sizeof(fsize));
				fsize.pixel_format = fmtdesc.pixelformat;

				for (fsize.index = 0; ; fsize.index++) {
					if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fsize) != 0)
						break;

					if (fsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
						printf("       %ux%u",
						       fsize.discrete.width,
						       fsize.discrete.height);

						/* ENUM_FRAMEINTERVALS */
						{
							struct v4l2_frmivalenum fival;
							memset(&fival, 0, sizeof(fival));
							fival.pixel_format = fmtdesc.pixelformat;
							fival.width  = fsize.discrete.width;
							fival.height = fsize.discrete.height;

							printf("  intervals: ");
							int first = 1;
							for (fival.index = 0; ; fival.index++) {
								if (xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fival) != 0)
									break;
								if (!first) printf(", ");
								first = 0;
								if (fival.type == V4L2_FRMIVAL_TYPE_DISCRETE)
									printf("%.2f fps",
									       (double)fival.discrete.denominator /
									       fival.discrete.numerator);
								else if (fival.type == V4L2_FRMIVAL_TYPE_STEPWISE)
									printf("%.2f-%.2f fps",
									       (double)fival.stepwise.min.denominator /
									       fival.stepwise.min.numerator,
									       (double)fival.stepwise.max.denominator /
									       fival.stepwise.max.numerator);
							}
						}
						printf("\n");

					} else if (fsize.type == V4L2_FRMSIZE_TYPE_STEPWISE ||
					           fsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
						const char *t = (fsize.type == V4L2_FRMSIZE_TYPE_STEPWISE)
							? "stepwise" : "continuous";
						printf("       %ux%u - %ux%u (%s)\n",
						       fsize.stepwise.min_width,
						       fsize.stepwise.min_height,
						       fsize.stepwise.max_width,
						       fsize.stepwise.max_height, t);
						break; /* only one entry for stepwise/continuous */
					}
				}
			}
		}
		printf("\n");
	}

out:
	close(fd);
	return 0;
}
