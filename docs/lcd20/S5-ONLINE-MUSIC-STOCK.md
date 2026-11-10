# S5：在线音乐 + 股票（对接自建服务 47.116.199.150:2233）

## 1. 找到了旧固件用的服务端

```
root@47.116.199.150 (Ubuntu 24.04)
/opt/xiaozhi-music/music_server.py   (486 行, stdlib, systemd: xiaozhi-music.service)
端口 2233，AUTH_REQUIRED=1，AUTH_SECRET=your-esp32-secret-key-2024
```

**鉴权**（服务端 `check_auth`）：
```
X-MAC-Address / X-Chip-ID / X-Timestamp / X-Key
X-Key = sha256("{mac}:{chip}:{ts}:{secret}")[:32]     # ts 用开机 uptime 即可
```

**原有接口**：`/search`、`/play?id=`、`/stream_pcm`、`/lyric/<id>.lrc`

## 2. 服务端新增（本次，全部为新增端点，原接口未改）

| 端点 | 作用 |
|---|---|
| `/stream_ogg?song=&artist=` 或 `?id=` | MP3 → **OGG/Opus 24kHz 单声道 32kbps**（ffmpeg），缓存到 `/opt/xiaozhi-music/cache/<id>.ogg` |
| `/lyric_json/<id>` | LRC → `{lines:[{start_ms,text}]}`，设备直接当歌词时间轴用 |
| `/stock/quote?code=` | 腾讯行情（A股 sh600519 / 港股 hk00700 / 美股 usNVDA），返回现价/涨跌/PE/PB/市值等 |
| `/stock/search?q=` | 腾讯 smartbox 股票搜索 |
| `/search` 行为增强 | 返回前**后台预热**首个可播放候选的 OGG，点播时秒开 |

安装：`apt-get install ffmpeg`（6.1.1）。重启：`systemctl restart xiaozhi-music`。
备份：`music_server.py.bak-20261109`。

**实测（PC 侧 curl，带正确 X-Key）**
```
/search            200  {"total":2,"items":[...]}
/stream_ogg        200  988285 B, 首次 5.3s（转码）/ 命中缓存 2.1s，OggS 头正确
/lyric_json/186064 200  多行 {start_ms,text}
/stock/quote?code=usNVDA 200 {"name":"英伟达","price":178.67,"change_pct":-4.27,...}
```

## 3. 设备端新增（本仓库）

| 文件 | 作用 |
|---|---|
| `main/music/music_api_client.{h,cc}` | 带鉴权头的 GET；`Search/GetLyrics/StreamUrlById/StockQuote/StockSearch`；SHA256 走 **PSA**（mbedTLS 4 已无 `mbedtls/sha256.h`） |
| `main/music/music_online.cc` | 4 个音乐工具（**对齐旧固件语义**） |
| `main/market/stock_tools.{h,cc}` | `self.stock.quote` / `self.stock.search`（JSON → 中文口语化文本） |
| `main/notify/notify_player.*` | 新增 `SetHttpTimeout()`；音乐播放放宽到 25s（首次点播服务端可能在转码） |
| `main/Kconfig.projbuild` | `MUSIC_API_URL` / `MUSIC_API_SECRET`（也可用 NVS `"music"/"api_url"` 覆盖） |

**MCP 工具（现在共 20 个）**
```
self.music.search_song(song_name, artist_name, offset)   → 服务端原始 JSON
self.music.play_song(song_name, artist_name) / (url) / (keyword) / (index)
self.music.play_song_by_id(song_id)
self.music.set_display_mode(lyrics|spectrum|cover|hidden)   ← 兼容旧值
self.stock.quote(code)      self.stock.search(keyword)
```

**启动日志（实机确认）**
```
I (1237) MusicApi: music API: http://47.116.199.150:2233 (mac=ac:a7:04:e0:f8:10)
I (1237) MusicOnline: online music tools registered
I (1267) StockTools: stock tools registered
工具数量: 20
```

## 4. 已知问题（待定）

**网易云搜索相关性差**：未登录（无 `MUSIC_U` cookie）时，网易云返回的候选多为翻唱/冒名账号，例如搜"七里香 周杰伦"返回的是"布拉格广场""我爱你 - 周杰伦♚"，真正的《七里香》进不了前 50 条。

- 影响：能播，但可能播成翻唱版本
- 解决：① 在服务端配置网易云 `MUSIC_U` cookie（登录态，效果最好）② 换音源（QQ音乐/酷我）③ 放宽策略接受翻唱

**时长未知**：`/search` 不返回时长，进度条用"歌词最后一行 + 20s"估算。

---

## 5. 歌词页改版（按反馈：原布局不合适）

旧布局问题：20px 中文字体下每行只能放 ~10 字，`LONG_DOT` 把长句截断；封面占 92px 太占地方。

**新布局（240×320）**

| 区域 | 内容 |
|---|---|
| 顶部 | 小封面 56×56（左上）+ 标题（跑马灯）+ 歌手（省略号） |
| 分隔线 | y=70，1px |
| 歌词 | **五行**：上上句(40%透明) / 上一句(70%) / **当前句(青色，最多折两行)** / 下一句(70%) / 下下句(40%) |
| 底部 | 进度条 12px 圆角 + 时间 |

接口从 `SetLyric(prev, cur, next)` 改为 `SetLyric(prev2, prev1, cur, next1, next2)`。

## 6. 播放打断（本次新增）

### 关键词（唤醒词）打断
- 播放期间保持 WakeNet 开启；检测到唤醒词 → `StopNotification()` → 停流 + 收音乐页 → 进入聆听
- 日志：`Wake word during playback -> stop music and listen`
- **关键改进：播放时自动 ducking**（`CONFIG_MUSIC_DUCK_VOLUME_PERCENT`，默认 60%）
  原因：本板 ES8311/PDM 没有 AEC 参考通道，外放声音会盖住麦克风，WakeNet 很难触发；降到 60% 后唤醒率显著提升，停播自动恢复原音量

### 按键打断
| 按键 | 行为 |
|---|---|
| **BOOT(GPIO0)** | 音乐播放中：单击或长按 → **立即停止播放**（打断）；非播放时保持原功能（点击对话） |
| `MUSIC_STOP_BUTTON_GPIO`（可选，默认 NC） | 专用停止键：按下即停 |
| `MUSIC_NEXT_BUTTON_GPIO`（可选，默认 NC） | 单击=下一首；**双击或长按=停止播放** |
| `MUSIC_PREV_BUTTON_GPIO`（可选，默认 NC） | 单击=上一首 |

接线同前：按键一脚接 GPIO、一脚接 GND（内部上拉，按下为低）。
