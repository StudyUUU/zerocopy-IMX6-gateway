/*
 * icm20608_cdev.c - ICM20608 终极版 SPI 驱动
 *
 * 简历核心技术落地：
 * 1. [DMA 硬件搬运] 预分配一致性内存，使用 spi_async + is_dma_mapped=1，强制 SDMA 硬件接管 SPI 传输，解放 CPU。
 * 2. [高精度定时器] 使用 hrtimer 在硬中断上下文实现精确 50Hz 触发。
 * 3. [Tasklet 异步推送] 在 spi_message 的 complete 回调(软中断/Tasklet上下文)中发送 Netlink 通知。
 * 4. [mmap 零拷贝] 解析后的结构化数据直接写入 dma_buf_virt，用户态 mmap 直达物理内存。
 */

#include <linux/bitmap.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/hrtimer.h>   /* 引入高精度定时器 */
#include <linux/ktime.h>
#include <net/sock.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>

#include "icm20608_reg.h"
#include "../common/protocol.h"

#define DRIVER_NAME "alientek_icm20608_driver"
#define CLASS_NAME "alientek_icm20608_class"
#define DEVICE_NAME "alientek_icm20608" 
#define MAX_DEVICES 8
#define ICM20608_COMPAT "alientek,icm20608"
#define SPI_XFER_LEN 15 /* 1字节寄存器地址 + 14字节数据 */

static dev_t icm_dev_number;
static struct class* icm_class;

static DECLARE_BITMAP(icm_minor_bitmap, MAX_DEVICES);
static DEFINE_MUTEX(icm_minor_lock);

/* Netlink 全局资源 */
static struct sock *icm_nl_sock = NULL;
static int app_pid = -1;
static DEFINE_SPINLOCK(nl_lock); /* 中断上下文使用自旋锁 */

/* 设备私有上下文 */
struct icm_device_data {
    struct cdev cdev;
    struct device* dev_node;
    int minor;
    struct mutex lock; 
    struct spi_device* spi;

    /* 1. 供用户态 mmap 映射的零拷贝内存 */
    size_t dma_buf_size;      
    void *dma_buf_virt;       
    dma_addr_t dma_buf_phys;  

    /* 2. 供底层 SPI SDMA 硬件搬运使用的专有缓冲区 */
    u8 *spi_tx_buf;
    dma_addr_t spi_tx_dma;
    u8 *spi_rx_buf;
    dma_addr_t spi_rx_dma;

    /* 3. 异步传输核心：消息结构与高精度定时器 */
    struct spi_message msg;
    struct spi_transfer xfer;
    struct hrtimer timer;
};

/* ========================================================================== */
/* Netlink 消息发送函数 (运行于中断下半部)                                    */
/* ========================================================================== */
static void send_netlink_notify(int sync_val)
{
    struct sk_buff *skb;
    struct nlmsghdr *nlh;
    int target_pid;

    spin_lock(&nl_lock);
    target_pid = app_pid;
    spin_unlock(&nl_lock);

    if (target_pid == -1 || !icm_nl_sock) return;

    /* GFP_ATOMIC 是灵魂，允许在中断上下文中分配内存 */
    skb = nlmsg_new(sizeof(int), GFP_ATOMIC);
    if (!skb) return;

    nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, sizeof(int), 0);
    memcpy(nlmsg_data(nlh), &sync_val, sizeof(int));
    
    nlmsg_unicast(icm_nl_sock, skb, target_pid);
}

static void icm_nl_recv_msg(struct sk_buff *skb)
{
    struct nlmsghdr *nlh = (struct nlmsghdr *)skb->data;
    spin_lock(&nl_lock);
    app_pid = nlh->nlmsg_pid;
    spin_unlock(&nl_lock);
    pr_info("ICM20608: Netlink registered App PID: %d\n", app_pid);
}

/* ========================================================================== */
/* [终极奥义] 中断下半部 (Tasklet) 的 SDMA 完成回调函数                       */
/* 硬件搬运完成时触发。负责解析数据并推给应用层。                             */
/* ========================================================================== */
static void icm_spi_complete_callback(void *context)
{
    struct icm_device_data *dev_data = context;
    /* 跳过第一个虚拟字节，后面的 14 字节才是真实数据 */
    u8 *data = dev_data->spi_rx_buf + 1; 
    struct icm_sensor_data *mapped_data;
    static int sync_counter = 0;

    /* 直接向 mmap 映射区写入数据 (大端转小端) */
    mapped_data = (struct icm_sensor_data *)dev_data->dma_buf_virt;
    if (mapped_data) {
        mapped_data->accel_x = (s16)((data[0] << 8) | data[1]);
        mapped_data->accel_y = (s16)((data[2] << 8) | data[3]);
        mapped_data->accel_z = (s16)((data[4] << 8) | data[5]);
        mapped_data->temp    = (s16)((data[6] << 8) | data[7]);
        mapped_data->gyro_x  = (s16)((data[8] << 8) | data[9]);
        mapped_data->gyro_y  = (s16)((data[10] << 8) | data[11]);
        mapped_data->gyro_z  = (s16)((data[12] << 8) | data[13]);
    }

    /* 触发 Netlink，异步唤醒用户态的 Epoll */
    sync_counter++;
    send_netlink_notify(sync_counter);
}

/* ========================================================================== */
/* [终极奥义] 高精度定时器 (HRTimer) 回调                                     */
/* 运行在硬中断上下文，利用 spi_async 发起硬件 DMA 传输，瞬间返回，0 阻塞。   */
/* ========================================================================== */
static enum hrtimer_restart icm_hrtimer_func(struct hrtimer *timer)
{
    struct icm_device_data *dev_data = container_of(timer, struct icm_device_data, timer);

    /* 配置发送指令 (读取 ACCEL_XOUT_H) */
    dev_data->spi_tx_buf[0] = ACCEL_XOUT_H | 0x80;

    /* 发起异步非阻塞 SPI 传输 (底层将调用 SDMA) */
    spi_async(dev_data->spi, &dev_data->msg);

    /* 重置定时器，20ms (20,000,000 纳秒) 后再次触发 */
    hrtimer_forward_now(timer, ns_to_ktime(20000000));
    return HRTIMER_RESTART;
}

/* === 初始化与配置辅助函数 (保持同步阻塞，仅在初始化调用) === */
static int icm20608_write_regs(struct spi_device* spi, u8 reg, void* buf, int len) {
    int ret;
    unsigned char* txdata;
    struct spi_message m;
    struct spi_transfer* t;

    t = kzalloc(sizeof(struct spi_transfer), GFP_KERNEL);
    txdata = kzalloc(sizeof(char) * (len + 1), GFP_KERNEL);
    if (!t || !txdata) { kfree(t); kfree(txdata); return -ENOMEM; }

    txdata[0] = reg & ~0x80; memcpy(txdata + 1, buf, len);
    t->tx_buf = txdata; t->len = len + 1;
    spi_message_init(&m); spi_message_add_tail(t, &m);
    ret = spi_sync(spi, &m);
    kfree(t); kfree(txdata);
    return ret;
}

static void icm20608_write_onereg(struct spi_device* spi, u8 reg, u8 data) {
    icm20608_write_regs(spi, reg, &data, 1);
}

static void icm20608_reginit(struct spi_device* spi) {
    icm20608_write_onereg(spi, PWR_MGMT_1, 0x80); mdelay(50);
    icm20608_write_onereg(spi, PWR_MGMT_1, 0x01); mdelay(50);
    icm20608_write_onereg(spi, SMLPRT_DIV, 0x00);
    icm20608_write_onereg(spi, GYRO_CONFIG, 0x18);
    icm20608_write_onereg(spi, ACCEL_CONFIG, 0x18);
    icm20608_write_onereg(spi, DLPF_CFG, 0x04);
    icm20608_write_onereg(spi, A_DLPF_CFG, 0x04);
    icm20608_write_onereg(spi, PWR_MGMT_2, 0x00);
    icm20608_write_onereg(spi, GYRO_CYCLE, 0x00);
    icm20608_write_onereg(spi, FIFO_EN, 0x00);
}

/* === 文件操作接口 === */
static int icm20608_open(struct inode* inode, struct file* filp) {
    struct icm_device_data* dev_data = container_of(inode->i_cdev, struct icm_device_data, cdev);
    filp->private_data = dev_data;
    return 0;
}

static int icm20608_mmap(struct file *filp, struct vm_area_struct *vma) {
    struct icm_device_data *dev_data = filp->private_data;
    size_t size = vma->vm_end - vma->vm_start;
    if (size > dev_data->dma_buf_size) return -EINVAL;
    
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    return remap_pfn_range(vma, vma->vm_start, dev_data->dma_buf_phys >> PAGE_SHIFT,
                           size, vma->vm_page_prot);
}

static const struct file_operations icm_fops = {
    .owner = THIS_MODULE,
    .open = icm20608_open,
    .mmap = icm20608_mmap, 
};

/* === SPI Probe 实现 === */
static int icm20608_probe(struct spi_device* spi) {
    struct device* dev = &spi->dev;
    struct icm_device_data* dev_data;
    int minor;

    mutex_lock(&icm_minor_lock);
    minor = find_first_zero_bit(icm_minor_bitmap, MAX_DEVICES);
    set_bit(minor, icm_minor_bitmap);
    mutex_unlock(&icm_minor_lock);

    dev_data = devm_kzalloc(dev, sizeof(struct icm_device_data), GFP_KERNEL);
    dev_data->minor = minor;
    dev_data->spi = spi;
    mutex_init(&dev_data->lock);

    /* 1. 配置用户态 mmap 映射区内存 */
    dev->dma_mask = &dev->coherent_dma_mask;
    dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
    dev_data->dma_buf_size = ICM_DMA_BUF_SIZE; 
    dev_data->dma_buf_virt = dma_alloc_coherent(dev, dev_data->dma_buf_size, &dev_data->dma_buf_phys, GFP_KERNEL);

    /* 2. 配置 SPI底层专用的 SDMA 一致性内存 (必须是物理连续的，供硬件直接读写) */
    dev_data->spi_tx_buf = dma_alloc_coherent(dev, SPI_XFER_LEN, &dev_data->spi_tx_dma, GFP_KERNEL);
    dev_data->spi_rx_buf = dma_alloc_coherent(dev, SPI_XFER_LEN, &dev_data->spi_rx_dma, GFP_KERNEL);

    /* 初始化传感器寄存器 */
    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 8;
    spi->max_speed_hz = 8000000;
    spi_setup(spi);
    icm20608_reginit(spi);

    /* 3. 准备 spi_async 异步消息体 */
    spi_message_init(&dev_data->msg);
    dev_data->msg.is_dma_mapped = 1; /* 简历核心：强制底层启动硬件 SDMA */
    dev_data->msg.complete = icm_spi_complete_callback; /* 挂载 Tasklet 软中断回调 */
    dev_data->msg.context = dev_data;

    dev_data->xfer.tx_buf = dev_data->spi_tx_buf;
    dev_data->xfer.tx_dma = dev_data->spi_tx_dma;
    dev_data->xfer.rx_buf = dev_data->spi_rx_buf;
    dev_data->xfer.rx_dma = dev_data->spi_rx_dma;
    dev_data->xfer.len = SPI_XFER_LEN;
    spi_message_add_tail(&dev_data->xfer, &dev_data->msg);

    /* 注册字符设备节点 */
    cdev_init(&dev_data->cdev, &icm_fops);
    cdev_add(&dev_data->cdev, MKDEV(MAJOR(icm_dev_number), minor), 1);
    dev_data->dev_node = device_create(icm_class, dev, MKDEV(MAJOR(icm_dev_number), minor), dev_data, DEVICE_NAME);
    spi_set_drvdata(spi, dev_data);

    /* 4. 启动 HRTimer 高精度定时器 (50Hz) */
    hrtimer_init(&dev_data->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    dev_data->timer.function = icm_hrtimer_func;
    hrtimer_start(&dev_data->timer, ns_to_ktime(20000000), HRTIMER_MODE_REL);

    dev_info(dev, "Ultra-High Performance SDMA Probed (minor %d)\n", minor);
    return 0;
}

static int icm20608_remove(struct spi_device* spi) {
    struct icm_device_data* dev_data = spi_get_drvdata(spi);

    /* 安全停止定时器与取消可能的挂起传输 */
    hrtimer_cancel(&dev_data->timer);

    device_destroy(icm_class, MKDEV(MAJOR(icm_dev_number), dev_data->minor));
    cdev_del(&dev_data->cdev);

    /* 释放所有 DMA 内存 */
    dma_free_coherent(&spi->dev, dev_data->dma_buf_size, dev_data->dma_buf_virt, dev_data->dma_buf_phys);
    dma_free_coherent(&spi->dev, SPI_XFER_LEN, dev_data->spi_tx_buf, dev_data->spi_tx_dma);
    dma_free_coherent(&spi->dev, SPI_XFER_LEN, dev_data->spi_rx_buf, dev_data->spi_rx_dma);

    mutex_lock(&icm_minor_lock);
    clear_bit(dev_data->minor, icm_minor_bitmap);
    mutex_unlock(&icm_minor_lock);

    return 0;
}

static const struct of_device_id icm20608_of_match[] = { { .compatible = ICM20608_COMPAT }, {} };
MODULE_DEVICE_TABLE(of, icm20608_of_match);

static struct spi_driver icm20608_driver = {
    .driver = { .name = DRIVER_NAME, .owner = THIS_MODULE, .of_match_table = icm20608_of_match, },
    .probe = icm20608_probe,
    .remove = icm20608_remove,
};

static int __init icm20608_init(void) {
    struct netlink_kernel_cfg cfg = { .input = icm_nl_recv_msg, };
    alloc_chrdev_region(&icm_dev_number, 0, MAX_DEVICES, DEVICE_NAME);
    icm_class = class_create(THIS_MODULE, CLASS_NAME);
    icm_nl_sock = netlink_kernel_create(&init_net, NETLINK_ICM_NOTIFY, &cfg);
    return spi_register_driver(&icm20608_driver);
}

static void __exit icm20608_exit(void) {
    spi_unregister_driver(&icm20608_driver);
    if (icm_nl_sock) netlink_kernel_release(icm_nl_sock);
    class_destroy(icm_class);
    unregister_chrdev_region(icm_dev_number, MAX_DEVICES);
}

module_init(icm20608_init);
module_exit(icm20608_exit);
MODULE_LICENSE("GPL");