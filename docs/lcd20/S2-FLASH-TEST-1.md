# 首次烧录 + 音乐闭环实测（2026-11-04）

> 目标：把 `esp32s3-lcd20` 固件烧进 COM15 设备，实机跑一次"取流 → 解码 → 歌词 → 页面"，
> 在音频硬件到货前把软件链路全部验证掉。

## 1. 烧录

```
powershell -NoProfile -ExecutionPolicy Bypass -Command "
  $env:MSYSTEM=''; $env:IDF_COMPONENT_CACHE_PATH='D:\PI\idf-cache';
  & 'C:\esp-idf-6.0.2\export.ps1' | Out-Null; cd 'D:\PI\XIAOZHI_ESP32-S3_LCD20';
  idf.py -p COM15 flash"
```
结果（全部 `Hash of data verified`）：

| 目标 | 偏移 |
|---|---|
| bootloader / partition-table / otadata | 0x0 / 0x8000 / 0xd000 |
| `xiaozhi.bin`（2,869,808 B） | 0x20000 |
| `generated_assets.bin`（1.66MB，20px 字体 + 64px emoji） | 0x800000 |

> 设备原固件已由 S0 权威备份保全：`D:\PI\tools\com15_full_backup_rom.bin`。

## 2. 启动验证（串口实录）

```
I (862) Board: UUID=faf666c4-... SKU=esp32s3-lcd20                  ← 新 SKU 生效
W (862) Esp32S3Lcd20Board: ES8311 not found at 7bit 0x18 / 8bit 0x30, audio falls back to dummy codec
I (1082) Esp32S3Lcd20Board: LCD 240x320 ready                        ← 面板初始化成功
I (1132) AudioCodec: Audio codec started                             ← dummy codec 占位
I (1182) MCP: Add tool: self.music.search_song
I (1182) MCP: Add tool: self.music.play_song
I (1192) MCP: Add tool: self.music.play_song_by_id
I (1192) MCP: Add tool: self.music.set_display_mode
I (7342) WifiStation: Got IP: 192.168.102.26
I (8132) Ota: Current is the latest version
I (8932) StateMachine: State: activating -> idle
```

## 3. 音乐闭环实测（bring-up 自检）

自检方式：`CONFIG_MUSIC_SELFTEST_URL`（新增的 bring-up 开关，默认空）指向 PC 上的
`test_song.ogg`（24s 双音 OGG/Opus，已入库 `tools/music-test/test_song.ogg`），开机 15s 自动播一次，
自带 0s / 8s / 16s 三句合成歌词。

```
I (16202) MusicPlayer: self-test: playing http://192.168.100.17:8123/test_song.ogg
I (16212) StateMachine: State: idle -> notifying
I (16242) MusicPage: music page created (240x320)                    ← 音乐页创建成功
I (16302) MusicPlayer: playing '自检音轨' - 'LCD20 bring-up'
I (16312) MusicPlayer: lyric[0] @0 ms:     第一句：链路自检开始
I (17592) MusicPlayer: lyric[1] @8000 ms:  第二句：HTTP 取流 + Opus 解码
I (18822) MusicPlayer: lyric[2] @16000 ms: 第三句：歌词与进度同步
I (20042) MusicPlayer: playback finished (ok)
I (20052) StateMachine: State: notifying -> idle                     ← 收尾正常
```

**结论：软件链路全部打通** —— HTTP 取流、OGG 解复用、Opus 解码、歌词按时间轴切换、音乐页渲染、播完回 idle。

**尚未验证（等硬件）**
- 实际出声：当前 `DummyAudioCodec`，写扬声器是空操作，所以**没有声音**；
- 由于没有 I2S 时钟"节奏"，进度比实时快约 4 倍（24s 音频 3.7s 走完）→ 歌词切换顺序正确但节奏被压缩。
  接上 ES8311 后 I2S 提供真实节拍，进度即与声音同步。
- 屏幕观感（颜色/镜像）需肉眼确认。

## 4. 本次实测发现并修掉的 4 个问题

| # | 问题 | 现象 | 修复 |
|---|---|---|---|
| 1 | **I2C 探测地址用了 8bit 形式** | 日志 `ES8311 not found at 0x30`；即便硬件接上也会永远探测失败 | `i2c_master_probe(bus, AUDIO_CODEC_ES8311_ADDR >> 1)`，日志同时打印 7bit/8bit |
| 2 | **单曲列表无限循环** | 列表只有 1 首时 `Next()` 会 `(0+1)%1=0` 重播 | 只有 `playlist_.size() > 1` 才自动续播 |
| 3 | **播完不回 idle** | 日志 `device busy (state=7), music ignored`，之后所有播放被拒 | `OnFinished` 收尾时 `Schedule(StopMusic())` 复位状态 |
| 4 | **HTTPS 到 raw.githubusercontent.com 失败** | `mbedtls_ssl_handshake returned -0x7280` / `ESP_ERR_NOT_FOUND` 连接失败 | 非阻塞：自建 OTA 与测试音源一律走 **plain HTTP**（设备在局域网可达 PC） |

## 5. 网络可达性（踩坑记录）

- PC 的地址：`192.168.100.17/23`（Hyper-V 虚拟交换机）；设备在 `192.168.102.26/23`（WiFi `JHS`）
  —— 看似不同网段，但**实际可互通**（PC→设备 ping 通；设备→PC 的 HTTP 请求成功，`netstat` 可见
  `192.168.100.17:8123 ← 192.168.102.26:63532`）。
- 本机 **8000 端口已被其它服务占用**（返回 8121 字节的 HTML），测试改用 **8123** 并加防火墙入站规则。
- 自检 URL 只在本地 `sdkconfig.defaults`/`sdkconfig` 临时设置，**未提交**；代码里的默认值为空。

## 6. 复现方法（下次开机复看）

```bash
# 1) PC 起一个静态 HTTP 服务（8123 已在防火墙放行）
cd D:\PI\XIAOZHI_ESP32-S3_LCD20\tools\music-test && python -m http.server 8123
# 2) 设备重启后等约 15s 自动播放
# 3) 想看串口：
python -c "import serial,time;s=serial.Serial('COM15',115200,timeout=0.2);t=time.time();b=b'';\
while time.time()-t<40: b+=s.read(8192);print(b.decode('utf-8','replace'))"
```

## 7. 遗留

| # | 项 | 说明 |
|---|---|---|
| 7.1 | 屏幕观感确认 | 需要人看：颜色是否正常（`DISPLAY_INVERT_COLOR`）、是否镜像、字体 20px 是否溢出 |
| 7.2 | 出声验证 | 等 ES8311 到货（约 4 天） |
| 7.3 | 音频到货后首次上电 | 期望日志 `ES8311 found at 7bit 0x18 (8bit 0x30)`，若未命中会打印全总线扫描结果 |
| 7.4 | 正式固件 | 自检开关在源码里默认空；正式发版前重新 `build.py` 一次即可（当前设备上是带自检的调试固件） |
