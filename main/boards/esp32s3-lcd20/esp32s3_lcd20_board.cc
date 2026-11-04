/*
 * ESP32-S3 LCD20 board — 2.0 inch 240x320 ST7789 SPI LCD + ES8311 codec.
 *
 * 设计要点
 *  - 显示：SPI3 + esp_lcd ST7789，240x320，背光 GPIO42 由 LEDC PWM 调节。
 *  - 音频：ES8311（I2C 控制 + I2S 双工 + PA 使能）。上电时主动 probe 0x18：
 *      * 探测成功 → Es8311AudioCodec（可录可放）
 *      * 探测失败 → DummyAudioCodec 降级（保证系统可启动、屏幕 / MCP 可用），
 *        并打印醒目告警，便于“音频未安装”阶段调试。
 *  - 无 SD 卡槽，不初始化 SDMMC（也避免与八线 PSRAM 的 GPIO33~37 冲突）。
 *  - 板载“灯”执行器 GPIO18，暴露 self.lamp.* 三个 MCP 工具。
 */

#include "wifi_board.h"
#include "audio_codec.h"
#include "codecs/dummy_audio_codec.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "music/music_player.h"

#include <memory>
#include "lvgl_display/lvgl_theme.h"

#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Esp32S3Lcd20Board"

/**
 * 音频硬件未安装时的"实时速率"假 codec。
 *
 * DummyAudioCodec::Write() 立即返回，导致播放队列被瞬间抽干：一段 24s 的音频
 * 3~4s 就"播完"（音乐页一闪而过、歌词节奏被压缩）。这里按输出采样率把 Write
 * 阻塞相应时长，模拟真实 DAC 的节拍，让无硬件阶段也能验证 UI/歌词/进度。
 * 装上 ES8311 后走 Es8311AudioCodec（真实 I2S 阻塞），本类不再参与。
 */
class PacedDummyAudioCodec : public DummyAudioCodec {
public:
    PacedDummyAudioCodec(int input_sample_rate, int output_sample_rate)
        : DummyAudioCodec(input_sample_rate, output_sample_rate) {}

private:
    int Write(const int16_t* data, int samples) override {
        (void)data;
        if (samples > 0 && output_sample_rate_ > 0 && output_channels_ > 0) {
            const int frames = samples / output_channels_;
            const int64_t us = static_cast<int64_t>(frames) * 1000000 / output_sample_rate_;
            if (us > 0) {
                vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000));
            }
        }
        return samples;
    }
};

class Esp32S3Lcd20Board : public WifiBoard {
private:
    Button boot_button_;
    std::unique_ptr<Button> next_button_;
    std::unique_ptr<Button> prev_button_;
    LcdDisplay* display_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    bool codec_present_ = false;

    /* ---------------- I2C + ES8311 探测 ---------------- */
    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = I2C_NUM_0;
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags = { .enable_internal_pullup = 1 };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        /* i2c_master_probe() 需要 7bit 地址；AUDIO_CODEC_ES8311_ADDR 是 8bit 形式（0x30 → 0x18） */
        const uint8_t probe_addr = static_cast<uint8_t>(AUDIO_CODEC_ES8311_ADDR >> 1);
        esp_err_t err = i2c_master_probe(i2c_bus_, probe_addr, 100);
        if (err == ESP_OK) {
            codec_present_ = true;
            ESP_LOGI(TAG, "ES8311 found at 7bit 0x%02X (8bit 0x%02X)", probe_addr,
                     AUDIO_CODEC_ES8311_ADDR);
            return;
        }

        codec_present_ = false;
        ESP_LOGW(TAG, "ES8311 not found at 7bit 0x%02X / 8bit 0x%02X (%s), audio falls back to dummy codec",
                 probe_addr, AUDIO_CODEC_ES8311_ADDR, esp_err_to_name(err));

        /* 帮助定位接线问题：扫描 0x08~0x77，把命中的地址全部打印出来 */
        for (uint8_t addr = 0x08; addr < 0x78; addr++) {
            if (i2c_master_probe(i2c_bus_, addr, 20) == ESP_OK) {
                ESP_LOGW(TAG, "  I2C device found at 0x%02X", addr);
            }
        }
    }

    /* ---------------- SPI + LCD ---------------- */
    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                    DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                    DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        ESP_LOGI(TAG, "LCD %dx%d ready", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    }

    /* ---------------- 按键 / 灯 ---------------- */
    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeTools() {
        static LampController lamp(LAMP_GPIO);
    }

    /* 可选实体切歌键：按钮另一端接 GND，内部上拉，按下为低电平 */
    void InitializeMusicButtons() {
        if (MUSIC_NEXT_BUTTON_GPIO != GPIO_NUM_NC) {
            next_button_ = std::unique_ptr<Button>(new Button(MUSIC_NEXT_BUTTON_GPIO));
            next_button_->OnClick([]() {
                /* 按键回调可能不在主任务，切歌必须投递到主任务 */
                Application::GetInstance().Schedule([]() { MusicPlayer::GetInstance().Next(); });
            });
            next_button_->OnDoubleClick([]() {
                Application::GetInstance().Schedule([]() { MusicPlayer::GetInstance().Stop(); });
            });
            ESP_LOGI(TAG, "music next button: GPIO%d (click=next, double=stop)", MUSIC_NEXT_BUTTON_GPIO);
        }
        if (MUSIC_PREV_BUTTON_GPIO != GPIO_NUM_NC) {
            prev_button_ = std::unique_ptr<Button>(new Button(MUSIC_PREV_BUTTON_GPIO));
            prev_button_->OnClick([]() {
                Application::GetInstance().Schedule([]() { MusicPlayer::GetInstance().Prev(); });
            });
            ESP_LOGI(TAG, "music prev button: GPIO%d (click=prev)", MUSIC_PREV_BUTTON_GPIO);
        }
    }

#ifdef CONFIG_LCD20_DIAG_MODE
    /* ---------------- bring-up 诊断：定位黑屏是背光 / 面板 / LVGL ---------------- */
    void StartDiagnostics() {
        auto& app = Application::GetInstance();
        static int phase = 0;
        static const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF};  /* R G B W */
        static const char* names[] = {"RED", "GREEN", "BLUE", "WHITE"};

        esp_timer_create_args_t args = {
            .callback = [](void*) {
                auto& app2 = Application::GetInstance();
                auto* display = Board::GetInstance().GetDisplay();
                phase++;
                if (phase == 1) {
                    ESP_LOGW(TAG, "DIAG 1/5: backlight PWM 100%% (normal polarity)");
                    Board::GetInstance().GetBacklight()->SetBrightness(100, true);
                } else if (phase == 3) {
                    ESP_LOGW(TAG, "DIAG 2/5: backlight pin forced LOW (3s)");
                    gpio_reset_pin(DISPLAY_BACKLIGHT_PIN);
                    gpio_set_direction(DISPLAY_BACKLIGHT_PIN, GPIO_MODE_OUTPUT);
                    gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
                } else if (phase == 6) {
                    ESP_LOGW(TAG, "DIAG 3/5: backlight pin forced HIGH (3s)");
                    gpio_set_level(DISPLAY_BACKLIGHT_PIN, 1);
                } else if (phase == 9) {
                    ESP_LOGW(TAG, "DIAG 4/5: restore PWM 100%%");
                    auto* bl = Board::GetInstance().GetBacklight();
                    if (bl) {
                        bl->SetBrightness(100, true);
                    }
                } else if (phase >= 10) {
                    int i = (phase - 10) / 3 % 4;
                    ESP_LOGW(TAG, "DIAG 5/5: screen bg -> %s (color #%d)", names[i], i);
                    if (display != nullptr) {
                        DisplayLockGuard lock(display);
                        lv_obj_t* scr = lv_screen_active();
                        lv_obj_set_style_bg_color(scr, lv_color_hex(
                            i == 0 ? 0xFF0000 : i == 1 ? 0x00FF00 : i == 2 ? 0x0000FF : 0xFFFFFF), 0);
                        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
                    }
                    (void)colors;
                }
                (void)app2;
            },
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "lcd20_diag",
            .skip_unhandled_events = true,
        };
        esp_timer_handle_t timer = nullptr;
        if (esp_timer_create(&args, &timer) == ESP_OK) {
            esp_timer_start_periodic(timer, 1000 * 1000);
            ESP_LOGW(TAG, "DIAG: display diagnostics started (backlight pin=%d)", DISPLAY_BACKLIGHT_PIN);
        }
        (void)app;
    }
#endif

public:
    Esp32S3Lcd20Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeTools();
        InitializeMusicButtons();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
#ifdef CONFIG_LCD20_DIAG_MODE
        StartDiagnostics();
#endif
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        if (codec_present_) {
            static Es8311AudioCodec audio_codec(
                i2c_bus_, I2C_NUM_0,
                AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
                AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR,
                AUDIO_INPUT_REFERENCE);
            return &audio_codec;
        }
        /* 音频未安装 / 未接线：降级为"按实时速率消费"的 dummy，保证系统可用且节奏正确 */
        static PacedDummyAudioCodec audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }
};

DECLARE_BOARD(Esp32S3Lcd20Board);
