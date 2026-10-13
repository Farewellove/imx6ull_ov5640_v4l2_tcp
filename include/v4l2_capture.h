/*
 * v4l2_capture.h - V4L2 MMAP 采集模块 API
 *
 * 提供:
 *   - 设备打开/关闭
 *   - 格式协商 (S_FMT, 驱动返回实际参数)
 *   - MMAP 缓冲区申请/映射/释放
 *   - STREAMON/STREAMOFF
 *   - poll + DQBUF 帧获取
 *   - 摄像头能力查询 (信息打印)
 */

#ifndef V4L2_CAPTURE_H
#define V4L2_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include <linux/videodev2.h>

/* 最大支持的缓冲区数量 */
#define V4L2_MAX_BUFFERS    8

/* 默认请求的缓冲数量 (原实现硬编码 3) */
#define V4L2_DEFAULT_BUFFERS 3

/* V4L2 采集上下文 */
struct v4l2_context {
    int fd;                         /* video 设备文件描述符 */
    int nbufs;                      /* 实际申请的 buffer 数量 */
    void *buf_mmap[V4L2_MAX_BUFFERS]; /* 每个 buffer 的 mmap 地址 */
    size_t buf_len[V4L2_MAX_BUFFERS];/* 每个 buffer 的长度 */
    uint32_t width;                 /* 实际采集宽度 */
    uint32_t height;                /* 实际采集高度 */
    uint32_t pixfmt;                /* 实际像素格式 */
    uint32_t bytesperline;          /* 一行字节数 */
    uint32_t sizeimage;             /* 一帧图像大小 */
    uint32_t fps_num;               /* G_PARM 读回的实际 timeperframe.numerator */
    uint32_t fps_den;               /* G_PARM 读回的实际 timeperframe.denominator */
    int      fps_supported;         /* 驱动是否声明 V4L2_CAP_TIMEPERFRAME */
};

/*
 * v4l2_init 的参数集合。
 *
 * 之所以改成结构体而不是继续加位置参数: 后续如果要加更多协商项
 * (如 colorspace、field、裁剪), 加字段不会破坏既有调用点。
 * 各字段为 0 表示"使用默认值/不请求", 因此全零结构体 = 原行为。
 */
struct v4l2_init_params {
    const char *devname;   /* 设备路径, 必填 */
    uint32_t    width;     /* 请求宽度, 0 = 不覆盖 */
    uint32_t    height;    /* 请求高度, 0 = 不覆盖 */
    uint32_t    pixfmt;    /* 请求 FOURCC, 0 = 不覆盖 */
    int         nbufs;     /* REQBUFS 数量, 0 = V4L2_DEFAULT_BUFFERS(3) */
    uint32_t    fps;       /* 请求帧率 (VIDIOC_S_PARM), 0 = 不设置 */
};

/*
 * 封装过的 ioctl，自动处理 EINTR 重试。
 * 失败时打印错误并返回 -1。
 */
int xioctl(int fd, int request, void *arg);

/*
 * 初始化 V4L2 设备:
 *   1. open
 *   2. QUERYCAP
 *   3. S_FMT
 *   4. REQBUFS (count=3)
 *   5. QUERYBUF + mmap
 *   6. QBUF
 *   7. STREAMON
 *
 * 参数:
 *   ctx      - 上下文指针
 *   devname  - 设备路径 "/dev/video1"
 *   width    - 请求宽度 (0 表示不请求)
 *   height   - 请求高度 (0 表示不请求)
 *   pixfmt   - 请求像素格式 (0 表示不请求)
 *
 * 返回: 成功返回 0，失败返回 -1。
 */
int v4l2_init(struct v4l2_context *ctx, const char *devname,
              uint32_t width, uint32_t height, uint32_t pixfmt);

/*
 * 完整版初始化 (支持指定 buffer 数量与目标帧率)。
 *
 * 流程与 v4l2_init 相同, 区别:
 *   - p->nbufs 指定 REQBUFS 数量 (0 = 3), 用于排查"串行采集/发送下
 *     缓冲不足导致丢帧"的问题
 *   - p->fps 在 S_FMT 之后、REQBUFS 之前通过 VIDIOC_S_PARM 请求帧率。
 *     顺序很关键: S_PARM 必须在 S_FMT 之后 (否则格式寄存器可能被覆盖),
 *     且最好在 STREAMON 之前 (流已启动后再改往往不生效)。
 *     驱动不支持 V4L2_CAP_TIMEPERFRAME 时只告警, 不视为失败。
 *
 * 无论是否请求帧率, 都会用 G_PARM 读回实际值填到
 * ctx->fps_num/fps_den/fps_supported, 供调用者打印与记录。
 *
 * 返回: 成功返回 0, 失败返回 -1。
 */
int v4l2_init_ex(struct v4l2_context *ctx,
                 const struct v4l2_init_params *p);

/*
 * 一帧的完整元信息 (v4l2_get_frame_info 的输出)。
 *
 * 比老的 v4l2_get_frame 多带了 sequence 和 flags:
 *   - sequence 由驱动维护, 每出一帧 +1。用户态用它做差就能发现
 *     "驱动侧漏帧"(sequence 跳变), 这是唯一能区分"应用没取到"和
 *     "驱动没产出"的依据。老接口把 sequence 丢掉了, 所以无法判断。
 *   - flags 用于识别 V4L2_BUF_FLAG_ERROR (该缓冲内容不可信)。
 */
struct v4l2_frame {
	void    *data;         /* mmap 地址 (对应 DQBUF 的 index) */
	size_t   bytesused;    /* 本帧有效字节数 (来自 DQBUF) */
	size_t   buf_len;      /* 该 buffer 的映射长度 (mapped_length) */
	int      index;        /* buffer index, QBUF 时回传 */
	uint32_t sequence;     /* 驱动帧序号 */
	uint32_t flags;        /* V4L2_BUF_FLAG_* */
	uint64_t timestamp_us; /* 驱动时间戳 (微秒) */
};

/*
 * 采集一帧并返回完整元信息:
 *   1. poll 等待帧就绪
 *   2. DQBUF 取出帧
 *   3. 校验 index 合法、bytesused <= buf_len
 *
 * 返回: 成功返回 0，poll 超时返回 -ETIMEDOUT，暂无数据返回 -EAGAIN，
 *       被信号打断返回 -EINTR，其他错误返回 -1。
 */
int v4l2_get_frame_info(struct v4l2_context *ctx, struct v4l2_frame *fr,
                        int timeout_ms);

/*
 * 采集一帧 (兼容旧接口):
 *   1. poll 等待帧就绪
 *   2. DQBUF 取出帧
 *
 * 参数:
 *   ctx        - 上下文指针
 *   data       - 输出: 指向帧数据的指针
 *   size       - 输出: 帧数据大小
 *   index      - 输出: buffer index (QBUF 时需要)
 *   timestamp  - 输出: 帧时间戳 (微秒)
 *   timeout_ms - poll 超时 (ms)
 *
 * 返回: 成功返回 0，超时返回 -ETIMEDOUT，错误返回 -1。
 *   (内部直接调用 v4l2_get_frame_info, 行为与旧版一致)
 */
int v4l2_get_frame(struct v4l2_context *ctx, void **data, size_t *size,
                   int *index, uint64_t *timestamp, int timeout_ms);

/*
 * 放回缓冲区 (QBUF):
 *   调用者必须在 v4l2_get_frame 成功后调用本函数将 buffer 重新入队。
 */
int v4l2_put_frame(struct v4l2_context *ctx, int index);

/*
 * 关闭 V4L2 设备:
 *   1. STREAMOFF
 *   2. munmap
 *   3. close
 */
void v4l2_deinit(struct v4l2_context *ctx);

/*
 * 查询并打印摄像头全部能力信息:
 *   - QUERYCAP
 *   - G_FMT
 *   - G_PARM
 *   - ENUM_FMT + ENUM_FRAMESIZES + ENUM_FRAMEINTERVALS
 */
void v4l2_print_capabilities(int fd);

#endif /* V4L2_CAPTURE_H */
