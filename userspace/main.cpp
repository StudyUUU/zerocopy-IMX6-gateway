/*
 * Main Application - Zero-Copy Data Gateway
 * 
 * Integrates:
 * - Netlink client for kernel notifications
 * - DMA buffer mapper for zero-copy access
 * - Epoll server for concurrent TCP connections
 */

#include <iostream>
#include <csignal>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include "netlink_client.h"
#include "dma_mapper.h"
#include "epoll_server.h"
#include "../common/protocol.h"

// Global running flag
static std::atomic<bool> g_running(true);

// Signal handler
void signalHandler(int sig)
{
    std::cout << "\nReceived signal " << sig << ", shutting down..." << std::endl;
    g_running = false;
}

// Calculate simple checksum
uint32_t calculateChecksum(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t*)data;
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i++) {
        sum += bytes[i];
    }
    return sum;
}

int main(int argc, char *argv[])
{
    int tcp_port = 8888;
    const char *device_path = DEVICE_PATH;
    
    // Parse command line arguments
    if (argc > 1) {
        tcp_port = atoi(argv[1]);
    }
    if (argc > 2) {
        device_path = argv[2];
    }
    
    std::cout << "=== Zero-Copy Data Gateway ===" << std::endl;
    std::cout << "TCP Port: " << tcp_port << std::endl;
    std::cout << "Device: " << device_path << std::endl;
    std::cout << "===============================" << std::endl;
    
    // Install signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // Initialize DMA mapper
    DmaMapper dma_mapper;
    if (!dma_mapper.init(device_path)) {
        std::cerr << "Failed to initialize DMA mapper" << std::endl;
        return 1;
    }
    
    // Initialize Netlink client
    NetlinkClient netlink_client;
    if (!netlink_client.init()) {
        std::cerr << "Failed to initialize Netlink client" << std::endl;
        return 1;
    }
    
    // Initialize Epoll server
    EpollServer epoll_server;
    if (!epoll_server.init(tcp_port)) {
        std::cerr << "Failed to initialize Epoll server" << std::endl;
        return 1;
    }
    
    // Register Netlink client to kernel
    if (!netlink_client.registerToKernel()) {
        std::cerr << "Failed to register to kernel" << std::endl;
        return 1;
    }
    
    // Add Netlink socket to epoll
    if (!epoll_server.addCustomFd(netlink_client.getFd(), EPOLLIN | EPOLLET)) {
        std::cerr << "Failed to add Netlink fd to epoll" << std::endl;
        return 1;
    }
    
    std::cout << "System initialized, waiting for events..." << std::endl;
    
    // Statistics
    uint64_t total_transfers = 0;
    auto start_time = std::chrono::steady_clock::now();
    
    // Set Netlink data ready callback
    netlink_client.setDataReadyCallback(
        [&](uint32_t seq, uint32_t size, uint64_t timestamp) {
            // Data is ready in DMA buffer
            std::cout << "Data ready: seq=" << seq << " size=" << size << std::endl;
            
            // Prepare TCP packet header
            struct tcp_data_header header;
            header.magic = TCP_MAGIC;
            header.seq_num = seq;
            header.data_size = size;
            header.timestamp = timestamp;
            
            // Read data from DMA buffer (zero-copy)
            void *dma_buffer = dma_mapper.getBuffer();
            if (dma_buffer && size <= dma_mapper.getBufferSize()) {
                header.checksum = calculateChecksum(dma_buffer, size);
                
                // Broadcast header + data to all clients
                size_t sent_clients = epoll_server.broadcastData(&header, sizeof(header));
                if (sent_clients > 0) {
                    epoll_server.broadcastData(dma_buffer, size);
                    total_transfers++;
                    
                    std::cout << "Broadcasted to " << sent_clients << " clients" << std::endl;
                }
            }
            
            // Trigger next DMA transfer
            dma_mapper.startDmaTransfer();
        }
    );
    
    // Set error callback
    netlink_client.setErrorCallback(
        [](int error_code) {
            std::cerr << "Kernel error: " << error_code << std::endl;
        }
    );
    
    // Trigger initial DMA transfer
    std::cout << "Starting initial DMA transfer..." << std::endl;
    dma_mapper.startDmaTransfer();
    
    // Main event loop
    const int MAX_EVENTS = 64;
    struct epoll_event events[MAX_EVENTS];
    
    while (g_running) {
        int nfds = epoll_server.waitEvents(events, MAX_EVENTS, 1000);
        
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "epoll_wait error: " << strerror(errno) << std::endl;
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;
            
            if (fd == netlink_client.getFd()) {
                // Netlink event
                netlink_client.receiveMessage();
            } else {
                // TCP server/client event
                if (fd == epoll_server.getEpollFd()) {
                    continue;
                }
                
                // Check if it's server socket (accept event)
                epoll_server.handleServerEvent();
                
                // Handle client events
                epoll_server.handleClientEvent(fd, ev);
            }
        }
        
        // Print statistics every 10 seconds
        static auto last_stats = start_time;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stats).count() >= 10) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            std::cout << "\n=== Statistics (runtime: " << elapsed << "s) ===" << std::endl;
            std::cout << "  Total transfers: " << total_transfers << std::endl;
            std::cout << "  Netlink messages: " << netlink_client.getTotalMessages() << std::endl;
            std::cout << "  Connected clients: " << epoll_server.getClientCount() << std::endl;
            std::cout << "  Total bytes sent: " << epoll_server.getTotalBytesSent() << std::endl;
            std::cout << "================================\n" << std::endl;
            last_stats = now;
        }
    }
    
    // Cleanup
    std::cout << "Shutting down..." << std::endl;
    epoll_server.close();
    netlink_client.close();
    dma_mapper.close();
    
    std::cout << "Goodbye!" << std::endl;
    return 0;
}
