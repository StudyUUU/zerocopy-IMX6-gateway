/*
 * epoll_server.c - 基于 Epoll 的异步事件循环实现 (纯 C 版)
 */

#include "epoll_server.h"
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/epoll.h>

static void print_sensor_data(const struct icm_sensor_data *data, int sync_counter) {
    float ax = (float)data->accel_x / 2048.0f;
    float ay = (float)data->accel_y / 2048.0f;
    float az = (float)data->accel_z / 2048.0f;

    float gx = (float)data->gyro_x / 16.4f;
    float gy = (float)data->gyro_y / 16.4f;
    float gz = (float)data->gyro_z / 16.4f;

    float temp_c = data->temp / 340.0f + 36.53f;

    /* \r 回到行首，\033[K 清除行尾内容 */
    printf("\r\033[K⚡ 唤醒 [%d] | Accel(g) -> X: %6.3f | Y: %6.3f | Z: %6.3f | Gyro(dps) -> X: %6.2f | Y: %6.2f | Z: %6.2f | Temp: %6.2f°C", 
           sync_counter, ax, ay, az, gx, gy, gz, temp_c);
    fflush(stdout); /* C 语言需要强制刷新缓冲区 */
}

static void handle_sensor_data(struct epoll_server_t *server, int sync_counter) {
    struct icm_sensor_data data;
    if (dma_mapper_get_latest_data(server->dma_mapper, &data)) {
        print_sensor_data(&data, sync_counter);
    }
}

bool epoll_server_init(struct epoll_server_t *server, 
                       struct netlink_client_t *nl_client, 
                       struct dma_mapper_t *mapper) {
    struct epoll_event ev;
    int nl_fd;

    if (!server || !nl_client || !mapper) return false;

    server->nl_client = nl_client;
    server->dma_mapper = mapper;
    server->is_running = false;

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

    printf("[EpollServer] 初始化成功，准备就绪。\n");
    return true;
}

void epoll_server_run(struct epoll_server_t *server) {
    struct epoll_event events[MAX_EPOLL_EVENTS];
    int nl_fd;

    if (!server || server->epoll_fd < 0) return;

    server->is_running = true;
    nl_fd = netlink_client_get_fd(server->nl_client);

    printf("\033[2J\033[H"); /* 清屏 */
    printf("=================================================\n");
    printf("  🚀 高性能零拷贝异步工业网关启动中 (纯 C 版)...  \n");
    printf("  (等待内核 Netlink 唤醒信号)           \n");
    printf("=================================================\n");

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
}