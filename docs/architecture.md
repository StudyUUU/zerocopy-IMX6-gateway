# 架构设计文档

## 1. 设计目标

在 i.MX6ULL (ARM Cortex-A7, 528MHz, 无 FPU) 平台上，实现一个高性能工业传感器网关，核心设计目标：

1. **零拷贝 (Zero-Copy)**：传感器数据从 SPI 硬件寄存器到用户态应用，全程无 `copy_to_user`/`copy_from_user`。
2. **CPU 零参与搬运**：SPI 数据传输完全由 i.MX6 SDMA (Smart DMA) 引擎完成，CPU 只做控制面决策。
3. **事件驱动**：从内核到用户态全链路采用异步通知 + Epoll，杜绝轮询和阻塞等待。
4. **低延迟广播**：传感器数据以 JSON 格式通过 UDP 广播至局域网，供多台上位机同时接收。

## 2. 整体架构

系统分为三层：**内核驱动层**、**用户态网关层**、**远端可视化层**。

```
┌─────────────────────────────────────────────────────────────────────┐
│                        远端可视化层 (PC)                             │
│                                                                     │
│   test_client.py ◄─── UDP 8888 ◄─── 局域网广播                      │
│   (matplotlib 实时波形显示)                                          │
└─────────────────────────────────────────────────────────────────────┘
                              ▲ UDP Broadcast
                              │
┌─────────────────────────────────────────────────────────────────────┐
│                      用户态网关层 (i.MX6ULL)                         │
│                                                                     │
│  ┌──────────────┐  ┌──────────────┐  ┌─────────────────────────┐   │
│  │ netlink_client│  │  dma_mapper  │  │     epoll_server        │   │
│  │              │  │              │  │                         │   │
│  │ PID 注册     │  │ mmap 零拷贝  │  │ Epoll 事件循环          │   │
│  │ 消息接收     │  │ 数据读取     │  │ UDP 广播转发            │   │
│  └──────┬───────┘  └──────┬───────┘  └──────────┬──────────────┘   │
│         │                 │                      │                  │
│         └────── main.c 统一编排初始化/清理 ───────┘                  │
└─────────────┬──────────────────────────────┬────────────────────────┘
              │ Netlink Socket               │ mmap (/dev/alientek_icm20608)
              │                              │
┌─────────────▼──────────────────────────────▼────────────────────────┐
│                      内核驱动层 (spi_sdma.ko)                        │
│                                                                     │
│  ┌───────────┐   ┌───────────────┐   ┌────────────────────────┐    │
│  │  HRTimer   │──▶│  SPI Async    │──▶│  SDMA DMA Engine      │    │
│  │  50Hz 触发 │   │  非阻塞提交   │   │  硬件搬运 SPI 数据     │    │
│  └───────────┘   └───────────────┘   └──────────┬─────────────┘    │
│                                                  │ DMA 完成中断     │
│  ┌────────────────────────┐   ┌──────────────────▼───────────┐     │
│  │  DMA Coherent Buffer   │◀──│  Tasklet 回调                │     │
│  │  (解析后的结构化数据)    │   │  解析原始字节 → 结构体       │     │
│  │  用户态可直接 mmap 访问  │   │  发送 Netlink 通知           │     │
│  └────────────────────────┘   └──────────────────────────────┘     │
│                                                                     │
│              ICM20608 Sensor ◄──── SPI Bus (8MHz) ────              │
└─────────────────────────────────────────────────────────────────────┘
```

## 3. 内核驱动设计 (`spi_sdma_driver.c`)

### 3.1 DMA 内存架构

驱动中使用了**两组独立的 DMA 一致性内存**，各司其职：

```
┌───────────────────────────────────────────────────────────────┐
│             DMA 一致性内存 #1: SPI 底层传输缓冲区              │
│                                                               │
│  spi_tx_buf (15B)          spi_rx_buf (15B)                  │
│  ┌──┬──────────────┐      ┌──┬──────────────┐               │
│  │Reg│   (Don't care)│      │XX│ 14B Raw Data  │               │
│  │0xBB│              │      │  │ (大端原始字节)  │               │
│  └──┴──────────────┘      └──┴──────────────┘               │
│  由 SDMA 硬件直接读写，CPU 不参与传输过程                      │
└───────────────────────────────────────────────────────────────┘
                        │
                        │ Tasklet 回调中解析
                        ▼
┌───────────────────────────────────────────────────────────────┐
│             DMA 一致性内存 #2: mmap 用户态映射区               │
│                                                               │
│  dma_buf_virt (4096B = 1 PAGE)                               │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ struct icm_sensor_data {                                │ │
│  │   int accel_x, accel_y, accel_z;                        │ │
│  │   int gyro_x, gyro_y, gyro_z;                           │ │
│  │   int temp;                                             │ │
│  │ }                                                       │ │
│  └─────────────────────────────────────────────────────────┘ │
│  用户态通过 mmap() 直接映射到此物理地址，读取无需系统调用       │
└───────────────────────────────────────────────────────────────┘
```

### 3.2 数据采集时序

每 20ms (50Hz) 完成一次完整的采集周期：

```
时间线 ──────────────────────────────────────────────────────────▶

t=0ms        t≈0.02ms         t≈0.15ms          t≈0.2ms
  │              │                │                  │
  ▼              ▼                ▼                  ▼
HRTimer      spi_async()     SDMA 完成         Netlink
 触发         提交传输         Tasklet 回调      唤醒用户态
  │              │                │                  │
  │   CPU 空闲   │   CPU 空闲     │  解析+写mmap     │
  │◄────────────▶│◄──────────────▶│  ≈几十ns         │
  │              │   SDMA 搬运    │                  │
                                                    ▼
                                              Epoll 唤醒
                                              读 mmap
                                              UDP 发送
                                              ≈0.1ms

总延迟: < 0.5ms (从传感器到 UDP 发出)
```

### 3.3 关键代码路径

#### HRTimer 回调 (硬中断上下文)

```
icm_hrtimer_func()
  ├── 设置 spi_tx_buf[0] = ACCEL_XOUT_H | 0x80   // 构造 SPI 读命令
  ├── spi_async(spi, &msg)                         // 提交异步 DMA 传输 (不阻塞)
  └── hrtimer_forward_now(timer, 20ms)             // 重置定时器
```

- 运行上下文：**硬中断** (CLOCK_MONOTONIC)
- 耗时：< 1μs (仅入队一个 SPI message)
- 关键约束：不能调用任何可能睡眠的函数

#### SPI 完成回调 (Tasklet/软中断上下文)

```
icm_spi_complete_callback()
  ├── data = spi_rx_buf + 1                        // 跳过虚拟字节
  ├── mapped_data->accel_x = (data[0]<<8)|data[1]  // 大端→小端解析
  ├── mapped_data->accel_y = ...                    // 直接写入 mmap 区
  ├── ...
  ├── sync_counter++
  └── send_netlink_notify(sync_counter)             // Netlink 异步通知
```

- 运行上下文：**软中断/Tasklet** (由 SPI 子系统调度)
- 内存分配：使用 `GFP_ATOMIC` (允许在中断上下文分配)
- 关键设计：数据解析后直接写入 mmap 映射区，用户态无需任何拷贝操作

### 3.4 SPI + SDMA 强制启用

```c
dev_data->msg.is_dma_mapped = 1;  // 告知 SPI 子系统: 缓冲区已做 DMA 映射
dev_data->xfer.tx_dma = spi_tx_dma;  // 提供物理地址
dev_data->xfer.rx_dma = spi_rx_dma;
```

设置 `is_dma_mapped = 1` 是启用 SDMA 硬件搬运的关键。它告诉 SPI 控制器驱动：
1. 传输缓冲区已经是 DMA 一致性内存
2. 不需要再做 `dma_map_single()` 映射
3. 可以直接将物理地址交给 SDMA 引擎

### 3.5 mmap 实现

```c
static int icm20608_mmap(struct file *filp, struct vm_area_struct *vma) {
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);  // 禁用 CPU 缓存
    return remap_pfn_range(vma, vma->vm_start,
                           dev_data->dma_buf_phys >> PAGE_SHIFT,
                           size, vma->vm_page_prot);
}
```

- `pgprot_noncached`: 关闭 CPU 缓存，保证用户态每次读取都直接从物理内存获取最新数据
- `remap_pfn_range`: 将 DMA 一致性内存的物理页框映射到用户态虚拟地址空间

## 4. 用户态网关设计

### 4.1 模块职责

| 模块 | 文件 | 职责 |
|------|------|------|
| **Netlink 客户端** | `netlink_client.c/.h` | 创建 Netlink Socket，向内核注册 PID，接收数据就绪通知 |
| **DMA 映射器** | `dma_mapper.c/.h` | 打开字符设备，mmap 映射 DMA 内存，提供零拷贝数据读取接口 |
| **Epoll 服务器** | `epoll_server.c/.h` | Epoll 事件循环，监听 Netlink fd，读取 mmap 数据，UDP 广播转发 |
| **主程序** | `main.c` | 统一初始化/清理，信号处理 (SIGINT/SIGTERM 优雅退出) |

### 4.2 初始化流程

```
main()
  ├── signal(SIGINT/SIGTERM, handler)    // 注册信号处理
  │
  ├── netlink_client_init()              // 1. 建立 Netlink 连接
  │   ├── socket(PF_NETLINK, SOCK_RAW, 31)
  │   ├── bind(pid)
  │   └── sendmsg("REG_PID")            //    向内核注册 PID
  │
  ├── dma_mapper_init()                  // 2. 映射 DMA 内存
  │   ├── open("/dev/alientek_icm20608")
  │   └── mmap(4096, MAP_SHARED)         //    零拷贝映射
  │
  ├── epoll_server_init()                // 3. 初始化事件循环
  │   ├── init_udp_broadcast()           //    创建 UDP 广播 Socket
  │   ├── epoll_create1()
  │   └── epoll_ctl(ADD, netlink_fd)     //    注册 Netlink fd
  │
  └── epoll_server_run()                 // 4. 进入事件循环 (阻塞)
      └── while(running):
          ├── epoll_wait()               //    等待内核通知
          ├── netlink_client_receive_msg()
          ├── dma_mapper_get_latest_data()  // 从 mmap 读数据
          ├── 单位转换 + JSON 打包
          └── sendto(UDP_BROADCAST)       //    广播到局域网
```

### 4.3 事件驱动模型

```
             ┌─────────────┐
             │  epoll_wait  │ ◄── 阻塞等待, CPU 休眠
             └──────┬───────┘
                    │ EPOLLIN (Netlink fd 可读)
                    ▼
        ┌───────────────────────┐
        │ netlink_client_receive │ ◄── recvmsg() 获取 sync_counter
        └───────────┬───────────┘
                    │
                    ▼
        ┌───────────────────────┐
        │ dma_mapper_get_latest  │ ◄── 直接从 mmap 地址读取结构体
        │ (零拷贝, 无系统调用)    │     *out_data = *sensor_data_ptr
        └───────────┬───────────┘
                    │
                    ▼
        ┌───────────────────────┐
        │ handle_sensor_data     │
        │  ├── 单位转换 (g, °/s) │
        │  ├── printf 本地显示   │
        │  ├── snprintf JSON    │
        │  └── sendto UDP 广播  │
        └───────────────────────┘
```

## 5. 通信协议 (`protocol.h`)

内核态和用户态共享同一头文件，确保数据结构严格一致：

```c
#define NETLINK_ICM_NOTIFY 31       // Netlink 协议号 (内核保留最大值)
#define ICM_DMA_BUF_SIZE   4096     // mmap 映射大小 (1 页)
#define ICM_DEV_PATH "/dev/alientek_icm20608"

struct icm_sensor_data {            // 28 字节, 内核写入, 用户态读取
    int gyro_x, gyro_y, gyro_z;    // 陀螺仪原始值
    int accel_x, accel_y, accel_z;  // 加速度计原始值
    int temp;                        // 温度原始值
};
```

### 5.1 Netlink 消息流

```
用户态                           内核态
  │                               │
  │──── "REG_PID" (注册) ────────▶│  icm_nl_recv_msg() 记录 app_pid
  │                               │
  │                               │  (每20ms, Tasklet回调中)
  │◀──── sync_counter (int) ─────│  send_netlink_notify()
  │                               │
  │  recvmsg() 获取计数器          │
  │  然后从 mmap 读取最新数据       │
```

### 5.2 UDP 广播数据格式

JSON 格式，便于跨平台解析：

```json
{"seq":12345, "accel":[0.012, -0.003, 1.001], "gyro":[0.50, -1.20, 0.30], "temp":25.60}
```

## 6. 可视化客户端 (`test_client.py`)

PC 端运行的 Python 实时监控工具，基于 matplotlib 动画：

- **UDP 接收线程**：后台守护线程持续接收广播数据，解析 JSON 存入 `deque` 环形缓冲区
- **matplotlib FuncAnimation**：30ms 刷新间隔，实时绘制加速度计和陀螺仪波形
- **双子图布局**：上方加速度计 (±3g)，下方陀螺仪 (±500°/s)，底部数值面板

## 7. 编译系统

### 7.1 交叉编译链

- **内核模块**: 使用内核构建系统 (`$(MAKE) -C $(KERN_DIR) M=$(PWD) modules`)
- **用户态应用**: `arm-linux-gnueabihf-gcc`，编译选项 `-std=gnu99 -Wall -O2`
- **顶层 Makefile**: 统一管理 `make all` / `make clean` / `make install`

### 7.2 部署脚本 (`build_all.sh`)

自动编译并将产物复制到 NFS rootfs 目标目录：
```
~/linux/rootfs/lib/modules/4.1.15/zerocopy/
├── spi_sdma.ko          # 内核模块
└── zerocopy_gateway      # 用户态程序
```

## 8. 设计决策与权衡

### 8.1 为什么用 HRTimer 而不是硬件中断？

ICM20608 的 INT 引脚可以配置数据就绪中断，但 HRTimer 方案的优势：
- 采样率完全由软件控制，无需额外 GPIO 连线
- 定时精度在 μs 级，对于 50Hz (20ms) 的采样需求绰绰有余
- 代码更简洁，不依赖特定的中断控制器配置

### 8.2 为什么用 Netlink 而不是 eventfd/信号？

- Netlink 可以携带数据负载 (sync_counter)，一次通知完成同步
- 天然支持内核→用户态的异步推送
- 在 Tasklet/软中断上下文中安全调用 (使用 `GFP_ATOMIC`)
- 无需特殊文件描述符，直接用 Socket API

### 8.3 为什么用 mmap 而不是 read()？

传统 `read()` 路径：
```
内核 DMA buf → copy_to_user → 用户态 buf → 处理
              (CPU 拷贝)
```

mmap 零拷贝路径：
```
内核 DMA buf ← remap_pfn_range → 用户态虚拟地址 → 直接读取
              (页表映射, 一次性)     (无拷贝)
```

mmap 方案在 50Hz 采样率下每秒节省 50 次 `copy_to_user` 系统调用和内存拷贝，对于资源受限的 i.MX6ULL 平台非常重要。

### 8.4 为什么用 UDP 广播而不是 TCP？

- 传感器数据是时序流，丢包可以容忍 (下一帧即刻覆盖)
- 广播模式支持多客户端同时接收，无需管理连接
- 无连接开销，延迟更低
- JSON 格式自描述，跨平台兼容
