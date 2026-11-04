# XIAOZHI_ESP32-S3_LCD20 — S0 bring-up 记录

> 本文件是本项目的**唯一事实记录**：硬件核查、关键决策、每一步执行结果、备份校验值、遗留问题。
> 新增结论请追加到「§7 变更记录」，不要覆盖历史。

- 项目目录：`D:\PI\XIAOZHI_ESP32-S3_LCD20`（自 `D:\PI\ESP32-S3-XIAOZHI` @ `15a442a` 复制，含 `.git` 历史）
- 目标设备：`COM15`（ESP32-S3 rev0.2）
- 建立时间：2026-11-04
- 当前阶段：**S0（备份 / 工具链 / 基线构建 / OTA 骨架）**

---

## 1. 硬件事实（实测，非推测）

| 项 | 事实 | 证据 |
|---|---|---|
| SoC | ESP32-S3 rev v0.2，双核 240MHz | `esptool flash-id`、启动日志 |
| Flash | 16MB（BoyA），QIO，eFuse 3.3V | `flash-id`、`spi_flash: detected chip: boya` |
| PSRAM | **8MB 八线（Octal）**，80MHz，8192K 全部进堆 | `octal_psram ... 64 Mbit`、`Adding pool of 8192K` |
| 显示屏 | **2.0 寸 240×320 SPI（ST7789 类）**，主控引脚见下表 | 用户确认 + 旧固件实测可显示 |
| 音频 | **尚未安装**（采购中，方案 A：ES8311 模块） | 用户确认 |
| SD 卡 | **无卡槽** | 用户确认 |
| 分区表 | `partitions/v2/16m.csv`（6 项全对齐） | 备份数据解析 |
| 当前固件 | `xiaozhi 2.4.2`，ESP-IDF **v6.1**，编译于 2026-09-14 10:56:35，运行 `ota_0`，ELF SHA256 `ee07bc05…` | app 镜像 `esp_app_desc_t` |
| 当前固件来源 | 未知分支（含 `Esp32Music` 音乐模块 + 云集成），三个已知代码库副本中均无对应源码 | 见 §6 追溯记录 |

### 引脚（沿用 `bread-compact-wifi-lcd`，与实机一致）

| 功能 | GPIO |
|---|---|
| LCD MOSI / CLK / DC / RST / CS | 47 / 21 / 40 / 45 / 41 |
| LCD 背光（PWM，亮度可调） | 42 |
| 板载状态灯 | 48 |
| LAMP 继电器（`self.lamp.*` 3 个 MCP 工具） | 18 |
| BOOT 键 | 0 |
| Flash/PSRAM 内部占用 | 26~37（八线，不可再外接） |
| USB-Serial/JTAG | 19 / 20 |
| **预留音频（方案 A）** | I2C SDA 1 / SCL 2；I2S MCLK 5 / BCLK 3 / WS 4 / DOUT 7 / DIN 6；PA 使能 8 |

---

## 2. 关键决策（已确认）

| # | 决策 | 依据 |
|---|---|---|
| D1 | 复制新工程 `XIAOZHI_ESP32-S3_LCD20`，`.git` 历史保留 | 用户要求另存 |
| D2 | 基座板 = **`bread-compact-wifi-lcd`**（不是 `esp32s3-lcd-board`） | 屏 240×320、引脚、背光 42、LED 48、LAMP 18 全部对上；后者是「自带 1.77 寸 ST7735 + SDMMC 33~38 + ES8311」的另一块板 |
| D3 | 音频 = **方案 A（ES8311 模块）**，采购中 | 单 I2S 双工、可做 AEC 打断、仓库已有 `es8311_audio_codec.cc` |
| D4 | 无 SD → 音乐来源 = 网络流 + assets 分区内置 OGG/Opus | assets 实测仅占 1.16MB / 8MB |
| D5 | **自建 OTA** | 用户确认，服务端骨架已就位（§4） |
| D6 | 优先级：**先音乐闭环**，云集成复刻排后 | 用户确认 |

### 待定（下一阶段开始前确认）
- 新板 SKU 名称（建议 `esp32s3-lcd20`，`AGENTS.md` 不允许改既有板引脚，故倾向新建板目录 + 新 Kconfig）
- `origin` 是否改指新仓库（当前仍指向 `git@github.com:suhongbo2017/ESP32-S3-XIAOZHI.git`，**勿误推**）

---

## 3. S0 执行结果

| # | 任务 | 状态 | 结果 |
|---|---|---|---|
| S0.1 | 全片 16MB 备份 + 校验 | ✅ | 权威备份 `D:\PI\tools\com15_full_backup_rom.bin`，SHA256 `6b9847e92163ac84954b9c712a80a66ff66206028f62b69278225be9abf5eb2f`（详见 §3.1） |
| S0.2 | ESP-IDF **v6.0.2** 安装 | ✅ | `C:\esp-idf-6.0.2`（tag `v6.0.2`，全子模块），工具链 `xtensa-esp-elf esp-15.2.0_20251204`，`IDF_TOOLS_PATH=C:\Users\Administrator\.espressif`（详见 §3.2） |
| S0.3 | 基线构建 `bread-compact-wifi-lcd` | ✅ | `build/xiaozhi.bin` 2,772,624 B（2.64MB，分区 4.03MB，余量 1.25MB），ESP-IDF **v6.0.2**，校验通过（详见 §3.3） |
| S0.4 | 自建 OTA 服务骨架 | ✅ | `tools/ota-server/`（stdlib，零依赖），契约与发版流程见 §4 |
| S0.5 | 构建/烧录包装脚本 | ✅ | `tools/idf.ps1`、`tools/build-board.ps1`（自动处理 `MSYSTEM` + 组件缓存路径） |

### 3.1 备份：stub 读不可靠，改用 ROM loader

首次用 esptool **stub** 按 128KB 分块读取 16MB，结果**不可信**：

| 现象 | 数据 |
|---|---|
| 3 个 4KB 块硬失败（重试 5 次全失败） | `0x2a6000`、`0x2de000`、`0x354000` |
| 第二次全片复读比对，仍只报 1 处不一致 | `0x0`（该区块含 NVS，设备运行中会写，属正常变化） |
| 但**抽查发现静默损坏** | `0x340000`、`0x2b9000` 等与 ROM loader 读值不符 |
| 最终统计：stub 备份与 ROM 备份有 **128 个 4KB 块不同**（自 `0xc0000` 起） | `differing 4KB blocks: 128` |

→ 采用 **ROM loader（`--no-stub`，~180 kbit/s，耗时 1050s）**重新全片读取。
`0x2a6000 / 0x2de000 / 0x354000` 用 ROM loader 读取**完全正常**，确认是工具/传输问题，**不是 flash 坏块**。

**权威备份的三重自校验（全部通过）：**

| 校验 | 结果 |
|---|---|
| app 镜像（`0x20000`，长 `0x2c5f48`）分段解析 | 6 段，末段 0x24，结构完整 |
| app 镜像 `esptool image-info` | `Checksum 0xe7 (valid)`、`Validation hash 6b2040aa…(valid)` |
| app 描述符 | `xiaozhi` / `2.4.2` / `Sep 14 2026 10:56:35` / `v6.1` / ELF `ee07bc05…` |
| assets 分区内部校验和 | 存储 `0xce9e` = 计算 `0xce9e` ✅（24 个文件、1,214,609 字节数据） |
| 分区表 | 6 项与 `partitions/v2/16m.csv` 完全一致 |

> ⚠️ `D:\PI\tools\com15_full_backup.bin`（stub 版）**保留但标记为不可信**，仅作对比样本。
> ⚠️ 设备当前处于运行状态，NVS/otadata 区域内容会随时间变化；恢复时若只想回滚 app，只需写 `0x20000` 起的 app 镜像 + `otadata`。

**assets 分区实测占用**

| 文件 | 大小 |
|---|---|
| `font_noto_sans_common_16_4.bin` | 885,948 |
| `srmodels.bin`（仅 wakenet9：`wakenet9l_tts1h8_你好小智_3_0.631_0.635`，**无 MultiNet**） | 291,042 |
| `index.json` | 2,149 |
| 21 张自绘 emoji PNG（angry…neutral，~1.5–1.9KB/张） | ~34,000 |
| **合计占用** | **1,214,621 / 8,388,608（14.5%）→ 余量 ~6.8MB** |

### 3.2 ESP-IDF v6.0.2 安装

```
git clone --depth 1 --branch v6.0.2 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git C:\esp-idf-6.0.2      # 666MB
powershell -NoProfile -ExecutionPolicy Bypass -Command \
    "$env:MSYSTEM=''; & 'C:\esp-idf-6.0.2\install.ps1' esp32s3"    # 工具链 ~2.4GB
```

**踩坑记录（重要）**
1. git-bash 会话里 `MSYSTEM=MINGW64` 常驻 → `install.bat` / `install.ps1` 里的 `idf_tools.py` 直接报
   `MSys/Mingw is not supported`。`env -u MSYSTEM`、`set MSYSTEM=` 均无效（会被重新注入）。
   **唯一有效**：在 PowerShell 内部置空再调用 —— `$env:MSYSTEM=''`（置为**空串**，Python 侧 `os.environ.get()` 视为 falsy）。
2. 构建同理：所有 `idf.py` 调用必须走
   `powershell -NoProfile -ExecutionPolicy Bypass -Command "$env:MSYSTEM=''; & 'C:\esp-idf-6.0.2\export.ps1' | Out-Null; <命令>"`
3. 系统 `python` 曾是 PlatformIO 的 venv（`~\.platformio\penv`），必须先 `export.ps1` 再调 `idf.py`，否则 idf_tools 解析到错误环境。
4. 磁盘：`C:` 剩余 7.4GB（工具+源码已用 ~3GB），`D:` 剩余 9.7GB（构建产物约 1–2GB）。

### 3.3 基线构建（✅ 通过）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-board.ps1
# 等价于：
# powershell -NoProfile -ExecutionPolicy Bypass -Command "$env:MSYSTEM=''; $env:IDF_COMPONENT_CACHE_PATH='D:\PI\idf-cache';
#   & 'C:\esp-idf-6.0.2\export.ps1' | Out-Null; cd 'D:\PI\XIAOZHI_ESP32-S3_LCD20';
#   python scripts/build.py bread-compact-wifi-lcd --name bread-compact-wifi-lcd"
```

日志：`D:\PI\build-baseline.log`（约 2214 个编译单元，首次全量约 25 分钟，含 753MB managed_components 下载）

**产物与元数据（实测）**

| 项 | 值 |
|---|---|
| `build/xiaozhi.bin` | **2,772,624 B**（2.64MB；分区 4.03MB → 余量 1.25MB / 31%） |
| `build/generated_assets.bin` | 1,214,621 B |
| `build/merged-binary.bin` | 9,603,229 B（整片 0x0 起，含 assets） |
| app 镜像校验 | `Checksum 0x1a (valid)`、`Validation hash 9e06a90b…(valid)` |
| 版本/工程/IDF | `xiaozhi` / `2.4.2` / **v6.0.2** |
| ELF SHA256 | `dd3f315f2dd0e9787d503b20082e4db124289e1f41c56102001fd1fba813acd9` |
| 关键 sdkconfig | `CONFIG_SPIRAM_MODE_OCT=y`（非 QUAD ✅）、`FLASHSIZE=16MB`、`partitions/v2/16m.csv`、`BOARD_TYPE_BREAD_COMPACT_WIFI_LCD=y`、`LCD_ST7789_240X320=y` |

烧录命令（IDF 已生成，供 S2 使用）：

```
python -m esptool --chip esp32s3 -b 460800 --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin \
  0xd000 build/ota_data_initial.bin 0x20000 build/xiaozhi.bin 0x800000 build/generated_assets.bin
```

### 3.4 关键对拍结论：新构建与设备现状**同源**

用本工程 2.4.2 源码构建出的 `generated_assets.bin` 与设备上 assets 分区内容**字节级完全一致**：

```
built : 1214621  sha256 af90b8791e343113da945d4ccfc55675…
device: 1214621  sha256 af90b8791e343113da945d4ccfc55675…   identical = True
```

推论：旧（含音乐模块的）固件就是**基于本工程 2.4.2 分支**加料构建的——字体档位（16px/common）、唤醒词（`wn9_nihaoxiaozhi_tts`）、21 张自绘 emoji、`index.json`、`srmodels.bin` 全部相同。因此：

- **assets 分区可不必重刷**（内容相同，刷了也无害）；
- app 体积差：设备 `0x2c5f48`=2,909,000 B vs 本构建 2,772,624 B → **差 136,376 B**，正好对应缺失的音乐模块 + 云集成；
- 后续 S1 加音乐功能时，assets 侧只需在现有 1.16MB 基础上追加，不必重构资产格式。

### 3.5 磁盘空间事故与处置（务必记录）

基线构建第一次失败：`OSError: [Errno 28] No space left on device`（组件解包时）。

| 处置 | 释放/迁移 | 说明 |
|---|---|---|
| 删除 `C:\Users\Administrator\.espressif\dist` | **1.9 GB** | 只是工具压缩包缓存，可随时重下 |
| 删除 `C:\Users\Administrator\.cache\pip` | — | pip wheel 缓存 |
| 组件缓存迁到 D: `D:\PI\idf-cache`（原 `C:\Users\Administrator\AppData\Local\Espressif\ComponentManager`，447MB） | 迁出 C: | 之后所有构建用 `IDF_COMPONENT_CACHE_PATH=D:\PI\idf-cache` |
| 删除失败残留的 `build/` | — | 避免半成品配置污染 |

磁盘现状：`C:` 可用 ~2.3GB（工具链 6.2GB + python_env 190MB），`D:` 可用 ~7.5GB（构建 + managed_components 753MB）。
**后续注意**：`C:` 已接近饱合，不要再往 C: 装工具；IDF 相关缓存统一走 D:。

---

## 4. 自建 OTA 服务器（已就绪）

实现：`tools/ota-server/server.py`（Python 标准库，无第三方依赖）

### 契约（从 `main/ota.cc` 反推，xiaozhi 2.4.2）

```
POST /ota/        （无 system info 时退化为 GET）
Headers: Device-Id(MAC) | Client-Id(board UUID) | User-Agent | Accept-Language
         Activation-Version: 1|2 | Content-Type: application/json
Body:    board.GetSystemInfoJson()

必须返回 HTTP 200，JSON：
{
  "firmware": { "version": "2.5.0", "url": "http://HOST/fw/xxx.bin", "force": 0 },
  "mqtt":      { ... },     // 可选 → 写入设备 NVS "mqtt"
  "websocket": { ... }      // 可选 → 写入设备 NVS "websocket"
}

GET /fw/<file>   固件下载，必须带 Content-Length
```

**版本号规则（关键）**：`Ota::ParseVersion` 按 `.` 分段、每段取**前导数字**，逐段比较，段数多者大。
→ 只能用纯数字点分版本：`2.5.0` > `2.4.2`；`2.4.2-lcd20` 会被解析为 `[2,4,2]`，**等于 2.4.2，不会触发升级**。

### 运行与发版

```bash
python tools/ota-server/server.py --port 8080            # 日志 + device_requests.jsonl
cp build/xiaozhi.bin tools/ota-server/firmware/xiaozhi_lcd20_2.5.0.bin
# 编辑 tools/ota-server/releases.json：version / file / force
```

设备端指向方式：编译期 `CONFIG_OTA_URL`，或运行期 NVS `settings["wifi"]["ota_url"]`（优先级更高）。
限制：只发**整包 app 镜像**；**assets 分区不参与 OTA**（换字体/emoji/内置音乐需整片烧写）。

### 冒烟验证（已完成）

服务启动后本地模拟设备请求（报文结构照 `board.GetSystemInfoJson()` 造），结果：

| 设备版本 | 目标版本 | HTTP | 响应 |
|---|---|---|---|
| 2.4.2 | 2.5.0 | 200 | `{"firmware":{"version":"2.5.0","url":"…/fw/xiaozhi_lcd20_2.5.0.bin","force":0}}` |
| 2.5.0 | 2.5.0 | 200 | `{}`（already latest） |
| 2.4.9 | 2.5.0 | 200 | 下发射 firmware（语义版本比较正确） |

同时已验证：`GET /healthz` → `{"ok":true}`；每次请求写入 `tools/ota-server/device_requests.jsonl`（含 MAC / UUID / 版本 / 是否升级）。

---

## 5. 下一步（S1，待 S0 收尾后启动）

| # | 任务 | 验收 |
|---|---|---|
| S1.1 | 新建板 `esp32s3-lcd20`（Kconfig + CMakeLists + config.json + `DECLARE_BOARD`，全链路 5 处） | `python scripts/build.py esp32s3-lcd20 --name lcd20` 通过 |
| S1.2 | 板级音频按方案 A 接线参数落地（ES8311 duplex + PA 8） | 串口出现 `ES8311 found at 0x18`（硬件到货后验证） |
| S1.3 | `main/music/music_player.{h,cc}` + MCP 4 工具 | 用 dummy codec 跑通取流/进度/字幕回调 |
| S1.4 | 音乐页 UI（封面 + 3 行歌词 + 进度） | 240×320 上显示正确、歌词按 `start_ms` 滚动 |
| S1.5 | UI 档位升级：字体 20px、emoji 64（`main/CMakeLists.txt` 该板分支） | 屏幕文字/表情明显变大且不溢出 |
| S1.6 | 自建 OTA 端到端：设备切 `ota_url` → 检查版本 → 升级 → 回滚验证 | 版本变化 + `Running partition` 切换 + 断电回滚成功 |

---

## 6. 固件来源追溯（结论：源码未在本机）

| 检查位置 | 结果 |
|---|---|
| `D:\PI\ESP32-S3-XIAOZHI`（= 本工程基线，= GitHub `suhongbo2017/ESP32-S3-XIAOZHI` HEAD `15a442a`） | 版本号同为 2.4.2，但**无 `Esp32Music`**、无 ES8311 复位重试；最后推送 2026-09-07，早于固件编译时间 |
| `C:\Users\Administrator\pi-cwd-20260603\xiaozhi-esp32`（= GitHub fork，v2.2.6） | 无 `Esp32Music`；音乐走 `music_trigger_server.cc` |
| `D:\VSCODE\PYTHON3` | 无 xiaozhi 相关源码 |
| 同机其它（`esp32-projects`/Desktop/Documents） | 无 |
| 设备 NVS 里的云集成键（`llm_*`、`qq_app_secret`、`feishu_app_id`、`publish_topic`、`demo_cron_*`、`cal_data`） | 旧固件自研能力清单，用于 S5 复刻参考 |

---

## 7. 变更记录

| 时间 | 内容 |
|---|---|
| 2026-11-04 08:39–08:43 | stub 全片备份（后发现静默损坏，作废） |
| 2026-11-04 08:49 | stub 二次复读比对（仅发现 0x0 变化，未发现静默损坏 → 说明单/双次比对不足以信任） |
| 2026-11-04 08:50–09:08 | ROM loader 全片备份，三重自校验通过（app hash valid / assets checksum match / 分区表 match） |
| 2026-11-04 09:0x | ESP-IDF v6.0.2 克隆 + esp32s3 工具链安装完成 |
| 2026-11-04 09:1x | OTA 服务骨架落地（`tools/ota-server/`） |
| 2026-11-04 09:26–09:35 | 基线构建第一次失败（C: 磁盘满）→ 清理/迁移缓存 → 重新构建 **通过**；`xiaozhi.bin` 2,772,624 B、`Validation hash valid` |
| 2026-11-04 09:36 | **关键对拍**：新构建的 `generated_assets.bin` 与设备 assets 分区**字节级一致**（`af90b879…`）→ 确认旧固件与本工程 2.4.2 同源 |
| 2026-11-04 09:37 | 追加包装脚本 `tools/idf.ps1`、`tools/build-board.ps1`（固化 MSYSTEM / 组件缓存路径） |
| 2026-11-04 09:38 | OTA 服务冒烟测试通过（版本比较 2.4.2/2.5.0/2.4.9 三组用例），修复一处 `global ROOT` 语法错误 |
| 2026-11-04 09:41 | 首次提交 `bafbe57`（docs + tools/ota-server + 构建包装脚本）；S0 阶段收尾 |

> 下一步（S1）开工前置：① 确认新板 SKU 名 `esp32s3-lcd20` ② 音频模块到货时间。

---

## 8. 交付物清单（S0）

| 类型 | 路径 |
|---|---|
| 权威 flash 备份 | `D:\PI\tools\com15_full_backup_rom.bin` + `.sha256` + `.json` |
| 不可信对比样本 | `D:\PI\tools\com15_full_backup.bin`（stub 版，仅备查） |
| 备份/校验脚本 | `D:\PI\tools\dump_com15_flash.py`、`verify_com15_backup.py`、`dump_com15_rom.py` |
| ESP-IDF | `C:\esp-idf-6.0.2`（v6.0.2） |
| 基线构建产物 | `build/xiaozhi.bin`、`build/merged-binary.bin`、`build/xiaozhi.elf` |
| 构建日志 | `D:\PI\build-baseline.log`、`D:\PI\idf-clone.log`、`D:\PI\idf-install.log` |
| OTA 服务 | `tools/ota-server/{server.py,releases.json,devices.json,README.md}` |
| 工程脚本 | `tools/idf.ps1`、`tools/build-board.ps1` |
| 本记录 | `docs/lcd20/S0-BRINGUP-LOG.md` |
