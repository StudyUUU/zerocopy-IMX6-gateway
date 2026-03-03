/*
 * Common Protocol Definitions
 * 
 * Shared between kernel and userspace
 */

#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Netlink protocol number */
#define NETLINK_SDMA_PROTO 31

/* Device path */
#define DEVICE_PATH "/dev/spi_sdma"

/* DMA buffer configuration */
#define DMA_BUFFER_SIZE (4096 * 4)  /* 16KB */

/* Netlink message types */
enum netlink_msg_type {
    NLMSG_DATA_READY = 1,
    NLMSG_ERROR_EVENT = 2,
    NLMSG_STATUS = 3,
};

/* Netlink message structure */
struct netlink_sdma_msg {
    uint32_t msg_type;
    uint32_t seq_num;
    uint32_t data_size;
    uint64_t timestamp;
} __attribute__((packed));

/* IOCTL commands */
#define IOCTL_START_DMA    0x01
#define IOCTL_GET_BUF_INFO 0x02

/* TCP protocol - data packet header */
struct tcp_data_header {
    uint32_t magic;          /* Magic number: 0x53444D41 "SDMA" */
    uint32_t seq_num;        /* Sequence number */
    uint32_t data_size;      /* Payload size */
    uint64_t timestamp;      /* Timestamp */
    uint32_t checksum;       /* Simple checksum */
} __attribute__((packed));

#define TCP_MAGIC 0x53444D41

#ifdef __cplusplus
}
#endif

#endif /* __PROTOCOL_H__ */
