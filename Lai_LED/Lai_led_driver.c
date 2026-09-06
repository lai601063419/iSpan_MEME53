#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include <linux/of_device.h>

#define DEVICE_NAME "Lai_gpio_led"
#define CLASS_NAME  "Lai_led_class"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Custom GPIO Driver");
MODULE_DESCRIPTION("RPi5 GPIO26 LED Character Driver");

static dev_t dev_num;
static struct cdev my_cdev;
static struct class *led_class = NULL;
static struct device *led_device = NULL;
static struct gpio_desc *led_gpio = NULL;

static ssize_t led_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    char kbuf;

    if (count == 0)
        return 0;

    if (copy_from_user(&kbuf, buf, 1))
        return -EFAULT;

    if (kbuf == '1') {
        gpiod_set_value(led_gpio, 1); // 亮燈
        pr_info("LED Driver: LED turned ON\n");
    } else if (kbuf == '0') {
        gpiod_set_value(led_gpio, 0); // 滅燈
        pr_info("LED Driver: LED turned OFF\n");
    }

    return count;
}

static int led_open(struct inode *inode, struct file *file) {
    return 0;
}

static int led_release(struct inode *inode, struct file *file) {
    return 0;
}

static const struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = led_open,
    .release = led_release,
    .write   = led_write,
};

static int led_probe(struct platform_device *pdev) {
    int ret;
    struct device *dev = &pdev->dev;

    // 1. 從 Device Tree 取得 GPIO descriptor (搭配 DTS 裡面的 "led-gpios")
    led_gpio = devm_gpiod_get(dev, "led", GPIOD_OUT_LOW);
    if (IS_ERR(led_gpio)) {
        dev_err(dev, "Failed to get GPIO from device tree\n");
        return PTR_ERR(led_gpio);
    }

    // 2. 動態分配字元裝置號
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) return ret;

    // 3. 初始化並註冊 cdev
    cdev_init(&my_cdev, &fops);
    my_cdev.owner = THIS_MODULE;
    ret = cdev_add(&my_cdev, dev_num, 1);
    if (ret < 0) goto unregister_chrdev;

    // 4. 建立裝置類別 (/sys/class/led_class)
    led_class = class_create(CLASS_NAME);
    if (IS_ERR(led_class)) {
        ret = PTR_ERR(led_class);
        goto del_cdev;
    }

    // 5. 自動產生裝置檔 (/dev/gpio_led)
    led_device = device_create(led_class, dev, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(led_device)) {
        ret = PTR_ERR(led_device);
        goto destroy_class;
    }

    pr_info("LED Driver: Registered successfully. Control via /dev/%s\n", DEVICE_NAME);
    return 0;

destroy_class:
    class_destroy(led_class);
del_cdev:
    cdev_del(&my_cdev);
unregister_chrdev:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

static void led_remove(struct platform_device *pdev) {
    gpiod_set_value(led_gpio, 0); // 關閉 LED
    device_destroy(led_class, dev_num);
    class_destroy(led_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev_num, 1);
    pr_info("LED Driver: Unregistered\n");
}

static const struct of_device_id led_of_match[] = {
    { .compatible = "custom,gpio-led", },
    { /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, led_of_match);

static struct platform_driver led_platform_driver = {
    .probe = led_probe,
    .remove = led_remove,
    .driver = {
        .name = DEVICE_NAME,
        .of_match_table = led_of_match,
    },
};

module_platform_driver(led_platform_driver);