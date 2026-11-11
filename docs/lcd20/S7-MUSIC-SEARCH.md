# 在线音乐搜索排查与改进（2026-11-10）

## 1. 问题：搜索出来的不是原唱

实测（服务端 `/search`）：

| 查询 | 修复前（网易云） | 修复后（酷我） |
|---|---|---|
| 七里香 | `Xai小爱` / `DJ大大怪` / `音屿`（全是翻唱、冒名） | **`kw94237` 七里香 - 周杰伦** ✅ 299s |
| 晴天 | `梦里啥都有` / `舟子` / `空匪` | **`kw228908` 晴天 - 周杰伦** ✅ 269s |
| 晚风心里吹 | 阿梨粤 / 李克勤 / 王赫野（均不可播） | 阿梨粤 ✅（原唱，可播） |

原因：**网易云未登录（无 `MUSIC_U` cookie）时搜索接口被翻唱/冒名账号淹没**，官方版本连前 50 条都进不去。

## 2. 修复：接入酷我(Kuwo)音源做搜索

| 接口 | 用途 |
|---|---|
| `http://search.kuwo.cn/r.s?...` | 搜索（返回 官方版本 优先排序） |
| `http://antiserver.kuwo.cn/anti.s?type=convert_url...` | 播放直链（MP3） |
| `http://m.kuwo.cn/newh5/singles/songinfoandlrc?musicId=` | 带时间轴歌词 |

实现要点：
- id 用 `kw<rid>` 前缀区分（如 `kw94237`），设备端把 id 当不透明字符串处理，**固件无需改动**
- 服务端 `kuwo_search()` 用 `ast.literal_eval` 解析（酷我返回的是 Python dict 字面量，不是 JSON）
- 空结果不缓存（否则一次抖动会导致 30 分钟搜不到）
- 搜索排序：歌名精确匹配 > 包含 > 歌手匹配

## 3. 新发现的坑：酷我未登录只给 **11 秒试听**

```
MUSIC_94237 = 七里香(周杰伦)，期望 299s
convert_url / convert_url2 / br=320kmp3 / vipver=1  → 都是同一段 181521 字节 / 11.31s
```

因此新增**时长校验**（服务端）：
1. 转码后用 `ffprobe` 取实际时长，若 < 期望时长 × 60% → 判定为试听片段，删除不缓存
2. 酷我判定失败 → 自动回退用「歌名+歌手」去网易云找完整音源
3. 两者都拿不到 → 返回 `503 {"error":"no_full_stream"}`（宁可报错，也不偷偷播 11 秒）

## 4. 验证结果（curl 带签名，等价设备请求）

| 歌曲 | 结果 |
|---|---|
| 布拉格广场 | ✅ 200，OGG **293.9s** |
| Whatever Happens | ✅ 200，OGG **296.7s** |
| 七里香 / 晴天 | ⚠️ 503 `no_full_stream`（酷我仅 11s 试听；网易云搜索找不到官方版本） |

## 5. 剩余阻塞：需要网易云登录态

服务端已支持环境变量 `NETEASE_COOKIE`（值为 `MUSIC_U=...`）：

```bash
# /etc/systemd/system/xiaozhi-music.service
Environment=NETEASE_COOKIE=MUSIC_U=xxxxx
systemctl daemon-reload && systemctl restart xiaozhi-music
```

配置后：网易云搜索会返回官方版本、且大部分歌曲可完整播放，`七里香/晴天` 这类歌即可正常播放。

开启日志：`journalctl -u xiaozhi-music -f`，会打印
`kuwo search '...' -> N item(s)` / `transcode too short ... (trial clip?)` / `kuwo clip rejected, falling back to netease`。

---

## 6. 最终方案：改用免费源（腾讯音乐 via vkeys 公共 API）

用户要求"用免费的源"，不再依赖网易云 cookie。实测了多个免费接口：

| 源 | 搜索准确性 | 免费完整性 | 结论 |
|---|---|---|---|
| 网易云(未登录) | ❌ 官方版本进不了前 50 | ✅ 部分歌可完整 | 搜索不可用 |
| 酷我 | ✅ 官方第一 | ❌ 仅 11s 试听 | 只能当搜索源 |
| 酷狗 | ✅ 官方第一 | ❌ 播放接口已封锁 | 不可用 |
| 千千/百度(taihe) | – | – | 502 已停服 |
| **腾讯音乐(vkeys 公共 API)** | ✅ **官方第一** | ✅ **免费曲目 320kbps/SQ 完整；VIP 曲目 60s 试听** | **采用** |

### 接口
```
搜索   https://api.vkeys.cn/v2/music/tencent?word=<歌名 歌手>&num=10
详情   https://api.vkeys.cn/v2/music/tencent?id=<id>        -> data.url / quality / kbps
歌词   https://api.vkeys.cn/v2/music/tencent/lyric?id=<id>  -> data.lrc (LRC 文本)
```
id 加前缀 `tx`（如 `tx102065756`），设备端零改动。

### 实测结果

| 查询 | 结果 |
|---|---|
| 七里香 | `tx102065756` **七里香 - 周杰伦**（299s，VIP→试听60s） |
| 晴天 | `tx97773` **晴天 - 周杰伦**（269s，VIP→试听60s） |
| 夜曲 | `tx718477` **夜曲 - 周杰伦**（226s，VIP→试听60s） |
| 晴天（超盛大弦乐） | 免费曲目 → `playable=true`，320kbps+ 完整播放 |
| 歌词 | `http=200, 38 行`，时间轴正确 |

### 关键设计
1. **搜索 <1.5s**：只查 detail 判 `quality` 是否含"试听"，不再做昂贵的 range 探测
2. `playable` 语义：`false` = 只有试听片段（VIP）；`true` = 可完整播放
3. **播放兜底链**：腾讯 → (失败) 酷我 → 网易云；都拿不到才返回 503
4. VIP 歌不再直接报错，而是**播 60s 试听**并在响应头标记 `X-Trial-Clip: 1`、日志告警
5. 时长校验（ffprobe）保留：防止把 11s/60s 片段当完整歌曲缓存
6. 仍支持 `NETEASE_COOKIE` 环境变量：哪天想完整播 VIP 歌，配上即可（优先走网易云完整源）

---

## 7. 最终定稿：默认使用"无版权限制"的免费源

用户要求"无版权限制的免费源"，实测后选择：

| 源 | 授权 | 能力 | 状态 |
|---|---|---|---|
| **ccMixter** (`ccmixter.org/api/query`) | CC BY / CC BY-NC | 关键词搜索 + MP3 直链（需 `Referer`，否则 403） | ✅ 主用 |
| **Incompetech**（Kevin MacLeod） | CC BY 4.0 | 12 首精选曲目直链，作"风格类查询"兜底 | ✅ 兜底 |
| 腾讯音乐(vkeys) | 版权受限 | 搜索准，VIP 仅 60s | 仅在特定歌名时回退 |
| 酷我 / 网易云 | 版权受限 | 11s / 需登录 | 末位兜底 |

### 关键词映射（两个源元数据都是英文）
```
钢琴→piano  吉他→guitar  爵士→jazz  轻音乐/纯音乐→instrumental
放松/舒缓→chill  氛围→ambient  欢快→upbeat  电影→music_for_film  ...
```

### 行为规则（重要）
- **风格/情绪类查询**（钢琴、jazz、轻音乐、宽松、空查询…）→ 免费源（ccMixter → Incompetech 目录）
- **具体歌名**（如"七里香"）→ 免费源没有就**回退腾讯**（官方原唱，VIP 仅 60s 试听，`playable=false` 明确标记）
  → 避免出现"点七里香却播放了无关的钢琴曲"

### 实测
| 请求 | 结果 |
|---|---|
| 搜"钢琴" | `ik0 Monkeys Spinning Monkeys - Kevin MacLeod / CC BY 4.0 / playable=true` |
| 搜"jazz" | `cc71324 Jazz Backing Track - JeffSpeed68 / Attribution Noncommercial` |
| 搜"轻音乐" | ccMixter 命中 |
| 搜"七里香" | 回退腾讯：`tx102065756 七里香 - 周杰伦（playable=false，VIP 试听）` |
| 播放 cc71324 | 200，OGG **287.4s** 完整 |
| 播放 ik0 | 200，OGG **125.1s** 完整 |

### 环境变量
```
MUSIC_PROVIDER=free      # 默认：免费源优先
MUSIC_PROVIDER=tencent   # 强制腾讯源
NETEASE_COOKIE=MUSIC_U=.. # 可选：让网易云也能用（含完整 VIP）
```

### 已知取舍
- 这类 CC 源**没有 LRC 歌词** → 音乐页只显示标题/歌手/进度条
- ccMixter 部分曲目是 **CC BY-NC（非商业）**；Incompetech 是 **CC BY（署名即可）**。设备音乐页会显示作者名作为署名
- 首次播放需服务端转码（5~20s，取决于曲目长度），之后命中缓存秒开

---

## 8. 播放器又报错的原因与修复（2026-11-10）

日志：
```
MusicPlayer: online play: '布拉格广场 - 刘至佳' (id=tx273773223, playable=1)
MusicApi: lyrics id=tx273773223 -> 85 line(s)
E NotifyPlayer: Notification HTTP request returned status 404
MusicPlayer: playback finished (failed)
```

根因：**腾讯 vkey 播放地址有有效期**。搜索时缓存了 detail（含 url，TTL 600s），
过几分钟再点播时该 url 已过期 → 服务端探测失败返回 404 → 设备拉流 404 → 播放失败。

修复：`/stream_ogg` 的 tx 分支在探测失败时**强制刷新 detail 再试一次**（`force=True`），
日志会打印 `tx url stale for tx..., refreshing`。实测 `tx273773223` 现在 200、可播放（VIP 60s）。

## 9. 关于 freemp3music.org（用户提议的站点）——不可用作"无版权"源

技术探测结果：
```
GET https://freemp3music.org/                200 (Nuxt SPA，39KB)
GET /search?q=adele                         301 -> /search/?q=adele -> 404
GET /api/search?q=adele                     404
GET /api/v1/search?q=adele / /api/tracks    404
robots.txt                                  Disallow: /*?q=  /*?term=  /*?query=  (说明搜索是 SPA 动态请求)
sitemap.xml                                 只有首页/faq/terms 等，无曲目页
```
- 站点没有公开 API，搜索完全依赖浏览器执行 JS 后发起的内部请求（需无头浏览器抓包才能拿到）
- 更关键：它是 **"Free MP3 Download" 聚合站**，内容是有版权的商业音乐，
  **不属于"无版权限制"**；这类站点普遍不稳定（加验证码/被封/被墙），也有法律风险
- 结论：不建议作为"固定免费源"；维持 CC 授权源（ccMixter / Incompetech）作为默认
