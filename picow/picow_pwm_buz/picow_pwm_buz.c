#include <stdio.h>
#include "pico/stdlib.h"

#define IO_BANK0_BASE1       0x40014000
#define PWM_BASE1            0x40050000

#define GP15_CTRL           ((volatile uint32_t *)(IO_BANK0_BASE1 + 0x07C))

#define PWM_SLICE7_CSR      ((volatile uint32_t *)(PWM_BASE1 + 0x8C + 0x00))
#define PWM_SLICE7_DIV      ((volatile uint32_t *)(PWM_BASE1 + 0x8C + 0x04))
#define PWM_SLICE7_CTR      ((volatile uint32_t *)(PWM_BASE1 + 0x8C + 0x08))
#define PWM_SLICE7_CC       ((volatile uint32_t *)(PWM_BASE1 + 0x8C + 0x0C))
#define PWM_SLICE7_TOP      ((volatile uint32_t *)(PWM_BASE1 + 0x8C + 0x10))

// 直寫暫存器設定蜂鳴器頻率
void set_buzzer_freq_direct(uint32_t freq) {
    if (freq == 0) {
        // 靜音：清空 Channel B (高 16 bits) 的 level
        uint32_t current_cc = *PWM_SLICE7_CC;
        *PWM_SLICE7_CC = (current_cc & 0x0000FFFF); 
        return;
    }

    // 計算 Wrap (1,000,000 / freq)
    uint32_t wrap = 1000000 / freq;
    uint32_t duty = wrap / 2; // 50% 佔空比

    *PWM_SLICE7_TOP = wrap - 1;

    // 設定 Channel B 為 50% Duty
    uint32_t current_cc = *PWM_SLICE7_CC;
    *PWM_SLICE7_CC = (current_cc & 0x0000FFFF) | (duty << 16);
}

int main() {
    stdio_init_all();

    // 1. GP15 功能設為 PWM (Func 4)
    *GP15_CTRL = 4;

    // 2. 時脈分頻設為 125.0 (125 << 4)
    *PWM_SLICE7_DIV = (125 << 4);

    // 3. 啟用 PWM Slice 7
    *PWM_SLICE7_CSR |= (1 << 0);

    // 4. 復歸靜音 1 秒
    set_buzzer_freq_direct(0);
    sleep_ms(1000);

    // 5. 輪流切換 440 Hz 與 880 Hz
    bool toggle = false;
    while (true) {
        if (toggle) {
            set_buzzer_freq_direct(440);
        } else {
            set_buzzer_freq_direct(880);
        }
        toggle = !toggle;
        sleep_ms(2000);
    }

    return 0;
}