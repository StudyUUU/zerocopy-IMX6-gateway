/*
 * Epoll Server - TCP Server with Epoll I/O Multiplexing
 */

#ifndef __EPOLL_SERVER_H__
#define __EPOLL_SERVER_H__

#include <vector>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <sys/epoll.h>

class EpollServer {
public:
    using DataSendCallback = std::function<void(int client_fd)>;
    
    EpollServer();
    ~EpollServer();
    
    // Initialize server
    bool init(int port, int max_clients = 100);
    
    // Cleanup
    void close();
    
    // Get epoll fd
    int getEpollFd() const { return epoll_fd_; }
    
    // Add custom fd to epoll (e.g., Netlink socket)
    bool addCustomFd(int fd, uint32_t events, void *ptr = nullptr);
    
    // Remove fd from epoll
    bool removeFd(int fd);
    
    // Wait for events (blocking)
    int waitEvents(struct epoll_event *events, int max_events, int timeout_ms);
    
    // Handle server socket events (accept new connections)
    void handleServerEvent();
    
    // Handle client socket events
    void handleClientEvent(int client_fd, uint32_t events);
    
    // Broadcast data to all connected clients
    size_t broadcastData(const void *data, size_t size);
    
    // Send data to specific client
    bool sendToClient(int client_fd, const void *data, size_t size);
    
    // Get number of connected clients
    size_t getClientCount() const { return clients_.size(); }
    
    // Get total bytes sent
    uint64_t getTotalBytesSent() const { return total_bytes_sent_; }
    
private:
    int server_fd_;
    int epoll_fd_;
    int max_clients_;
    
    std::unordered_map<int, bool> clients_;  // fd -> connected
    std::atomic<uint64_t> total_bytes_sent_;
    
    bool setNonBlocking(int fd);
};

#endif /* __EPOLL_SERVER_H__ */
