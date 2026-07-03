/*
 * pc_receiver.c - PC 端接收保存程序
 *
 * 功能:
 *   作为 TCP server 接收 i.MX6ULL 板端发送的图像帧，并保存到本地文件。
 *   对 RGB565 额外保存一份 PPM 图片，方便 PC 直接查看。
 *
 * 用法:
 *   ./pc_receiver -p 5000 -o ./frames
 *
 *   -p port    TCP 监听端口 (默认 5000)
 *   -o outdir  保存目录 (默认 ./frames)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <arpa/inet.h>  /* for ntohl/ntohs */

#include "frame_proto.h"
#include "tcp_socket.h"

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
    fprintf(stderr, "\n[INFO] Received signal, exiting...\n");
}

/* 接收帧头 (网络字节序转主机序) */
static int recv_frame_header(int fd, struct frame_header *hdr)
{
    struct frame_header net_hdr;
    int ret;

    ret = recv_all(fd, &net_hdr, sizeof(net_hdr));
    if (ret < 0)
        return -1;

    hdr->magic        = ntohl(net_hdr.magic);
    hdr->version      = ntohl(net_hdr.version);
    hdr->header_size  = ntohl(net_hdr.header_size);
    hdr->width        = ntohl(net_hdr.width);
    hdr->height       = ntohl(net_hdr.height);
    hdr->pixfmt       = ntohl(net_hdr.pixfmt);
    hdr->size         = ntohl(net_hdr.size);
    hdr->frame_id     = ntohl(net_hdr.frame_id);
    hdr->timestamp_us =
        (((uint64_t)ntohl((uint32_t)(net_hdr.timestamp_us >> 32))) << 32)
        | ntohl((uint32_t)(net_hdr.timestamp_us & 0xffffffff));

    return 0;
}

/*
 * 将 RGB565 原始数据转换为 PPM (P6 binary) 格式并保存。
 * RGB565: RRRRRGGGGGGBBBBB (16 bits per pixel)
 * PPM:    8 bits per channel (24 bits per pixel)
 */
static int save_rgb565_as_ppm(const char *filename,
                              const uint8_t *rgb565_data,
                              uint32_t width, uint32_t height)
{
    FILE *fp;
    uint32_t x, y;
    uint8_t *ppm_line = NULL;

    fp = fopen(filename, "wb");
    if (!fp) {
        perror("fopen ppm");
        return -1;
    }

    fprintf(fp, "P6\n%u %u\n255\n", width, height);

    /* 分配一行 PPM 数据 */
    ppm_line = (uint8_t *)malloc(width * 3);
    if (!ppm_line) {
        fprintf(stderr, "[ERROR] malloc ppm_line failed\n");
        fclose(fp);
        return -1;
    }

    for (y = 0; y < height; y++) {
        const uint16_t *row = (const uint16_t *)(rgb565_data + y * width * 2);
        uint8_t *dst = ppm_line;

        for (x = 0; x < width; x++) {
            uint16_t pixel = row[x];
            /* RGB565 -> RGB888 */
            uint8_t r = ((pixel >> 11) & 0x1f) << 3;  /* 5 -> 8 bits */
            uint8_t g = ((pixel >> 5)  & 0x3f) << 2;  /* 6 -> 8 bits */
            uint8_t b = (pixel         & 0x1f) << 3;  /* 5 -> 8 bits */

            /* Fill lower bits for smoother gradient */
            r |= (r >> 5);
            g |= (g >> 6);
            b |= (b >> 5);

            *dst++ = r;
            *dst++ = g;
            *dst++ = b;
        }
        fwrite(ppm_line, width * 3, 1, fp);
    }

    free(ppm_line);
    fclose(fp);
    return 0;
}

/* 保存原始帧数据 */
static int save_raw_frame(const char *filename,
                          const uint8_t *data, uint32_t size)
{
    FILE *fp;

    fp = fopen(filename, "wb");
    if (!fp) {
        perror("fopen raw");
        return -1;
    }

    if (fwrite(data, 1, size, fp) != size) {
        perror("fwrite");
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -p <port>     Listen port, default 5000\n"
        "  -o <dir>      Output directory, default ./frames\n"
        "\n"
        "Example:\n"
        "  %s -p 5000 -o ./frames\n",
        prog, prog);
}

int main(int argc, char *argv[])
{
    int port = 5000;
    const char *outdir = "./frames";
    int opt;

    int listen_fd = -1;
    int client_fd = -1;
    uint8_t *frame_buf = NULL;
    uint32_t frame_buf_cap = 0;

    while ((opt = getopt(argc, argv, "p:o:")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'o': outdir = optarg; break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    printf("================================================================\n");
    printf("  PC Frame Receiver\n");
    printf("================================================================\n");
    printf("  Port    : %d\n", port);
    printf("  Output  : %s\n", outdir);
    printf("================================================================\n\n");

    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 创建输出目录 */
    if (mkdir(outdir, 0755) < 0 && errno != EEXIST) {
        perror("mkdir");
        return 1;
    }

    /* 开始监听 */
    listen_fd = tcp_server_listen(port, 1);
    if (listen_fd < 0) {
        fprintf(stderr, "[ERROR] Failed to create server socket\n");
        return 1;
    }

    printf("[INFO] Waiting for connection from i.MX6ULL board...\n");

    client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno != EINTR) {
            perror("accept");
        }
        close(listen_fd);
        return 1;
    }

    printf("[INFO] Client connected (fd=%d)\n", client_fd);
    printf("[INFO] Receiving frames (Ctrl+C to stop)...\n");

    /* 接收帧循环 */
    while (g_running) {
        struct frame_header hdr;
        int ret;
        char filename[512];
        char ppm_filename[512];

        /* 接收帧头 */
        ret = recv_frame_header(client_fd, &hdr);
        if (ret < 0) {
            fprintf(stderr, "[ERROR] Failed to receive frame header\n");
            break;
        }

        /* 检查魔数 */
        if (hdr.magic != FRAME_MAGIC) {
            fprintf(stderr,
                    "[ERROR] Bad magic: 0x%08X (expected 0x%08X)\n",
                    hdr.magic, FRAME_MAGIC);
            break;
        }

        /* 确保接收缓冲区足够大 */
        if (hdr.size > frame_buf_cap) {
            uint8_t *p = (uint8_t *)realloc(frame_buf, hdr.size);
            if (!p) {
                fprintf(stderr,
                        "[ERROR] realloc frame_buf failed (size=%u)\n",
                        hdr.size);
                break;
            }
            frame_buf = p;
            frame_buf_cap = hdr.size;
        }

        /* 接收帧数据 */
        ret = recv_all(client_fd, frame_buf, hdr.size);
        if (ret < 0) {
            fprintf(stderr,
                    "[ERROR] Failed to receive frame data (id=%u)\n",
                    hdr.frame_id);
            break;
        }

        /* 保存帧 */
        const char *ext = pixfmt_to_ext(hdr.pixfmt);
        snprintf(filename, sizeof(filename),
                 "%s/frame_%06u.%s", outdir, hdr.frame_id, ext);

        ret = save_raw_frame(filename, frame_buf, hdr.size);
        if (ret < 0) {
            fprintf(stderr,
                    "[ERROR] Failed to save %s\n", filename);
        } else {
            printf("[FRAME %u] %s (%ux%u, %u bytes, ts=%lu us)\n",
                   hdr.frame_id, filename,
                   hdr.width, hdr.height, hdr.size,
                   (unsigned long)hdr.timestamp_us);
        }

        /* 对 RGB565 额外保存 PPM */
        if (hdr.pixfmt == V4L2_PIX_FMT_RGB565) {
            snprintf(ppm_filename, sizeof(ppm_filename),
                     "%s/frame_%06u.ppm", outdir, hdr.frame_id);
            ret = save_rgb565_as_ppm(ppm_filename, frame_buf,
                                     hdr.width, hdr.height);
            if (ret < 0) {
                fprintf(stderr,
                        "[WARN] Failed to save PPM %s\n", ppm_filename);
            } else {
                printf("  -> PPM: %s\n", ppm_filename);
            }
        }
    }

    /* 清理 */
    printf("[INFO] Cleaning up...\n");
    free(frame_buf);
    if (client_fd >= 0) {
        close(client_fd);
        printf("[INFO] Client socket closed\n");
    }
    if (listen_fd >= 0) {
        close(listen_fd);
        printf("[INFO] Server socket closed\n");
    }
    printf("[INFO] Done.\n");

    return 0;
}
