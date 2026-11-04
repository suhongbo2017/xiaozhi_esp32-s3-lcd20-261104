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

#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include <driver/i2c_master.h>

#define TAG "Esp32S3Lcd20Board"

class Esp32S3Lcd20Board : public WifiBoard {
private:
    Button boot_button_;
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

        esp_err_t err = i2c_master_probe(i2c_bus_, AUDIO_CODEC_ES8311_ADDR, 100);
        if (err == ESP_OK) {
            codec_present_ = true;
            ESP_LOGI(TAG, "ES8311 found at 0x%02X", AUDIO_CODEC_ES8311_ADDR);
            return;
        }

        codec_present_ = false;
        ESP_LOGW(TAG, "ES8311 not found at 0x%02X (%s), audio falls back to dummy codec",
                 AUDIO_CODEC_ES8311_ADDR, esp_err_to_name(err));

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

public:
    Esp32S3Lcd20Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeTools();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
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
        /* 音频未安装 / 未接线：降级为 dummy，保证系统其余功能可用 */
        static DummyAudioCodec audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE);
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
