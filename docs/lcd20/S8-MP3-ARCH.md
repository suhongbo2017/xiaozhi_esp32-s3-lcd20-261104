# MP3 播放架构重做（2026-11-11）

## 1. 旧架构的问题

```
设备 → NotifyPlayer(HTTP) → 服务端 /stream_ogg → ffmpeg 转码 → OGG/Opus → 设备解码
```
- 服务端必须 **ffmpeg 转码**：首播 20~180 秒，超时即 503（实测 ccMixter 下载 ~72KB/s，180s 超时）
- 设备只支持 Opus：任何"只有 MP3"的源都必须先转码，链路长、失败点多
- 转码失败/超时 → 设备 `NotifyPlayer: status 503` → "播放到一半退出/直接失败"

## 2. 新架构（设备端解码 MP3）

```
设备 → 服务端 /play?id=  → {audio_url: MP3 直链, title, artist}
     → Mp3StreamPlayer: HTTP 分块拉流 → minimp3 逐帧解码 → 单声道化 → 重采样 24k
     → AudioService 播放队列（raw_pcm 包，跳过 Opus 解码）
     → I2S → NS4168 → 喇叭
```
- **服务端不再转码**：只做"搜索 + 解析直链 + 歌词"
- 设备 **边下边播**，任意时长；断流/结束会正常收尾并回调 `OnFinished`
- 失败自动回退旧链路（`/stream_ogg`），不会"整首歌消失"

## 3. 代码变更清单

| 文件 | 变更 |
|---|---|
| `main/music/minimp3/`（新增） | minimp3.h（public domain/CC0）+ `minimp3_impl.c` 单编译单元 |
| `main/music/mp3_stream_player.{h,cc}`（新增） | HTTP 拉流 → minimp3 解码 → 单声道化 → raw_pcm 入队；`Start/Stop/position` |
| `main/protocols/protocol.h` | `AudioStreamPacket` 增加 `raw_pcm` 标记 |
| `main/audio/audio_service.cc` | 解码任务支持 `raw_pcm` 直通（含重采样），跳过 Opus |
| `main/music/music_api_client.{h,cc}` | 新增 `PlayInfo()`（`/play?id=` → MP3 直链 + 标题/歌手/时长） |
| `main/music/music_player.cc` | `PlayOnlineById()` 改走 **MP3 直放**，失败自动回退 `stream_ogg` |
| `main/application.{h,cc}` | 新增 `StartMusicMp3()`（同一套状态机/ducking/唤醒词打断），`StopMusic` 同时停 mp3 |
| `main/CMakeLists.txt` | 加入 mp3 源文件与 minimp3 |

## 4. 服务端配套（`tools/xiaozhi-music/music_server.py`）
- `/play?id=` 返回 `audio_url`（MP3 直链）+ title/artist，tx 分支带 **3 次重试**（vkeys 偶发 503）
- ccMixter 曲目支持**按 upload_id 现查**（服务重启后缓存为空也能解析）
- `/stream_ogg` 保留作为兜底

## 5. 实测（PC 侧，签名等价设备）
| id | /play 结果 |
|---|---|
| tx511331370 | title=CRAZY，MP3 直链 ✓ |
| tx102065756 | title=七里香，MP3 直链 ✓ |
| ik0 | title=Monkeys Spinning Monkeys，MP3 直链 ✓ |
| cc71324 | title=Jazz Backing Track，MP3 直链 ✓（现查未命中缓存也 OK） |

## 6. 仍然存在的现实（与架构无关）
- QQ 音乐 **VIP 曲目只给 60 秒试听**（`RS02...mp3`），这是版权方限制；免费源拿不到完整版
- CC 曲目（Incompetech / ccMixter）可以完整播放
- 想要 VIP 完整版只能用自己的会员 Cookie（服务端已支持 `NETEASE_COOKIE`）或盗版站（不建议）
