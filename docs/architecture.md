# 系统架构设计文档

## 概述

本项目在 i.MX6ULL 平台上实现了一个高性能的零拷贝数据采集与转发系统，采用"SDMA硬件搬运 -> Netlink内核通信 -> Epoll网络分发"的全流程架构。

## 核心技术特点

1. **零拷贝数据传输**：通过 mmap 直接映射内核 DMA 缓冲区到用户空间
2. **异步通知机制**：使用 Netlink Socket 实现内核到用户态的事件通知
3. **高效并发处理**：基于 Epoll 的 I/O 多路复用处理多路 TCP 连接
4. **硬件 DMA 加速**：利用 SDMA 引擎自主搬运数据，降低 CPU 负载

## 系统架构

```
+-------------------+
|   SPI 设备        |
+-------------------+
        |
        | (DMA)
        v
+-------------------+
| SDMA Controller   |
+-------------------+
        |
        v
+-------------------+
|  Kernel Driver    |
|  - SPI Driver     |
|  - DMA Buffer     |
|  - Netlink        |
+-------------------+
        |
        | (mmap + Netlink)
        v
+-------------------+
| Userspace Server  |
|  - Netlink Client |
|  - DMA Mapper     |
|  - Epoll Server   |
+-------------------+
        |
        | (TCP)
        v
+-------------------+
|   多路客户端      |
+-------------------+
```

## 关键模块

### 1. 内核驱动模块 (driver/)

#### spi_sdma_driver.c
- **功能**：SPI 字符设备驱动主体
- **关键实现**：
  - 使用 `dma_alloc_coherent()` 分配一致性 DMA 缓冲区
  - 通过 Linux DMA Engine API 管理 SDMA 通道
  - 实现 `mmap` 操作供用户态访问 DMA 缓冲区
  - 在 DMA 回调中通过 Tasklet 发送 Netlink 通知

#### netlink_comm.c/h
- **功能**：内核侧 Netlink 通信模块
- **关键实现**：
  - 创建 Netlink 套接字（协议号：31）
  - 使用 Tasklet 在中断下半部发送消息
  - 支持数据就绪、错误事件等多种消息类型

### 2. 用户态服务程序 (userspace/)

#### netlink_client.cpp
- **功能**：用户态 Netlink 客户端
- **关键实现**：
  - 创建 Netlink socket 并注册到内核
  - 接收内核异步通知
  - 通过回调机制处理数据就绪事件

#### dma_mapper.cpp
- **功能**：DMA 缓冲区映射管理
- **关键实现**：
  - 打开字符设备并 mmap DMA 缓冲区
  - 提供零拷贝数据读取接口
  - 通过 ioctl 触发 DMA 传输

#### epoll_server.cpp
- **功能**：基于 Epoll 的 TCP 服务器
- **关键实现**：
  - Edge-Triggered 边缘触发模式
  - 非阻塞 socket 操作
  - 支持并发多客户端连接
  - 数据广播到所有连接的客户端

#### main.cpp
- **功能**：主程序，集成所有模块
- **工作流程**：
  1. 初始化 DMA mapper、Netlink client、Epoll server
  2. 将 Netlink fd 添加到 epoll 监听
  3. 注册数据就绪回调，在回调中广播数据
  4. 主循环中 epoll_wait 等待事件
  5. 处理 Netlink 通知和 TCP 连接事件

### 3. 通用协议 (common/)

#### protocol.h
- 定义内核与用户态共享的数据结构
- Netlink 消息格式
- TCP 数据包格式
- IOCTL 命令定义

## 数据流程

```
1. 用户态触发 DMA 传输 (ioctl)
   └─> 内核驱动提交 DMA 请求到 SDMA

2. SDMA 硬件搬运数据 (SPI -> Memory)
   └─> 完成后产生 DMA 完成中断

3. DMA 回调函数执行 (中断上下文)
   └─> 调度 Tasklet

4. Tasklet 发送 Netlink 通知
   └─> 用户态收到数据就绪事件

5. 用户态读取 DMA 缓冲区 (mmap, 零拷贝)
   └─> 封装 TCP 数据包

6. Epoll 广播数据到所有 TCP 客户端
   └─> 多路并发传输

7. 触发下一次 DMA 传输
   └─> 循环往复
```

## 性能优化

### 零拷贝实现
1. **内核侧**：使用 `dma_alloc_coherent()` 分配 DMA 可访问的一致性内存
2. **用户侧**：通过 `mmap()` 映射到用户空间，避免 copy_to_user
3. **网络侧**：直接从映射区域发送数据，无需额外拷贝

### CPU 负载优化
1. **硬件 DMA**：SDMA 自主搬运数据，CPU 只需配置和处理中断
2. **异步通知**：Netlink 替代轮询，减少无效系统调用
3. **边缘触发**：Epoll ET 模式减少事件通知次数

### 并发性能
1. **非阻塞 I/O**：所有 socket 设置为非阻塞
2. **多路复用**：单线程 epoll 处理多个连接
3. **TCP_NODELAY**：禁用 Nagle 算法，降低延迟

## 关键系统调用

```c
// 内核侧
dma_alloc_coherent()      // 分配 DMA 内存
dma_request_chan()        // 请求 DMA 通道
dmaengine_prep_slave_single() // 准备 DMA 传输
netlink_kernel_create()   // 创建 Netlink socket
nlmsg_unicast()          // 发送 Netlink 消息
remap_pfn_range()        // mmap 实现

// 用户侧
socket(AF_NETLINK, ...)  // 创建 Netlink socket
mmap()                   // 映射 DMA 缓冲区
epoll_create1()          // 创建 epoll 实例
epoll_wait()             // 等待 I/O 事件
ioctl()                  // 控制驱动
```

## 内存布局

```
物理内存:
+------------------+
| DMA Buffer       | <- SDMA 写入
| (物理地址连续)    |
+------------------+

内核虚拟地址空间:
+------------------+
| DMA Buffer       | <- dma_alloc_coherent 返回
| (内核可访问)      |
+------------------+

用户虚拟地址空间:
+------------------+
| DMA Buffer       | <- mmap 映射
| (用户可访问)      |
+------------------+

三者映射到同一物理内存，实现零拷贝！
```

## 同步机制

1. **DMA 完成同步**：使用 `completion` 机制
2. **多客户端访问**：使用 spinlock 保护 Netlink PID
3. **数据就绪标志**：使用 `atomic_t` 原子操作

## 错误处理

1. **DMA 传输失败**：记录错误计数，发送 Netlink 错误通知
2. **客户端断开**：Epoll 检测到 EPOLLHUP/EPOLLERR，自动清理
3. **Netlink 发送失败**：返回错误码，不影响主流程
4. **内存分配失败**：probe 阶段检测，启动失败

## 扩展性

1. **支持多设备**：修改驱动支持多个 SPI 设备实例
2. **可配置缓冲区**：通过模块参数调整 DMA 缓冲区大小
3. **协议扩展**：在 protocol.h 中添加新的消息类型
4. **自定义过滤**：在数据就绪回调中添加处理逻辑

## 安全考虑

1. **权限检查**：字符设备权限设置为 666（可配置）
2. **参数验证**：所有 ioctl 参数需验证
3. **缓冲区边界**：mmap 大小限制，防止越界访问
4. **资源限制**：限制最大客户端连接数

## 调试方法

```bash
# 查看内核日志
dmesg | tail -50

# 查看驱动详细信息
cat /proc/modules | grep spi_sdma

# 查看 Netlink 统计
ss -A netlink

# 查看 TCP 连接
netstat -antp | grep zerocopy_gateway

# 监控系统调用
strace -e trace=epoll_wait,recvmsg,sendto ./userspace/zerocopy_gateway

# 性能分析
perf stat -e cpu-cycles,instructions,cache-misses ./userspace/zerocopy_gateway
```

## 局限性

1. **单生产者**：当前实现假设单一 DMA 数据源
2. **固定缓冲区**：DMA 缓冲区大小编译时确定
3. **协议简单**：TCP 协议未考虑流控和重传
4. **无持久化**：数据不落盘，仅实时转发

## 未来改进方向

1. 实现 scatter-gather DMA 支持
2. 添加数据压缩功能
3. 支持 TLS/SSL 加密传输
4. 实现客户端订阅机制（选择性推送）
5. 添加 QoS 和优先级控制
