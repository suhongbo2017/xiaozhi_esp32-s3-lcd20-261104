#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

/*
 * ESP32-S3 LCD20 — 2.0 inch 240x320 SPI LCD + ES8311 codec
 *
 * 硬件：
 *   - ESP32-S3 模组 16MB Flash / 8MB 八线(Octal) PSRAM（N16R8 类）
 *   - 2.0 inch 240x320 ST7789 SPI 屏（无触摸、无 SD 卡槽）
 *   - ES8311 codec + PA（方案 A，I2C 控制 + I2S 双工）
 *
 * 引脚规划原则：避开 Flash/PSRAM 占用的 GPIO26~37、USB 19/20、BOOT 0，
 * 以及屏幕 40/41/45/47/21、背光 42、状态灯 48、LAMP 18。
 */

/* ---------------- 音频：ES8311（I2S 双工） ---------------- */
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

/* ES8311 需要 MCLK + 双工 I2S，故不定义 AUDIO_I2S_METHOD_SIMPLEX */
#define AUDIO_I2S_GPIO_MCLK    GPIO_NUM_5
#define AUDIO_I2S_GPIO_BCLK    GPIO_NUM_3
#define AUDIO_I2S_GPIO_WS      GPIO_NUM_4
#define AUDIO_I2S_GPIO_DOUT    GPIO_NUM_7
#define AUDIO_I2S_GPIO_DIN     GPIO_NUM_6

/* 未装 codec 时的降级路径（NoAudioCodecSimplex 需要的宏，保持与仓库约定一致） */
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_3
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_8

/* ES8311 I2C 控制总线 */
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_1
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_2

/* ES8311 PA 使能（接 NS4150B SD 脚） */
#define AUDIO_CODEC_PA_PIN GPIO_NUM_8
#define AUDIO_CODEC_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR  /* AD0=GND → 0x18 */

/* 单麦，无回采参考声道（后续做打断时再打开） */
#define AUDIO_INPUT_REFERENCE    false

/* ---------------- 按键 / 灯 ---------------- */
#define BUILTIN_LED_GPIO        GPIO_NUM_48
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_NC
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

/* 板载“灯”执行器（继电器 / MOS 管），对外暴露 self.lamp.* 三个 MCP 工具 */
#define LAMP_GPIO GPIO_NUM_18

/* ---------------- 2.0 inch 240x320 LCD（SPI3，背光 PWM 可调） ---------------- */
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_42
#define DISPLAY_MOSI_PIN      GPIO_NUM_47
#define DISPLAY_CLK_PIN       GPIO_NUM_21
#define DISPLAY_DC_PIN        GPIO_NUM_40
#define DISPLAY_RST_PIN       GPIO_NUM_45
#define DISPLAY_CS_PIN        GPIO_NUM_41

/* 面板参数：默认 IPS 版本；选了 NO_IPS 时改用非 IPS 参数 */
#define LCD_TYPE_ST7789_SERIAL
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  320
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE 0

#ifdef CONFIG_LCD_ST7789_240X320_NO_IPS
#define DISPLAY_INVERT_COLOR    false
#else
#define DISPLAY_INVERT_COLOR    true
#endif

#endif // _BOARD_CONFIG_H_
