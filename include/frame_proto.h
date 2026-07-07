/*
 * frame_proto.h - TCP图像帧传输协议定义
 *
 * 协议格式:
 *   [frame_header] [raw image data]
 *
 * 所有多字节字段使用网络字节序 (big-endian)，通过 htonl/ntohl 转换。
 */

#ifndef FRAME_PROTO_H
#define FRAME_PROTO_H

#include <stdint.h>

/* 帧魔数 */
#define FRAME_MAGIC     0x12345678
#define FRAME_VERSION   1

/* ================================================================
 * 像素格式定义
 * ================================================================ */

#ifndef V4L2_PIX_FMT_RGB565
#define V4L2_PIX_FMT_RGB565    0x50424752  /* 'R' 'G' 'B' 'P' = RGBP */
#endif
#ifndef V4L2_PIX_FMT_RGB565X
#define V4L2_PIX_FMT_RGB565X   0x50524742
#endif
#ifndef V4L2_PIX_FMT_YUYV
#define V4L2_PIX_FMT_YUYV      0x56595559  /* 'Y' 'U' 'Y' 'V' */
#endif
#ifndef V4L2_PIX_FMT_UYVY
#define V4L2_PIX_FMT_UYVY      0x59565955  /* 'U' 'Y' 'V' 'Y' */
#endif
#ifndef V4L2_PIX_FMT_MJPEG
#define V4L2_PIX_FMT_MJPEG     0x47504A4D  /* 'M' 'J' 'P' 'G' */
#endif

/* 项目内部自定义 FOURCC */
#define PIX_FMT_BGR565         0x40000001  /* BGR565 LE */
#define PIX_FMT_BGR565X        0x40000002  /* BGR565 BE */
#define PIX_FMT_YVYU           0x40000010  /* YVYU 4:2:2 */
#define PIX_FMT_VYUY           0x40000011  /* VYUY 4:2:2 */

/* ================================================================
 * 帧头结构 (40 bytes)
 * ================================================================ */
struct frame_header {
	uint32_t magic;
	uint32_t version;
	uint32_t header_size;
	uint32_t width;
	uint32_t height;
	uint32_t pixfmt;
	uint32_t size;
	uint32_t frame_id;
	uint64_t timestamp_us;
};

/* ================================================================
 * 辅助函数
 * ================================================================ */
static inline const char *pixfmt_to_string(uint32_t pixfmt)
{
	switch (pixfmt) {
	case V4L2_PIX_FMT_RGB565:  return "RGB565_LE";
	case V4L2_PIX_FMT_RGB565X: return "RGB565X_BE";
	case V4L2_PIX_FMT_YUYV:    return "YUYV";
	case V4L2_PIX_FMT_UYVY:    return "UYVY";
	case PIX_FMT_YVYU:         return "YVYU";
	case PIX_FMT_VYUY:         return "VYUY";
	case V4L2_PIX_FMT_MJPEG:   return "MJPEG";
	case PIX_FMT_BGR565:       return "BGR565_LE";
	case PIX_FMT_BGR565X:      return "BGR565X_BE";
	default:                   return "UNKNOWN";
	}
}

static inline const char *pixfmt_to_name(uint32_t pixfmt)
{
	switch (pixfmt) {
	case V4L2_PIX_FMT_RGB565:  return "RGB565";
	case V4L2_PIX_FMT_RGB565X: return "RGB565X";
	case V4L2_PIX_FMT_YUYV:    return "YUYV";
	case V4L2_PIX_FMT_UYVY:    return "UYVY";
	case PIX_FMT_YVYU:         return "YVYU";
	case PIX_FMT_VYUY:         return "VYUY";
	case V4L2_PIX_FMT_MJPEG:   return "MJPEG";
	case PIX_FMT_BGR565:       return "BGR565";
	case PIX_FMT_BGR565X:      return "BGR565X";
	default:                   return "UNKNOWN";
	}
}

static inline const char *pixfmt_to_ext(uint32_t pixfmt)
{
	switch (pixfmt) {
	case V4L2_PIX_FMT_RGB565:
	case V4L2_PIX_FMT_RGB565X:
	case PIX_FMT_BGR565:
	case PIX_FMT_BGR565X:      return "rgb565";
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_UYVY:
	case PIX_FMT_YVYU:
	case PIX_FMT_VYUY:         return "yuv";
	case V4L2_PIX_FMT_MJPEG:   return "mjpg";
	default:                   return "raw";
	}
}

static inline int pixfmt_is_rgb_variant(uint32_t pixfmt)
{
	return (pixfmt == V4L2_PIX_FMT_RGB565  ||
	        pixfmt == V4L2_PIX_FMT_RGB565X ||
	        pixfmt == PIX_FMT_BGR565       ||
	        pixfmt == PIX_FMT_BGR565X);
}

static inline int pixfmt_is_yuv422(uint32_t pixfmt)
{
	return (pixfmt == V4L2_PIX_FMT_YUYV ||
	        pixfmt == V4L2_PIX_FMT_UYVY ||
	        pixfmt == PIX_FMT_YVYU      ||
	        pixfmt == PIX_FMT_VYUY);
}

static inline int pixfmt_is_displayable(uint32_t pixfmt)
{
	return (pixfmt_is_rgb_variant(pixfmt) ||
	        pixfmt_is_yuv422(pixfmt));
}

#endif /* FRAME_PROTO_H */
