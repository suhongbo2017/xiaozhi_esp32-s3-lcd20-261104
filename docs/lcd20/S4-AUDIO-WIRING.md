# 音频模块接线（NS4168 + PDM 麦克风）

## 模块识别（2026-11-04 拍照确认）

| 丝印分组 | 引脚 | 实际含义 |
|---|---|---|
| SPK | LRCLK | I2S 帧时钟（WS） |
| SPK | BCLK | I2S 位时钟 |
| SPK | **SDA** | ⚠️ **SDATA = I2S 数据输入**，不是 I2C SDA |
| MIC | CLK | PDM 时钟（ESP32 输出给麦克风） |
| MIC | DATA | PDM 数据（麦克风输出给 ESP32） |
| PWR | VCC / GND | 3.3V（NS4168 支持 3.0~5.5V）/ 地 |

- 主芯片 **NS4168**：I2S 数字输入 D 类功放（自带 JST 喇叭座），**无 I2C、无需 MCLK、无 PA 使能脚**
- 板载 MEMS 麦克风（**PDM** 数字麦，丝印 HT33 附近）

→ 因此本板不需要 ES8311/ES8388 那类 I2C codec，走仓库现成的
`NoAudioCodecSimplexPdm`（标准 I2S 输出 + PDM 输入）。

## 接线表（照这个接，固件已按此配置）

| 模块 | → | ESP32-S3 |
|---|---|---|
| LRCLK | ← | **GPIO9** |
| BCLK | ← | **GPIO8** |
| SDA (SDATA) | ← | **GPIO10** |
| MIC CLK | ← | **GPIO3** |
| MIC DATA | → | **GPIO11** |
| VCC | | **3.3V**（音量偏小可试 5V，但模块上若有 LDO 请先量电压） |
| GND | | **GND**（必须共地） |
| JST 喇叭座 | | 4Ω/8Ω 小喇叭（3W 以内） |

> 只需 5 根信号线 + 电源地。**没有 I2C**，之前 ES8311 方案里的 SDA1/SCL2 全部不用。

## 固件改动（已完成）

- `config.h`：改为 NS4168/PDM 引脚定义，`AUDIO_CODEC_PA_PIN = NC`（模块无使能脚）
- `esp32s3_lcd20_board.cc`：
  * 删除 I2C 初始化与 ES8311 探测
  * `GetAudioCodec()` 改为 `NoAudioCodecSimplexPdm(16000, 24000, BCLK8, LRCK9, DOUT10, PDM_CLK3, PDM_DIN11)`
  * 删除临时的 `PacedDummyAudioCodec`（现在由真实 I2S DMA 提供节拍）
- 启动日志应出现：`NoAudioCodec: Simplex channels created` → `Audio codec started`

## 验证步骤

1. 接线上电，串口（COM14）确认：
   ```
   I (1184) NoAudioCodec: Simplex channels created
   I (1184) AudioCodec: Audio codec started
   ```
2. 开机 15s 自动播放测试曲（440Hz+660Hz 双音，24s）→ **应能听到**
3. 麦克风：喊「你好小智」，串口出现唤醒日志即为正常
4. 声音太小/失真：先用 MCP `self.audio_speaker.set_volume` 调音量；仍小则把 VCC 换 5V（若模块允许）

## 常见故障对照

| 现象 | 排查 |
|---|---|
| 完全无声 | VCC 是否有电、GND 是否共地、LRCLK/BCLK 是否接反、喇叭是否插在 JST 座上 |
| 只有"咔哒"声/爆音 | 采样率或位宽不匹配（NS4168 一般自适应 16/24/32bit，可先降输出采样率到 16k 试） |
| 声音极小 | 音量设置、喇叭阻抗（8Ω 比 4Ω 小 3dB）、VCC 5V 供电 |
| 麦克风无反应 | MIC CLK/DATA 接反、PDM 麦需要 1.8~3.3V 供电（模块内部供电则无此问题） |
