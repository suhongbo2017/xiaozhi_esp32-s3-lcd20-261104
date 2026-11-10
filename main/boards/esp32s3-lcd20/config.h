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

/* ---------------- 音频：NS4168 I2S 功放 + PDM 数字麦克风 ----------------
 * 模块实测型号（2026-11-04 拍照确认）：
 *   - 功放：NS4168（I2S 数字输入 D 类功放，自带扬声器座子，无 I2C、无需 MCLK）
 *     丝印分组 SPK：LRCLK / BCLK / SDA(=SDATA，即 I2S 数据输入)
 *   - 麦克风：板载 MEMS PDM 麦（HT33）
 *     丝印分组 MIC：CLK（PDM 时钟，ESP 输出）/ DATA（PDM 数据，ESP 输入）
 *
 * 因此本板走「标准 I2S 输出 + PDM 输入」= NoAudioCodecSimplexPdm，
 * 不需要 ES8311/ES8388 那类 I2C 控制总线。
 */
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

/* 扬声器侧：标准 I2S（ESP32 → NS4168） */
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_8    /* → 模块 BCLK */
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_9    /* → 模块 LRCLK */
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_10   /* → 模块 SDA/SDATA */

/* 麦克风侧：PDM（ESP32 → 模块 CLK，模块 DATA → ESP32） */
#define AUDIO_PDM_MIC_GPIO_CLK  GPIO_NUM_3    /* → 模块 MIC CLK */
#define AUDIO_PDM_MIC_GPIO_DIN  GPIO_NUM_11   /* ← 模块 MIC DATA */

/* 兼容仓库里 NoAudioCodecSimplex 的宏命名（本板不使用 I2S 麦，仅占位） */
#define AUDIO_I2S_MIC_GPIO_WS   AUDIO_PDM_MIC_GPIO_CLK
#define AUDIO_I2S_MIC_GPIO_SCK  AUDIO_PDM_MIC_GPIO_CLK
#define AUDIO_I2S_MIC_GPIO_DIN  AUDIO_PDM_MIC_GPIO_DIN

/* 无 codec 芯片、无 I2C、无 PA 使能脚 */
#define AUDIO_CODEC_PA_PIN      GPIO_NUM_NC
#define AUDIO_INPUT_REFERENCE   false

/* ---------------- 按键 / 灯 ---------------- */
#define BUILTIN_LED_GPIO        GPIO_NUM_48
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_NC
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

/* 板载“灯”执行器（继电器 / MOS 管），对外暴露 self.lamp.* 三个 MCP 工具 */
#define LAMP_GPIO GPIO_NUM_18

/* ---------------- 实体切歌按键（可选，默认不启用） ----------------
 * 接法：按键一端接该 GPIO，另一端接 GND（内部上拉，按下=低电平）。
 * 改完这里重新 build + flash 即生效；保持 GPIO_NUM_NC 表示不启用。
 * 建议用空闲脚：13 / 14 / 16 / 17 / 21 / 38 / 39 / 40 / 41 / 42 / 47
 * 已占用：0( BOOT) 1,2(I2C) 3,8,9,10,11(I2S) 4,5,6,7,15,45(LCD) 12(PA) 18(LAMP) 19,20(USB) 26~37(Flash/PSRAM) 43,44(UART0) 48(LED)
 */
#define MUSIC_NEXT_BUTTON_GPIO GPIO_NUM_NC   /* 单击=下一首，双击=停止 */
#define MUSIC_PREV_BUTTON_GPIO GPIO_NUM_NC   /* 单击=上一首 */
/* 专用“停止/打断”键（可选）：按下立即停止音乐播放 */
#define MUSIC_STOP_BUTTON_GPIO GPIO_NUM_NC

/* ---------------- 2.0 inch 240x320 LCD（SPI3，背光 PWM 可调） ----------------
 * 实机接线（用户提供，2026-11-04）：
 *   BLK  → GPIO45      CS   → GPIO15     DC   → GPIO7
 *   RES  → GPIO6       SDA(MOSI) → GPIO5  SCL(CLK) → GPIO4
 *   VCC  → 3.3V        GND  → GND
 */
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_45
#define DISPLAY_MOSI_PIN      GPIO_NUM_5
#define DISPLAY_CLK_PIN       GPIO_NUM_4
#define DISPLAY_DC_PIN        GPIO_NUM_7
#define DISPLAY_RST_PIN       GPIO_NUM_6
#define DISPLAY_CS_PIN        GPIO_NUM_15

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
