#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/spi/spi.h>
#include <linux/mutex.h>

#define DEVICE_NAME "rc522"
#define CLASS_NAME  "rc522_class"

// RC522 暫存器定義 (範例使用 CommandReg 與 VersionReg)
#define RC522_COMMAND_REG 0x01
#define RC522_VERSION_REG 0x37

struct rc522_dev {
    struct spi_device *spi;
    dev_t devt;
    struct cdev cdev;
    struct class *class;
    struct device *device;
    struct mutex lock;
};

static struct rc522_dev *rc522_device;

/* SPI 暫存器讀寫輔助函式 */
static u8 rc522_read_reg(struct spi_device *spi, u8 reg) {
    u8 tx_buf[2];
    u8 rx_buf[2] = {0};
    
    // RC522 SPI 讀取格式: (Addr << 1) | 0x80
    tx_buf[0] = ((reg << 1) & 0x7E) | 0x80;
    tx_buf[1] = 0x00;

    spi_write_then_read(spi, tx_buf, 2, rx_buf, 2);
    return rx_buf[1];
}

static void rc522_write_reg(struct spi_device *spi, u8 reg, u8 val) {
    u8 tx_buf[2];

    // RC522 SPI 寫入格式: (Addr << 1) & 0x7E
    tx_buf[0] = (reg << 1) & 0x7E;
    tx_buf[1] = val;

    spi_write(spi, tx_buf, 2);
}

/* File Operations */
static int rc522_open(struct inode *inode, struct file *filp) {
    filp->private_data = rc522_device;
    return 0;
}

static int rc522_release(struct inode *inode, struct file *filp) {
    return 0;
}

/* 讀取裝置檔案時，傳回 RC522 的 VersionReg 晶片版本 */
static ssize_t rc522_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos) {
    struct rc522_dev *dev = filp->private_data;
    u8 version;
    int ret;

    if (*f_pos > 0)
        return 0;

    mutex_lock(&dev->lock);
    version = rc522_read_reg(dev->spi, RC522_VERSION_REG);
    mutex_unlock(&dev->lock);

    ret = copy_to_user(buf, &version, 1);
    if (ret)
        return -EFAULT;

    *f_pos += 1;
    return 1;
}

static const struct file_operations rc522_fops = {
    .owner   = THIS_MODULE,
    .open    = rc522_open,
    .release = rc522_release,
    .read    = rc522_read,
};

/* SPI 驅動 probe 與 remove */
static int rc522_probe(struct spi_device *spi) {
    int ret;

    pr_info("rc522: Probing RC522 SPI driver\n");

    rc522_device = kzalloc(sizeof(struct rc522_dev), GFP_KERNEL);
    if (!rc522_device)
        return -ENOMEM;

    rc522_device->spi = spi;
    mutex_init(&rc522_device->lock);

    // 1. 動態分配字元裝置 Major/Minor 號碼
    ret = alloc_chrdev_region(&rc522_device->devt, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("rc522: Failed to allocate chrdev region\n");
        goto fail_alloc;
    }

    // 2. 初始化並註冊 cdev
    cdev_init(&rc522_device->cdev, &rc522_fops);
    rc522_device->cdev.owner = THIS_MODULE;
    ret = cdev_add(&rc522_device->cdev, rc522_device->devt, 1);
    if (ret < 0) {
        pr_err("rc522: Failed to add cdev\n");
        goto fail_cdev;
    }

    // 3. 建立 Device Class (適用 Linux 6.x / RPi 5 Kernel)
    rc522_device->class = class_create(CLASS_NAME);
    if (IS_ERR(rc522_device->class)) {
        pr_err("rc522: Failed to create device class\n");
        ret = PTR_ERR(rc522_device->class);
        goto fail_class;
    }

    // 4. 自動建立 /dev/rc522 裝置檔案
    rc522_device->device = device_create(rc522_device->class, NULL,
                                         rc522_device->devt, NULL, DEVICE_NAME);
    if (IS_ERR(rc522_device->device)) {
        pr_err("rc522: Failed to create device\n");
        ret = PTR_ERR(rc522_device->device);
        goto fail_device;
    }

    // 軟體重置 RC522 (CommandReg = 0x0F)
    rc522_write_reg(spi, RC522_COMMAND_REG, 0x0F);

    pr_info("rc522: Device /dev/%s created successfully!\n", DEVICE_NAME);
    return 0;

fail_device:
    class_destroy(rc522_device->class);
fail_class:
    cdev_del(&rc522_device->cdev);
fail_cdev:
    unregister_chrdev_region(rc522_device->devt, 1);
fail_alloc:
    kfree(rc522_device);
    return ret;
}

static void rc522_remove(struct spi_device *spi) {
    device_destroy(rc522_device->class, rc522_device->devt);
    class_destroy(rc522_device->class);
    cdev_del(&rc522_device->cdev);
    unregister_chrdev_region(rc522_device->devt, 1);
    kfree(rc522_device);

    pr_info("rc522: Driver removed\n");
}

/* Device Tree 匹配列表 */
static const struct of_device_id rc522_dt_ids[] = {
    { .compatible = "nxp,rc522" },
    { .compatible = "ilitek,rc522" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rc522_dt_ids);

static struct spi_driver rc522_spi_driver = {
    .driver = {
        .name = DEVICE_NAME,
        .of_match_table = rc522_dt_ids,
    },
    .probe = rc522_probe,
    .remove = rc522_remove,
};

module_spi_driver(rc522_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RPi5 Developer");
MODULE_DESCRIPTION("RC522 SPI Character Driver for Raspberry Pi 5");