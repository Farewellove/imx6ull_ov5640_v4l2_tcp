/*
 * tcp_socket.h - TCP Socket 封装
 *
 * 提供:
 *   - send_all / recv_all (保证完整发送和接收)
 *   - TCP 客户端连接
 *   - TCP 服务器监听
 */

#ifndef TCP_SOCKET_H
#define TCP_SOCKET_H

#include <stddef.h>
#include <stdint.h>

/*
 * 发送 len 字节数据，保证全部发送完毕。
 * 遇到信号中断自动重试。
 * 返回: 成功返回 0，失败返回 -1。
 */
int send_all(int fd, const void *buf, size_t len);

/*
 * 接收 len 字节数据，保证全部接收完毕。
 * 遇到信号中断自动重试。
 * 返回: 成功返回 0，失败返回 -1。
 */
int recv_all(int fd, void *buf, size_t len);

/*
 * 创建 TCP 客户端并连接到服务器。
 * 参数:
 *   ip   - 服务器 IP 地址字符串
 *   port - 服务器端口 (host byte order)
 * 返回: 成功返回 socket fd，失败返回 -1。
 */
int tcp_client_connect(const char *ip, int port);

/*
 * 创建 TCP 服务器并监听。
 * 参数:
 *   port    - 监听端口 (host byte order)
 *   backlog - 连接队列长度
 * 返回: 成功返回 listen fd，失败返回 -1。
 */
int tcp_server_listen(int port, int backlog);

#endif /* TCP_SOCKET_H */
