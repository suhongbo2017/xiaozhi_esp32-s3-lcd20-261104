# 音乐 MCP 逻辑分析（当前实现，2026-11-16）

## 1. 全景

```
┌─────────────── 设备（ESP32-S3） ───────────────┐        ┌──── 服务器 47.116.199.150:2233 ────┐
│ LLM/语音 → MCP 工具（self.music.*）            │        │ music_server.py（stdlib, :2233）    │
│   → MusicPlayer（业务：曲库/队列/歌词/页面）    │  HTTP  │  /search  /play  /proxy            │
│     → Application::StartMusicMp3（状态机+抢占） │ ─────► │  /library/*  /lyric_json           │
│       → Mp3StreamPlayer（HTTP→minimp3→PCM）     │        │  /stream_ogg(兜底) /stock/*        │
│         → AudioService（raw_pcm 直通+重采样）   │ ◄───── │  数据源：本地 CC 库 + ccMixter     │
│           → AudioCodec → I2S → NS4168 → 喇叭   │  音频  │  + Incompetech（全部 CC 授权）      │
└────────────────────────────────────────────────┘        └────────────────────────────────────┘
```

## 2. MCP 工具契约（4 个，注册于 `main/music/music_online.cc`）

| 工具 | 参数 | 返回 | 副作用 |
|---|---|---|---|
| `self.music.search_song` | `song_name`*(必填)*, `artist_name`, `offset` | 服务端原始 JSON：`{total, offset, count, has_more, items:[{index,id,name,artist,album,duration_ms,playable,trial}]}` | 无播放动作；填充设备侧 `song_cache_` |
| `self.music.play_song` | `song_name`, `artist_name`, `url`, `keyword`, `index` | `true/false` | 优先级 `url` > `song_name` > `keyword` > `index` → 触发播放 |
| `self.music.play_song_by_id` | `song_id`*(必填)* | `true/false` | 播放指定 id（`lb*`/`cc*`/`ik*`） |
| `self.music.set_display_mode` | `mode`：`lyrics`/`lyric`/`spectrum`/`cover`/`hidden` | `true/false` | 0=歌词页 1=封面/频谱 2=隐藏 |

其它板级工具：`self.stock.quote` / `self.stock.search`（走同一服务端 `/stock/*`）。

## 3. 主流程（点歌）

```
用户"放点爵士"
 └─ LLM 调 self.music.play_song{song_name:"爵士"}
     └─ MusicPlayer::PlayOnline("爵士","")
         ├─ MusicApiClient::Search → GET /search?song=爵士&limit=5
         │    服务端: 本地曲库(标签/标题) → ccMixter(tags=jazz) → Incompetech 目录
         │    返回 items（id 前缀 l*/cc*/ik*）
         ├─ RememberSong(id,title,artist) 填入 song_cache_
         ├─ SetOnlineQueue(ids, 首个 playable 下标)   ← 供"下一首/上一首"
         └─ PlayOnlineById(id)
             ├─ MusicApiClient::PlayInfo → GET /play?id=xx   → {audio_url, title, artist, duration_ms}
             │    本地库: http://47.116.199.150:2233/library/*.mp3
             │    远端 CC: http://47.116.199.150:2233/proxy?id=cc..  （服务端补 Referer）
             ├─ MusicApiClient::GetLyrics → GET /lyric_json/xx  （CC 曲目为 {lines:[]}）
             └─ Application::StartMusicMp3(audio_url, lyrics)
                 ├─ 若 state≠idle：AbortSpeaking / CloseAudioChannel → 500ms 后在主任务重试
                 ├─ ducking：音量 ×CONFIG_MUSIC_DUCK_VOLUME_PERCENT(60%)
                 ├─ SetDeviceState(notifying)  + EnableWakeWordDetection(true)
                 └─ Mp3StreamPlayer::Start(url, playback_id, cb)
                     └─ 任务：HTTP 分块读 → minimp3 逐帧解码 → 单声道化
                        → 打包 raw_pcm 包（含 media_position_ms）→ PushPacketToDecodeQueue
                          └─ AudioService 解码任务：raw_pcm 直通 + 重采样到 24k → 播放队列 → codec
                          └─ on_playback_progress → MusicPlayer::OnProgress → 歌词/进度页
```

## 4. 分支流程

| 场景 | 路径 |
|---|---|
| 按 id 播放 | `play_song_by_id{song_id}` → PlayOnlineById（跳过搜索） |
| URL 直推 | `play_song{url}` → MusicPlayer::PlayTrack → 旧 `StartMusic`（OGG/Opus 链路） |
| 随机播放 | `play_song{}`（无参）→ PlayOnline("") → 服务端返回本地库随机曲 |
| 下一首/上一首 | 实体键/语音 → `MusicPlayer::Next()/Prev()`：先走 `online_queue_`，再走本地 `playlist_` |
| 唤醒词打断 | AFE 检测 → `WakeWordInvoke` → `StopNotification()`/`OnEngineStopped()` → 停 mp3 + 收页面 + 进聆听 |
| 按键打断 | BOOT 单击/长按 → `MusicPlayer::Stop()` |
| 自然结束 | `Mp3StreamPlayer` 回调 `OnFinished(ok)` → 队列非空则 `Next()`，否则收页 + `StopMusic()`（恢复音量） |
| 流异常 | HTTP 读失败 → `ok=any_frame`；无有效帧则播放失败，回退 `stream_ogg` 链路 |

## 5. 状态与数据模型（设备侧）

| 成员 | 作用 |
|---|---|
| `playlist_` / `current_index_` | 本地曲目列表（`SetPlaylist/PlayIndex`，目前仅有测试用途） |
| `online_queue_` / `online_index_` | **在线播放队列**（搜索结果的 id 序列），切歌用 |
| `song_cache_` | id → {title, artist, duration_ms}（by_id 播放时补元信息） |
| `current_` | 当前曲目（含歌词时间轴） |
| `display_mode_` | 0 歌词 / 1 封面 / 2 隐藏 |
| `page_` | `MusicPage`（LVGL 五行歌词页，懒创建） |
| `playing_` | 是否在播（按键打断、状态判断用） |

音频线程模型：`mp3_stream` 任务（8KB 栈，prio 3）→ `AudioService` 解码任务 → 播放任务 → I2S；
LVGL 任务在 core1；所有 UI 操作走 `DisplayLockGuard`，跨任务收尾统一 `Application::Schedule()`。

## 6. 服务端

| 端点 | 鉴权 | 说明 |
|---|---|---|
| `GET /search` | X-Key | 本地库(标签/标题词) + ccMixter(`search=`/`tags=`/`usernames=`，支持 `offset` 分页) + Incompetech 目录；404 时返回 `available_genres` |
| `GET /play?id=` | X-Key | 返回 `audio_url`：`lb*`→`/library/…`；`cc*/ik*`→`/proxy?id=`（服务端补 Referer） |
| `GET /proxy?id=` | **免鉴权** | 带 `Referer` 转发 ccmixter/incompetech MP3（解决 403），边转发边下发 |
| `GET /library/<file>` | **免鉴权** | 本地 CC 曲库直发 MP3（Range 未实现，整文件流式下发） |
| `GET /lyric_json/<id>` | X-Key | CC 曲目无歌词 → `{lines: []}` |
| `GET /stream_ogg?song=/id=` | X-Key | 旧链路兜底（ffmpeg 转 OGG/Opus，仅免费源） |
| `GET /stream_pcm?song=` | X-Key | 兼容旧的"返回 audio_url"调用 |
| `GET /stock/quote`、`/stock/search` | X-Key | 腾讯行情（A股/港股/美股） |

鉴权：`X-Key = sha256("{mac}:{chip}:{uptime_s}:{secret}")[:32]`；`/library` 与 `/proxy` 放开（设备 MP3 播放器不带 header）。

## 7. 失败矩阵（每步失败会怎样）

| 失败点 | 表现 | 兜底 |
|---|---|---|
| `/search` 404 | 助手提示"只有免费曲库"，并给出可用风格 | `available_genres` 引导换风格 |
| ccMixter 限流/超时 | 结果变少 | 本地库 + Incompetech 目录 |
| `/play` 取不到 audio_url | 播放失败 | 回退 `stream_ogg`（免费源转码） |
| `/proxy` 上游 403/超时 | 无音频 | 无（下次再试；日志 `proxy failed`） |
| minimp3 首帧前数据不足 | 缓冲等待 | 缓冲 4 倍上限后丢包重同步（`mp3dec_init`） |
| 解码队列满 | 丢包/停止 | 日志 + cancel |
| 状态不是 idle | 拒绝播放 | 抢占：abort → 500ms 后重试 |
| 播放结束/流断 | 自动收尾 | 队列非空则下一首 |

## 8. 已知缺陷与技术债（按严重度）

| # | 问题 | 影响 | 建议 |
|---|---|---|---|
| 1 | **服务端 `do_GET` 存在重复端点块**（`/search`、`/play`、`/stream_pcm`、`/stock/*` 各出现 2~3 次，后者为死代码） | 改动容易落在死代码里（之前"还是出错"就踩过）；文件 2090 行偏大 | 重构成路由表 `{path: handler}` 或 `if/elif` 单块；我尝试过自动去重，因块边界误删到 `main()`，已回滚到可用版本（git `70ef020`） |
| 2 | `play_song{index}` 走的是**本地 `playlist_`**（在线播放时为空） | 说"放第 3 首"会失败 | index 应映射到 `online_queue_` |
| 3 | `OnFinished(0, ok)` 传 `playback_id=0` | 引擎侧无法按 id 过滤（当前无副作用，但不严谨） | 传真实 playback_id |
| 4 | 首次播 ccMixter 曲目要经 `/proxy` 实时转发（72KB/s 级） | 设备 120s 超时内可能失败 | 服务端按需缓存到 `library/`，或预取前 2 首 |
| 5 | 无暂停/继续/拖动进度 | 只能停/下一首 | 需要 AudioService 的暂停语义 + MP3 字节偏移 seek |
| 6 | 歌词仅来自旧链路（CC 曲目无 LRC） | 音乐页只显示「♪ 纯音乐（CC 授权）」 | 可选：接歌词源或展示曲目/授权信息 |
| 7 | `/library` 不支持 Range | 无法断点续传/seek | 增加 Range 支持（写 206） |
| 8 | 音量 ducking 依赖 `output_volume()` 快照 | 若播放中用户手动调音量，恢复时会覆盖 | 记录"用户手动改过"标志 |
| 9 | `online_queue_` 不去重 | 多次搜索可能重复入队 | 入队前按 id 去重 |
| 10 | 音源全部 CC（无流行曲） | 用户点流行歌必然搜不到 | 设计如此（版权源已移除）；如需恢复走 `NETEASE_COOKIE` |

## 9. 与旧架构对比

| 维度 | 旧（music fork） | 现 |
|---|---|---|
| 音频解码 | 设备端 minimp3（`Esp32Music`） | 设备端 minimp3（`Mp3StreamPlayer`，重新实现） |
| 音源 | 自建 2233（网易云代理，11s~完整混合） | 本地 CC + ccMixter + Incompetech（全部 CC） |
| 歌词 | 服务端 LRC | 结构保留，CC 曲目为空 |
| 显示模式 | SPECTRUM / LYRICS | lyrics / cover / hidden（兼容旧值） |
| 打断 | 无（据日志） | 唤醒词 + 按键 + ducking |
