/*
 * epoll_server.c - 基于 Epoll 的异步事件循环实现 (纯 C 版 + UDP 广播转发)
 * * 核心功能：
 * 1. 监听内核 Netlink 异步唤醒信号。
 * 2. 唤醒后，通过 mmap 零拷贝读取最新传感器数据。
 * 3. 将数据打包，通过 UDP 广播发送到局域网，供上位机实时接收。
 */

#include "epoll_server.h"
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* === UDP 广播配置 === */
#define UDP_BROADCAST_PORT 8888  /* 上位机监听此端口即可接收数据 */

/* UDP 发送套接字和目标地址 */
static int udp_sock_fd = -1;
static struct sockaddr_in udp_dest_addr;

/* ========================================================================== */
/* 初始化 UDP 广播 Socket                                                     */
/* ========================================================================== */
static bool init_udp_broadcast(void) {
    int broadcast_enable = 1;

    /* 1. 创建 UDP Socket */
    udp_sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock_fd < 0) {
        perror("[EpollServer] 错误: 创建 UDP Socket 失败");
        return false;
    }

    /* 2. 设置 Socket 选项：允许发送广播消息 */
    if (setsockopt(udp_sock_fd, SOL_SOCKET, SO_BROADCAST, 
                   &broadcast_enable, sizeof(broadcast_enable)) < 0) {
        perror("[EpollServer] 错误: 设置 SO_BROADCAST 失败");
        close(udp_sock_fd);
        udp_sock_fd = -1;
        return false;
    }

    /* 3. 配置目标地址 (局域网广播地址 255.255.255.255，指定端口) */
    memset(&udp_dest_addr, 0, sizeof(udp_dest_addr));
    udp_dest_addr.sin_family = AF_INET;
    udp_dest_addr.sin_port = htons(UDP_BROADCAST_PORT);
    udp_dest_addr.sin_addr.s_addr = inet_addr("255.255.255.255"); 

    printf("[EpollServer] UDP 广播套接字初始化成功 (目标端口: %d)\n", UDP_BROADCAST_PORT);
    return true;
}

/* ========================================================================== */
/* 数据处理与转发逻辑                                                         */
/* ========================================================================== */
static void handle_sensor_data(struct epoll_server_t *server, int sync_counter) {
    struct icm_sensor_data data;
    char send_buf[256];
    int len;

    /* 1. 从 DMA 映射区获取数据 (零拷贝) */
    if (!dma_mapper_get_latest_data(server->dma_mapper, &data)) {
        return;
    }

    /* 2. 数据单位转换 */
    float ax = (float)data.accel_x / 2048.0f;
    float ay = (float)data.accel_y / 2048.0f;
    float az = (float)data.accel_z / 2048.0f;

    float gx = (float)data.gyro_x / 16.4f;
    float gy = (float)data.gyro_y / 16.4f;
    float gz = (float)data.gyro_z / 16.4f;

    float temp_c = data.temp / 340.0f + 36.53f;

    /* 3. 本地终端打印 (刷新显示) */
    printf("\r\033[K⚡ [%d] | A:[%6.2f %6.2f %6.2f] | G:[%6.1f %6.1f %6.1f] | T:%6.1f°C", 
           sync_counter, ax, ay, az, gx, gy, gz, temp_c);
    fflush(stdout);

    /* 4. 打包数据准备发送 (可以使用 JSON 格式方便上位机解析) */
    len = snprintf(send_buf, sizeof(send_buf), 
                   "{\"seq\":%d, \"accel\":[%.3f, %.3f, %.3f], \"gyro\":[%.2f, %.2f, %.2f], \"temp\":%.2f}",
                   sync_counter, ax, ay, az, gx, gy, gz, temp_c);

    /* 5. 通过 UDP 广播发送 */
    if (udp_sock_fd >= 0) {
        sendto(udp_sock_fd, send_buf, len, 0, 
               (struct sockaddr *)&udp_dest_addr, sizeof(udp_dest_addr));
    }
}

/* ========================================================================== */
/* Epoll 服务器核心实现                                                       */
/* ========================================================================== */
bool epoll_server_init(struct epoll_server_t *server, 
                       struct netlink_client_t *nl_client, 
                       struct dma_mapper_t *mapper) {
    struct epoll_event ev;
    int nl_fd;

    if (!server || !nl_client || !mapper) return false;

    server->nl_client = nl_client;
    server->dma_mapper = mapper;
    server->is_running = false;

    /* 初始化 UDP 广播 */
    if (!init_udp_broadcast()) {
        return false;
    }

    server->epoll_fd = epoll_create1(0);
    if (server->epoll_fd < 0) {
        printf("[EpollServer] 错误: 创建 epoll 失败。\n");
        return false;
    }

    nl_fd = netlink_client_get_fd(nl_client);
    ev.events = EPOLLIN;
    ev.data.fd = nl_fd;

    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, nl_fd, &ev) < 0) {
        printf("[EpollServer] 错误: 无法将 Netlink Socket 添加到 epoll。\n");
        close(server->epoll_fd);
        server->epoll_fd = -1;
        return false;
    }

    printf("[EpollServer] Epoll 事件循环初始化成功，准备就绪。\n");
    return true;
}

void epoll_server_run(struct epoll_server_t *server) {
    struct epoll_event events[MAX_EPOLL_EVENTS];
    int nl_fd;

    if (!server || server->epoll_fd < 0) return;

    server->is_running = true;
    nl_fd = netlink_client_get_fd(server->nl_client);

    printf("\033[2J\033[H"); /* 清屏 */
    printf("================================================================\n");
    printf("  🚀 高性能零拷贝异步工业网关启动中 (纯 C 版 + UDP 广播)...\n");
    printf("  [监听] 内核 Netlink 异步唤醒信号...\n");
    printf("  [分发] UDP 广播地址: 255.255.255.255, 端口: %d\n", UDP_BROADCAST_PORT);
    printf("================================================================\n");

    while (server->is_running) {
        int nfds = epoll_wait(server->epoll_fd, events, MAX_EPOLL_EVENTS, -1);
        int i;
        
        if (nfds < 0) {
            if (errno == EINTR) continue;
            printf("\n[EpollServer] epoll_wait 出错\n");
            break;
        }

        for (i = 0; i < nfds; ++i) {
            if (events[i].data.fd == nl_fd) {
                int sync_counter = netlink_client_receive_msg(server->nl_client);
                if (sync_counter >= 0) {
                    handle_sensor_data(server, sync_counter);
                }
            }
        }
    }
}

void epoll_server_stop(struct epoll_server_t *server) {
    if (server) {
        server->is_running = false;
    }
}

void epoll_server_cleanup(struct epoll_server_t *server) {
    if (!server) return;
    
    if (server->epoll_fd >= 0) {
        close(server->epoll_fd);
        server->epoll_fd = -1;
    }
    
    /* 清理 UDP 套接字 */
    if (udp_sock_fd >= 0) {
        close(udp_sock_fd);
        udp_sock_fd = -1;
    }
}