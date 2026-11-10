/*
 * ESP32-S3 LCD20 board — 2.0 inch 240x320 ST7789 SPI LCD + ES8311 codec.
 *
 * 设计要点
 *  - 显示：SPI3 + esp_lcd ST7789，240x320，背光 GPIO42 由 LEDC PWM 调节。
 *  - 音频：NS4168 I2S 功放 + 板载 PDM 麦克风（NoAudioCodecSimplexPdm）。
 *      * 输出：标准 I2S（BCLK8 / WS9 / DOUT10）→ NS4168 → 喇叭
 *      * 输入：PDM（CLK3 / DATA11）
 *      * 无 I2C 控制、无 PA 使能脚（模块自带功放常开）
 *  - 无 SD 卡槽，不初始化 SDMMC（也避免与八线 PSRAM 的 GPIO33~37 冲突）。
 *  - 板载“灯”执行器 GPIO18，暴露 self.lamp.* 三个 MCP 工具。
 */

#include "wifi_board.h"
#include "audio_codec.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include <led_strip.h>
#include "music/music_player.h"

#include <memory>
#include "lvgl_display/lvgl_theme.h"

#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Esp32S3Lcd20Board"

class Esp32S3Lcd20Board : public WifiBoard {
private:
    Button boot_button_;
    std::unique_ptr<Button> next_button_;
    std::unique_ptr<Button> stop_button_;
    std::unique_ptr<Button> prev_button_;
    LcdDisplay* display_ = nullptr;

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
            /* 按键打断：音乐播放中按 BOOT 键立即停止播放 */
            if (MusicPlayer::GetInstance().playing()) {
                ESP_LOGI(TAG, "BOOT button: interrupt music playback");
                Application::GetInstance().Schedule([]() { MusicPlayer::GetInstance().Stop(); });
                return;
            }
            app.ToggleChatState();
        });
        boot_button_.OnLongPress([this]() {
            if (MusicPlayer::GetInstance().playing()) {
                ESP_LOGI(TAG, "BOOT long press: interrupt music playback");
                Application::GetInstance().Schedule([]() { MusicPlayer::GetInstance().Stop(); });
            }
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
            next_button_->OnLongPress([]() {
                Application::GetInstance().Schedule([]() { MusicPlayer::GetInstance().Stop(); });
            });
            ESP_LOGI(TAG, "music next button: GPIO%d (click=next, double/long=stop)",
                     MUSIC_NEXT_BUTTON_GPIO);
        }
        if (MUSIC_STOP_BUTTON_GPIO != GPIO_NUM_NC) {
            stop_button_ = std::unique_ptr<Button>(new Button(MUSIC_STOP_BUTTON_GPIO));
            stop_button_->OnClick([]() {
                Application::GetInstance().Schedule([]() { MusicPlayer::GetInstance().Stop(); });
            });
            ESP_LOGI(TAG, "music stop button: GPIO%d", MUSIC_STOP_BUTTON_GPIO);
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
                } else if (phase >= 10 && (phase - 10) % 2 == 0) {
                    /* LED 自检：直接用 led_strip 驱动 WS2812，红 → 绿 → 蓝 → 灭，每 2 秒一档 */
                    static led_strip_handle_t diag_strip = nullptr;
                    if (diag_strip == nullptr) {
                        led_strip_config_t cfg = {};
                        cfg.strip_gpio_num = BUILTIN_LED_GPIO;
                        cfg.max_leds = 1;
                        cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
                        cfg.led_model = LED_MODEL_WS2812;
                        led_strip_rmt_config_t rmt = {};
                        rmt.resolution_hz = 10 * 1000 * 1000;
                        esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &diag_strip);
                        ESP_LOGW(TAG, "DIAG LED: create strip on GPIO%d -> %s", BUILTIN_LED_GPIO,
                                 esp_err_to_name(err));
                    }
                    if (diag_strip != nullptr) {
                        const int step = ((phase - 10) / 2) % 4;
                        if (step == 3) {
                            led_strip_clear(diag_strip);
                        } else {
                            const uint8_t rgb[3][3] = {{64, 0, 0}, {0, 64, 0}, {0, 0, 64}};
                            led_strip_set_pixel(diag_strip, 0, rgb[step][0], rgb[step][1], rgb[step][2]);
                            esp_err_t err = led_strip_refresh(diag_strip);
                            if (err != ESP_OK) {
                                ESP_LOGW(TAG, "DIAG LED refresh failed: %s", esp_err_to_name(err));
                            }
                        }
                        ESP_LOGW(TAG, "DIAG LED -> %s", step == 0 ? "RED" : step == 1 ? "GREEN" : step == 2 ? "BLUE" : "OFF");
                    }
                }
                if (phase >= 10) {
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
        /* NS4168（I2S 输入功放）+ 板载 PDM 数字麦：标准 I2S 输出 + PDM 输入，无需 I2C */
        static NoAudioCodecSimplexPdm audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_PDM_MIC_GPIO_CLK, AUDIO_PDM_MIC_GPIO_DIN);
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
