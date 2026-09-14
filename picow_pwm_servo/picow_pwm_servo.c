#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "pico/cyw43_arch.h"
#include "hardware/sync.h"


#define RESETS_BASE             0x4000C000
// RP2040 Atomic Clear 暫存器別名 (Base + 0x3000)，寫入 1 代表將該 Bit 清零
#define RESETS_RESET_CLR        (*(volatile uint32_t *)(RESETS_BASE + 0x3000 + 0x00))
#define RESETS_RESET_DONE       (*(volatile uint32_t *)(RESETS_BASE + 0x00 + 0x08))
#define RESETS_PWM_BIT          (1 << 14) // PWM 模組為 Bit 14

// --- PADS BANK0 (GPIO 電氣屬性) ---
#define PADS_BANK0_BASE         0x4001C000
#define GPIO15_PAD              (*(volatile uint32_t *)(PADS_BANK0_BASE + 0x040))

// --- IO BANK0 (GPIO 功能選擇) ---
#define IO_BANK0_BASE           0x40014000
#define GPIO15_CTRL             (*(volatile uint32_t *)(IO_BANK0_BASE + 0x07C))
#define GPIO_FUNC_PWM           4         // GP15 切換為 PWM 模式

// GPIO22 的控制暫存器 (CTRL) 位址 offset 為 0x0B4 (22 * 8 + 4)
#define GPIO22_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x0B4))
#define GPIO_FUNC_SIO 5 // Function Select 設定值 (Function 5 代表 SIO 控制)

// --- PWM 控制器 (Base: 0x40050000) ---
#define PWM_BASE                0x40050000 // 修正：RP2040 PWM 正確 Base 位址
#define PWM_SLICE7_BASE         (PWM_BASE + (7 * 0x14)) // 0x4005008C

#define PWM_CH7_CSR             (*(volatile uint32_t *)(PWM_SLICE7_BASE + 0x00)) // 0x4005008C
#define PWM_CH7_DIV             (*(volatile uint32_t *)(PWM_SLICE7_BASE + 0x04)) // 0x40050090
#define PWM_CH7_CTR             (*(volatile uint32_t *)(PWM_SLICE7_BASE + 0x08)) // 0x40050094
#define PWM_CH7_CC              (*(volatile uint32_t *)(PWM_SLICE7_BASE + 0x0C)) // 0x40050098
#define PWM_CH7_TOP             (*(volatile uint32_t *)(PWM_SLICE7_BASE + 0x10)) // 0x4005009C

#define PWM_CSR_EN_BIT          (1 << 0)  // 啟動 PWM 控制位元

// SIO (Single-cycle I/O) 暫存器區塊，基底位址 0xd0000000
#define SIO_BASE           0xd0000000
#define SIO_GPIO_OE_SET    (*(volatile uint32_t *)(SIO_BASE + 0x024)) // 輸出啟用 Set
#define SIO_GPIO_OUT_SET   (*(volatile uint32_t *)(SIO_BASE + 0x014)) // GPIO 輸出 高電位 (Set)
#define SIO_GPIO_OUT_CLR   (*(volatile uint32_t *)(SIO_BASE + 0x018)) // GPIO 輸出 低電位 (Clear)

volatile int servo_lock = 1;

// =============================================================================
// 2. 伺服馬達角度控制函式
// =============================================================================

uint32_t angle_to_duty(float degree) {
    if (degree < 0) degree = 0;
    if (degree > 180) degree = 180;
    return (uint32_t)(500 + (degree / 180.0f) * 1900);
}

void set_servo_angle_mmio(float degree) {
    uint32_t duty_us = angle_to_duty(degree);
    
    // GP15 控制 Channel B (高 16 位元)，Channel A 為低 16 位元
    uint32_t current_cc = PWM_CH7_CC;
    current_cc &= 0x0000FFFF;                 // 清除 Channel B
    current_cc |= ((duty_us & 0xFFFF) << 16); // 寫入 duty_us 到 Channel B
    
    PWM_CH7_CC = current_cc;
}

bool timer_callback(struct repeating_timer *t) {
    servo_lock = 1;
    __dmb();

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    set_servo_angle_mmio(0.0f);
    SIO_GPIO_OUT_SET = (1u << 22);
    printf("1\n");
    fflush(stdout);
    return false;
}

int main() {
    stdio_init_all();

    sleep_ms(1000);
    if (cyw43_arch_init()) {
        fprintf(stderr, "cyw43_arch_init() failed\n");
        return -1;
    }

    RESETS_RESET_CLR = RESETS_PWM_BIT;
    __dsb();

    while (!(RESETS_RESET_DONE & RESETS_PWM_BIT)) {
        // 等待 Reset 完成
    }

    // 步驟 2: 設定 GPIO15 電氣屬性 (對照 Log 寫入 0x50: IE=1, Drive=4mA)
    GPIO15_PAD = 0x50;

    // 步驟 3: 設定 GPIO15 功能為 PWM (FUNCSEL = 4)
    GPIO15_CTRL = GPIO_FUNC_PWM;

    // 步驟 4: 初始化 PWM Slice 7 暫存器
    PWM_CH7_CSR = 0;           // 先關閉 PWM Slice
    PWM_CH7_DIV = (125 << 4);  // 125MHz / 125 = 1MHz (0x000007D0)
    PWM_CH7_TOP = 19999;       // 20ms 週期 / 50Hz (0x00004E1F)
    PWM_CH7_CTR = 0;           // 計數器歸零

    set_servo_angle_mmio(0.0f); // 設定初始 0 度 (CC: 0x01F40000)

    __dsb();
    PWM_CH7_CSR = PWM_CSR_EN_BIT;       // 啟動 PWM

    GPIO22_CTRL = GPIO_FUNC_SIO;
    // 2. 透過 SIO_GPIO_OE_SET 設定 GPIO22 為輸出模式 (bit 22 設為 1)
    SIO_GPIO_OE_SET = (1u << 22);

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    char read_buf[20];

    sleep_ms(1000);
    struct repeating_timer timer;

    while (true) {
        memset(read_buf, 0, sizeof(read_buf));
        fgets(read_buf, sizeof(read_buf), stdin);
        if(servo_lock == 0){
            cancel_repeating_timer(&timer);
        }
        if(read_buf[0] - '0' >= 1){
            servo_lock = read_buf[0] - '0';
        }
        else if(servo_lock <= 1){
            servo_lock = read_buf[0] - '0';
        }
        switch(servo_lock){
            case 0:
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            set_servo_angle_mmio(90.0f);
            SIO_GPIO_OUT_CLR = (1u << 22);
            add_repeating_timer_ms(-3000, timer_callback, NULL, &timer);
            break;
            case 1:
            case 2:
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
            set_servo_angle_mmio(0.0f);
            SIO_GPIO_OUT_SET = (1u << 22);
            break;
            case 3:
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            set_servo_angle_mmio(90.0f);
            SIO_GPIO_OUT_CLR = (1u << 22); 
            break;
        }
        read_buf[0] = servo_lock + '0';
        printf("%s", read_buf);
        fflush(stdout);
    }
    return 0;
}