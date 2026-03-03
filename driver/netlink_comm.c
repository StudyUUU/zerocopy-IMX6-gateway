/*
 * Netlink Communication Module Implementation
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include "netlink_comm.h"

static struct sock *nl_sock = NULL;
static u32 nl_user_pid = 0;
static DEFINE_SPINLOCK(nl_lock);
static struct tasklet_struct nl_tasklet;

/* Tasklet structure for deferred notification */
struct nl_notify_work {
    unsigned long seq_num;
    size_t data_size;
    u32 msg_type;
};

static struct nl_notify_work pending_notify;

/* Send Netlink message to userspace */
static int netlink_send_msg(struct netlink_sdma_msg *msg)
{
    struct sk_buff *skb;
    struct nlmsghdr *nlh;
    unsigned long flags;
    u32 pid;
    int ret;
    
    spin_lock_irqsave(&nl_lock, flags);
    pid = nl_user_pid;
    spin_unlock_irqrestore(&nl_lock, flags);
    
    if (pid == 0) {
        /* No userspace listener */
        return -ENOENT;
    }
    
    /* Allocate socket buffer */
    skb = nlmsg_new(sizeof(*msg), GFP_ATOMIC);
    if (!skb) {
        pr_err("Failed to allocate Netlink skb\n");
        return -ENOMEM;
    }
    
    /* Fill Netlink message header */
    nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, sizeof(*msg), 0);
    if (!nlh) {
        pr_err("Failed to put Netlink header\n");
        kfree_skb(skb);
        return -EMSGSIZE;
    }
    
    /* Copy message payload */
    memcpy(nlmsg_data(nlh), msg, sizeof(*msg));
    
    /* Send to userspace */
    ret = nlmsg_unicast(nl_sock, skb, pid);
    if (ret < 0) {
        pr_err("Failed to send Netlink message: %d\n", ret);
        return ret;
    }
    
    return 0;
}

/* Tasklet handler for async notification */
static void netlink_tasklet_handler(unsigned long data)
{
    struct netlink_sdma_msg msg;
    
    msg.msg_type = pending_notify.msg_type;
    msg.seq_num = pending_notify.seq_num;
    msg.data_size = pending_notify.data_size;
    msg.timestamp = get_jiffies_64();
    
    netlink_send_msg(&msg);
}

/* Netlink socket receive callback */
static void netlink_recv_msg(struct sk_buff *skb)
{
    struct nlmsghdr *nlh;
    unsigned long flags;
    
    nlh = (struct nlmsghdr *)skb->data;
    
    spin_lock_irqsave(&nl_lock, flags);
    nl_user_pid = nlh->nlmsg_pid;
    spin_unlock_irqrestore(&nl_lock, flags);
    
    pr_info("Netlink: Registered userspace PID %u\n", nl_user_pid);
}

/* Initialize Netlink socket */
int netlink_init(void)
{
    struct netlink_kernel_cfg cfg = {
        .input = netlink_recv_msg,
    };
    
    nl_sock = netlink_kernel_create(&init_net, NETLINK_SDMA_PROTO, &cfg);
    if (!nl_sock) {
        pr_err("Failed to create Netlink socket\n");
        return -ENOMEM;
    }
    
    /* Initialize tasklet */
    tasklet_init(&nl_tasklet, netlink_tasklet_handler, 0);
    
    pr_info("Netlink communication initialized (protocol %d)\n", NETLINK_SDMA_PROTO);
    return 0;
}

/* Cleanup Netlink socket */
void netlink_exit(void)
{
    tasklet_kill(&nl_tasklet);
    
    if (nl_sock) {
        netlink_kernel_release(nl_sock);
        nl_sock = NULL;
    }
    
    pr_info("Netlink communication exited\n");
}

/* Notify userspace that data is ready */
int netlink_notify_data_ready(unsigned long seq, size_t size)
{
    /* Schedule tasklet to send notification */
    pending_notify.msg_type = NLMSG_DATA_READY;
    pending_notify.seq_num = seq;
    pending_notify.data_size = size;
    
    tasklet_schedule(&nl_tasklet);
    
    return 0;
}

/* Notify userspace of error */
int netlink_notify_error(int error_code)
{
    struct netlink_sdma_msg msg;
    
    msg.msg_type = NLMSG_ERROR_EVENT;
    msg.seq_num = error_code;
    msg.data_size = 0;
    msg.timestamp = get_jiffies_64();
    
    return netlink_send_msg(&msg);
}

EXPORT_SYMBOL(netlink_init);
EXPORT_SYMBOL(netlink_exit);
EXPORT_SYMBOL(netlink_notify_data_ready);
EXPORT_SYMBOL(netlink_notify_error);
