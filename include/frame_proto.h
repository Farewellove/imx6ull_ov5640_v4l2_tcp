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

/* 帧魔数，标识数据流边界 */
#define FRAME_MAGIC     0x12345678

/* 协议版本 */
#define FRAME_VERSION   1

/* 像素格式枚举 (复用 V4L2 定义值) */
#ifndef V4L2_PIX_FMT_RGB565
#define V4L2_PIX_FMT_RGB565  0x50424752  /* 'R' 'G' 'B' 'P' (little-endian) = RGBP = RGB565 */
#endif

#ifndef V4L2_PIX_FMT_YUYV
#define V4L2_PIX_FMT_YUYV    0x56595559  /* 'Y' 'U' 'Y' 'V' */
#endif

#ifndef V4L2_PIX_FMT_MJPEG
#define V4L2_PIX_FMT_MJPEG   0x47504A4D  /* 'M' 'J' 'P' 'G' */
#endif

/* 帧头结构 (40 bytes) */
struct frame_header {
    uint32_t magic;         /* 魔数 FRAME_MAGIC */
    uint32_t version;       /* 协议版本 */
    uint32_t header_size;   /* 帧头大小 (sizeof(struct frame_header)) */
    uint32_t width;         /* 图像宽度 */
    uint32_t height;        /* 图像高度 */
    uint32_t pixfmt;        /* 像素格式 (V4L2_PIX_FMT_xxx) */
    uint32_t size;          /* 图像数据长度 (bytes) */
    uint32_t frame_id;      /* 帧序号 (从0递增) */
    uint64_t timestamp_us;  /* 采集时间戳 (微秒) */
};

/* 像素格式名称辅助函数 */
static inline const char *pixfmt_to_name(uint32_t pixfmt)
{
    switch (pixfmt) {
    case V4L2_PIX_FMT_RGB565: return "RGB565";
    case V4L2_PIX_FMT_YUYV:   return "YUYV";
    case V4L2_PIX_FMT_MJPEG:  return "MJPEG";
    default:                   return "UNKNOWN";
    }
}

/* 像素格式文件扩展名 */
static inline const char *pixfmt_to_ext(uint32_t pixfmt)
{
    switch (pixfmt) {
    case V4L2_PIX_FMT_RGB565: return "rgb565";
    case V4L2_PIX_FMT_YUYV:   return "yuyv";
    case V4L2_PIX_FMT_MJPEG:  return "mjpg";
    default:                   return "raw";
    }
}

#endif /* FRAME_PROTO_H */
