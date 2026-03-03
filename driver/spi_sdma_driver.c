/*
 * icm20608_cdev.c - ICM20608 SPI 驱动 (现代 Sysfs 规范版 + 工作队列自动采集 + Netlink 异步通知)
 *
 * 核心改进：
 * 1. [mmap 零拷贝] 预分配 DMA 一致性内存并映射至用户空间。
 * 2. [后台自动采集] 引入 delayed_work，每隔 20ms 自动在后台读取 SPI 数据并写入 DMA 内存。
 * 3. 规避了 Timer 无法休眠的问题，实现了安全的并发访问。
 * 4. [手术 3: Netlink 异步通知] 引入 Netlink，采集完成后主动通知应用层，彻底消除轮询。
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
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/workqueue.h> /* === [手术 2 新增] 工作队列头文件 === */
#include <net/sock.h>        /* === [手术 3 新增] Netlink 核心头文件 === */
#include <linux/netlink.h>   /* === [手术 3 新增] Netlink 协议头文件 === */
#include <linux/skbuff.h>    /* === [手术 3 新增] Socket Buffer 头文件 === */

#include "icm20608_reg.h"
#include "../common/protocol.h" /* === [手术 3 新增] 引入共享协议头文件 === */

#define DRIVER_NAME "alientek_icm20608_driver"
#define CLASS_NAME "alientek_icm20608_class"
#define DEVICE_NAME "icm20608" 
#define MAX_DEVICES 8
#define ICM20608_COMPAT "alientek,icm20608"

static dev_t icm_dev_number;
static struct class* icm_class;

static DECLARE_BITMAP(icm_minor_bitmap, MAX_DEVICES);
static DEFINE_MUTEX(icm_minor_lock);

/* === [手术 3 新增] Netlink 全局资源 === */
static struct sock *icm_nl_sock = NULL;
static int app_pid = -1; /* 记录应用层进程 ID */
static DEFINE_MUTEX(nl_mutex);

/* 传感器原始数据结构 */
/* 注意：这里我们使用 protocol.h 中定义的 struct icm_sensor_data，不再重复定义 */

/* 设备私有上下文 */
struct icm_device_data {
    struct cdev cdev;
    struct device* dev_node;
    int minor;
    struct mutex lock; 
    struct spi_device* spi;
    struct icm_sensor_data data; /* 使用共享协议中的结构体 */

    /* DMA 一致性内存三件套 */
    size_t dma_buf_size;      
    void *dma_buf_virt;       
    dma_addr_t dma_buf_phys;  

    /* === [手术 2 新增] 延迟工作队列，用于后台循环采集 === */
    struct delayed_work dwork;
};

/* ========================================================================== */
/* [手术 3 新增] Netlink 消息发送函数                                           */
/* ========================================================================== */
static void send_netlink_notify(int data_ready_flag)
{
    struct sk_buff *skb;
    struct nlmsghdr *nlh;
    int msg_size = sizeof(int);
    int res;
    int target_pid;

    /* 1. 安全获取目标 PID */
    mutex_lock(&nl_mutex);
    target_pid = app_pid;
    mutex_unlock(&nl_mutex);

    /* 如果用户态没有注册 PID，或者 Netlink Socket 未初始化，则跳过发送 */
    if (target_pid == -1 || !icm_nl_sock) {
        return;
    }

    /* 2. 分配 Socket Buffer (GFP_ATOMIC 允许在软中断或工作队列中使用) */
    skb = nlmsg_new(msg_size, GFP_ATOMIC);
    if (!skb) {
        return;
    }

    /* 3. 构造 Netlink 消息头 */
    nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, msg_size, 0);
    if (!nlh) {
        kfree_skb(skb);
        return;
    }

    /* 4. 填充有效负载数据 (发送一个标志位或计数值) */
    memcpy(nlmsg_data(nlh), &data_ready_flag, msg_size);

    /* 5. 通过单播 (Unicast) 发送给指定的 PID */
    res = nlmsg_unicast(icm_nl_sock, skb, target_pid);
    if (res < 0) {
        /* 发送失败处理，比如应用已退出 */
    }
}

/* ========================================================================== */
/* [手术 3 新增] 接收用户态 Netlink 消息的回调函数                              */
/* 用于用户态程序启动时，主动向内核发送自己的 PID 进行“注册”                     */
/* ========================================================================== */
static void icm_nl_recv_msg(struct sk_buff *skb)
{
    struct nlmsghdr *nlh;

    /* 解析接收到的 Netlink 消息头 */
    nlh = (struct nlmsghdr *)skb->data;

    /* 记录发送方的 PID */
    mutex_lock(&nl_mutex);
    app_pid = nlh->nlmsg_pid;
    mutex_unlock(&nl_mutex);

    pr_info("ICM20608: Netlink registered App PID: %d\n", app_pid);
}

/* === 底层 SPI 读写辅助函数 (保持不变) === */
static int icm20608_read_regs(struct spi_device* spi, u8 reg, void* buf, int len) {
    int ret = -1;
    unsigned char *txdata, *rxdata;
    struct spi_message m;
    struct spi_transfer* t;

    t = kzalloc(sizeof(struct spi_transfer), GFP_KERNEL);
    txdata = kzalloc(sizeof(char) * (len + 1), GFP_KERNEL);
    rxdata = kzalloc(sizeof(char) * (len + 1), GFP_KERNEL);
    if (!t || !txdata || !rxdata) {
        kfree(t); kfree(txdata); kfree(rxdata);
        return -ENOMEM;
    }

    txdata[0] = reg | 0x80;
    t->tx_buf = txdata;
    t->rx_buf = rxdata;
    t->len = len + 1;

    spi_message_init(&m);
    spi_message_add_tail(t, &m);
    ret = spi_sync(spi, &m);
    if (ret >= 0) memcpy(buf, rxdata + 1, len);

    kfree(t); kfree(txdata); kfree(rxdata);
    return ret;
}

static int icm20608_write_regs(struct spi_device* spi, u8 reg, void* buf, int len) {
    int ret = -1;
    unsigned char* txdata;
    struct spi_message m;
    struct spi_transfer* t;

    t = kzalloc(sizeof(struct spi_transfer), GFP_KERNEL);
    txdata = kzalloc(sizeof(char) * (len + 1), GFP_KERNEL);
    if (!t || !txdata) {
        kfree(t); kfree(txdata);
        return -ENOMEM;
    }

    txdata[0] = reg & ~0x80;
    memcpy(txdata + 1, buf, len);
    t->tx_buf = txdata;
    t->rx_buf = NULL;
    t->len = len + 1;

    spi_message_init(&m);
    spi_message_add_tail(t, &m);
    ret = spi_sync(spi, &m);

    kfree(t); kfree(txdata);
    return ret;
}

static unsigned char icm20608_read_onereg(struct spi_device* spi, u8 reg) {
    u8 data = 0;
    icm20608_read_regs(spi, reg, &data, 1);
    return data;
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

static void icm20608_readdata(struct icm_device_data* dev_data) {
    unsigned char data[14] = { 0 };
    icm20608_read_regs(dev_data->spi, ACCEL_XOUT_H, data, 14);

    dev_data->data.accel_x = (signed short)((data[0] << 8) | data[1]);
    dev_data->data.accel_y = (signed short)((data[2] << 8) | data[3]);
    dev_data->data.accel_z = (signed short)((data[4] << 8) | data[5]);
    dev_data->data.temp = (signed short)((data[6] << 8) | data[7]);
    dev_data->data.gyro_x = (signed short)((data[8] << 8) | data[9]);
    dev_data->data.gyro_y = (signed short)((data[10] << 8) | data[11]);
    dev_data->data.gyro_z = (signed short)((data[12] << 8) | data[13]);

    /* 将数据同步拷贝到 DMA 映射区 (供 mmap 读取) */
    if (dev_data->dma_buf_virt) {
        memcpy(dev_data->dma_buf_virt, &dev_data->data, sizeof(struct icm_sensor_data));
    }
}

/* ========================================================================== */
/* [手术 2 新增] 工作队列回调函数 (相当于可以睡眠的定时器)                    */
/* ========================================================================== */
static void icm_work_func(struct work_struct *work)
{
    /* 通过 container_of 找到私有数据 (针对 delayed_work 的标准用法) */
    struct icm_device_data *dev_data = container_of(work, struct icm_device_data, dwork.work);
    static int sync_counter = 0;

    /* 1. 安全地读取数据并填充内存 */
    mutex_lock(&dev_data->lock);
    icm20608_readdata(dev_data);
    mutex_unlock(&dev_data->lock);

    /* === [手术 3 新增] 2. 触发 Netlink 异步通知 === */
    sync_counter++;
    send_netlink_notify(sync_counter);

    /* 3. 重新调度自己，20ms 后再次执行 (实现 50Hz 自动刷新) */
    schedule_delayed_work(&dev_data->dwork, msecs_to_jiffies(20));
}

/* ========================================================================== */
/* Sysfs 属性组实现                                                           */
/* ========================================================================== */

static ssize_t accel_data_show(struct device *dev, struct device_attribute *attr, char *buf) {
    struct icm_device_data *priv = dev_get_drvdata(dev);
    /* 因为后台有 workqueue 在不断更新数据，这里直接打印最新内存即可，无需调用 readdata */
    return sprintf(buf, "X:%d Y:%d Z:%d\n", priv->data.accel_x, priv->data.accel_y, priv->data.accel_z);
}

static ssize_t gyro_data_show(struct device *dev, struct device_attribute *attr, char *buf) {
    struct icm_device_data *priv = dev_get_drvdata(dev);
    return sprintf(buf, "X:%d Y:%d Z:%d\n", priv->data.gyro_x, priv->data.gyro_y, priv->data.gyro_z);
}

static ssize_t temp_data_show(struct device *dev, struct device_attribute *attr, char *buf) {
    struct icm_device_data *priv = dev_get_drvdata(dev);
    return sprintf(buf, "Temp ADC:%d\n", priv->data.temp);
}

static DEVICE_ATTR_RO(accel_data);
static DEVICE_ATTR_RO(gyro_data);
static DEVICE_ATTR_RO(temp_data);

static struct attribute *icm_sysfs_attrs[] = {
    &dev_attr_accel_data.attr,
    &dev_attr_gyro_data.attr,
    &dev_attr_temp_data.attr,
    NULL,
};

static const struct attribute_group icm_attr_group = {
    .attrs = icm_sysfs_attrs,
};

static const struct attribute_group *icm_attr_groups[] = {
    &icm_attr_group,
    NULL,
};

/* === 文件操作 (fops) === */

static int icm20608_open(struct inode* inode, struct file* filp) {
    struct icm_device_data* dev_data = container_of(inode->i_cdev, struct icm_device_data, cdev);
    unsigned char id;

    filp->private_data = dev_data;

    mutex_lock(&dev_data->lock);
    id = icm20608_read_onereg(dev_data->spi, WHO_AM_I);
    if (id != 0xAE) { 
        pr_err("Error: Invalid device ID! (0x%02X)\n", id);
        mutex_unlock(&dev_data->lock);
        return -ENODEV;
    }
    icm20608_reginit(dev_data->spi);
    mutex_unlock(&dev_data->lock);
    return 0;
}

static ssize_t icm20608_read(struct file* filp, char __user* buf, size_t len, loff_t* off) {
    struct icm_device_data* dev_data = filp->private_data;

    if (len < sizeof(struct icm_sensor_data)) return -EINVAL;
    if (mutex_lock_interruptible(&dev_data->lock)) return -ERESTARTSYS;

    /* 手动读取一次 (为了兼容老的 read 接口) */
    icm20608_readdata(dev_data);
    mutex_unlock(&dev_data->lock);

    if (copy_to_user(buf, &dev_data->data, sizeof(struct icm_sensor_data))) return -EFAULT;
    return sizeof(struct icm_sensor_data); 
}

static int icm20608_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct icm_device_data *dev_data = filp->private_data;
    size_t size = vma->vm_end - vma->vm_start;

    if (size > dev_data->dma_buf_size)
        return -EINVAL;

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

    if (remap_pfn_range(vma, vma->vm_start,
                        dev_data->dma_buf_phys >> PAGE_SHIFT,
                        size, vma->vm_page_prot)) {
        return -EAGAIN;
    }
    return 0;
}

static int icm20608_release(struct inode* inode, struct file* filp) {
    filp->private_data = NULL;
    return 0;
}

static const struct file_operations icm_fops = {
    .owner = THIS_MODULE,
    .open = icm20608_open,
    .read = icm20608_read,
    .mmap = icm20608_mmap, 
    .release = icm20608_release,
};

/* === SPI 驱动实现 === */

static int icm20608_probe(struct spi_device* spi) {
    struct device* dev = &spi->dev;
    struct icm_device_data* dev_data;
    int ret, minor;
    dev_t devt;
    const char* device_name_str = NULL;

    mutex_lock(&icm_minor_lock);
    minor = find_first_zero_bit(icm_minor_bitmap, MAX_DEVICES);
    if (minor >= MAX_DEVICES) {
        mutex_unlock(&icm_minor_lock);
        return -ENODEV;
    }
    set_bit(minor, icm_minor_bitmap);
    mutex_unlock(&icm_minor_lock);

    dev_data = devm_kzalloc(dev, sizeof(struct icm_device_data), GFP_KERNEL);
    if (!dev_data) { ret = -ENOMEM; goto fail_clear_bit; }

    dev_data->minor = minor;
    devt = MKDEV(MAJOR(icm_dev_number), minor);
    mutex_init(&dev_data->lock);
    dev_data->spi = spi;

    /* 显式设置 DMA Mask */
    if (!dev->dma_mask) {
        dev->dma_mask = &dev->coherent_dma_mask;
    }
    ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
    if (ret) {
        dev_err(dev, "Failed to set DMA mask\n");
        goto fail_clear_bit;
    }

    /* 分配 DMA 一致性内存 */
    dev_data->dma_buf_size = ICM_DMA_BUF_SIZE; 
    dev_data->dma_buf_virt = dma_alloc_coherent(dev, dev_data->dma_buf_size,
                                                &dev_data->dma_buf_phys, GFP_KERNEL);
    if (!dev_data->dma_buf_virt) {
        dev_err(dev, "Failed to allocate DMA buffer\n");
        ret = -ENOMEM;
        goto fail_clear_bit;
    }
    memset(dev_data->dma_buf_virt, 0, dev_data->dma_buf_size);

    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 8;
    spi->max_speed_hz = 8000000;
    ret = spi_setup(spi);
    if (ret) goto fail_clear_bit;

    cdev_init(&dev_data->cdev, &icm_fops);
    ret = cdev_add(&dev_data->cdev, devt, 1);
    if (ret) goto fail_clear_bit;

    if (spi->dev.of_node) {
        of_property_read_string(spi->dev.of_node, "label", &device_name_str);
    }

    if (device_name_str) {
        dev_data->dev_node = device_create_with_groups(
            icm_class, dev, devt, dev_data, icm_attr_groups, "%s", device_name_str);
    } else {
        dev_data->dev_node = device_create_with_groups(
            icm_class, dev, devt, dev_data, icm_attr_groups, DEVICE_NAME "%d", minor);
    }

    if (IS_ERR(dev_data->dev_node)) {
        ret = PTR_ERR(dev_data->dev_node);
        goto fail_del_cdev;
    }

    spi_set_drvdata(spi, dev_data);

    /* === [手术 2 新增] 初始化并启动工作队列自动采集 === */
    INIT_DELAYED_WORK(&dev_data->dwork, icm_work_func);
    schedule_delayed_work(&dev_data->dwork, msecs_to_jiffies(20)); // 延迟 20ms 后第一次执行

    dev_info(dev, "probed successfully (minor %d)\n", minor);
    return 0;

fail_del_cdev:
    cdev_del(&dev_data->cdev);
fail_clear_bit:
    mutex_lock(&icm_minor_lock);
    clear_bit(minor, icm_minor_bitmap);
    mutex_unlock(&icm_minor_lock);
    return ret;
}

static int icm20608_remove(struct spi_device* spi) {
    struct icm_device_data* dev_data = spi_get_drvdata(spi);
    int minor = dev_data->minor;
    dev_t devt = MKDEV(MAJOR(icm_dev_number), minor);

    /* === [手术 2 新增] 必须先停止工作队列，防止其在内存释放后继续运行导致内核崩溃 === */
    cancel_delayed_work_sync(&dev_data->dwork);

    device_destroy(icm_class, devt);
    cdev_del(&dev_data->cdev);

    if (dev_data->dma_buf_virt) {
        dma_free_coherent(&spi->dev, dev_data->dma_buf_size,
                          dev_data->dma_buf_virt, dev_data->dma_buf_phys);
    }

    mutex_lock(&icm_minor_lock);
    clear_bit(minor, icm_minor_bitmap);
    mutex_unlock(&icm_minor_lock);

    dev_info(&spi->dev, "removed minor %d\n", minor);
    return 0;
}

static const struct of_device_id icm20608_of_match[] = {
    { .compatible = ICM20608_COMPAT },
    {},
};
MODULE_DEVICE_TABLE(of, icm20608_of_match);

static struct spi_driver icm20608_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .owner = THIS_MODULE,
        .of_match_table = icm20608_of_match,
    },
    .probe = icm20608_probe,
    .remove = icm20608_remove,
};

static int __init icm20608_init(void) {
    int ret;
    /* === [手术 3 新增] Netlink 配置结构体 === */
    struct netlink_kernel_cfg cfg = {
        .input = icm_nl_recv_msg, /* 绑定接收用户态消息的回调函数 */
    };

    ret = alloc_chrdev_region(&icm_dev_number, 0, MAX_DEVICES, DEVICE_NAME);
    if (ret < 0) return ret;
    
    icm_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(icm_class)) {
        unregister_chrdev_region(icm_dev_number, MAX_DEVICES);
        return PTR_ERR(icm_class);
    }

    /* === [手术 3 新增] 创建 Netlink 套接字 === */
    icm_nl_sock = netlink_kernel_create(&init_net, NETLINK_ICM_NOTIFY, &cfg);
    if (!icm_nl_sock) {
        pr_err("icm20608: Failed to create netlink socket\n");
        class_destroy(icm_class);
        unregister_chrdev_region(icm_dev_number, MAX_DEVICES);
        return -ENOMEM;
    }
    
    ret = spi_register_driver(&icm20608_driver);
    if (ret) {
        netlink_kernel_release(icm_nl_sock);
        class_destroy(icm_class);
        unregister_chrdev_region(icm_dev_number, MAX_DEVICES);
    }
    return ret;
}

static void __exit icm20608_exit(void) {
    spi_unregister_driver(&icm20608_driver);

    /* === [手术 3 新增] 释放 Netlink 套接字 === */
    if (icm_nl_sock) {
        netlink_kernel_release(icm_nl_sock);
        icm_nl_sock = NULL;
    }

    class_destroy(icm_class);
    unregister_chrdev_region(icm_dev_number, MAX_DEVICES);
}

module_init(icm20608_init);
module_exit(icm20608_exit);
MODULE_LICENSE("GPL");