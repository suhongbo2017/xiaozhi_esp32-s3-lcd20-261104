#ifndef _SINGLE_LED_H_
#define _SINGLE_LED_H_

#include "led.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <led_strip.h>
#include <esp_timer.h>
#include <atomic>
#include <mutex>

class SingleLed : public Led {
public:
    SingleLed(gpio_num_t gpio);
    virtual ~SingleLed();

    void OnStateChanged() override;

    /* 模式：Follow / Off / Fixed（NVS 持久化：led/mode、led/color） */
    void SetMode(Mode mode) override;
    Mode GetMode() const override { return mode_; }
    void SetFixedColor(uint8_t r, uint8_t g, uint8_t b) override;
    void GetFixedColor(uint8_t* r, uint8_t* g, uint8_t* b) const override;

    /* 手动控制（MCP self.led.* 用）。注意：下一次设备状态变化会覆盖手动颜色 */
    bool SupportsColor() const override { return true; }
    void SetStatusEnabled(bool enabled) override;
    bool StatusEnabled() const override { return status_enabled_; }
    void SetColor(uint8_t r, uint8_t g, uint8_t b) override;
    void TurnOn() override;
    void TurnOff() override;
    void Reinit() override;

private:
    Mode mode_ = Mode::Follow;      /* 当前模式 */
    bool status_enabled_ = true;    /* 兼容旧字段：等于 mode_ == Follow */
    uint8_t fixed_r_ = 0;           /* Fixed 模式的固定颜色 */
    uint8_t fixed_g_ = 0;
    uint8_t fixed_b_ = 0;
    std::mutex mutex_;
    gpio_num_t gpio_num_ = GPIO_NUM_NC;
    TaskHandle_t blink_task_ = nullptr;
    led_strip_handle_t led_strip_ = nullptr;
    uint8_t r_ = 0, g_ = 0, b_ = 0;
    int blink_counter_ = 0;
    int blink_interval_ms_ = 0;
    esp_timer_handle_t blink_timer_ = nullptr;

    bool CreateStrip();             /* 创建 RMT 设备并接管 GPIO */
    void ApplyMode();               /* 按当前模式立刻刷新输出 */
    void SaveMode();                /* 写 NVS */
    void StartBlinkTask(int times, int interval_ms);
    void OnBlinkTimer();

    void BlinkOnce();
    void Blink(int times, int interval_ms);
    void StartContinuousBlink(int interval_ms);
};

#endif // _SINGLE_LED_H_
