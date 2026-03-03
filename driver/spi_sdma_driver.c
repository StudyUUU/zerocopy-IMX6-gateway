/*
 * icm20608_cdev.c - ICM20608 SPI 驱动 (标准多设备模板版)
 *
 * 1. 状态私有化: spi_device 和 data_buf 移入私有结构体。
 * 2. 多设备支持: 动态分配次设备号。
 * 3. 稳定命名: 使用DTS "label" 属性创建 /dev 节点。
 */

#include <linux/bitmap.h>
#include <linux/cdev.h>
#include <linux/delay.h> /* mdelay/msleep/udelay */
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
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

/* 包含您提供的寄存器定义 */
#include "icm20608_reg.h"

/* === 1. 全局资源 (cdev 管理) === */
#define DRIVER_NAME "alientek_icm20608_driver"
#define CLASS_NAME "alientek_icm20608_class"
#define DEVICE_NAME "icm20608" /* 【回退时使用】 */
#define MAX_DEVICES 8
#define ICM20608_COMPAT "alientek,icm20608"

static dev_t icm_dev_number;
static struct class* icm_class;
static const struct file_operations icm_fops;

static DECLARE_BITMAP(icm_minor_bitmap, MAX_DEVICES);
static DEFINE_MUTEX(icm_minor_lock);

/* === 2. 设备私有上下文 === */

/* (来自原驱动) 传感器原始数据 */
struct original_data {
    signed int gyro_x_adc;
    signed int gyro_y_adc;
    signed int gyro_z_adc;
    signed int accel_x_adc;
    signed int accel_y_adc;
    signed int accel_z_adc;
    signed int temp_adc;
};

/* 【新】私有数据结构体 */
struct icm_device_data {
    struct cdev cdev;
    struct device* dev_node;
    int minor;
    struct mutex lock; /* 【新】增加互斥锁 */

    /* 【总线特定】SPI 句柄 (从全局移入) */
    struct spi_device* spi;

    /* 【状态私有化】数据缓冲区 (从全局移入) */
    /* 注意: 直接嵌入结构体, 而不是指针, 更简洁 */
    struct original_data data;
};

/* === 3. 辅助函数 (fops) === */
/* (SPI 读写函数 'icm20608_read/write_regs' 是无状态的, 无需修改) */
/* (我们直接复制原驱动的 SPI 辅助函数) */

static int icm20608_read_regs(struct spi_device* spi, u8 reg, void* buf, int len)
{
    int ret = -1;
    unsigned char *txdata, *rxdata;
    struct spi_message m;
    struct spi_transfer* t;

    t = kzalloc(sizeof(struct spi_transfer), GFP_KERNEL);
    if (!t)
        return -ENOMEM;
    txdata = kzalloc(sizeof(char) * (len + 1), GFP_KERNEL);
    if (!txdata) {
        kfree(t);
        return -ENOMEM;
    }
    rxdata = kzalloc(sizeof(char) * (len + 1), GFP_KERNEL);
    if (!rxdata) {
        kfree(t);
        kfree(txdata);
        return -ENOMEM;
    }

    txdata[0] = reg | 0x80;
    t->tx_buf = txdata;
    t->rx_buf = rxdata;
    t->len = len + 1;

    spi_message_init(&m);
    spi_message_add_tail(t, &m);
    ret = spi_sync(spi, &m);
    if (ret < 0) {
        pr_err("spi_sync failed in read_regs\n");
    } else {
        memcpy(buf, rxdata + 1, len);
    }

    kfree(t);
    kfree(txdata);
    kfree(rxdata);
    return ret;
}

static int icm20608_write_regs(struct spi_device* spi, u8 reg, void* buf, int len)
{
    int ret = -1;
    unsigned char* txdata;
    struct spi_message m;
    struct spi_transfer* t;

    t = kzalloc(sizeof(struct spi_transfer), GFP_KERNEL);
    if (!t)
        return -ENOMEM;
    txdata = kzalloc(sizeof(char) * (len + 1), GFP_KERNEL);
    if (!txdata) {
        kfree(t);
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
    if (ret < 0) {
        pr_err("spi_sync failed in write_regs\n");
    }

    kfree(t);
    kfree(txdata);
    return ret;
}

static unsigned char icm20608_read_onereg(struct spi_device* spi, u8 reg)
{
    u8 data = 0;
    icm20608_read_regs(spi, reg, &data, 1);
    return data;
}

static void icm20608_write_onereg(struct spi_device* spi, u8 reg, u8 data)
{
    icm20608_write_regs(spi, reg, &data, 1);
}

/* 【修改】reginit, 接受 spi 句柄 */
static void icm20608_reginit(struct spi_device* spi)
{
    icm20608_write_onereg(spi, PWR_MGMT_1, 0x80);
    mdelay(50);
    icm20608_write_onereg(spi, PWR_MGMT_1, 0x01);
    mdelay(50);
    icm20608_write_onereg(spi, SMLPRT_DIV, 0x00);
    icm20608_write_onereg(spi, GYRO_CONFIG, 0x18);
    icm20608_write_onereg(spi, ACCEL_CONFIG, 0x18);
    icm20608_write_onereg(spi, DLPF_CFG, 0x04);
    icm20608_write_onereg(spi, A_DLPF_CFG, 0x04);
    icm20608_write_onereg(spi, PWR_MGMT_2, 0x00);
    icm20608_write_onereg(spi, GYRO_CYCLE, 0x00);
    icm20608_write_onereg(spi, FIFO_EN, 0x00);
}

/* 【修改】readdata, 接受私有数据指针 */
static void icm20608_readdata(struct icm_device_data* dev_data)
{
    unsigned char data[14] = { 0 };
    /* 使用私有的 spi 句柄 */
    icm20608_read_regs(dev_data->spi, ACCEL_XOUT_H, data, 14);

    /* 写入私有的 data 缓冲区 */
    dev_data->data.accel_x_adc = (signed short)((data[0] << 8) | data[1]);
    dev_data->data.accel_y_adc = (signed short)((data[2] << 8) | data[3]);
    dev_data->data.accel_z_adc = (signed short)((data[4] << 8) | data[5]);
    dev_data->data.temp_adc = (signed short)((data[6] << 8) | data[7]);
    dev_data->data.gyro_x_adc = (signed short)((data[8] << 8) | data[9]);
    dev_data->data.gyro_y_adc = (signed short)((data[10] << 8) | data[11]);
    dev_data->data.gyro_z_adc = (signed short)((data[12] << 8) | data[13]);
}

/* === 4. 文件操作 (fops) === */

static int icm20608_open(struct inode* inode, struct file* filp)
{
    struct icm_device_data* dev_data;
    unsigned char id;

    /* 标准模板: container_of 获取私有数据 */
    dev_data = container_of(inode->i_cdev, struct icm_device_data, cdev);
    /* 标准模板: 存入 filp */
    filp->private_data = dev_data;

    pr_info("%s: opened minor %d (%s)\n", DRIVER_NAME, dev_data->minor,
        dev_data->dev_node ? dev_name(dev_data->dev_node) : "unknown");

    /* 【修改】使用私有 spi 句柄 */
    mutex_lock(&dev_data->lock);

    id = icm20608_read_onereg(dev_data->spi, WHO_AM_I);
    if (id != 0xAE) { /* (假设 0xAE 是 ID, 保持原逻辑) */
        pr_err("Error: Invalid device ID! (0x%02X)\n", id);
        mutex_unlock(&dev_data->lock);
        return -ENODEV;
    }

    icm20608_reginit(dev_data->spi);

    mutex_unlock(&dev_data->lock);
    return 0;
}

static ssize_t icm20608_read(struct file* filp, char __user* buf, size_t len, loff_t* off)
{
    signed int data[7];
    long err = 0;
    /* 标准模板: 从 filp 获取私有数据 */
    struct icm_device_data* dev_data = filp->private_data;

    if (len < sizeof(data))
        return -EINVAL;

    if (mutex_lock_interruptible(&dev_data->lock))
        return -ERESTARTSYS;

    /* 【修改】使用私有数据 */
    icm20608_readdata(dev_data);
    data[0] = dev_data->data.gyro_x_adc;
    data[1] = dev_data->data.gyro_y_adc;
    data[2] = dev_data->data.gyro_z_adc;
    data[3] = dev_data->data.accel_x_adc;
    data[4] = dev_data->data.accel_y_adc;
    data[5] = dev_data->data.accel_z_adc;
    data[6] = dev_data->data.temp_adc;

    mutex_unlock(&dev_data->lock);

    err = copy_to_user(buf, data, sizeof(data));
    if (err)
        return -EFAULT;

    return sizeof(data); /* 【修复】返回读取的字节数 */
}

static int icm20608_release(struct inode* inode, struct file* filp)
{
    filp->private_data = NULL;
    return 0;
}

static const struct file_operations icm_fops = {
    .owner = THIS_MODULE,
    .open = icm20608_open,
    .read = icm20608_read,
    .release = icm20608_release,
};

/* === 5. 【总线特定】SPI 驱动实现 === */

static int icm20608_probe(struct spi_device* spi)
{
    struct device* dev = &spi->dev;
    struct icm_device_data* dev_data;
    int ret, minor;
    dev_t devt;
    const char* device_name_str = NULL;

    /* 1. 分配次设备号 */
    mutex_lock(&icm_minor_lock);
    minor = find_first_zero_bit(icm_minor_bitmap, MAX_DEVICES);
    if (minor >= MAX_DEVICES) {
        dev_err(dev, "No free minor numbers available\n");
        mutex_unlock(&icm_minor_lock);
        return -ENODEV;
    }
    set_bit(minor, icm_minor_bitmap);
    mutex_unlock(&icm_minor_lock);

    /* 2. 分配私有上下文 */
    dev_data = devm_kzalloc(dev, sizeof(struct icm_device_data), GFP_KERNEL);
    if (!dev_data) {
        ret = -ENOMEM;
        goto fail_clear_bit;
    }

    /* 3. 初始化私有数据 (cdev + SPI) */
    dev_data->minor = minor;
    devt = MKDEV(MAJOR(icm_dev_number), minor);
    mutex_init(&dev_data->lock);

    /* 【修改】保存私有 SPI 句柄 */
    dev_data->spi = spi;

    /* 4. SPI 硬件设置 (来自原 probe) */
    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 8;
    spi->max_speed_hz = 8000000;
    ret = spi_setup(spi);
    if (ret) {
        dev_err(dev, "Failed to setup SPI device\n");
        goto fail_clear_bit;
    }

    /* 5. CDEV 注册 */
    cdev_init(&dev_data->cdev, &icm_fops);
    ret = cdev_add(&dev_data->cdev, devt, 1);
    if (ret) {
        dev_err(dev, "cdev_add failed: %d\n", ret);
        goto fail_clear_bit;
    }

    /* 6. 创建 /dev 节点 (使用 label) */
    if (spi->dev.of_node) {
        ret = of_property_read_string(spi->dev.of_node, "label", &device_name_str);
        if (ret < 0) {
            dev_warn(dev, "No 'label' property, falling back to %s%d\n", DEVICE_NAME, minor);
            device_name_str = NULL;
        }
    }

    if (device_name_str) {
        dev_data->dev_node = device_create(icm_class, dev, devt, dev_data,
            "%s", device_name_str);
    } else {
        dev_data->dev_node = device_create(icm_class, dev, devt, dev_data,
            DEVICE_NAME "%d", minor);
    }

    if (IS_ERR(dev_data->dev_node)) {
        ret = PTR_ERR(dev_data->dev_node);
        dev_err(dev, "Failed to create /dev node: %d\n", ret);
        goto fail_del_cdev;
    }

    /* 7. 保存私有数据 */
    spi_set_drvdata(spi, dev_data);

    dev_info(dev, "probed successfully (minor %d)\n", minor);
    return 0;

/* 错误回滚 */
fail_del_cdev:
    cdev_del(&dev_data->cdev);
fail_clear_bit:
    mutex_lock(&icm_minor_lock);
    clear_bit(minor, icm_minor_bitmap);
    mutex_unlock(&icm_minor_lock);
    return ret;
}

static int icm20608_remove(struct spi_device* spi)
{
    struct icm_device_data* dev_data = spi_get_drvdata(spi);
    int minor = dev_data->minor;
    dev_t devt = MKDEV(MAJOR(icm_dev_number), minor);

    /* 1. 销毁 /dev 节点 */
    device_destroy(icm_class, devt);

    /* 2. 删除 cdev */
    cdev_del(&dev_data->cdev);

    /* 3. 释放次设备号 */
    mutex_lock(&icm_minor_lock);
    clear_bit(minor, icm_minor_bitmap);
    mutex_unlock(&icm_minor_lock);

    /* (devm_kzalloc 分配的 dev_data 会自动释放) */
    dev_info(&spi->dev, "removed minor %d\n", minor);
    return 0;
}

/* (DTS 匹配表, 保持不变) */
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

/* === 6. 模块入口/出口 === */

static int __init icm20608_init(void)
{
    int ret;
    pr_info("%s: loading...\n", DRIVER_NAME);

    /* 【修改】使用 alloc_chrdev_region */
    ret = alloc_chrdev_region(&icm_dev_number, 0, MAX_DEVICES, DEVICE_NAME);
    if (ret < 0) {
        pr_err("Failed to alloc chrdev region: %d\n", ret);
        return ret;
    }

    /* 【修改】重命名 class */
    icm_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(icm_class)) {
        ret = PTR_ERR(icm_class);
        pr_err("class_create failed: %d\n", ret);
        goto fail_unreg_chrdev;
    }

    ret = spi_register_driver(&icm20608_driver);
    if (ret) {
        pr_err("spi_register_driver failed: %d\n", ret);
        goto fail_destroy_class;
    }

    pr_info("%s: loaded, major=%d\n", DRIVER_NAME, MAJOR(icm_dev_number));
    return 0;

fail_destroy_class:
    class_destroy(icm_class);
fail_unreg_chrdev:
    unregister_chrdev_region(icm_dev_number, MAX_DEVICES);
    return ret;
}

static void __exit icm20608_exit(void)
{
    spi_unregister_driver(&icm20608_driver);
    class_destroy(icm_class);
    unregister_chrdev_region(icm_dev_number, MAX_DEVICES);
    pr_info("%s: unloaded\n", DRIVER_NAME);
}

module_init(icm20608_init);
module_exit(icm20608_exit);
MODULE_LICENSE("GPL");