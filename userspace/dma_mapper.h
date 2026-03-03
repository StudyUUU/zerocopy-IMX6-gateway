/*
 * DMA Buffer Mapper - mmap DMA buffer from kernel
 */

#ifndef __DMA_MAPPER_H__
#define __DMA_MAPPER_H__

#include <cstddef>
#include <cstdint>

class DmaMapper {
public:
    DmaMapper();
    ~DmaMapper();
    
    // Open device and map DMA buffer
    bool init(const char *device_path);
    
    // Cleanup
    void close();
    
    // Get mapped buffer pointer
    void* getBuffer() const { return mapped_addr_; }
    
    // Get buffer size
    size_t getBufferSize() const { return buffer_size_; }
    
    // Get device fd
    int getFd() const { return dev_fd_; }
    
    // Trigger DMA transfer via ioctl
    bool startDmaTransfer();
    
    // Read data from buffer at offset
    bool readData(void *dest, size_t offset, size_t size);
    
private:
    int dev_fd_;
    void *mapped_addr_;
    size_t buffer_size_;
};

#endif /* __DMA_MAPPER_H__ */
