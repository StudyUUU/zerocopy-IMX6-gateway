/*
 * Netlink Client Implementation
 */

#include "netlink_client.h"
#include <unistd.h>
#include <string.h>
#include <iostream>
#include <sys/types.h>

NetlinkClient::NetlinkClient()
    : nl_sock_(-1), msg_count_(0)
{
    memset(&src_addr_, 0, sizeof(src_addr_));
    memset(&dest_addr_, 0, sizeof(dest_addr_));
}

NetlinkClient::~NetlinkClient()
{
    close();
}

bool NetlinkClient::init()
{
    // Create Netlink socket
    nl_sock_ = socket(AF_NETLINK, SOCK_RAW, NETLINK_SDMA_PROTO);
    if (nl_sock_ < 0) {
        std::cerr << "Failed to create Netlink socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Bind socket
    src_addr_.nl_family = AF_NETLINK;
    src_addr_.nl_pid = getpid();
    src_addr_.nl_groups = 0;
    
    if (bind(nl_sock_, (struct sockaddr*)&src_addr_, sizeof(src_addr_)) < 0) {
        std::cerr << "Failed to bind Netlink socket: " << strerror(errno) << std::endl;
        ::close(nl_sock_);
        nl_sock_ = -1;
        return false;
    }
    
    // Setup destination address (kernel)
    dest_addr_.nl_family = AF_NETLINK;
    dest_addr_.nl_pid = 0;  // Kernel
    dest_addr_.nl_groups = 0;
    
    std::cout << "Netlink client initialized (PID: " << getpid() << ")" << std::endl;
    return true;
}

void NetlinkClient::close()
{
    if (nl_sock_ >= 0) {
        ::close(nl_sock_);
        nl_sock_ = -1;
    }
}

bool NetlinkClient::registerToKernel()
{
    struct {
        struct nlmsghdr nlh;
        char payload[128];
    } msg;
    
    memset(&msg, 0, sizeof(msg));
    msg.nlh.nlmsg_len = NLMSG_LENGTH(0);
    msg.nlh.nlmsg_pid = getpid();
    msg.nlh.nlmsg_flags = 0;
    msg.nlh.nlmsg_type = NLMSG_DONE;
    
    struct iovec iov;
    iov.iov_base = &msg;
    iov.iov_len = msg.nlh.nlmsg_len;
    
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_name = &dest_addr_;
    mh.msg_namelen = sizeof(dest_addr_);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    
    if (sendmsg(nl_sock_, &mh, 0) < 0) {
        std::cerr << "Failed to register to kernel: " << strerror(errno) << std::endl;
        return false;
    }
    
    std::cout << "Registered to kernel successfully" << std::endl;
    return true;
}

bool NetlinkClient::receiveMessage()
{
    char buffer[4096];
    struct iovec iov;
    struct msghdr msg;
    struct nlmsghdr *nlh;
    
    iov.iov_base = buffer;
    iov.iov_len = sizeof(buffer);
    
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    
    ssize_t len = recvmsg(nl_sock_, &msg, 0);
    if (len < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;  // No data available
        }
        std::cerr << "Netlink recvmsg error: " << strerror(errno) << std::endl;
        return false;
    }
    
    nlh = (struct nlmsghdr*)buffer;
    if (!NLMSG_OK(nlh, len)) {
        std::cerr << "Invalid Netlink message" << std::endl;
        return false;
    }
    
    // Parse message payload
    struct netlink_sdma_msg *sdma_msg = (struct netlink_sdma_msg*)NLMSG_DATA(nlh);
    
    msg_count_++;
    
    switch (sdma_msg->msg_type) {
    case NLMSG_DATA_READY:
        if (data_ready_cb_) {
            data_ready_cb_(sdma_msg->seq_num, sdma_msg->data_size, sdma_msg->timestamp);
        }
        break;
        
    case NLMSG_ERROR_EVENT:
        if (error_cb_) {
            error_cb_(sdma_msg->seq_num);
        }
        break;
        
    default:
        std::cerr << "Unknown Netlink message type: " << sdma_msg->msg_type << std::endl;
        break;
    }
    
    return true;
}
