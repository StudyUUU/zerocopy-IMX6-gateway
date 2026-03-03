/*
 * epoll_server.h - 基于 Epoll 的异步事件循环 (纯 C 版)
 */

#ifndef EPOLL_SERVER_H
#define EPOLL_SERVER_H

#include "netlink_client.h"
#include "dma_mapper.h"
#include <stdbool.h>

#define MAX_EPOLL_EVENTS 10

struct epoll_server_t {
    int epoll_fd;
    volatile bool is_running;
    struct netlink_client_t *nl_client;
    struct dma_mapper_t *dma_mapper;
};

/* 初始化 Epoll 服务 */
bool epoll_server_init(struct epoll_server_t *server, 
                       struct netlink_client_t *nl_client, 
                       struct dma_mapper_t *mapper);

/* 运行事件循环 */
void epoll_server_run(struct epoll_server_t *server);

/* 停止服务器 */
void epoll_server_stop(struct epoll_server_t *server);

/* 清理资源 */
void epoll_server_cleanup(struct epoll_server_t *server);

#endif /* EPOLL_SERVER_H */