# S1 阶段一：新建板 `esp32s3-lcd20`（已完成，待硬件验证）

> 阶段目标：在不动上游板型的前提下，为「2.0 寸 240×320 + ES8311」这块硬件建立**唯一 SKU** 的板级实现，
> 并把 UI 档位从 16px/emoji32 升到 **20px/emoji64**。音乐播放器（S1.3/S1.4）在下一阶段。

- 时间：2026-11-04
- 提交：见 `git log`（提交信息以 `S1:` 开头）
- 远端：`origin` = `git@github.com:suhongbo2017/xiaozhi_esp32-s3-lcd20-261104.git`，`upstream` = 原 `ESP32-S3-XIAOZHI`

---

## 1. 新增文件

| 文件 | 内容 |
|---|---|
| `main/boards/esp32s3-lcd20/config.json` | `type=esp32s3-lcd20`，target `esp32s3`，`sdkconfig_append: CONFIG_LCD_ST7789_240X320=y` |
| `main/boards/esp32s3-lcd20/config.h` | 引脚 + 音频 + 面板参数（见 §2） |
| `main/boards/esp32s3-lcd20/sdkconfig.defaults` | `BOARD_TYPE_ESP32S3_LCD20=y`、`LCD_ST7789_240X320=y`、显式 `SPIRAM_MODE_OCT` |
| `main/boards/esp32s3-lcd20/esp32s3_lcd20_board.cc` | `Esp32S3Lcd20Board` + **唯一** `DECLARE_BOARD(Esp32S3Lcd20Board)` |

## 2. 板级参数（与装配图对应）

### 显示 / 背光 / 灯 / 按键
| 功能 | GPIO | 说明 |
|---|---|---|
| LCD MOSI / CLK / DC / RST / CS | 47 / 21 / 40 / 45 / 41 | SPI3_HOST，40MHz，ST7789 240×320，`SWAP_XY=false`、`MIRROR=false`、`INVERT=true` |
| 背光 | 42 | `PwmBacklight`（LEDC 25kHz，带渐变），支持 `self.screen.set_brightness` |
| 状态灯 | 48 | `SingleLed` |
| LAMP 执行器 | 18 | `LampController` → `self.lamp.get_state/turn_on/turn_off` |
| BOOT 键 | 0 | 单击：启动中→配网；运行中→`ToggleChatState` |

### 音频（方案 A：ES8311）
| 功能 | GPIO |
|---|---|
| I2C SDA / SCL（codec 控制） | 1 / 2 |
| I2S MCLK / BCLK / WS / DOUT / DIN | 5 / 3 / 4 / 7 / 6 |
| PA 使能（NS4150B SD） | 8 |
| codec 地址 | `ES8311_CODEC_DEFAULT_ADDR`（0x18，AD0=GND） |
| 采样率 | in 16kHz / out 24kHz，`AUDIO_INPUT_REFERENCE=false`（暂无回采，后续做打断再开） |

### 音频初始化策略（关键设计，适配“音频还没装”的当前状态）
```
开机 → i2c_master_probe(0x18)
   ├─ 成功 → Es8311AudioCodec（双工，可录可放）
   └─ 失败 → 打印告警 + 全总线扫描(0x08~0x77)打印命中地址
             → 降级 DummyAudioCodec（系统照常启动，屏/MCP/网络全可用）
```
好处：音频到货前也能完整验证显示、网络、MCP、OTA；接线错误时串口会直接告出实际 I2C 地址，便于定位 AD0/引脚问题。

## 3. 全链路改动（`AGENTS.md` 要求的 5 处）

| 环节 | 改动 |
|---|---|
| `main/boards/esp32s3-lcd20/config.json` | 新增（`scripts/build.py` 自动发现，`--list-boards` 已能列出） |
| `main/Kconfig.projbuild` | 新增 `BOARD_TYPE_ESP32S3_LCD20`；并把该板加入 `choice DISPLAY_LCD_TYPE` 的 `depends on` 列表 |
| `main/CMakeLists.txt` | 新增 `elseif(CONFIG_BOARD_TYPE_ESP32S3_LCD20)` 分支：`BOARD_DIR=esp32s3-lcd20`、字体 `font_noto_sans_basic_20_4`、图标 `font_material_symbols_20_4`、emoji `noto-color-emoji_64` |
| 板级源码 | `DECLARE_BOARD(Esp32S3Lcd20Board)` **仅一处** |
| 文档 | 本文件 |

未改动任何既有板型的引脚或 SKU（符合 `AGENTS.md` 硬规则）。

## 4. 构建验证（✅ 通过）

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-board.ps1 \
    -Board esp32s3-lcd20 -Name esp32s3-lcd20
```

| 项 | 值 |
|---|---|
| `build/xiaozhi.bin` | **2,853,088 B**（2.72MB；分区 4.03MB → 余量 30%） |
| app 校验 | `Checksum 0x06 (valid)`、`Validation hash 002bda5b…(valid)` |
| 版本 / 工程 | `2.4.2` / `xiaozhi` |
| ESP-IDF | **v6.0.2** |
| 编译进镜像的 SKU | `-DBOARD_NAME=esp32s3-lcd20`（镜像内出现 4 次） |
| `build/generated_assets.bin` | **1,664,265 B**（1.59MB / 8MB = **19.8%**，余量 6.4MB） |

### assets 变化（UI 档位升级结果）

| 项 | 升级前（bread-compact-wifi-lcd 16px/32） | 升级后（esp32s3-lcd20 20px/64） |
|---|---|---|
| 文本字体 | `font_noto_sans_common_16_4.bin` 885,948 | `font_noto_sans_common_20_4.bin` **1,287,460** |
| 唤醒模型 | `srmodels.bin` 291,042（仅 wakenet9） | 同（291,042） |
| emoji | 21 张 PNG，1.4~1.9KB（32px） | 21 张 PNG，**3.2~4.6KB（64px）** |
| 合计 | 1,214,609 | **1,664,253** |

> 结论：8MB assets 分区完全够用；后续再加 MultiNet 离线命令词模型（预计 +1~2MB）也无压力。

## 5. 待硬件验证（S2 门槛）

| # | 验证项 | 期望 |
|---|---|---|
| 5.1 | 屏幕点亮 | 240×320 正常、无花屏/镜像错；必要时调 `DISPLAY_INVERT_COLOR` / `MIRROR_*` |
| 5.2 | 背光 | 亮度可调（MCP `self.screen.set_brightness`） |
| 5.3 | ES8311 探测 | 串口 `ES8311 found at 0x18`；否则看扫描输出定位接线/AD0 |
| 5.4 | 录音 / 放音 | 能唤醒、能听到回复 |
| 5.5 | 云连接 | WiFi + MQTT/WS + 音量/主题 MCP 工具可用 |

**烧录前必须确认**：会覆盖设备上现有的“音乐固件”。回滚方案 = 用 S0 权威备份恢复：
```
python -m esptool -p COM15 write-flash 0x0 D:\PI\tools\com15_full_backup_rom.bin   # 整片回滚
```
且 S0 已证明：本工程 assets 与设备 assets **字节级一致**，只回滚 app 亦可（`0x20000` + `0xd000`）。

## 6. 下一步

- **S1 阶段二（不需硬件）**：`main/music/music_player.*` + 4 个 MCP 音乐工具 + 240×320 音乐页（封面/歌词/进度），用 dummy codec 跑通逻辑。
- **S2（需硬件）**：接线上电 → 5.1~5.5 逐项验证 → 出声。
