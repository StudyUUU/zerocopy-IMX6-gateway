/*
 * DMA Buffer Mapper Implementation
 */

#include "dma_mapper.h"
#include "../common/protocol.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <string.h>
#include <iostream>

DmaMapper::DmaMapper()
    : dev_fd_(-1), mapped_addr_(nullptr), buffer_size_(0)
{
}

DmaMapper::~DmaMapper()
{
    close();
}

bool DmaMapper::init(const char *device_path)
{
    // Open character device
    dev_fd_ = open(device_path, O_RDWR);
    if (dev_fd_ < 0) {
        std::cerr << "Failed to open device " << device_path << ": " 
                  << strerror(errno) << std::endl;
        return false;
    }
    
    // Get buffer size via ioctl
    int buf_size = ioctl(dev_fd_, IOCTL_GET_BUF_INFO);
    if (buf_size <= 0) {
        std::cerr << "Failed to get buffer info" << std::endl;
        ::close(dev_fd_);
        dev_fd_ = -1;
        return false;
    }
    buffer_size_ = buf_size;
    
    // Map DMA buffer to userspace
    mapped_addr_ = mmap(NULL, buffer_size_, PROT_READ | PROT_WRITE, 
                       MAP_SHARED, dev_fd_, 0);
    if (mapped_addr_ == MAP_FAILED) {
        std::cerr << "Failed to mmap DMA buffer: " << strerror(errno) << std::endl;
        ::close(dev_fd_);
        dev_fd_ = -1;
        mapped_addr_ = nullptr;
        return false;
    }
    
    std::cout << "DMA buffer mapped: size=" << buffer_size_ 
              << " addr=" << mapped_addr_ << std::endl;
    
    return true;
}

void DmaMapper::close()
{
    if (mapped_addr_ && mapped_addr_ != MAP_FAILED) {
        munmap(mapped_addr_, buffer_size_);
        mapped_addr_ = nullptr;
    }
    
    if (dev_fd_ >= 0) {
        ::close(dev_fd_);
        dev_fd_ = -1;
    }
}

bool DmaMapper::startDmaTransfer()
{
    if (dev_fd_ < 0) {
        std::cerr << "Device not opened" << std::endl;
        return false;
    }
    
    if (ioctl(dev_fd_, IOCTL_START_DMA) < 0) {
        std::cerr << "Failed to start DMA transfer: " << strerror(errno) << std::endl;
        return false;
    }
    
    return true;
}

bool DmaMapper::readData(void *dest, size_t offset, size_t size)
{
    if (!mapped_addr_ || offset + size > buffer_size_) {
        return false;
    }
    
    memcpy(dest, (char*)mapped_addr_ + offset, size);
    return true;
}
