/*
 * imx6ull_sender.c - i.MX6ULL 板端采集发送程序
 *
 * 功能:
 *   采集 OV5640 摄像头图像，通过 TCP 发送到 PC 端。
 *
 * 用法:
 *   ./imx6ull_sender -d /dev/video1 -s 192.168.1.100 -p 5000 -w 640 -h 480 -f RGB565 -n 0
 *
 * Ctrl+C 优雅退出: STREAMOFF -> munmap -> close socket -> close video fd
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
#include <arpa/inet.h>  /* for htonl/htons/ntohl/ntohs */

#include "frame_proto.h"
#include "v4l2_capture.h"
#include "tcp_socket.h"

/* 全局标志，用于信号处理优雅退出 */
static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
    fprintf(stderr, "\n[INFO] Received signal, exiting...\n");
}

/* 将像素格式名字符串转为 V4L2 常量 */
static uint32_t name_to_pixfmt(const char *name)
{
    if (strcasecmp(name, "RGB565") == 0)
        return V4L2_PIX_FMT_RGB565;
    if (strcasecmp(name, "YUYV") == 0)
        return V4L2_PIX_FMT_YUYV;
    if (strcasecmp(name, "MJPEG") == 0)
        return V4L2_PIX_FMT_MJPEG;
    return 0;
}

/* 发送帧头 (网络字节序) */
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
    net_hdr.timestamp_us = (((uint64_t)htonl((uint32_t)(hdr->timestamp_us >> 32))) << 32)
                           | htonl((uint32_t)(hdr->timestamp_us & 0xffffffff));

    return send_all(sockfd, &net_hdr, sizeof(net_hdr));
}

/* 打印用法 */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -d <dev>      Video device, default /dev/video1\n"
        "  -s <ip>       Server IP address (required)\n"
        "  -p <port>     TCP port, default 5000\n"
        "  -w <width>    Image width, default 640\n"
        "  -h <height>   Image height, default 480\n"
        "  -f <fmt>      Pixel format: RGB565, YUYV, MJPEG (default RGB565)\n"
        "  -n <frames>   Number of frames to send, 0=unlimited (default 0)\n"
        "\n"
        "Example:\n"
        "  %s -d /dev/video1 -s 192.168.1.100 -p 5000 -w 640 -h 480 -f RGB565 -n 0\n",
        prog, prog);
}

int main(int argc, char *argv[])
{
    const char *devname = "/dev/video1";
    const char *server_ip = NULL;
    int port = 5000;
    uint32_t req_width = 640;
    uint32_t req_height = 480;
    uint32_t req_pixfmt = V4L2_PIX_FMT_RGB565;
    int max_frames = 0;  /* 0 = unlimited */
    int opt;

    struct v4l2_context ctx;
    int sockfd = -1;
    uint32_t frame_id = 0;
    int ret;

    /* 解析命令行参数 */
    while ((opt = getopt(argc, argv, "d:s:p:w:h:f:n:")) != -1) {
        switch (opt) {
        case 'd': devname = optarg; break;
        case 's': server_ip = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'w': req_width = (uint32_t)atoi(optarg); break;
        case 'h': req_height = (uint32_t)atoi(optarg); break;
        case 'f':
            req_pixfmt = name_to_pixfmt(optarg);
            if (req_pixfmt == 0) {
                fprintf(stderr, "[ERROR] Unknown pixel format: %s\n", optarg);
                fprintf(stderr, "        Supported: RGB565, YUYV, MJPEG\n");
                return 1;
            }
            break;
        case 'n': max_frames = atoi(optarg); break;
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
    printf("  Device  : %s\n", devname);
    printf("  Server  : %s:%d\n", server_ip, port);
    printf("  Format  : %s (%ux%u)\n",
           pixfmt_to_name(req_pixfmt), req_width, req_height);
    printf("  Frames  : %s\n", max_frames == 0 ? "unlimited" : argv[optind - 1]);
    printf("================================================================\n\n");

    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 1. 初始化 V4L2 采集 */
    printf("[INFO] Initializing V4L2 capture...\n");
    ret = v4l2_init(&ctx, devname, req_width, req_height, req_pixfmt);
    if (ret < 0) {
        fprintf(stderr, "[ERROR] V4L2 init failed\n");
        return 1;
    }
    printf("[INFO] Capture: %ux%u %s, bytesperline=%u, sizeimage=%u\n",
           ctx.width, ctx.height, pixfmt_to_name(ctx.pixfmt),
           ctx.bytesperline, ctx.sizeimage);

    /* 2. 连接 TCP 服务器 */
    sockfd = tcp_client_connect(server_ip, port);
    if (sockfd < 0) {
        fprintf(stderr, "[ERROR] TCP connect failed\n");
        v4l2_deinit(&ctx);
        return 1;
    }

    /* 3. 采集 + 发送循环 */
    printf("[INFO] Starting capture loop (Ctrl+C to stop)...\n");

    while (g_running && (max_frames == 0 || (int)frame_id < max_frames)) {
        void *data = NULL;
        size_t size = 0;
        int buf_idx = -1;
        uint64_t timestamp = 0;
        struct frame_header hdr;

        /* 采集一帧 */
        ret = v4l2_get_frame(&ctx, &data, &size, &buf_idx,
                             &timestamp, 2000);
        if (ret < 0) {
            if (ret == -ETIMEDOUT) {
                /* poll 超时，继续等待 */
                continue;
            }
            if (ret == -EINTR) {
                /* 被信号中断 */
                break;
            }
            if (ret == -EAGAIN) {
                /* DQBUF EAGAIN */
                continue;
            }
            fprintf(stderr, "[ERROR] v4l2_get_frame failed (ret=%d)\n", ret);
            break;
        }

        /* 构造帧头 */
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic        = FRAME_MAGIC;
        hdr.version      = FRAME_VERSION;
        hdr.header_size  = sizeof(hdr);
        hdr.width        = ctx.width;
        hdr.height       = ctx.height;
        hdr.pixfmt       = ctx.pixfmt;
        hdr.size         = (uint32_t)size;
        hdr.frame_id     = frame_id;
        hdr.timestamp_us = timestamp;

        /* 发送帧头 + 帧数据 */
        if (send_frame_header(sockfd, &hdr) < 0) {
            fprintf(stderr, "[ERROR] Failed to send frame header (id=%u)\n",
                    frame_id);
            v4l2_put_frame(&ctx, buf_idx);
            break;
        }

        if (send_all(sockfd, data, size) < 0) {
            fprintf(stderr, "[ERROR] Failed to send frame data (id=%u)\n",
                    frame_id);
            v4l2_put_frame(&ctx, buf_idx);
            break;
        }

        printf("[FRAME %u] size=%zu bytes, ts=%lu us\n",
               frame_id, size, (unsigned long)timestamp);

        frame_id++;

        /* 放回缓冲区 */
        if (v4l2_put_frame(&ctx, buf_idx) < 0) {
            fprintf(stderr, "[ERROR] QBUF failed (index=%d)\n", buf_idx);
            break;
        }
    }

    printf("[INFO] Sent %u frames total\n", frame_id);

    /* 4. 清理 */
    printf("[INFO] Cleaning up...\n");
    if (sockfd >= 0) {
        close(sockfd);
        printf("[INFO] Socket closed\n");
    }
    v4l2_deinit(&ctx);
    printf("[INFO] Done.\n");

    return 0;
}
