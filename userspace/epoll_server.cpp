/*
 * Epoll Server Implementation
 */

#include "epoll_server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <iostream>

EpollServer::EpollServer()
    : server_fd_(-1), epoll_fd_(-1), max_clients_(100), total_bytes_sent_(0)
{
}

EpollServer::~EpollServer()
{
    close();
}

bool EpollServer::setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

bool EpollServer::init(int port, int max_clients)
{
    max_clients_ = max_clients;
    
    // Create TCP socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "Failed to create server socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt SO_REUSEADDR failed" << std::endl;
        ::close(server_fd_);
        return false;
    }
    
    // Enable TCP_NODELAY for low latency
    if (setsockopt(server_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt TCP_NODELAY failed" << std::endl;
    }
    
    // Set non-blocking
    if (!setNonBlocking(server_fd_)) {
        std::cerr << "Failed to set server socket non-blocking" << std::endl;
        ::close(server_fd_);
        return false;
    }
    
    // Bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind: " << strerror(errno) << std::endl;
        ::close(server_fd_);
        return false;
    }
    
    // Listen
    if (listen(server_fd_, max_clients) < 0) {
        std::cerr << "Failed to listen: " << strerror(errno) << std::endl;
        ::close(server_fd_);
        return false;
    }
    
    // Create epoll instance
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        std::cerr << "Failed to create epoll: " << strerror(errno) << std::endl;
        ::close(server_fd_);
        return false;
    }
    
    // Add server socket to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;  // Edge-triggered
    ev.data.fd = server_fd_;
    
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev) < 0) {
        std::cerr << "Failed to add server socket to epoll" << std::endl;
        ::close(epoll_fd_);
        ::close(server_fd_);
        return false;
    }
    
    std::cout << "TCP server initialized on port " << port << std::endl;
    return true;
}

void EpollServer::close()
{
    // Close all client connections
    for (auto &pair : clients_) {
        ::close(pair.first);
    }
    clients_.clear();
    
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
    
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

bool EpollServer::addCustomFd(int fd, uint32_t events, void *ptr)
{
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    if (ptr) {
        ev.data.ptr = ptr;
    }
    
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        std::cerr << "Failed to add custom fd to epoll: " << strerror(errno) << std::endl;
        return false;
    }
    
    return true;
}

bool EpollServer::removeFd(int fd)
{
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        return false;
    }
    return true;
}

int EpollServer::waitEvents(struct epoll_event *events, int max_events, int timeout_ms)
{
    return epoll_wait(epoll_fd_, events, max_events, timeout_ms);
}

void EpollServer::handleServerEvent()
{
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // No more connections
            }
            std::cerr << "Accept failed: " << strerror(errno) << std::endl;
            break;
        }
        
        // Set non-blocking
        if (!setNonBlocking(client_fd)) {
            std::cerr << "Failed to set client socket non-blocking" << std::endl;
            ::close(client_fd);
            continue;
        }
        
        // Enable TCP_NODELAY
        int opt = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        
        // Add to epoll
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.fd = client_fd;
        
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            std::cerr << "Failed to add client to epoll" << std::endl;
            ::close(client_fd);
            continue;
        }
        
        clients_[client_fd] = true;
        
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        std::cout << "New client connected: " << ip_str << ":" 
                  << ntohs(client_addr.sin_port) << " (fd=" << client_fd << ")" << std::endl;
    }
}

void EpollServer::handleClientEvent(int client_fd, uint32_t events)
{
    if (events & (EPOLLERR | EPOLLHUP)) {
        std::cout << "Client disconnected (fd=" << client_fd << ")" << std::endl;
        removeFd(client_fd);
        ::close(client_fd);
        clients_.erase(client_fd);
        return;
    }
    
    if (events & EPOLLIN) {
        // Handle client data (optional - for bidirectional communication)
        char buffer[1024];
        ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                std::cout << "Client closed connection (fd=" << client_fd << ")" << std::endl;
                removeFd(client_fd);
                ::close(client_fd);
                clients_.erase(client_fd);
            }
        }
    }
}

bool EpollServer::sendToClient(int client_fd, const void *data, size_t size)
{
    ssize_t sent = send(client_fd, data, size, MSG_NOSIGNAL);
    if (sent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "Send failed: " << strerror(errno) << std::endl;
            return false;
        }
        return true;  // Would block, but not an error
    }
    
    total_bytes_sent_ += sent;
    return true;
}

size_t EpollServer::broadcastData(const void *data, size_t size)
{
    size_t successful = 0;
    std::vector<int> to_remove;
    
    for (auto &pair : clients_) {
        int fd = pair.first;
        if (sendToClient(fd, data, size)) {
            successful++;
        } else {
            to_remove.push_back(fd);
        }
    }
    
    // Remove failed clients
    for (int fd : to_remove) {
        removeFd(fd);
        ::close(fd);
        clients_.erase(fd);
    }
    
    return successful;
}
