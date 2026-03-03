/*
 * dma_mapper.c - DMA 一致性内存映射封装实现 (纯 C 版)
 */

#include "dma_mapper.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>

bool dma_mapper_init(struct dma_mapper_t *mapper, const char *dev_path) {
    if (!mapper || !dev_path) return false;

    mapper->fd = -1;
    mapper->mapped_mem = MAP_FAILED;
    mapper->sensor_data_ptr = NULL;

    mapper->fd = open(dev_path, O_RDWR);
    if (mapper->fd < 0) {
        printf("[DmaMapper] 错误: 无法打开设备 %s\n", dev_path);
        return false;
    }

    mapper->mapped_mem = mmap(NULL, ICM_DMA_BUF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mapper->fd, 0);
    if (mapper->mapped_mem == MAP_FAILED) {
        printf("[DmaMapper] 错误: 内存映射 (mmap) 失败。\n");
        close(mapper->fd);
        mapper->fd = -1;
        return false;
    }

    mapper->sensor_data_ptr = (struct icm_sensor_data *)mapper->mapped_mem;
    printf("[DmaMapper] 成功映射设备 %s 到用户空间内存。\n", dev_path);
    return true;
}

bool dma_mapper_get_latest_data(struct dma_mapper_t *mapper, struct icm_sensor_data *out_data) {
    if (!mapper || !mapper->sensor_data_ptr || !out_data) return false;
    
    /* 结构体直接赋值拷贝 */
    *out_data = *(mapper->sensor_data_ptr);
    return true;
}

void dma_mapper_cleanup(struct dma_mapper_t *mapper) {
    if (!mapper) return;
    if (mapper->mapped_mem != MAP_FAILED) {
        munmap(mapper->mapped_mem, ICM_DMA_BUF_SIZE);
        mapper->mapped_mem = MAP_FAILED;
    }
    if (mapper->fd >= 0) {
        close(mapper->fd);
        mapper->fd = -1;
    }
}