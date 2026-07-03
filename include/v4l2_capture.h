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
 * 采集一帧:
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
