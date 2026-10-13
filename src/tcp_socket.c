/*
 * tcp_socket.c - TCP Socket 封装实现
 *
 * 提供 send_all/recv_all 保证完整数据传输。
 * 兼容 Linux 4.1.15 和老交叉编译环境。
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "tcp_socket.h"

/* ================================================================
 * send_all - 完整发送 len 字节
 * ================================================================ */
int send_all_stat(int fd, const void *buf, size_t len, struct io_stats *st)
{
    const char *ptr = (const char *)buf;
    size_t remaining = len;
    size_t want;
    ssize_t n;

    while (remaining > 0) {
        want = remaining;
        n = send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (st != NULL)
            st->calls++;
        if (n < 0) {
            if (errno == EINTR) {
                if (st != NULL)
                    st->eintr++;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (st != NULL)
                    st->timeouts++;
                fprintf(stderr, "[ERROR] send timed out (EAGAIN)\n");
                return -1;
            }
            if (st != NULL)
                st->errors++;
            perror("send");
            return -1;
        }
        if (n == 0) {
            if (st != NULL)
                st->errors++;
            fprintf(stderr, "[ERROR] send returned 0 (connection closed?)\n");
            return -1;
        }
        if (st != NULL) {
            st->bytes += (uint64_t)n;
            /* 没有一次发完请求的全部字节 -> 记一次 partial */
            if ((size_t)n < want)
                st->partial++;
        }
        ptr += n;
        remaining -= (size_t)n;
    }
    return 0;
}

int send_all(int fd, const void *buf, size_t len)
{
    return send_all_stat(fd, buf, len, NULL);
}

/* ================================================================
 * recv_all - 完整接收 len 字节
 * ================================================================ */
int recv_all_stat(int fd, void *buf, size_t len, struct io_stats *st)
{
    char *ptr = (char *)buf;
    size_t remaining = len;
    size_t want;
    ssize_t n;

    while (remaining > 0) {
        want = remaining;
        n = recv(fd, ptr, remaining, 0);
        if (st != NULL)
            st->calls++;
        if (n < 0) {
            if (errno == EINTR) {
                if (st != NULL)
                    st->eintr++;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (st != NULL)
                    st->timeouts++;
                fprintf(stderr, "[ERROR] recv timed out (EAGAIN)\n");
                return -1;
            }
            if (st != NULL)
                st->errors++;
            perror("recv");
            return -1;
        }
        if (n == 0) {
            if (st != NULL)
                st->errors++;
            fprintf(stderr, "[ERROR] recv returned 0 (peer disconnected)\n");
            return -1;
        }
        if (st != NULL) {
            st->bytes += (uint64_t)n;
            /* 一次 recv 只拿到部分数据 -> 需要跨调用重组 */
            if ((size_t)n < want)
                st->partial++;
        }
        ptr += n;
        remaining -= (size_t)n;
    }
    return 0;
}

int recv_all(int fd, void *buf, size_t len)
{
    return recv_all_stat(fd, buf, len, NULL);
}

/* ================================================================
 * tcp_client_connect - 创建 TCP 客户端连接
 * ================================================================ */
int tcp_client_connect(const char *ip, int port)
{
    int fd, ret, on = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* 设置 TCP_NODELAY，减少延迟 */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (inet_aton(ip, &addr.sin_addr) == 0) {
        struct hostent *he = gethostbyname(ip);
        if (he == NULL) {
            fprintf(stderr, "[ERROR] Cannot resolve host: %s\n", ip);
            close(fd);
            return -1;
        }
        if (he->h_addrtype != AF_INET) {
            fprintf(stderr, "[ERROR] Host %s is not AF_INET\n", ip);
            close(fd);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }

    printf("[INFO] Connecting to %s:%d ...\n", ip, port);
    ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    printf("[INFO] Connected to %s:%d (fd=%d)\n", ip, port, fd);
    return fd;
}

/* ================================================================
 * tcp_set_timeouts - 设置 SO_RCVTIMEO / SO_SNDTIMEO
 *
 * 宏名在各平台历史上分别是 SO_RCVTIMEO / SO_RCVTIMEO_OLD, 这里做个兜底,
 * 避免在只有旧宏的头文件上编译失败。
 * ================================================================ */
#ifndef SO_RCVTIMEO
#define SO_RCVTIMEO 20        /* Linux 通用值 */
#endif
#ifndef SO_SNDTIMEO
#define SO_SNDTIMEO 21
#endif

int tcp_set_timeouts(int fd, int recv_sec, int send_sec)
{
    int ret = 0;

    if (recv_sec > 0) {
        struct timeval tv;
        tv.tv_sec  = recv_sec;
        tv.tv_usec = 0;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            perror("setsockopt SO_RCVTIMEO");
            ret = -1;
        }
    }
    if (send_sec > 0) {
        struct timeval tv;
        tv.tv_sec  = send_sec;
        tv.tv_usec = 0;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
            perror("setsockopt SO_SNDTIMEO");
            ret = -1;
        }
    }
    return ret;
}

/* ================================================================
 * tcp_server_listen - 创建 TCP 服务器监听
 * ================================================================ */
int tcp_server_listen(int port, int backlog)
{
    int fd, ret, on = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* 允许端口重用 */
    ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (ret < 0) {
        perror("setsockopt SO_REUSEADDR");
        /* 非致命，继续 */
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    ret = listen(fd, backlog);
    if (ret < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    printf("[INFO] Listening on port %d (fd=%d)\n", port, fd);
    return fd;
}
