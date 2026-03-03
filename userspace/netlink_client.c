/*
 * netlink_client.c - 应用层 Netlink 通信实现 (纯 C 版)
 */

#include "netlink_client.h"
#include "../common/protocol.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>

#define MAX_PAYLOAD 1024

bool netlink_client_init(struct netlink_client_t *client) {
    struct sockaddr_nl src_addr, dest_addr;
    struct nlmsghdr *nlh = NULL;
    struct iovec iov;
    struct msghdr msg;
    ssize_t ret;

    if (!client) return false;
    client->sock_fd = -1;

    client->sock_fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_ICM_NOTIFY);
    if (client->sock_fd < 0) {
        printf("[Netlink] 错误: 无法创建 Netlink Socket。\n");
        return false;
    }

    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.nl_family = AF_NETLINK;
    src_addr.nl_pid = getpid(); 
    src_addr.nl_groups = 0;     

    if (bind(client->sock_fd, (struct sockaddr*)&src_addr, sizeof(src_addr)) < 0) {
        printf("[Netlink] 错误: 无法绑定 Socket。\n");
        close(client->sock_fd);
        client->sock_fd = -1;
        return false;
    }

    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.nl_family = AF_NETLINK;
    dest_addr.nl_pid = 0;       
    dest_addr.nl_groups = 0;    

    nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    if (!nlh) return false;

    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
    nlh->nlmsg_len = NLMSG_SPACE(MAX_PAYLOAD);
    nlh->nlmsg_pid = getpid();  
    nlh->nlmsg_flags = 0;

    strcpy((char *)NLMSG_DATA(nlh), ICM_CMD_REGISTER_PID);

    iov.iov_base = (void *)nlh;
    iov.iov_len = nlh->nlmsg_len;

    memset(&msg, 0, sizeof(msg));
    msg.msg_name = (void *)&dest_addr;
    msg.msg_namelen = sizeof(dest_addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    printf("[Netlink] 正在向内核注册 PID: %d ...\n", getpid());
    ret = sendmsg(client->sock_fd, &msg, 0);
    free(nlh);

    if (ret < 0) {
        printf("[Netlink] 错误: PID 注册消息发送失败。\n");
        return false;
    }

    printf("[Netlink] 注册成功，等待内核数据推送...\n");
    return true;
}

int netlink_client_get_fd(const struct netlink_client_t *client) {
    if (!client) return -1;
    return client->sock_fd;
}

int netlink_client_receive_msg(struct netlink_client_t *client) {
    struct sockaddr_nl dest_addr;
    struct nlmsghdr *nlh = NULL;
    struct iovec iov;
    struct msghdr msg;
    ssize_t ret;
    int sync_counter = -1;

    if (!client || client->sock_fd < 0) return -1;

    nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    if (!nlh) return -1;

    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
    iov.iov_base = (void *)nlh;
    iov.iov_len = NLMSG_SPACE(MAX_PAYLOAD);

    memset(&msg, 0, sizeof(msg));
    msg.msg_name = (void *)&dest_addr;
    msg.msg_namelen = sizeof(dest_addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    ret = recvmsg(client->sock_fd, &msg, 0);
    if (ret < 0) {
        printf("[Netlink] 错误: 接收消息失败。\n");
        free(nlh);
        return -1;
    }

    if (NLMSG_OK(nlh, ret)) {
        memcpy(&sync_counter, NLMSG_DATA(nlh), sizeof(int));
    }

    free(nlh);
    return sync_counter;
}

void netlink_client_cleanup(struct netlink_client_t *client) {
    if (!client) return;
    if (client->sock_fd >= 0) {
        close(client->sock_fd);
        client->sock_fd = -1;
    }
}