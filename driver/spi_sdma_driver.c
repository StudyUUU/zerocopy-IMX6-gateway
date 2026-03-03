/*
 * icm20608_cdev.c - ICM20608 SPI 驱动 (现代 Sysfs 规范版)
 *
 * 核心改进：
 * 1. 引入 Attribute Groups 实现设备与 Sysfs 属性的原子化创建。
 * 2. 增加 accel_data, gyro_data, temp_data 三个 Sysfs 只读节点，方便命令行直接调试。
 * 3. 严格遵循 OOP 思想，上下文数据通过 dev_get_drvdata 传递。
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
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "icm20608_reg.h"

#define DRIVER_NAME "alientek_icm20608_driver"
#define CLASS_NAME "alientek_icm20608_class"
#define DEVICE_NAME "icm20608" 
#define MAX_DEVICES 8
#define ICM20608_COMPAT "alientek,icm20608"

static dev_t icm_dev_number;
static struct class* icm_class;

static DECLARE_BITMAP(icm_minor_bitmap, MAX_DEVICES);
static DEFINE_MUTEX(icm_minor_lock);

/* 传感器原始数据结构 */
struct original_data {
    signed int gyro_x_adc;
    signed int gyro_y_adc;
    signed int gyro_z_adc;
    signed int accel_x_adc;
    signed int accel_y_adc;
    signed int accel_z_adc;
    signed int temp_adc;
};

/* 设备私有上下文 */
struct icm_device_data {
    struct cdev cdev;
    struct device* dev_node;
    int minor;
    struct mutex lock; 
    struct spi_device* spi;
    struct original_data data;
};

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

    dev_data->data.accel_x_adc = (signed short)((data[0] << 8) | data[1]);
    dev_data->data.accel_y_adc = (signed short)((data[2] << 8) | data[3]);
    dev_data->data.accel_z_adc = (signed short)((data[4] << 8) | data[5]);
    dev_data->data.temp_adc = (signed short)((data[6] << 8) | data[7]);
    dev_data->data.gyro_x_adc = (signed short)((data[8] << 8) | data[9]);
    dev_data->data.gyro_y_adc = (signed short)((data[10] << 8) | data[11]);
    dev_data->data.gyro_z_adc = (signed short)((data[12] << 8) | data[13]);
}

/* ========================================================================== */
/* [新增] Sysfs 属性组实现 (用于命令行极速调试)                             */
/* ========================================================================== */

static ssize_t accel_data_show(struct device *dev, struct device_attribute *attr, char *buf) {
    struct icm_device_data *priv = dev_get_drvdata(dev);
    
    mutex_lock(&priv->lock);
    icm20608_readdata(priv);
    mutex_unlock(&priv->lock);
    
    return sprintf(buf, "X:%d Y:%d Z:%d\n", priv->data.accel_x_adc, priv->data.accel_y_adc, priv->data.accel_z_adc);
}

static ssize_t gyro_data_show(struct device *dev, struct device_attribute *attr, char *buf) {
    struct icm_device_data *priv = dev_get_drvdata(dev);
    
    mutex_lock(&priv->lock);
    icm20608_readdata(priv);
    mutex_unlock(&priv->lock);
    
    return sprintf(buf, "X:%d Y:%d Z:%d\n", priv->data.gyro_x_adc, priv->data.gyro_y_adc, priv->data.gyro_z_adc);
}

static ssize_t temp_data_show(struct device *dev, struct device_attribute *attr, char *buf) {
    struct icm_device_data *priv = dev_get_drvdata(dev);
    
    mutex_lock(&priv->lock);
    icm20608_readdata(priv);
    mutex_unlock(&priv->lock);
    
    return sprintf(buf, "Temp ADC:%d\n", priv->data.temp_adc);
}

/* 定义只读属性宏 */
static DEVICE_ATTR_RO(accel_data);
static DEVICE_ATTR_RO(gyro_data);
static DEVICE_ATTR_RO(temp_data);

/* 步骤 1：放入属性数组 */
static struct attribute *icm_sysfs_attrs[] = {
    &dev_attr_accel_data.attr,
    &dev_attr_gyro_data.attr,
    &dev_attr_temp_data.attr,
    NULL,
};

/* 步骤 2：封装为属性组 */
static const struct attribute_group icm_attr_group = {
    .attrs = icm_sysfs_attrs,
};

/* 步骤 3：构造供创建函数使用的指针数组 */
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
    signed int data[7];
    struct icm_device_data* dev_data = filp->private_data;

    if (len < sizeof(data)) return -EINVAL;
    if (mutex_lock_interruptible(&dev_data->lock)) return -ERESTARTSYS;

    icm20608_readdata(dev_data);
    data[0] = dev_data->data.gyro_x_adc;
    data[1] = dev_data->data.gyro_y_adc;
    data[2] = dev_data->data.gyro_z_adc;
    data[3] = dev_data->data.accel_x_adc;
    data[4] = dev_data->data.accel_y_adc;
    data[5] = dev_data->data.accel_z_adc;
    data[6] = dev_data->data.temp_adc;

    mutex_unlock(&dev_data->lock);

    if (copy_to_user(buf, data, sizeof(data))) return -EFAULT;
    return sizeof(data); 
}

static int icm20608_release(struct inode* inode, struct file* filp) {
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

    /* * [核心改进] 使用 device_create_with_groups 实现原子性创建
     * 一次性创建设备节点 (/dev/xxx) 和关联的 Sysfs 属性 (/sys/class/...)
     */
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

    /* device_destroy 会自动清理绑定的 Sysfs 属性，无需手动 remove */
    device_destroy(icm_class, devt);
    cdev_del(&dev_data->cdev);

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
    int ret = alloc_chrdev_region(&icm_dev_number, 0, MAX_DEVICES, DEVICE_NAME);
    if (ret < 0) return ret;
    
    icm_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(icm_class)) {
        unregister_chrdev_region(icm_dev_number, MAX_DEVICES);
        return PTR_ERR(icm_class);
    }
    
    ret = spi_register_driver(&icm20608_driver);
    if (ret) {
        class_destroy(icm_class);
        unregister_chrdev_region(icm_dev_number, MAX_DEVICES);
    }
    return ret;
}

static void __exit icm20608_exit(void) {
    spi_unregister_driver(&icm20608_driver);
    class_destroy(icm_class);
    unregister_chrdev_region(icm_dev_number, MAX_DEVICES);
}

module_init(icm20608_init);
module_exit(icm20608_exit);
MODULE_LICENSE("GPL");