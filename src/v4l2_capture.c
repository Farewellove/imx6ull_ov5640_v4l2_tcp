/*
 * v4l2_capture.c - V4L2 MMAP 采集模块实现
 *
 * 实现 xioctl, v4l2_init, v4l2_get_frame, v4l2_put_frame, v4l2_deinit,
 * 和 v4l2_print_capabilities。
 *
 * 关键修改: v4l2_init 中的 S_FMT 之后，始终使用驱动实际返回的 pixfmt，
 * 即使与用户请求的不同 (不再将格式不匹配视为错误)。
 *
 * 兼容 Linux 4.1.15 (i.MX6ULL)。
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <poll.h>
#include <linux/videodev2.h>

#include "v4l2_capture.h"

/* ================================================================
 * xioctl - ioctl wrapper, retry on EINTR
 * ================================================================ */
int xioctl(int fd, int request, void *arg)
{
	int ret;

	for (;;) {
		ret = ioctl(fd, request, arg);
		if (ret == -1 && errno == EINTR)
			continue;
		return ret;
	}
}

/* ================================================================
 * v4l2_init - 完整初始化 V4L2 采集
 *
 * 重要: S_FMT 之后始终使用驱动实际返回的 pixfmt/width/height，
 * 即使与用户请求不同。不再将格式不匹配视为 fatal error。
 * ================================================================ */
int v4l2_init(struct v4l2_context *ctx, const char *devname,
              uint32_t width, uint32_t height, uint32_t pixfmt)
{
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	int i;

	memset(ctx, 0, sizeof(*ctx));
	ctx->fd = -1;

	/* 1. 打开设备 */
	ctx->fd = open(devname, O_RDWR | O_NONBLOCK, 0);
	if (ctx->fd < 0) {
		perror("open");
		fprintf(stderr, "[ERROR] Failed to open %s\n", devname);
		goto fail_fd;
	}
	printf("[INFO] Opened %s (fd=%d)\n", devname, ctx->fd);

	/* 2. QUERYCAP */
	memset(&cap, 0, sizeof(cap));
	if (xioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) < 0) {
		perror("VIDIOC_QUERYCAP");
		fprintf(stderr, "[ERROR] %s is not a V4L2 device\n", devname);
		goto fail;
	}
	printf("[INFO] Driver: %s, Card: %s, Bus: %s\n",
	       cap.driver, cap.card, cap.bus_info);

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
		fprintf(stderr, "[ERROR] %s does not support video capture\n",
		        devname);
		goto fail;
	}
	if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
		fprintf(stderr, "[ERROR] %s does not support streaming I/O\n",
		        devname);
		goto fail;
	}
	printf("[INFO] Device supports capture + streaming\n");

	/* 3. G_FMT (只读初始值) → 设置参数 → S_FMT
	 * NOTE: mx6s-csi 驱动 G_FMT 可能返回 pixelformat=0 / bytesperline=0,
	 * 因此初始 G_FMT 仅用于读取可能的有效值, 不可靠也不必失败.
	 * 格式最终以 S_FMT 返回值为准, 不使用 G_FMT 覆盖.
	 */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (xioctl(ctx->fd, VIDIOC_G_FMT, &fmt) < 0) {
		/* G_FMT 失败不是致命错误, 只 warn */
		printf("[WARN] VIDIOC_G_FMT failed (errno=%d), "
		       "using defaults\n", errno);
	}

	/* 如果 G_FMT 返回无效值, 设置合理默认值 */
	if (fmt.fmt.pix.width == 0 || fmt.fmt.pix.height == 0) {
		fmt.fmt.pix.width  = 640;
		fmt.fmt.pix.height = 480;
	}
	if (fmt.fmt.pix.pixelformat == 0)
		fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;

	/* 用用户请求覆盖 */
	if (width > 0)
		fmt.fmt.pix.width = width;
	if (height > 0)
		fmt.fmt.pix.height = height;
	if (pixfmt != 0)
		fmt.fmt.pix.pixelformat = pixfmt;

	fmt.fmt.pix.field = V4L2_FIELD_ANY;

	printf("[INFO] Requesting: %ux%u, pixfmt=0x%08X (%.4s)\n",
	       fmt.fmt.pix.width, fmt.fmt.pix.height,
	       fmt.fmt.pix.pixelformat, (char *)&fmt.fmt.pix.pixelformat);

	if (xioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("VIDIOC_S_FMT");
		fprintf(stderr, "[ERROR] VIDIOC_S_FMT failed\n");
		goto fail;
	}

	/* ---- 使用 S_FMT 返回值 (当前驱动最可靠的格式来源) ----
	 * 不要再用 G_FMT 覆盖 S_FMT 结果.
	 * mx6s-csi 驱动的 G_FMT 可能返回 pixelformat=0.
	 */
	ctx->width        = fmt.fmt.pix.width;
	ctx->height       = fmt.fmt.pix.height;
	ctx->pixfmt       = fmt.fmt.pix.pixelformat;
	ctx->bytesperline = fmt.fmt.pix.bytesperline;
	ctx->sizeimage    = fmt.fmt.pix.sizeimage;

	/* 详细日志 */
	printf("[INFO] --- S_FMT result (reliable source) ---\n");
	printf("       pixfmt      = 0x%08X (%.4s)\n",
	       ctx->pixfmt, (char *)&ctx->pixfmt);
	printf("       width       = %u\n", ctx->width);
	printf("       height      = %u\n", ctx->height);
	printf("       bytesperline= %u\n", ctx->bytesperline);
	printf("       sizeimage   = %u\n", ctx->sizeimage);

	if (pixfmt != 0 && ctx->pixfmt != pixfmt) {
		printf("[WARN] S_FMT changed pixfmt:\n"
		       "      requested 0x%08X (%.4s)\n"
		       "      actual    0x%08X (%.4s)\n"
		       "      Using S_FMT result (not an error)\n",
		       pixfmt, (char *)&pixfmt,
		       ctx->pixfmt, (char *)&ctx->pixfmt);
	}

	/* 4. REQBUFS */
	memset(&req, 0, sizeof(req));
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	req.count  = 3;

	if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("VIDIOC_REQBUFS");
		fprintf(stderr, "[ERROR] REQBUFS failed\n");
		goto fail;
	}

	ctx->nbufs = req.count;
	if (ctx->nbufs < 1) {
		fprintf(stderr, "[ERROR] REQBUFS returned %d buffers\n",
		        ctx->nbufs);
		goto fail;
	}
	if (ctx->nbufs > V4L2_MAX_BUFFERS) {
		fprintf(stderr, "[WARN] Clamping buffers from %d to %d\n",
		        ctx->nbufs, V4L2_MAX_BUFFERS);
		ctx->nbufs = V4L2_MAX_BUFFERS;
	}
	printf("[INFO] Allocated %d buffer(s)\n", ctx->nbufs);

	/* 5. QUERYBUF + mmap + QBUF */
	for (i = 0; i < ctx->nbufs; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = i;

		if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("VIDIOC_QUERYBUF");
			fprintf(stderr, "[ERROR] QUERYBUF index=%d failed\n", i);
			goto fail_map;
		}

		ctx->buf_len[i] = buf.length;
		ctx->buf_mmap[i] = mmap(NULL, buf.length,
		                        PROT_READ | PROT_WRITE,
		                        MAP_SHARED, ctx->fd, buf.m.offset);

		if (ctx->buf_mmap[i] == MAP_FAILED) {
			perror("mmap");
			fprintf(stderr, "[ERROR] mmap index=%d failed\n", i);
			goto fail_map;
		}

		printf("[INFO] Buffer %d: addr=%p, length=%u\n",
		       i, ctx->buf_mmap[i], buf.length);

		if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
			perror("VIDIOC_QBUF");
			fprintf(stderr, "[ERROR] QBUF index=%d failed\n", i);
			goto fail_map;
		}
	}

	/* 6. STREAMON */
	{
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
			perror("VIDIOC_STREAMON");
			fprintf(stderr, "[ERROR] STREAMON failed\n");
			goto fail_map;
		}
		printf("[INFO] STREAMON success\n");
	}

	printf("[INFO] V4L2 init complete\n");
	return 0;

fail_map:
	for (i = 0; i < ctx->nbufs; i++) {
		if (ctx->buf_mmap[i] != NULL && ctx->buf_mmap[i] != MAP_FAILED) {
			munmap(ctx->buf_mmap[i], ctx->buf_len[i]);
			ctx->buf_mmap[i] = NULL;
		}
	}
fail:
	close(ctx->fd);
	ctx->fd = -1;
fail_fd:
	return -1;
}

/* ================================================================
 * v4l2_get_frame - poll + DQBUF 获取一帧
 * ================================================================ */
int v4l2_get_frame(struct v4l2_context *ctx, void **data, size_t *size,
                   int *index, uint64_t *timestamp, int timeout_ms)
{
	struct pollfd fds[1];
	struct v4l2_buffer buf;
	int ret;

	fds[0].fd     = ctx->fd;
	fds[0].events = POLLIN;

	ret = poll(fds, 1, timeout_ms);
	if (ret < 0) {
		if (errno == EINTR)
			return -EINTR;
		perror("poll");
		return -1;
	}
	if (ret == 0) {
		fprintf(stderr, "[WARN] poll timeout (%d ms)\n", timeout_ms);
		return -ETIMEDOUT;
	}

	if (!(fds[0].revents & POLLIN)) {
		fprintf(stderr, "[WARN] poll revents=0x%04X (no POLLIN)\n",
		        fds[0].revents);
		return -1;
	}

	memset(&buf, 0, sizeof(buf));
	buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;

	ret = xioctl(ctx->fd, VIDIOC_DQBUF, &buf);
	if (ret < 0) {
		if (errno == EAGAIN) {
			fprintf(stderr, "[WARN] DQBUF EAGAIN, will retry\n");
			return -EAGAIN;
		}
		perror("VIDIOC_DQBUF");
		return -1;
	}

	if (buf.index >= (unsigned int)ctx->nbufs) {
		fprintf(stderr, "[ERROR] DQBUF returned invalid index %d\n",
		        buf.index);
		return -1;
	}

	*data      = ctx->buf_mmap[buf.index];
	*size      = buf.bytesused;
	*index     = buf.index;
	*timestamp = buf.timestamp.tv_sec * 1000000ULL + buf.timestamp.tv_usec;

	return 0;
}

/* ================================================================
 * v4l2_put_frame - QBUF
 * ================================================================ */
int v4l2_put_frame(struct v4l2_context *ctx, int index)
{
	struct v4l2_buffer buf;

	memset(&buf, 0, sizeof(buf));
	buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index  = index;

	if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
		perror("VIDIOC_QBUF");
		fprintf(stderr, "[ERROR] QBUF index=%d failed\n", index);
		return -1;
	}
	return 0;
}

/* ================================================================
 * v4l2_deinit - STREAMOFF + munmap + close
 * ================================================================ */
void v4l2_deinit(struct v4l2_context *ctx)
{
	int i;
	enum v4l2_buf_type type;

	if (ctx->fd >= 0) {
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (xioctl(ctx->fd, VIDIOC_STREAMOFF, &type) < 0) {
			perror("VIDIOC_STREAMOFF");
		} else {
			printf("[INFO] STREAMOFF success\n");
		}
	}

	for (i = 0; i < ctx->nbufs; i++) {
		if (ctx->buf_mmap[i] != NULL && ctx->buf_mmap[i] != MAP_FAILED) {
			if (munmap(ctx->buf_mmap[i], ctx->buf_len[i]) < 0) {
				perror("munmap");
			} else {
				printf("[INFO] munmap buffer %d done\n", i);
			}
			ctx->buf_mmap[i] = NULL;
		}
	}

	if (ctx->fd >= 0) {
		if (close(ctx->fd) < 0)
			perror("close");
		printf("[INFO] Device closed\n");
		ctx->fd = -1;
	}
}

/* ================================================================
 * v4l2_print_capabilities - 打印摄像头全部能力
 * ================================================================ */
void v4l2_print_capabilities(int fd)
{
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	struct v4l2_streamparm parm;
	struct v4l2_fmtdesc fmtdesc;
	char pixstr[5];

	/* QUERYCAP */
	memset(&cap, 0, sizeof(cap));
	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		perror("VIDIOC_QUERYCAP");
		return;
	}

	printf("--- VIDIOC_QUERYCAP ---\n");
	printf("  Driver     : %s\n", cap.driver);
	printf("  Card       : %s\n", cap.card);
	printf("  Bus Info   : %s\n", cap.bus_info);
	printf("  Version    : %u.%u.%u\n",
	       (cap.version >> 16) & 0xff,
	       (cap.version >> 8) & 0xff,
	       cap.version & 0xff);

	printf("  Capabilities: 0x%08X\n", cap.capabilities);
	if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)
		printf("    VIDEO_CAPTURE\n");
	if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
		printf("    VIDEO_CAPTURE_MPLANE\n");
	if (cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)
		printf("    VIDEO_OUTPUT\n");
	if (cap.capabilities & V4L2_CAP_STREAMING)
		printf("    STREAMING\n");
	if (cap.capabilities & V4L2_CAP_READWRITE)
		printf("    READWRITE\n");
	if (cap.capabilities & V4L2_CAP_TIMEPERFRAME)
		printf("    TIMEPERFRAME\n");
	if (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE)
		printf("    Device Caps: VIDEO_CAPTURE\n");
	if (cap.device_caps & V4L2_CAP_STREAMING)
		printf("    Device Caps: STREAMING\n");
	printf("\n");

	/* G_FMT */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
		perror("VIDIOC_G_FMT");
	} else {
		printf("--- VIDIOC_G_FMT ---\n");
		printf("  Width        : %u\n", fmt.fmt.pix.width);
		printf("  Height       : %u\n", fmt.fmt.pix.height);
		printf("  Pixel format : 0x%08X (%.4s)\n",
		       fmt.fmt.pix.pixelformat,
		       (char *)&fmt.fmt.pix.pixelformat);
		printf("  Field        : %u\n", fmt.fmt.pix.field);
		printf("  Bytesperline : %u\n", fmt.fmt.pix.bytesperline);
		printf("  Sizeimage    : %u\n", fmt.fmt.pix.sizeimage);
		printf("  Colorspace   : %u\n", fmt.fmt.pix.colorspace);
		printf("\n");
	}

	/* G_PARM */
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
		printf("  Capture mode  : %u\n",
		       parm.parm.capture.capturemode);
		printf("  Timeperframe  : %u/%u (%.2f fps)\n",
		       parm.parm.capture.timeperframe.numerator,
		       parm.parm.capture.timeperframe.denominator,
		       (double)parm.parm.capture.timeperframe.denominator /
		       parm.parm.capture.timeperframe.numerator);
		printf("  Readbuffers   : %u\n",
		       parm.parm.capture.readbuffers);
		printf("\n");
	}

	/* ENUM_FMT + ENUM_FRAMESIZES + ENUM_FRAMEINTERVALS */
	printf("--- VIDIOC_ENUM_FMT ---\n");
	memset(&fmtdesc, 0, sizeof(fmtdesc));
	fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	for (fmtdesc.index = 0; ; fmtdesc.index++) {
		if (xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) != 0)
			break;

		snprintf(pixstr, sizeof(pixstr), "%.4s",
		         (char *)&fmtdesc.pixelformat);
		printf("  [%d] %s (0x%08X)\n",
		       fmtdesc.index, fmtdesc.description, fmtdesc.pixelformat);

		{
			struct v4l2_frmsizeenum fsize;
			memset(&fsize, 0, sizeof(fsize));
			fsize.pixel_format = fmtdesc.pixelformat;

			printf("       Frame sizes:\n");
			for (fsize.index = 0; ; fsize.index++) {
				if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fsize) != 0)
					break;

				if (fsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
					printf("         %ux%u\n",
					       fsize.discrete.width,
					       fsize.discrete.height);

					struct v4l2_frmivalenum fival;
					memset(&fival, 0, sizeof(fival));
					fival.pixel_format = fmtdesc.pixelformat;
					fival.width  = fsize.discrete.width;
					fival.height = fsize.discrete.height;

					printf("           Intervals: ");
					int first = 1;
					for (fival.index = 0; ; fival.index++) {
						if (xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS,
						           &fival) != 0)
							break;
						if (!first) printf(", ");
						first = 0;
						if (fival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
							printf("%.2f fps",
							       1.0 / (fival.discrete.numerator /
							              (double)fival.discrete.denominator));
						} else if (fival.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
							printf("%.2f-%.2f fps",
							       1.0 / (fival.stepwise.max.numerator /
							              (double)fival.stepwise.max.denominator),
							       1.0 / (fival.stepwise.min.numerator /
							              (double)fival.stepwise.min.denominator));
						}
					}
					printf("\n");

				} else if (fsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
					printf("         %ux%u - %ux%u (step %ux%u)\n",
					       fsize.stepwise.min_width,
					       fsize.stepwise.min_height,
					       fsize.stepwise.max_width,
					       fsize.stepwise.max_height,
					       fsize.stepwise.step_width,
					       fsize.stepwise.step_height);
				} else if (fsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
					printf("         %ux%u - %ux%u (continuous)\n",
					       fsize.stepwise.min_width,
					       fsize.stepwise.min_height,
					       fsize.stepwise.max_width,
					       fsize.stepwise.max_height);
				}
			}
		}
	}
	printf("\n");
}
