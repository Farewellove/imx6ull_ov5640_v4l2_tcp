/*
 * frame_proto.h - TCP图像帧传输协议定义
 *
 * 协议格式:
 *   [frame_header] [raw image data]
 *
 * 一帧 = 定长 40 字节帧头 + 紧跟其后的 size 字节原始图像数据。
 * 图像数据不做任何压缩/转换, 采集到什么样就发什么样 (MJPEG 除外,
 * 它本身在驱动侧就是压缩的)。PC 端拿到后按帧头里的 pixfmt 决定
 * 怎么解释这些字节。
 *
 * 所有多字节字段使用网络字节序 (big-endian)，通过 htonl/ntohl 转换。
 * 发送端和接收端共用本头文件, 保证结构体布局和 FOURCC 定义一致。
 */

#ifndef FRAME_PROTO_H
#define FRAME_PROTO_H

#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>

/* 帧魔数: 收到帧头时先校验这个, 能在字节流错位时立刻发现 */
#define FRAME_MAGIC     0x12345678
/*
 * 协议版本。
 *
 * 版本 2 的改动: 修正 timestamp_us 的线上编码。
 *   版本 1 用 `(htonl(hi) << 32) | htonl(lo)` 的整数拼接来"序列化" 64 位
 *   字段 —— 在小端主机上落盘字节是 [BE(lo)][BE(hi)], 在大端主机上却是
 *   [BE(hi)][BE(lo)], 即同一份代码在不同字节序平台上产生不同线格式,
 *   不符合"网络字节序"约定。实测字节对比 (ts = 0x00000001FFFFFFFF):
 *       旧写法: FF FF FF FF 00 00 00 01
 *       新写法: 00 00 00 01 FF FF FF FF
 *   版本 2 改为逐字节写出高 32 位、再写低 32 位, 与主机字节序无关。
 *
 * 因此版本 1 与版本 2 的收发端不能混用: 新接收端会把版本 1 的帧判为
 * bad_version 并明确报错, 而不是静默解析出错误的微秒时间戳。
 * 升级时必须同时重新编译并部署板端与 PC 端。
 */
#define FRAME_VERSION   2

/* ================================================================
 * 像素格式定义 (FOURCC)
 *
 * 值就是 4 个字符按小端堆进一个 u32: 'Y''U''Y''V' → 0x56595559。
 * 下面用 #ifndef 包住是因为老内核 (4.1.15) 的 videodev2.h 里可能
 * 没有这些定义, 缺了就自己补, 保证 include 本文件即可编译。
 * ================================================================ */

#ifndef V4L2_PIX_FMT_RGB565
#define V4L2_PIX_FMT_RGB565    0x50424752  /* 'R' 'G' 'B' 'P' = RGBP */
#endif
#ifndef V4L2_PIX_FMT_RGB565X
#define V4L2_PIX_FMT_RGB565X   0x50524742  /* 'B' 'G' 'R' 'P', 高字节在前 */
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

/*
 * 项目内部自定义 FOURCC (0x40000001 起的保留段)
 *
 * 这几个值只在 PC 端解码时当"标签"用, 绝不会传给 V4L2 ioctl ——
 * 驱动不认识它们。用途是描述标准 FOURCC 表达不了的差异:
 *   BGR565/BGR565X: i.MX6ULL CSI 通路 R/B 交换, 驱动报 RGB565
 *                   但实际数据按 BGR 排, 需要一个名字来标定
 *   YVYU/VYUY:      老内核头文件没有这两个 YUV422 排列的定义
 */
#define PIX_FMT_BGR565         0x40000001  /* BGR565 LE */
#define PIX_FMT_BGR565X        0x40000002  /* BGR565 BE */
#define PIX_FMT_YVYU           0x40000010  /* YVYU 4:2:2 */
#define PIX_FMT_VYUY           0x40000011  /* VYUY 4:2:2 */

/* ================================================================
 * 帧头结构 (40 bytes)
 *
 * 发送端: 填好本机字节序的值 → 逐个 htonl → send_all
 * 接收端: recv_all 到临时结构 → 逐个 ntohl → 使用
 *
 * 注意 struct 自身不做 packing, 这里字段全是 4 字节对齐的,
 * 在 ARM 和 x86 上布局一致 (8 + 8*4 = 40), 无 padding 隐患;
 * 但若以后新增字段, 必须留意对齐差异。
 * ================================================================ */
struct frame_header {
	uint32_t magic;        /* FRAME_MAGIC, 用于流同步校验 */
	uint32_t version;      /* FRAME_VERSION */
	uint32_t header_size;  /* 本结构大小 (sizeof), 供将来扩展协议 */
	uint32_t width;        /* 图像宽 (像素) */
	uint32_t height;       /* 图像高 (像素) */
	uint32_t pixfmt;       /* 像素格式 FOURCC, 决定 PC 端如何解码 */
	uint32_t size;         /* 后续图像数据的字节数 */
	uint32_t frame_id;     /* 帧序号, 从 0 递增, 用于发现丢帧 */
	uint64_t timestamp_us; /* 采集时间戳 (微秒), 来源 V4L2 buffer timestamp */
};

/* ================================================================
 * 序列化 / 反序列化 (单一实现, 收发两端共用)
 *
 * 放在头文件里的理由: 之前发送端和接收端各写了一份 htonl/ntohl 代码,
 * 一旦两边不一致就会得到"能收帧但时间戳乱掉"这种很难查的问题。共用
 * 一份实现可以从结构上排除这种分歧 —— 字节序只在这里定义一次。
 * ================================================================ */

/* 本机序 → 网络序 (写入 n, 不修改 h) */
static inline void frame_header_hton(const struct frame_header *h,
                                     struct frame_header *n)
{
	uint8_t *ts = (uint8_t *)&n->timestamp_us;
	uint32_t hi = (uint32_t)(h->timestamp_us >> 32);
	uint32_t lo = (uint32_t)(h->timestamp_us & 0xffffffffu);

	n->magic       = htonl(h->magic);
	n->version     = htonl(h->version);
	n->header_size = htonl(h->header_size);
	n->width       = htonl(h->width);
	n->height      = htonl(h->height);
	n->pixfmt      = htonl(h->pixfmt);
	n->size        = htonl(h->size);
	n->frame_id    = htonl(h->frame_id);

	/* 64 位字段显式按字节写大端: 高 32 位在前, 低 32 位在后。
	 * 不用 (htonl(hi)<<32)|htonl(lo) 的整数拼接 —— 那在不同字节序
	 * 主机上会得到不同的内存字节序列 (这就是版本 1 的缺陷)。 */
	ts[0] = (uint8_t)(hi >> 24);
	ts[1] = (uint8_t)(hi >> 16);
	ts[2] = (uint8_t)(hi >> 8);
	ts[3] = (uint8_t)(hi);
	ts[4] = (uint8_t)(lo >> 24);
	ts[5] = (uint8_t)(lo >> 16);
	ts[6] = (uint8_t)(lo >> 8);
	ts[7] = (uint8_t)(lo);
}

/* 网络序 → 本机序 (从 n 读出到 h) */
static inline void frame_header_ntoh(const struct frame_header *n,
                                     struct frame_header *h)
{
	const uint8_t *ts = (const uint8_t *)&n->timestamp_us;

	h->magic       = ntohl(n->magic);
	h->version     = ntohl(n->version);
	h->header_size = ntohl(n->header_size);
	h->width       = ntohl(n->width);
	h->height      = ntohl(n->height);
	h->pixfmt      = ntohl(n->pixfmt);
	h->size        = ntohl(n->size);
	h->frame_id    = ntohl(n->frame_id);

	h->timestamp_us =
		((uint64_t)ts[0] << 56) | ((uint64_t)ts[1] << 48) |
		((uint64_t)ts[2] << 40) | ((uint64_t)ts[3] << 32) |
		((uint64_t)ts[4] << 24) | ((uint64_t)ts[5] << 16) |
		((uint64_t)ts[6] << 8)  | ((uint64_t)ts[7]);
}

/* ================================================================
 * 辅助函数 (全部 static inline, 只在本头文件里给打印/判断用)
 *
 * 两套名字看着重复, 区别在用途:
 *   pixfmt_to_string - 带字节序后缀 (RGB565_LE), 打日志用, 要精确
 *   pixfmt_to_name   - 不带后缀 (RGB565),   SDL 窗口标题用, 要短
 * ================================================================ */

/* FOURCC → 带字节序后缀的可读名, 用于终端日志 */
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

/* FOURCC → 不带后缀的短名, 用于 SDL 窗口标题等窄空间 */
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

/* FOURCC → 落盘文件名扩展名 (.rgb565/.yuv/.mjpg/.raw), 只图一眼能认出来 */
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

/* 是否为 16bpp RGB565 家族 (含 R/B 交换的两个变体) */
static inline int pixfmt_is_rgb_variant(uint32_t pixfmt)
{
	return (pixfmt == V4L2_PIX_FMT_RGB565  ||
	        pixfmt == V4L2_PIX_FMT_RGB565X ||
	        pixfmt == PIX_FMT_BGR565       ||
	        pixfmt == PIX_FMT_BGR565X);
}

/* 是否为 YUV 4:2:2 家族 (四种字节排列都算)。--yuv-order 只对它生效 */
static inline int pixfmt_is_yuv422(uint32_t pixfmt)
{
	return (pixfmt == V4L2_PIX_FMT_YUYV ||
	        pixfmt == V4L2_PIX_FMT_UYVY ||
	        pixfmt == PIX_FMT_YVYU      ||
	        pixfmt == PIX_FMT_VYUY);
}

/*
 * 本项目的解码器能否处理该格式。
 * 即 565 家族 + YUV422 家族 —— MJPEG 需要额外的 JPEG 解码库,
 * 不在当前实现范围内, 所以它返回 0 (只落盘不显示)。
 */
static inline int pixfmt_is_displayable(uint32_t pixfmt)
{
	return (pixfmt_is_rgb_variant(pixfmt) ||
	        pixfmt_is_yuv422(pixfmt));
}

#endif /* FRAME_PROTO_H */
