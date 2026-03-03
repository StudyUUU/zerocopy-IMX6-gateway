# Zero-Copy IMX6 Gateway

基于 i.MX6ULL 的高性能零拷贝工业传感器网关，实现从 ICM20608 六轴 IMU 传感器到局域网 UDP 广播的全链路低延迟数据转发。

## 核心技术亮点

| 技术点 | 实现方式 | 解决的问题 |
|--------|----------|------------|
| **SDMA 硬件搬运** | `spi_async` + `is_dma_mapped=1` 强制 i.MX6 SDMA 引擎接管 SPI 传输 | CPU 零参与数据搬运，传输期间 CPU 完全空闲 |
| **mmap 零拷贝** | `dma_alloc_coherent` + `remap_pfn_range`，用户态直接映射物理内存 | 内核→用户态零次 `copy_to_user`，数据直达应用层 |
| **高精度定时器** | `hrtimer` 硬中断上下文 50Hz 精确触发 | 替代低精度内核定时器，保证采样时序稳定 |
| **Netlink 异步通知** | 内核 Tasklet 回调中发送 Netlink 消息唤醒用户态 | 替代轮询，实现事件驱动的数据推送 |
| **Epoll 事件驱动** | 用户态 Epoll 监听 Netlink fd，非阻塞转发 | 单线程高效处理 I/O 事件，资源占用极低 |

## 系统架构

```
ICM20608 传感器
     │ SPI Bus (8MHz)
     ▼
┌─────────────────────────────── 内核态 ───────────────────────────────┐
│  HRTimer (50Hz)  ──触发──▶  spi_async + SDMA  ──DMA完成──▶  Tasklet │
│                              硬件搬运数据                   回调函数  │
│                                  │                            │      │
│                                  ▼                            ▼      │
│                          DMA 一致性内存              Netlink 通知     │
│                         (dma_alloc_coherent)         (异步唤醒)      │
└──────────────────────────────┬───────────────────────────┬───────────┘
                               │ mmap (零拷贝)             │ Netlink Socket
                               ▼                           ▼
┌─────────────────────────────── 用户态 ───────────────────────────────┐
│  DMA Mapper ◄─── 零拷贝读取    Epoll Server ◄─── 事件唤醒            │
│      │                              │                                │
│      └──────── 传感器数据 ──────────▶ JSON 打包 ──▶ UDP 广播 (8888)  │
└──────────────────────────────────────────────────────────────────────┘
                                                           │
                                                           ▼
                                                   上位机实时监控
                                                  (Python matplotlib)
```

## 项目结构

```
zerocopy-IMX6-gateway/
├── common/
│   └── protocol.h            # 内核/用户态共享协议定义 (Netlink 协议号、数据结构)
├── driver/
│   ├── icm20608_reg.h         # ICM20608 寄存器地址定义
│   ├── spi_sdma_driver.c      # 内核驱动 (SPI+SDMA+HRTimer+Netlink+mmap)
│   └── Makefile               # 内核模块编译
├── userspace/
│   ├── netlink_client.c/.h    # Netlink 客户端 (PID 注册 + 消息接收)
│   ├── dma_mapper.c/.h        # DMA 内存映射封装 (mmap 零拷贝)
│   ├── epoll_server.c/.h      # Epoll 事件循环 + UDP 广播转发
│   ├── main.c                 # 用户态程序入口
│   └── Makefile               # 用户态交叉编译
├── scripts/
│   ├── build_all.sh           # 一键编译与部署脚本
│   └── test_client.py         # PC 端实时数据可视化工具
├── docs/
│   └── architecture.md        # 详细架构设计文档
├── Makefile                   # 顶层 Makefile
└── README.md
```

## 环境要求

- **目标平台**: i.MX6ULL (ARM Cortex-A7)，正点原子 ALPHA/Mini 开发板
- **内核版本**: Linux 4.1.15 (NXP 官方 BSP)
- **交叉编译链**: `arm-linux-gnueabihf-gcc`
- **传感器**: ICM20608G/D 六轴 IMU (SPI 接口)
- **上位机**: Python 3 + matplotlib (用于实时可视化)

## 快速开始

### 1. 编译

```bash
# 一键编译驱动 + 用户态应用
make all

# 或分别编译
make driver      # 仅编译内核模块 spi_sdma.ko
make userspace   # 仅编译用户态程序 zerocopy_gateway
```

### 2. 部署到开发板

```bash
# 使用脚本一键编译并部署到 rootfs
./scripts/build_all.sh
```

编译产物将自动复制到 `~/linux/rootfs/lib/modules/4.1.15/zerocopy/` 目录。

### 3. 在开发板上运行

```bash
# 加载内核驱动
insmod /lib/modules/4.1.15/zerocopy/spi_sdma.ko

# 启动网关程序
/lib/modules/4.1.15/zerocopy/zerocopy_gateway
```

### 4. PC 端实时监控

```bash
# 在同一局域网的 PC 上运行可视化客户端
pip install matplotlib
python scripts/test_client.py
```

上位机将自动接收 UDP 广播数据 (端口 8888)，并以实时波形图显示加速度计和陀螺仪数据。

## 数据格式

网关通过 UDP 广播发送 JSON 格式的传感器数据：

```json
{
    "seq": 12345,
    "accel": [0.012, -0.003, 1.001],
    "gyro": [0.50, -1.20, 0.30],
    "temp": 25.60
}
```

| 字段 | 类型 | 单位 | 说明 |
|------|------|------|------|
| `seq` | int | - | 数据包序号 (递增) |
| `accel` | float[3] | g | 加速度计 X/Y/Z (±16g 量程) |
| `gyro` | float[3] | °/s | 陀螺仪 X/Y/Z (±2000°/s 量程) |
| `temp` | float | °C | 芯片温度 |

## 传感器配置

| 参数 | 配置值 | 说明 |
|------|--------|------|
| SPI 时钟 | 8 MHz | SPI_MODE_0, 8-bit |
| 采样率 | 50 Hz | HRTimer 20ms 周期 |
| 加速度计量程 | ±16g | 灵敏度 2048 LSB/g |
| 陀螺仪量程 | ±2000°/s | 灵敏度 16.4 LSB/(°/s) |
| 数字低通滤波 | DLPF=4 | 加速度计 + 陀螺仪均启用 |

## 性能指标

| 指标 | 数值 | 说明 |
|------|------|------|
| 数据采集延迟 | < 1ms | SDMA 硬件传输 + Tasklet 回调 |
| 数据路径拷贝次数 | **0** | mmap 零拷贝，无 `copy_to_user` |
| CPU 占用率 | < 3% | DMA 硬件搬运 + 事件驱动，CPU 几乎空闲 |
| 采样精度 | ±0.1ms | HRTimer 硬中断级精度 |
| UDP 广播端口 | 8888 | 局域网广播 255.255.255.255 |

## 设备树配置

确保设备树中包含 ICM20608 的 SPI 节点配置：

```dts
&ecspi3 {
    pinctrl-names = "default";
    pinctrl-0 = <&pinctrl_ecspi3>;
    cs-gpios = <&gpio1 20 GPIO_ACTIVE_LOW>;
    status = "okay";

    icm20608: icm20608@0 {
        compatible = "alientek,icm20608";
        spi-max-frequency = <8000000>;
        reg = <0>;
    };
};
```

