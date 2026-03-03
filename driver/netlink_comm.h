/*
 * Netlink Communication Module Header
 * 
 * Description: Kernel-to-userspace async communication via Netlink
 */

#ifndef __NETLINK_COMM_H__
#define __NETLINK_COMM_H__

#include <linux/types.h>

/* Netlink protocol number (user-defined) */
#define NETLINK_SDMA_PROTO 31

/* Netlink message types */
enum netlink_msg_type {
    NLMSG_DATA_READY = 1,    /* Data ready notification */
    NLMSG_ERROR_EVENT = 2,   /* Error event */
    NLMSG_STATUS = 3,        /* Status update */
};

/* Netlink message structure */
struct netlink_sdma_msg {
    __u32 msg_type;          /* Message type */
    __u32 seq_num;           /* Sequence number */
    __u32 data_size;         /* Data size in bytes */
    __u64 timestamp;         /* Timestamp (jiffies) */
} __attribute__((packed));

/* Initialize Netlink socket */
int netlink_init(void);

/* Cleanup Netlink socket */
void netlink_exit(void);

/* Send data ready notification to userspace */
int netlink_notify_data_ready(unsigned long seq, size_t size);

/* Send error notification */
int netlink_notify_error(int error_code);

#endif /* __NETLINK_COMM_H__ */
