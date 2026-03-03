/*
 * Netlink Client - Userspace Netlink Communication
 */

#ifndef __NETLINK_CLIENT_H__
#define __NETLINK_CLIENT_H__

#include <sys/socket.h>
#include <linux/netlink.h>
#include <functional>
#include <atomic>
#include "../common/protocol.h"

class NetlinkClient {
public:
    using DataReadyCallback = std::function<void(uint32_t seq, uint32_t size, uint64_t timestamp)>;
    using ErrorCallback = std::function<void(int error_code)>;
    
    NetlinkClient();
    ~NetlinkClient();
    
    // Initialize Netlink socket
    bool init();
    
    // Close Netlink connection
    void close();
    
    // Get Netlink socket fd (for epoll)
    int getFd() const { return nl_sock_; }
    
    // Register to kernel (send our PID)
    bool registerToKernel();
    
    // Receive and process Netlink message
    bool receiveMessage();
    
    // Set callbacks
    void setDataReadyCallback(DataReadyCallback cb) { data_ready_cb_ = cb; }
    void setErrorCallback(ErrorCallback cb) { error_cb_ = cb; }
    
    // Statistics
    uint64_t getTotalMessages() const { return msg_count_; }
    
private:
    int nl_sock_;
    struct sockaddr_nl src_addr_;
    struct sockaddr_nl dest_addr_;
    
    DataReadyCallback data_ready_cb_;
    ErrorCallback error_cb_;
    
    std::atomic<uint64_t> msg_count_;
};

#endif /* __NETLINK_CLIENT_H__ */
