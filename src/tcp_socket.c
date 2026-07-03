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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "tcp_socket.h"

/* ================================================================
 * send_all - 完整发送 len 字节
 * ================================================================ */
int send_all(int fd, const void *buf, size_t len)
{
    const char *ptr = (const char *)buf;
    size_t remaining = len;
    ssize_t n;

    while (remaining > 0) {
        n = send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("send");
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "[ERROR] send returned 0 (connection closed?)\n");
            return -1;
        }
        ptr += n;
        remaining -= n;
    }
    return 0;
}

/* ================================================================
 * recv_all - 完整接收 len 字节
 * ================================================================ */
int recv_all(int fd, void *buf, size_t len)
{
    char *ptr = (char *)buf;
    size_t remaining = len;
    ssize_t n;

    while (remaining > 0) {
        n = recv(fd, ptr, remaining, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("recv");
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "[ERROR] recv returned 0 (peer disconnected)\n");
            return -1;
        }
        ptr += n;
        remaining -= n;
    }
    return 0;
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
