/*
 * main.c - 高性能零拷贝网关入口程序 (纯 C 版)
 */

#include "netlink_client.h"
#include "dma_mapper.h"
#include "epoll_server.h"
#include "../common/protocol.h"

#include <stdio.h>
#include <signal.h>

/* 全局指针用于信号处理函数优雅退出 */
static struct epoll_server_t *g_server = NULL;

static void signal_handler(int signum) {
    printf("\n[Main] 收到终止信号 (%d)，正在安全退出...\n", signum);
    if (g_server) {
        epoll_server_stop(g_server);
    }
}

int main(void) {
    struct netlink_client_t nl_client;
    struct dma_mapper_t dma_mapper;
    struct epoll_server_t server;

    /* 注册 Ctrl+C 信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 1. 初始化 Netlink 客户端 */
    if (!netlink_client_init(&nl_client)) {
        return -1;
    }

    /* 2. 初始化 DMA 内存映射 */
    if (!dma_mapper_init(&dma_mapper, ICM_DEV_PATH)) {
        netlink_client_cleanup(&nl_client);
        return -1;
    }

    /* 3. 初始化并运行 Epoll 服务器 */
    if (!epoll_server_init(&server, &nl_client, &dma_mapper)) {
        dma_mapper_cleanup(&dma_mapper);
        netlink_client_cleanup(&nl_client);
        return -1;
    }

    g_server = &server;
    
    /* 开始阻塞监听 (事件驱动) */
    epoll_server_run(&server);

    /* 4. 清理资源 */
    epoll_server_cleanup(&server);
    dma_mapper_cleanup(&dma_mapper);
    netlink_client_cleanup(&nl_client);

    printf("[Main] 程序已退出。\n");
    return 0;
}