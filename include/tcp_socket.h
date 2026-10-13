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
 * 单次"完整收发"调用的统计信息。
 *
 * TCP 是字节流, send/recv 一次调用不保证处理完 len 字节, 因此代码里用
 * 循环补齐 (send_all/recv_all)。这里的计数就是用来量化这种"短收发":
 *   calls   = 循环里实际调用 send/recv 的次数
 *   partial = 返回值 < 请求长度 的次数 (即被迫分多次的帧/次数)
 *   bytes   = 累计成功字节数
 *   eintr   = 被信号中断重试的次数
 *   errors  = 出错退出次数
 *
 * 一次 send_all(len) 如果 calls == 1 && partial == 0, 说明内核一次性
 * 收下了全部数据; calls > 1 说明发生了分片/阻塞后继续, 这正是"TCP 没有
 * 消息边界"的直接证据, 也是接收端必须做重组的原因。
 */
struct io_stats {
	uint64_t calls;
	uint64_t partial;
	uint64_t bytes;
	uint64_t eintr;
	uint64_t timeouts;   /* 因 SO_RCVTIMEO/SO_SNDTIMEO 到期而 EAGAIN */
	uint64_t errors;
};

/*
 * 发送 len 字节数据，保证全部发送完毕。
 * 遇到信号中断自动重试。
 * 返回: 成功返回 0，失败返回 -1。
 */
int send_all(int fd, const void *buf, size_t len);

/*
 * 与 send_all 等价, 但额外把本次收发的分片情况累加到 st (st 可为 NULL,
 * 此时行为与 send_all 完全相同)。
 */
int send_all_stat(int fd, const void *buf, size_t len, struct io_stats *st);

/*
 * 接收 len 字节数据，保证全部接收完毕。
 * 遇到信号中断自动重试。
 * 返回: 成功返回 0，失败返回 -1。
 */
int recv_all(int fd, void *buf, size_t len);

/*
 * 与 recv_all 等价, 但额外统计本次重组的分片情况 (st 可为 NULL)。
 */
int recv_all_stat(int fd, void *buf, size_t len, struct io_stats *st);

/*
 * 创建 TCP 客户端并连接到服务器。
 * 参数:
 *   ip   - 服务器 IP 地址字符串
 *   port - 服务器端口 (host byte order)
 * 返回: 成功返回 socket fd，失败返回 -1。
 */
int tcp_client_connect(const char *ip, int port);

/*
 * 设置收发超时 (秒)。0 表示不设该方向超时 (保持阻塞语义)。
 *
 * 用途: 自动化测试里必须避免"对端挂起 -> 本端永久阻塞"。设了
 * SO_RCVTIMEO 之后, recv 到期返回 -1/EAGAIN, 调用方就能主动结束,
 * 而不是把整个测试脚本一起拖死。
 *
 * 返回: 0 成功, -1 失败。
 */
int tcp_set_timeouts(int fd, int recv_sec, int send_sec);

/*
 * 创建 TCP 服务器并监听。
 * 参数:
 *   port    - 监听端口 (host byte order)
 *   backlog - 连接队列长度
 * 返回: 成功返回 listen fd，失败返回 -1。
 */
int tcp_server_listen(int port, int backlog);

#endif /* TCP_SOCKET_H */
