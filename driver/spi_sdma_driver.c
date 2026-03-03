/*
 * SPI SDMA Driver for i.MX6ULL
 * 
 * Description: SPI character device driver with SDMA DMA support
 * Author: Your Name
 * Date: 2026-03-03
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/spi/spi.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include "netlink_comm.h"

#define DRIVER_NAME "spi_sdma"
#define DMA_BUFFER_SIZE (4096 * 4)  /* 16KB DMA buffer */
#define MAX_TRANSFER_SIZE 4096

/* Device private data structure */
struct spi_sdma_dev {
    dev_t devno;
    struct cdev cdev;
    struct class *class;
    struct device *device;
    struct spi_device *spi;
    
    /* DMA resources */
    void *dma_buffer_virt;          /* Virtual address of DMA buffer */
    dma_addr_t dma_buffer_phys;     /* Physical address of DMA buffer */
    struct dma_chan *dma_chan;      /* DMA channel */
    struct completion dma_complete;  /* DMA completion */
    
    /* Synchronization */
    spinlock_t lock;
    wait_queue_head_t wait_queue;
    atomic_t data_ready;
    
    /* Statistics */
    unsigned long transfer_count;
    unsigned long error_count;
};

static struct spi_sdma_dev *g_spi_dev = NULL;
static int major = 0;

/* DMA callback function - runs in interrupt context */
static void spi_dma_callback(void *data)
{
    struct spi_sdma_dev *dev = (struct spi_sdma_dev *)data;
    
    atomic_set(&dev->data_ready, 1);
    complete(&dev->dma_complete);
    
    /* Schedule tasklet to send Netlink notification */
    netlink_notify_data_ready(dev->transfer_count, DMA_BUFFER_SIZE);
    
    dev->transfer_count++;
    
    wake_up_interruptible(&dev->wait_queue);
}

/* Setup and submit DMA transfer */
static int spi_submit_dma_transfer(struct spi_sdma_dev *dev, size_t len)
{
    struct dma_async_tx_descriptor *desc;
    dma_cookie_t cookie;
    enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
    
    if (!dev->dma_chan) {
        dev_err(dev->device, "DMA channel not available\n");
        return -ENODEV;
    }
    
    /* Prepare DMA descriptor */
    desc = dmaengine_prep_slave_single(dev->dma_chan,
                                       dev->dma_buffer_phys,
                                       len,
                                       DMA_DEV_TO_MEM,
                                       flags);
    if (!desc) {
        dev_err(dev->device, "Failed to prepare DMA descriptor\n");
        return -ENOMEM;
    }
    
    desc->callback = spi_dma_callback;
    desc->callback_param = dev;
    
    /* Submit DMA transfer */
    cookie = dmaengine_submit(desc);
    if (dma_submit_error(cookie)) {
        dev_err(dev->device, "Failed to submit DMA\n");
        return -EIO;
    }
    
    dma_async_issue_pending(dev->dma_chan);
    
    return 0;
}

/* Character device: mmap operation */
static int spi_sdma_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct spi_sdma_dev *dev = filp->private_data;
    unsigned long size = vma->vm_end - vma->vm_start;
    
    if (size > DMA_BUFFER_SIZE) {
        dev_err(dev->device, "mmap size too large\n");
        return -EINVAL;
    }
    
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    
    if (remap_pfn_range(vma, vma->vm_start,
                        dev->dma_buffer_phys >> PAGE_SHIFT,
                        size, vma->vm_page_prot)) {
        dev_err(dev->device, "remap_pfn_range failed\n");
        return -EAGAIN;
    }
    
    return 0;
}

/* Character device: open operation */
static int spi_sdma_open(struct inode *inode, struct file *filp)
{
    struct spi_sdma_dev *dev = container_of(inode->i_cdev, struct spi_sdma_dev, cdev);
    filp->private_data = dev;
    
    dev_info(dev->device, "Device opened\n");
    return 0;
}

/* Character device: release operation */
static int spi_sdma_release(struct inode *inode, struct file *filp)
{
    struct spi_sdma_dev *dev = filp->private_data;
    dev_info(dev->device, "Device closed\n");
    return 0;
}

/* Character device: ioctl operation */
static long spi_sdma_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct spi_sdma_dev *dev = filp->private_data;
    int ret = 0;
    
    switch (cmd) {
    case 0x01: /* Start DMA transfer */
        reinit_completion(&dev->dma_complete);
        atomic_set(&dev->data_ready, 0);
        ret = spi_submit_dma_transfer(dev, MAX_TRANSFER_SIZE);
        break;
        
    case 0x02: /* Get buffer info */
        /* Return DMA buffer size */
        return DMA_BUFFER_SIZE;
        
    default:
        ret = -EINVAL;
        break;
    }
    
    return ret;
}

static const struct file_operations spi_sdma_fops = {
    .owner = THIS_MODULE,
    .open = spi_sdma_open,
    .release = spi_sdma_release,
    .mmap = spi_sdma_mmap,
    .unlocked_ioctl = spi_sdma_ioctl,
};

/* SPI device probe */
static int spi_sdma_probe(struct spi_device *spi)
{
    struct spi_sdma_dev *dev;
    int ret;
    
    dev_info(&spi->dev, "Probing SPI SDMA driver\n");
    
    /* Allocate device structure */
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;
    
    dev->spi = spi;
    spi_set_drvdata(spi, dev);
    g_spi_dev = dev;
    
    /* Initialize synchronization primitives */
    spin_lock_init(&dev->lock);
    init_waitqueue_head(&dev->wait_queue);
    init_completion(&dev->dma_complete);
    atomic_set(&dev->data_ready, 0);
    
    /* Allocate coherent DMA buffer */
    dev->dma_buffer_virt = dma_alloc_coherent(&spi->dev, DMA_BUFFER_SIZE,
                                              &dev->dma_buffer_phys, GFP_KERNEL);
    if (!dev->dma_buffer_virt) {
        dev_err(&spi->dev, "Failed to allocate DMA buffer\n");
        ret = -ENOMEM;
        goto err_free_dev;
    }
    
    dev_info(&spi->dev, "DMA buffer allocated: virt=%p phys=%pad size=%d\n",
             dev->dma_buffer_virt, &dev->dma_buffer_phys, DMA_BUFFER_SIZE);
    
    /* Request DMA channel */
    dev->dma_chan = dma_request_chan(&spi->dev, "rx");
    if (IS_ERR(dev->dma_chan)) {
        dev_err(&spi->dev, "Failed to request DMA channel\n");
        ret = PTR_ERR(dev->dma_chan);
        dev->dma_chan = NULL;
        goto err_free_dma;
    }
    
    /* Register character device */
    ret = alloc_chrdev_region(&dev->devno, 0, 1, DRIVER_NAME);
    if (ret < 0) {
        dev_err(&spi->dev, "Failed to allocate device number\n");
        goto err_release_dma;
    }
    major = MAJOR(dev->devno);
    
    cdev_init(&dev->cdev, &spi_sdma_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->devno, 1);
    if (ret) {
        dev_err(&spi->dev, "Failed to add cdev\n");
        goto err_unregister_chrdev;
    }
    
    /* Create device class and device node */
    dev->class = class_create(THIS_MODULE, DRIVER_NAME);
    if (IS_ERR(dev->class)) {
        ret = PTR_ERR(dev->class);
        goto err_cdev_del;
    }
    
    dev->device = device_create(dev->class, &spi->dev, dev->devno, NULL, DRIVER_NAME);
    if (IS_ERR(dev->device)) {
        ret = PTR_ERR(dev->device);
        goto err_class_destroy;
    }
    
    /* Initialize Netlink communication */
    ret = netlink_init();
    if (ret) {
        dev_err(&spi->dev, "Failed to initialize Netlink\n");
        goto err_device_destroy;
    }
    
    dev_info(&spi->dev, "SPI SDMA driver probed successfully\n");
    return 0;
    
err_device_destroy:
    device_destroy(dev->class, dev->devno);
err_class_destroy:
    class_destroy(dev->class);
err_cdev_del:
    cdev_del(&dev->cdev);
err_unregister_chrdev:
    unregister_chrdev_region(dev->devno, 1);
err_release_dma:
    if (dev->dma_chan)
        dma_release_channel(dev->dma_chan);
err_free_dma:
    dma_free_coherent(&spi->dev, DMA_BUFFER_SIZE,
                     dev->dma_buffer_virt, dev->dma_buffer_phys);
err_free_dev:
    kfree(dev);
    return ret;
}

/* SPI device remove */
static int spi_sdma_remove(struct spi_device *spi)
{
    struct spi_sdma_dev *dev = spi_get_drvdata(spi);
    
    netlink_exit();
    
    device_destroy(dev->class, dev->devno);
    class_destroy(dev->class);
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->devno, 1);
    
    if (dev->dma_chan)
        dma_release_channel(dev->dma_chan);
    
    dma_free_coherent(&spi->dev, DMA_BUFFER_SIZE,
                     dev->dma_buffer_virt, dev->dma_buffer_phys);
    
    kfree(dev);
    g_spi_dev = NULL;
    
    dev_info(&spi->dev, "SPI SDMA driver removed\n");
    return 0;
}

static const struct of_device_id spi_sdma_of_match[] = {
    { .compatible = "alientek,spi-sdma", },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, spi_sdma_of_match);

static struct spi_driver spi_sdma_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = spi_sdma_of_match,
    },
    .probe = spi_sdma_probe,
    .remove = spi_sdma_remove,
};

module_spi_driver(spi_sdma_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("SPI SDMA Driver with Netlink for i.MX6ULL");
MODULE_VERSION("1.0");
