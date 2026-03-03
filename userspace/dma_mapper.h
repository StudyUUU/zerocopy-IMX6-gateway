/*
 * dma_mapper.h - DMA 一致性内存映射封装 (纯 C 版)
 */

#ifndef DMA_MAPPER_H
#define DMA_MAPPER_H

#include "../common/protocol.h"
#include <stdbool.h>

/* 相当于 C++ 中的 Class 成员变量 */
struct dma_mapper_t {
    int fd;
    void* mapped_mem;
    struct icm_sensor_data* sensor_data_ptr;
};

/* 初始化映射 */
bool dma_mapper_init(struct dma_mapper_t *mapper, const char *dev_path);

/* 获取最新数据 */
bool dma_mapper_get_latest_data(struct dma_mapper_t *mapper, struct icm_sensor_data *out_data);

/* 清理资源 */
void dma_mapper_cleanup(struct dma_mapper_t *mapper);

#endif /* DMA_MAPPER_H */