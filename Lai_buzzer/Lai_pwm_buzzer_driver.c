#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/pwm.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#define DEVICE_NAME "rpi5_pwm"
#define CLASS_NAME  "rpi5_pwm_class"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Raspberry Pi 5 Developer");
MODULE_DESCRIPTION("RPi 5 PWM Platform & Char Driver (Kernel 6.6+)");
MODULE_VERSION("1.2");

static int major_number;
static struct class *rpi5_pwm_class = NULL;
static struct device *rpi5_pwm_device = NULL;
static struct cdev rpi5_pwm_cdev;

static struct pwm_device *pwm0 = NULL;
static u64 period_ns = 20000000;
static u64 duty_ns   = 1000000;

static int dev_open(struct inode *inodep, struct file *filep) {
    return 0;
}

static int dev_release(struct inode *inodep, struct file *filep) {
    return 0;
}

static ssize_t dev_write(struct file *filep, const char __user *buffer, size_t len, loff_t *offset) {
    char kbuf[64];
    u64 new_duty = 0, new_period = 0;
    int ret;

    if (len >= sizeof(kbuf))
        return -EINVAL;

    if (copy_from_user(kbuf, buffer, len))
        return -EFAULT;

    kbuf[len] = '\0';

    ret = sscanf(kbuf, "%llu,%llu", &new_duty, &new_period);
    if (ret == 1) {
        duty_ns = new_duty;
    } else if (ret == 2) {
        duty_ns = new_duty;
        period_ns = new_period;
    } else {
        return -EINVAL;
    }

    if (pwm0) {
        struct pwm_state state;
        pwm_get_state(pwm0, &state);
        
        state.period = period_ns;
        state.duty_cycle = duty_ns;
        state.enabled = (duty_ns > 0);

        ret = pwm_apply_might_sleep(pwm0, &state);
        if (ret) {
            pr_err("rpi5_pwm: Failed to apply PWM state: %d\n", ret);
            return ret;
        }
    }

    return len;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = dev_open,
    .write = dev_write,
    .release = dev_release,
};

static int rpi5_pwm_probe(struct platform_device *pdev) {
    dev_t dev;
    int ret;

    /* 1. 配置字符驅動節點 */
    ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
    if (ret < 0) return ret;
    major_number = MAJOR(dev);

    cdev_init(&rpi5_pwm_cdev, &fops);
    rpi5_pwm_cdev.owner = THIS_MODULE;
    ret = cdev_add(&rpi5_pwm_cdev, dev, 1);
    if (ret < 0) goto unregister_chrdev;

    rpi5_pwm_class = class_create(CLASS_NAME);
    if (IS_ERR(rpi5_pwm_class)) {
        ret = PTR_ERR(rpi5_pwm_class);
        goto del_cdev;
    }

    rpi5_pwm_device = device_create(rpi5_pwm_class, &pdev->dev, dev, NULL, DEVICE_NAME);
    if (IS_ERR(rpi5_pwm_device)) {
        ret = PTR_ERR(rpi5_pwm_device);
        goto destroy_class;
    }

    /* 2. 從 Device Tree 安全取得 PWM (自動管理資源) */
    pwm0 = devm_pwm_get(&pdev->dev, NULL);
    if (IS_ERR(pwm0)) {
        dev_err(&pdev->dev, "Failed to get PWM from Device Tree: %ld\n", PTR_ERR(pwm0));
        pwm0 = NULL;
    } else {
        struct pwm_state state;
        pwm_get_state(pwm0, &state);
        state.period = period_ns;
        state.duty_cycle = duty_ns;
        state.enabled = true;
        pwm_apply_might_sleep(pwm0, &state);
        dev_info(&pdev->dev, "PWM successfully initialized!\n");
    }

    return 0;

destroy_class:
    class_destroy(rpi5_pwm_class);
del_cdev:
    cdev_del(&rpi5_pwm_cdev);
unregister_chrdev:
    unregister_chrdev_region(dev, 1);
    return ret;
}

static void rpi5_pwm_remove(struct platform_device *pdev) {
    dev_t dev = MKDEV(major_number, 0);

    if (pwm0) {
        pwm_disable(pwm0);
    }

    device_destroy(rpi5_pwm_class, dev);
    class_destroy(rpi5_pwm_class);
    cdev_del(&rpi5_pwm_cdev);
    unregister_chrdev_region(dev, 1);
}

static const struct of_device_id rpi5_pwm_of_match[] = {
    { .compatible = "raspberrypi,rpi5-pwm-custom", },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rpi5_pwm_of_match);

static struct platform_driver rpi5_pwm_driver = {
    .probe = rpi5_pwm_probe,
    .remove = rpi5_pwm_remove,
    .driver = {
        .name = "rpi5_pwm_driver",
        .of_match_table = rpi5_pwm_of_match,
    },
};

module_platform_driver(rpi5_pwm_driver);