# S1 阶段二：音乐闭环（引擎 + MCP + 音乐页）

> 目标：在无音频硬件的情况下先把「选曲 → 取流 → 歌词滚动 → 音乐页」整条链路跑通，
> 音频到货后只剩「换 codec 出声」这一步验证。

- 时间：2026-11-04
- 构建：✅ `xiaozhi.bin` **2,867,120 B**（+14KB），`Checksum/Hash valid`，ESP-IDF v6.0.2
- 4 个 MCP 工具名均已编译进镜像（各 1 处命中）

---

## 1. 架构：不自建音频引擎，复用 NotifyPlayer

```
MCP self.music.*  ──►  MusicPlayer（业务层：曲库/歌词/UI）
                          │
                          ├─► Application::StartMusic(url, lyrics)
                          │        └─ 复用 notify_player_（HTTP 流 + Opus 解码 + 时间轴字幕）
                          │           状态：idle → notifying（唤醒词可自然打断）
                          └─► MusicPage（LVGL 全屏页：封面/标题/3 行歌词/进度条）
                                   ▲
       AudioService::on_playback_progress ──► Application ──► MusicPlayer::OnProgress
```

**为什么这么做**
- `NotifyPlayer` 已经是"HTTP 音频流 + 时间轴字幕"的成熟实现（OGG/Opus 解复用 + 解码队列 + 断流下溢处理），歌词本质就是时间轴字幕，直接复用零风险；
- 沿用 `kDeviceStateNotifying` 状态 → **唤醒词打断免费获得**（`WakeWordInvoke()` 在该状态下会自动 `StopNotification()` 并开始监听）；
- 不改动 `LcdDisplay` 内部布局，音乐页作为独立 LVGL 全屏容器挂在 `lv_screen_active()` 上，只影响调用它的板型。

## 2. 新增 / 修改文件

| 文件 | 说明 |
|---|---|
| `main/music/music_player.h/.cc` | 曲库 + 播放控制 + 歌词时间轴 + 4 个 MCP 工具（~430 行） |
| `main/music/music_page.h/.cc` | 240×320 音乐页（LVGL，字体取主题 `text_font`，中文歌词可显示） |
| `main/application.h` | 新增 **public** `StartMusic(url, lyrics)` / `StopMusic()` |
| `main/application.cc` | ① 实现 StartMusic/StopMusic（音频处理开关、drain 上行队列、ResetDecoder、状态切换）<br>② `on_playback_progress` 回调分发给 MusicPlayer<br>③ `StopNotification()` 里通知 MusicPlayer 收尾（被唤醒词打断时 UI 不残留）<br>④ `Application::Initialize()` 里 `MusicPlayer::Initialize()` 注册工具 |
| `main/CMakeLists.txt` | 加入 `music/music_player.cc`、`music/music_page.cc`，include 目录加 `music` |

## 3. MCP 工具契约（服务端 / LLM 侧照此对接）

### `self.music.search_song`
| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| keyword | string | 否（默认 ""） | 按标题 / 歌手 / id 模糊匹配；空则列全部 |

返回：JSON 数组字符串
```json
[{"id":"1","title":"夜曲","artist":"周杰伦","duration_ms":227000}]
```

### `self.music.play_song`
| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| url | string | 否 | 直接推流地址（`http(s)://…`，OGG/Opus）。给了就走"临时曲目" |
| title / artist | string | 否 | 配合 url 显示用 |
| keyword | string | 否 | 从曲库按关键词选曲 |
| index | integer | 否（默认 -1） | 从曲库按 0 基下标选曲 |

优先级：`url` > `keyword` > `index`（≥0）> 默认第 0 首。返回 `true`/`false`。

### `self.music.play_song_by_id`
| 参数 | 类型 | 必填 |
|---|---|---|
| id | string | 是 |

### `self.music.set_display_mode`
| 参数 | 类型 | 必填 | 取值 |
|---|---|---|---|
| mode | string | 是 | `lyric`（歌词页，默认）/ `cover`（只显示封面+进度）/ `hidden`（不显示音乐页） |

**前置条件**：播放要求设备处于 `idle`（正在对话/播报时会被拒绝并打印 `device busy`）。

## 4. 音乐页布局（240×320）

```
┌──────────────────────────┐
│        ┌──────────┐      │  封面占位 96×96（圆角 + 音符图标，后续可换真图）
│        │    ♪     │      │
│        └──────────┘      │
│        夜曲（20px，跑马灯）│  标题：theme text_font
│         周杰伦（70% 透明）│  歌手
│                          │
│      上一句（50% 透明）    │
│      ▶ 当前歌词 ◀         │  当前行整行高亮
│      下一句（50% 透明）    │
│                          │
│  ▬▬▬▬▬▬▬▬▬▬░░░░░░░░░░    │  进度条
│     01:12 / 03:47        │  时间
└──────────────────────────┘
```
- 歌词切换由 `on_playback_progress`（来自音频包的真实 `media_position_ms`）驱动，不是靠定时器估算，因此与声音同步；
- 所有 LVGL 操作在 `DisplayLockGuard` 内执行；
- 歌词为空时不报错，只留空行。

## 5. 关键行为

| 场景 | 行为 |
|---|---|
| 播放中用户喊唤醒词 | `WakeWordInvoke` → `StopNotification()` → `MusicPlayer::OnEngineStopped()` → 播放停止、音乐页收起、进入监听 |
| 一首歌播完 | `OnFinished` → 自动播下一首（曲库非空时）；否则收起音乐页、状态回 idle |
| 播放中来 server 通知(notify) | `StartNotification` 检查 `notify_player_.IsBusy()` → 忽略通知（音乐优先），日志告警 |
| 设备不在 idle | `play_song` 返回 false，日志 `device busy (state=…)` |
| 断流/下溢 | 由 NotifyPlayer 处理并打 WARN；连续下溢不会崩 |

## 6. 构建验证

| 项 | 结果 |
|---|---|
| `build/xiaozhi.bin` | 2,867,120 B（2.73MB / 4.03MB，余量 32%） |
| 校验 | `Checksum 0x52 (valid)`、`Validation hash 132a3b07…(valid)` |
| 音乐模块目标文件 | `music_player.cc.obj` 1.39MB、`music_page.cc.obj` 177KB |
| MCP 工具字符串 | `self.music.play_song` / `search_song` / `play_song_by_id` / `set_display_mode` 各命中 1 次 |
| assets | 1,664,265 B（未变） |

## 7. 已知限制（下一阶段处理）

| # | 限制 | 计划 |
|---|---|---|
| 7.1 | 曲库默认**为空**：无 SD 卡槽，曲目来源靠 `play_song{url}` 直推或服务端下发 | 接入自建音乐服务（`60s-news-mcp/music_server.py` 可复用）；或把 3~5 首 OGG 打进 assets 分区（余量 6.4MB） |
| 7.2 | 仅支持 **OGG/Opus**（NotifyPlayer 的 demuxer 只认 OGG） | MP3 需引入 minimp3（独立 `main/music/mp3_*`），或服务端统一转 OGG |
| 7.3 | 无暂停/继续/拖动进度 | 需要 AudioService 的暂停语义，单列任务 |
| 7.4 | 歌词只跟着进度走，长句换行依赖 `LONG_WRAP` | 视实际歌词观感微调 |
| 7.5 | 播放期间上行语音关闭（同 notify 语义），不能边放边对话 | 若要做"边听边说"需 AEC 参考声道（S2 音频硬件到位后评估） |
| 7.6 | 音量仍走 `self.audio_speaker.set_volume` | 可在音乐页加音量控件（可选） |
| 7.7 | **无 LVGL 的板型**（OLED/无屏）`music_page` 走空实现 stub：音乐照常播放，只是不显示页面 | 已验证编译期用 `HAVE_LVGL` 分流 |

### 构建流程注意（踩坑）
- **构建必须前台执行**：用 `nohup ... &` 后台跑 `powershell -File tools/build-board.ps1` 会偶发在 CMake configure 阶段静默退出（日志停在 “Generated default assets flash configured”，无错误、无产物更新）。
  前台执行（`powershell ... -File tools/build-board.ps1 ...` 直接等待）稳定，退出码 0 且日志出现 `Project build complete`。
- 判定“构建是否真的成功”要看三样：退出码、`Project build complete` 出现次数、`build/xiaozhi.bin` 时间戳与
  `esptool image-info` 的 `Checksum/Hash valid`。

## 8. 下一步

1. **S2（硬件到货，约 4 天）**：接线 → `ES8311 found at 0x18` → 出声 → 用上面的 7.1 方式准备 1 首 OGG 做端到端验证；
2. **S1 收尾（不需硬件）**：把服务端曲库/歌词接口定下来（`GET /music/search`、`/music/<id>` 返回 `{url, lyrics[{start_ms,text}], duration_ms}`），设备侧 `play_song{keyword}` 走 MCP → 服务端。
