# 黑屏排查记录（2026-11-04）

## 现象
- 烧录 `esp32s3-lcd20` 后：**屏幕一直黑屏**，设备 LED 常亮。
- 串口软件侧全部正常：
  ```
  I (1081) Esp32S3Lcd20Board: LCD 240x320 ready
  I (1101) Backlight: Set brightness to 100
  I (1072) LcdDisplay: Turning display on
  I (1082) LVGL: Starting LVGL task
  ```

## 已排除（软件侧）
- SPI 总线初始化成功、`esp_lcd_new_panel_io_spi` / `esp_lcd_new_panel_st7789` 无错误
- 面板参数与旧固件（`bread-compact-wifi-lcd` + `CONFIG_LCD_ST7789_240X320`）**完全相同**：
  MOSI47 / CLK21 / DC40 / RST45 / CS41 / BL42，240×320，INVERT=true，MIRROR=false，SWAP=false，SPI mode 0，40MHz
- LVGL 端口、draw buffer（`buff_dma=1, buff_spiram=0`）配置正常，任务已启动
- 背光 PWM 驱动正常（日志 `Set brightness to 100`）

## 诊断模式（已实现，`CONFIG_LCD20_DIAG_MODE`，默认关闭）
烧录一版 diag 固件后，串口时序与屏幕动作对应如下（每秒一个 tick）：

| 时刻 | 串口日志 | 期望可见现象 |
|---|---|---|
| 1s | `DIAG 1/5: backlight PWM 100%` | 屏幕应亮（有背光） |
| 4s | `DIAG 2/5: backlight pin forced LOW` | 背光应灭（若高有效）|
| 7s | `DIAG 3/5: backlight pin forced HIGH` | 背光应亮 |
| 10s | `DIAG 4/5: restore PWM 100%` | 保持亮 |
| 11s 起 | `DIAG 5/5: screen bg -> RED/GREEN/BLUE/WHITE`（各 3s 循环） | 整屏纯色循环 |

判读规则：
- **纯色在循环** → 面板 + SPI + 背光全部正常，问题只在 LVGL 主题/内容（下一步改 UI 配置）
- **背光有明暗变化但没有颜色** → 背光 OK，面板 SPI/初始化有问题（换 ST7796 驱动、调 offset/invert/BGR、降 SPI 频率）
- **完全无变化** → 背光或面板供电/接线问题（先量 BLK、VCC、GND，再核对引脚）

## 待用户确认
1. 当前 diag 固件屏幕上是否出现红/绿/蓝/白纯色循环？
2. 4s~7s（背光强制拉低）与 7s~10s（强制拉高）屏幕明暗是否有变化？
3. 旧固件（音乐固件）时期屏幕是否显示过内容？
4. 2.0 寸屏模块的接线：CS / RES / DC / SDA(MOSI) / SCL(CLK) / BLK / VCC / GND 分别接哪个 GPIO？

---

## 结论（2026-11-04 更新）：黑屏 = 引脚定义错误

用户提供的实机接线：`BLK45 / CS15 / DC7 / RES6 / SDA(MOSI)5 / SCL(CLK)4 / VCC 3.3V`。
之前用的是仓库里 `bread-compact-wifi-lcd` 的默认引脚（47/21/40/45/41），与实际接线完全不符 → 自然全黑。

改为实际接线后：**屏幕点亮，显示小智 emoji 图标，音乐页正常**。

### 附带的两处调整

1. **诊断模式看不到纯色**：`lv_obj_set_style_bg_color(lv_screen_active(), …)` 被 LcdDisplay 的 UI 层
   （全屏容器 + 主题背景）遮住，所以诊断固件只在"背光/无 UI"时才有效。
   结论：`CONFIG_LCD20_DIAG_MODE` 保留但仅用于背光排查；显示是否点亮直接看 UI 更快。
2. **音频引脚全部让位**（屏幕占用 4/5/6/7/15/45）：

   | 功能 | 新 GPIO |
   |---|---|
   | ES8311 I2C SDA / SCL | 1 / 2 |
   | I2S MCLK / BCLK / WS / DOUT / DIN | 3 / 8 / 9 / 10 / 11 |
   | PA 使能（NS4150B SD） | 12 |

### 串口口位变更
设备原生 USB（COM15）不再枚举，现由 **CH343 → COM14** 承担控制台/烧录。
`idf.py -p COM14 flash` 已验证可用。

### 显示参数（沿用，观感正确）
`INVERT_COLOR=true`、`MIRROR_X/Y=false`、`SWAP_XY=false`、`SPI mode 0`、`40MHz`、`240x320`。

---

## 追加修复：音乐页只显示 3 秒就消失（无音频硬件时的节奏问题）

**现象**：自检播放 24s 的曲子，音乐页不到 3 秒就消失。

**根因**：`DummyAudioCodec::Write()` 立即返回，播放队列被瞬间抽干 —— 24s 音频 3.7s 就"播完"，
进度/歌词时间轴被压缩约 6~8 倍。真实 codec（ES8311）是靠 I2S DMA 阻塞形成节拍的，dummy 没有节拍。

**修复**：板级新增 `PacedDummyAudioCodec`（继承 `DummyAudioCodec`，覆盖私有的 `Write`），
按 `output_sample_rate` 把 Write 阻塞对应时长，模拟 DAC 节拍。只在音频硬件未安装时使用，
装上 ES8311 后走 `Es8311AudioCodec`，本类不参与。（不动仓库共享的 `dummy_audio_codec.cc`）

**验证（串口实录，时间轴完全对上）**：
```
I (16261) MusicPlayer: playing '自检音轨' - 'LCD20 bring-up'
I (16381) lyric[0] @0 ms      第一句
I (24381) lyric[1] @8000 ms   第二句   ← 距开始 8.0s
I (32381) lyric[2] @16000 ms  第三句   ← 距开始 16.0s
I (40401) playback finished (ok)        ← 距开始 24.0s
I (40401) StateMachine: notifying -> idle
```
