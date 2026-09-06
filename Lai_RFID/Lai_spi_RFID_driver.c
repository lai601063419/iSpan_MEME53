#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/spi/spi.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/gpio/consumer.h> /* 新增：GPIO 子系統標頭檔 */

#define DEVICE_NAME "rc522"
#define CLASS_NAME  "rc522_class"

/* RC522 暫存器定義 */
#define CommandReg    0x01
#define CommIEnReg    0x02
#define CommIrqReg    0x04
#define ErrorReg      0x06
#define FIFODataReg   0x09
#define FIFOLevelReg  0x0A
#define ControlReg    0x0C
#define BitFramingReg 0x0D
#define ModeReg       0x11
#define TxModeReg     0x12
#define RxModeReg     0x13
#define TxControlReg  0x14
#define TxASKReg      0x15
#define RFCfgReg      0x26
#define TModeReg      0x2A
#define TPrescalerReg 0x2B
#define TReloadRegL   0x2C
#define TReloadRegH   0x2D

/* RC522 與 PICC 指令 */
#define PCD_IDLE       0x00
#define PCD_TRANSCEIVE 0x0C
#define PCD_RESETRST   0x0F

#define PICC_REQIDL    0x26
#define PICC_ANTICOLL  0x93
#define PICC_REQALL    0x52
#define PICC_HALT      0x50

struct rc522_dev {
    struct spi_device *spi;
    dev_t devt;
    struct cdev cdev;
    struct class *class;
    struct device *device;
    struct mutex lock;
    struct gpio_desc *reset_gpio; /* 新增：Reset GPIO 描述符 */
};

static struct rc522_dev *rc522_device;

/* SPI 暫存器讀寫輔助函式 */
static u8 rc522_read_reg(struct spi_device *spi, u8 reg) {
    u8 tx_buf[2] = { ((reg << 1) & 0x7E) | 0x80, 0x00 };
    u8 rx_buf[2] = {0};
    struct spi_transfer t = {
        .tx_buf = tx_buf,
        .rx_buf = rx_buf,
        .len = 2,
    };
    struct spi_message m;

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    spi_sync(spi, &m);

    return rx_buf[1];
}

static void rc522_write_reg(struct spi_device *spi, u8 reg, u8 val) {
    u8 tx_buf[2];

    tx_buf[0] = (reg << 1) & 0x7E;
    tx_buf[1] = val;

    spi_write(spi, tx_buf, 2);
}

/* 開啟天線 */
static void rc522_antenna_on(struct spi_device *spi) {
    u8 temp = rc522_read_reg(spi, TxControlReg);
    if (!(temp & 0x03)) {
        rc522_write_reg(spi, TxControlReg, temp | 0x03);
    }
}

/* 核心硬體初始化 */
static void rc522_init_hardware(struct spi_device *spi) {
    rc522_write_reg(spi, CommandReg, PCD_RESETRST);
    msleep(50);

    rc522_write_reg(spi, TModeReg, 0x8D);
    rc522_write_reg(spi, TPrescalerReg, 0x3E);
    rc522_write_reg(spi, TReloadRegL, 30);
    rc522_write_reg(spi, TReloadRegH, 0);
    
    rc522_write_reg(spi, TxASKReg, 0x40);
    rc522_write_reg(spi, ModeReg, 0x3D);
    rc522_write_reg(spi, RFCfgReg, 0x60); // 設定 RxGain 為 48dB

    rc522_antenna_on(spi);
}

/* 資料傳輸通訊邏輯 */
static int rc522_to_card(struct spi_device *spi, u8 cmd, u8 *send_data, int send_len, u8 *back_data, u32 *back_bits) {
    u8 irq_en = 0x77;
    u8 wait_irq = 0x30;
    u8 n, last_bits;
    int i;

    rc522_write_reg(spi, CommIEnReg, irq_en | 0x80);
    rc522_write_reg(spi, CommIrqReg, 0x7F);
    rc522_write_reg(spi, CommandReg, PCD_IDLE);
    rc522_write_reg(spi, FIFOLevelReg, 0x80);

    for (i = 0; i < send_len; i++) {
        rc522_write_reg(spi, FIFODataReg, send_data[i]);
    }

    rc522_write_reg(spi, CommandReg, cmd);
    if (cmd == PCD_TRANSCEIVE) {
        rc522_write_reg(spi, BitFramingReg, rc522_read_reg(spi, BitFramingReg) | 0x80);
    }

    i = 80;
    do {
        usleep_range(20, 50);
        n = rc522_read_reg(spi, CommIrqReg);
        i--;
    } while ((i != 0) && !(n & 0x01) && !(n & wait_irq));

    rc522_write_reg(spi, BitFramingReg, rc522_read_reg(spi, BitFramingReg) & (~0x80));

    if (i == 0 || (rc522_read_reg(spi, ErrorReg) & 0x1B)) {
        return -1;
    }

    if (back_data && back_bits) {
        n = rc522_read_reg(spi, FIFOLevelReg);
        last_bits = rc522_read_reg(spi, ControlReg) & 0x07;
        if (last_bits) {
            *back_bits = (n - 1) * 8 + last_bits;
        } else {
            *back_bits = n * 8;
        }

        if (n == 0) n = 1;
        if (n > 16) n = 16;

        for (i = 0; i < n; i++) {
            back_data[i] = rc522_read_reg(spi, FIFODataReg);
        }
    }
    return 0;
}

/* 尋卡機制 */
static int rc522_request_card(struct spi_device *spi, u8 req_mode, u8 *tag_type) {
    u32 back_bits;
    rc522_write_reg(spi, BitFramingReg, 0x07);
    tag_type[0] = req_mode;

    if (rc522_to_card(spi, PCD_TRANSCEIVE, tag_type, 1, tag_type, &back_bits) == 0) {
        if (back_bits == 0x10) return 0;
    }
    return -1;
}

/* 防碰撞與讀取卡片 UID */
static int rc522_get_card_uid(struct spi_device *spi, u8 *uid) {
    u32 back_bits;
    u8 send_buf[2] = { PICC_ANTICOLL, 0x20 };

    rc522_write_reg(spi, BitFramingReg, 0x00);
    if (rc522_to_card(spi, PCD_TRANSCEIVE, send_buf, 2, uid, &back_bits) == 0) {
        if (uid[0] != 0x00 && (uid[0] ^ uid[1] ^ uid[2] ^ uid[3]) == uid[4]) {
            return 0;
        }
    }
    return -1;
}

/* 選卡機制：帶入 UID 將卡片切換至 ACTIVE 態 */
static int rc522_select_card(struct spi_device *spi, u8 *uid) {
    u8 send_buf[9];
    u8 back_buf[16];
    u32 back_bits;
    int i;

    send_buf[0] = PICC_ANTICOLL; // 0x93
    send_buf[1] = 0x70;          // Select NVB (Number of Valid Bits)
    for (i = 0; i < 5; i++) {
        send_buf[i + 2] = uid[i]; // 放入 4 Bytes UID + 1 Byte Checkbyte
    }

    /* SELECT 指令必須開啟 CRC 功能 */
    rc522_write_reg(spi, TxModeReg, 0x80);
    rc522_write_reg(spi, RxModeReg, 0x80);
    rc522_write_reg(spi, BitFramingReg, 0x00);

    /* 發送 7 Bytes (0x93, 0x70, UID0~3, BCC)，RC522 會自動附加 2 Bytes CRC */
    if (rc522_to_card(spi, PCD_TRANSCEIVE, send_buf, 7, back_buf, &back_bits) == 0) {
        rc522_write_reg(spi, TxModeReg, 0x00);
        rc522_write_reg(spi, RxModeReg, 0x00);
        return 0; // 選卡成功，卡片進入 ACTIVE 態
    }

    rc522_write_reg(spi, TxModeReg, 0x00);
    rc522_write_reg(spi, RxModeReg, 0x00);
    return -1;
}

static void rc522_halt(struct spi_device *spi) {
    u32 unLen;
    u8 buff[4];

    buff[0] = PICC_HALT;
    buff[1] = 0x00;
    
    rc522_write_reg(spi, TxModeReg, 0x80); // 開啟 TxCRCEn，發送時硬體自動補 CRC
    rc522_write_reg(spi, RxModeReg, 0x80);

    rc522_write_reg(spi, BitFramingReg, 0x00);
    rc522_to_card(spi, PCD_TRANSCEIVE, buff, 2, buff, &unLen);

    rc522_write_reg(spi, TxModeReg, 0x0); // 開啟 TxCRCEn，發送時硬體自動補 CRC
    rc522_write_reg(spi, RxModeReg, 0x0);
}

/* File Operations */
static int rc522_open(struct inode *inode, struct file *filp) {
    filp->private_data = rc522_device;
    rc522_init_hardware(rc522_device->spi);
    return 0;
}

static int rc522_release(struct inode *inode, struct file *filp) {
    return 0;
}

/* 讀取裝置檔案時，自動尋卡並傳回 5 Bytes UID (含 Checkbyte) */
static ssize_t rc522_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos) {
    struct rc522_dev *dev = filp->private_data;
    u8 tag_type[2];
    u8 uid[5];
    int ret;

    if (count < 5)
        return -EINVAL;

    mutex_lock(&dev->lock);

    if (rc522_request_card(dev->spi, PICC_REQIDL, tag_type) == 0) {
        if (rc522_get_card_uid(dev->spi, uid) == 0) {
            /* 1. 先選卡（將卡片帶入 ACTIVE 態） */
            if (rc522_select_card(dev->spi, uid) == 0) {
                /* 2. 再休眠（ACTIVE 態的卡片才能成功執行 HALT） */
                rc522_halt(dev->spi);
            }
            
            mutex_unlock(&dev->lock);
            
            ret = copy_to_user(buf, uid, 5);
            if (ret)
                return -EFAULT;

            return 5; // 傳回 5 Bytes UID
        }
    }

    mutex_unlock(&dev->lock);
    return 0; // 未感應到卡片時傳回 0
}

static const struct file_operations rc522_fops = {
    .owner   = THIS_MODULE,
    .open    = rc522_open,
    .release = rc522_release,
    .read    = rc522_read,
};

static int rc522_probe(struct spi_device *spi) {
    int ret;

    pr_info("rc522: Probing RC522 SPI driver\n");

    rc522_device = kzalloc(sizeof(struct rc522_dev), GFP_KERNEL);
    if (!rc522_device)
        return -ENOMEM;

    rc522_device->spi = spi;
    mutex_init(&rc522_device->lock);

    /* 新增：從 Device Tree 獲取 GPIO25 Reset 引腳 */
    rc522_device->reset_gpio = devm_gpiod_get_optional(&spi->dev, "reset", GPIOD_OUT_LOW);
    if (IS_ERR(rc522_device->reset_gpio)) {
        ret = PTR_ERR(rc522_device->reset_gpio);
        dev_err(&spi->dev, "Failed to get reset GPIO\n");
        goto fail_gpio;
    }

    /* 新增：執行硬體 Reset（拉低 20ms 後拉高 50ms 復位 RC522） */
    if (rc522_device->reset_gpio) {
        gpiod_set_value_cansleep(rc522_device->reset_gpio, 0);
        msleep(20);
        gpiod_set_value_cansleep(rc522_device->reset_gpio, 1);
        msleep(50);
    }

    ret = alloc_chrdev_region(&rc522_device->devt, 0, 1, DEVICE_NAME);
    if (ret < 0) goto fail_alloc;

    cdev_init(&rc522_device->cdev, &rc522_fops);
    rc522_device->cdev.owner = THIS_MODULE;
    ret = cdev_add(&rc522_device->cdev, rc522_device->devt, 1);
    if (ret < 0) goto fail_cdev;

    rc522_device->class = class_create(CLASS_NAME);
    if (IS_ERR(rc522_device->class)) {
        ret = PTR_ERR(rc522_device->class);
        goto fail_class;
    }

    rc522_device->device = device_create(rc522_device->class, NULL,
                                         rc522_device->devt, NULL, DEVICE_NAME);
    if (IS_ERR(rc522_device->device)) {
        ret = PTR_ERR(rc522_device->device);
        goto fail_device;
    }

    // 初始化 RC522 暫存器與天線
    rc522_init_hardware(spi);

    pr_info("rc522: Device /dev/%s created successfully!\n", DEVICE_NAME);
    return 0;

fail_device:
    class_destroy(rc522_device->class);
fail_class:
    cdev_del(&rc522_device->cdev);
fail_cdev:
    unregister_chrdev_region(rc522_device->devt, 1);
fail_alloc:
fail_gpio:
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
MODULE_DESCRIPTION("RC522 RFID SPI Character Driver with GPIO Reset");