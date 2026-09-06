#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>      
#include <unistd.h>
#include <termios.h>  

#define USB_DEV "/dev/ttyACM0"
#define SPI_DEV "/dev/rc522"
#define PWM_DEV "/dev/rpi5_pwm"
#define LED_DEV "/dev/Lai_gpio_led"
#define SUCCESS_MUSIC "8,1"
#define FAIL_MUSIC "2,2"

int init_USB(int USB_fd);