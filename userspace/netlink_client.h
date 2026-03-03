/*
 * netlink_client.h - 应用层 Netlink 通信封装 (纯 C 版)
 */

#ifndef NETLINK_CLIENT_H
#define NETLINK_CLIENT_H

#include <stdbool.h>

struct netlink_client_t {
    int sock_fd;
};

/* 初始化并向内核注册 PID */
bool netlink_client_init(struct netlink_client_t *client);

/* 获取 FD 供 epoll 使用 */
int netlink_client_get_fd(const struct netlink_client_t *client);

/* 接收内核消息，返回内核同步计数器 */
int netlink_client_receive_msg(struct netlink_client_t *client);

/* 清理套接字 */
void netlink_client_cleanup(struct netlink_client_t *client);

#endif /* NETLINK_CLIENT_H */