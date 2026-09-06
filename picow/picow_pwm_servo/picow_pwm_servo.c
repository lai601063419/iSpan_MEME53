#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"

#define SERVO_PIN 15

// 將角度（0 ~ 180）轉換為 PWM 脈衝寬度（以微秒 µs 為單位）
// SG90 標準脈衝寬度：0度 ≈ 500µs，180度 ≈ 2400µs
uint32_t angle_to_duty(float degree) {
    if (degree < 0) degree = 0;
    if (degree > 180) degree = 180;
    return (uint32_t)(500 + (degree / 180.0f) * 1900);
}

// 設定伺服馬達角度
void set_servo_angle(uint slice_num, uint channel, float degree) {
    pwm_set_chan_level(slice_num, channel, angle_to_duty(degree));
}

int main() {
    stdio_init_all();

    // 1. 設定 GPIO 腳位功能為 PWM
    gpio_set_function(SERVO_PIN, GPIO_FUNC_PWM);
    
    // 2. 取得該 GPIO 對應的 PWM slice 與 channel
    uint slice_num = pwm_gpio_to_slice_num(SERVO_PIN);
    uint channel = pwm_gpio_to_channel(SERVO_PIN);

    // 3. 設定 PWM 時脈分頻與 Wrap 值，產生 50Hz 訊號
    // Pico 預設系統時脈 125 MHz / 125 = 1 MHz (每 1µs 計時 1 次)
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 125.0f);
    pwm_config_set_wrap(&config, 20000 - 1); // 20000 µs = 20 ms = 50Hz
    pwm_init(slice_num, &config, true);

    // 4. 復歸階段：回到 0 度，靜止 1 秒確認定位
    set_servo_angle(slice_num, channel, 0);
    sleep_ms(1000);

    // 5. 主迴圈：每隔 2 秒在 90 度與 179 度之間切換
    bool toggle = false;
    while (true) {
        if (toggle) {
            set_servo_angle(slice_num, channel, 90.0f);
        } else {
            set_servo_angle(slice_num, channel, 179.0f);
        }
        toggle = !toggle;
        sleep_ms(2000); // 停頓 2 秒
    }

    return 0;
}