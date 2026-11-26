#include "single_led.h"
#include "application.h"
#include "settings.h"
#include <esp_log.h>
#include <cstdio> 

#define TAG "SingleLed"

#define DEFAULT_BRIGHTNESS 4
#define HIGH_BRIGHTNESS 16
#define LOW_BRIGHTNESS 2

#define BLINK_INFINITE -1


SingleLed::SingleLed(gpio_num_t gpio) {
    if (gpio == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "SingleLed initialized with GPIO_NUM_NC, LED will not function");
        return;
    }

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = gpio;
    strip_config.max_leds = 1;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000; // 10MHz

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_));
    led_strip_clear(led_strip_);

    {
        Settings settings("led", false);
        /* mode 优先；没有 mode 时按旧的 status_enabled 兼容 */
        std::string mode = settings.GetString("mode", "");
        if (mode == "off") {
            mode_ = Mode::Off;
        } else if (mode == "color" || mode == "fixed") {
            mode_ = Mode::Fixed;
        } else if (mode == "on" || mode == "follow") {
            mode_ = Mode::Follow;
        } else {
            mode_ = settings.GetInt("status_enabled", 1) != 0 ? Mode::Follow : Mode::Off;
        }
        status_enabled_ = (mode_ == Mode::Follow);

        /* 固定颜色："#RRGGBB" */
        std::string color = settings.GetString("color", "");
        if (color.size() == 7 && color[0] == '#') {
            unsigned int v = 0;
            if (sscanf(color.c_str() + 1, "%06x", &v) == 1) {
                fixed_r_ = (v >> 16) & 0xFF;
                fixed_g_ = (v >> 8) & 0xFF;
                fixed_b_ = v & 0xFF;
            }
        }
        ESP_LOGI(TAG, "led mode=%d color=#%02X%02X%02X", (int)mode_, fixed_r_, fixed_g_, fixed_b_);
    }

    esp_timer_create_args_t blink_timer_args = {
        .callback = [](void *arg) {
            auto led = static_cast<SingleLed*>(arg);
            led->OnBlinkTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "blink_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&blink_timer_args, &blink_timer_));
}

SingleLed::~SingleLed() {
    if (blink_timer_ != nullptr) {
        esp_timer_stop(blink_timer_);
    }
    if (led_strip_ != nullptr) {
        led_strip_del(led_strip_);
    }
}


void SingleLed::SetColor(uint8_t r, uint8_t g, uint8_t b) {
    r_ = r;
    g_ = g;
    b_ = b;
}

void SingleLed::TurnOn() {
    if (led_strip_ == nullptr) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    esp_err_t e1 = led_strip_set_pixel(led_strip_, 0, r_, g_, b_);
    esp_err_t e2 = led_strip_refresh(led_strip_);
    ESP_LOGD(TAG, "TurnOn rgb=(%u,%u,%u) set=%s refresh=%s", r_, g_, b_, esp_err_to_name(e1),
             esp_err_to_name(e2));
}

void SingleLed::TurnOff() {
    if (led_strip_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    esp_err_t e = led_strip_clear(led_strip_);
    ESP_LOGD(TAG, "TurnOff clear=%s", esp_err_to_name(e));
}

void SingleLed::SetStatusEnabled(bool enabled) {
    /* 兼容旧接口：true=跟随状态，false=常灭 */
    SetMode(enabled ? Mode::Follow : Mode::Off);
}

void SingleLed::SaveMode() {
    Settings settings("led", true);
    switch (mode_) {
        case Mode::Off:
            settings.SetString("mode", "off");
            break;
        case Mode::Fixed:
            settings.SetString("mode", "color");
            break;
        default:
            settings.SetString("mode", "on");
            break;
    }
    /* 兼容旧字段 */
    settings.SetInt("status_enabled", mode_ == Mode::Follow ? 1 : 0);
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", fixed_r_, fixed_g_, fixed_b_);
    settings.SetString("color", hex);
}

void SingleLed::SetMode(Mode mode) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_ = mode;
        status_enabled_ = (mode == Mode::Follow);
    }
    SaveMode();
    ESP_LOGI(TAG, "led mode -> %s", mode == Mode::Off ? "off"
                                 : (mode == Mode::Fixed ? "fixed color" : "follow"));
    ApplyMode();
}

void SingleLed::SetFixedColor(uint8_t r, uint8_t g, uint8_t b) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fixed_r_ = r;
        fixed_g_ = g;
        fixed_b_ = b;
    }
    SaveMode();
    if (GetMode() == Mode::Fixed) {
        ApplyMode();
    }
}

void SingleLed::GetFixedColor(uint8_t* r, uint8_t* g, uint8_t* b) const {
    if (r) *r = fixed_r_;
    if (g) *g = fixed_g_;
    if (b) *b = fixed_b_;
}

void SingleLed::ApplyMode() {
    switch (GetMode()) {
        case Mode::Off:
            TurnOff();
            break;
        case Mode::Fixed:
            SetColor(fixed_r_, fixed_g_, fixed_b_);
            TurnOn();
            break;
        default:
            OnStateChanged();
            break;
    }
}

void SingleLed::BlinkOnce() {
    Blink(1, 100);
}

void SingleLed::Blink(int times, int interval_ms) {
    StartBlinkTask(times, interval_ms);
}

void SingleLed::StartContinuousBlink(int interval_ms) {
    StartBlinkTask(BLINK_INFINITE, interval_ms);
}

void SingleLed::StartBlinkTask(int times, int interval_ms) {
    if (led_strip_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    
    blink_counter_ = times * 2;
    blink_interval_ms_ = interval_ms;
    esp_timer_start_periodic(blink_timer_, interval_ms * 1000);
}

void SingleLed::OnBlinkTimer() {
    std::lock_guard<std::mutex> lock(mutex_);
    blink_counter_--;
    if (blink_counter_ & 1) {
        led_strip_set_pixel(led_strip_, 0, r_, g_, b_);
        led_strip_refresh(led_strip_);
    } else {
        led_strip_clear(led_strip_);

        if (blink_counter_ == 0) {
            esp_timer_stop(blink_timer_);
        }
    }
}


void SingleLed::OnStateChanged() {
    if (mode_ == Mode::Off) {
        TurnOff();   /* 常灭：任何状态变化都不点灯 */
        return;
    }
    if (mode_ == Mode::Fixed) {
        SetColor(fixed_r_, fixed_g_, fixed_b_);   /* 固定颜色：状态变化不改变颜色 */
        TurnOn();
        return;
    }
    auto& app = Application::GetInstance();
    auto device_state = app.GetDeviceState();
    switch (device_state) {
        case kDeviceStateStarting:
            SetColor(0, 0, DEFAULT_BRIGHTNESS);
            StartContinuousBlink(100);
            break;
        case kDeviceStateWifiConfiguring:
            SetColor(0, 0, DEFAULT_BRIGHTNESS);
            StartContinuousBlink(500);
            break;
        case kDeviceStateIdle:
            TurnOff();
            break;
        case kDeviceStateConnecting:
            SetColor(0, 0, DEFAULT_BRIGHTNESS);
            TurnOn();
            break;
        case kDeviceStateListening:
        case kDeviceStateAudioTesting:
            if (app.IsVoiceDetected()) {
                SetColor(HIGH_BRIGHTNESS, 0, 0);
            } else {
                SetColor(LOW_BRIGHTNESS, 0, 0);
            }
            TurnOn();
            break;
        case kDeviceStateSpeaking:
        case kDeviceStateNotifying:
            SetColor(0, DEFAULT_BRIGHTNESS, 0);
            TurnOn();
            break;
        case kDeviceStateUpgrading:
            SetColor(0, DEFAULT_BRIGHTNESS, 0);
            StartContinuousBlink(100);
            break;
        case kDeviceStateActivating:
            SetColor(0, DEFAULT_BRIGHTNESS, 0);
            StartContinuousBlink(500);
            break;
        default:
            ESP_LOGW(TAG, "Unknown led strip event: %d", device_state);
            return;
    }
}
