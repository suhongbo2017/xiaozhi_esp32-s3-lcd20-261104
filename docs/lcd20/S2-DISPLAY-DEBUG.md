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
