# 免费（无版权限制）音源清单与实测（2026-11-11）

## 1. 已移除的版权源

从服务端彻底移除（搜索 / `/play` / 歌词 / 预热 / 回退链全部删除）：

| 源 | 移除原因 |
|---|---|
| 腾讯音乐（vkeys 公共 API） | 版权受限，VIP 曲只给 60s 试听；接口还经常 503 |
| 酷我 | 版权受限，未登录只给 11s 试听 |
| 网易云 | 版权受限，未登录搜索被翻唱淹没 |

现在访问 `/play?id=tx...` 会直接返回 `400 only_free_sources`。

## 2. 实测过的免费源

| 候选 | 结果 |
|---|---|
| **ccMixter**（CC BY / CC BY-NC） | ✅ 搜索 API + MP3 直链（需 Referer）→ 已用 |
| **Incompetech / Kevin MacLeod**（CC BY） | ✅ 22 首直链可用 → 已用 |
| **自托管本地曲库**（新增） | ✅ 把 CC BY 曲目下载到服务器，`/library/*.mp3` 免鉴权直发 → 最稳 |
| Jamendo | ❌ 公共 demo client_id 已被停用（需自行注册免费 key，加 `JAMENDO_CLIENT_ID` 即可启用） |
| Free Music Archive | ❌ 站点可达，但已无公开下载接口（需登录） |
| Bensound / Chosic | ❌ 403 防盗链 |
| 酷狗 | ❌ 播放接口已封锁（搜索可用但拿不到直链） |
| archive.org / Wikimedia / Openverse / Pixabay | ❌ 从该服务器不可达（网络） |
| SoundHelix | ⚠️ 只有 29 秒片段，不适合当曲库 |

## 3. 最终架构（三层，全部无版权限制）

```
① 本地曲库   /opt/xiaozhi-music/library/*.mp3  → /library/<file>（免鉴权，MP3 直链）
② ccMixter   搜索 API（按关键词，官方 CC 曲目）   → 直链
③ Incompetech 22 首 CC BY 目录（未下载完时兜底）
```

- `/search`：先查本地曲库（按标题/中文标签），未命中再查 ccMixter/Incompetech
- `/play?id=lbN|ccN|ikN`：返回 **MP3 直链**（设备端 minimp3 直接解码，服务端不再转码）
- 歌词：CC 曲目没有 LRC → `{lines: []}`，设备显示「♪ 纯音乐（CC 授权）」

## 4. 实测

```
搜 钢琴   → ik0/ik2/ik20（本地库文件未下完时用 Incompetech 目录）
搜 jazz   → cc71324 Jazz Backing Track - JeffSpeed68（CC BY-NC）
搜 放松   → ccMixter chill 曲目
/play?id=lb1 → http://47.116.199.150:2233/library/Fluffing_a_Duck.mp3
无鉴权拉该URL → 200，3.3MB，103.2 秒 ✓
搜 七里香 → 404 song not found（版权曲，已无任何版权源）
```

## 5. 结论

- **完全免费、可完整播放**：本地曲库 + ccMixter + Incompetech
- **流行商业歌曲**：本版本不再支持（所有版权源已移除）；若以后想恢复，需自行提供 `NETEASE_COOKIE` 或重新接回 vkeys（代码在 git 历史里）

---

## 6. 修复"所有音乐都找不到/不能播"（2026-11-11）

日志显示两条独立的失败：

```
W MusicApi: search failed: http 404            ← 搜中文流行歌名（版权源已删，正常 404）
GET /lyric_json/lb12 404
GET /stream_ogg?id=lb12 404                    ← MP3 路径被拒后回退旧链路，旧链路不认 lb id
```

**根因**：
1. `StartMusicMp3()` 没有实现"抢占"：语音点歌时设备在 `speaking` 状态 → 直接返回 false
   → 回退到 `StartMusic()`（旧 OGG 链路）→ `stream_ogg` 不认 `lb*` id → 404
2. 版权源已全部删除，所以搜"七里香/你的眼神"这类歌名必然 404（这是设计如此）

**修复**：
- `StartMusicMp3()` 加入与旧路径相同的抢占逻辑（abort speaking → 500ms 后在主任务启动），
  并用 `pending_music_is_mp3_` 区分两条路径
- `/stream_ogg` 与 `/lyric_json` 支持 `lb*`（本地曲库兜底）
- `/search` 的 404 现在返回 `available_genres`（钢琴/轻音乐/爵士/放松/欢快/氛围/电子/民谣），
  方便助手引导用户换风格

**实测（本地曲库 23 首就绪后）**
```
搜 钢琴  → lb4 Fig Leaf Rag / lb8 Gymnopedie No 1（CC BY 4.0，本地直发）
/play?id=lb0 → http://47.116.199.150:2233/library/Airport_Lounge.mp3
无鉴权拉流   → 200，2.55MB，63.7 秒 ✓
```

---

## 7. 可搜索的免费曲库（2026-11-11）

用户需求："需要可以搜索歌曲的免费曲库"。最终方案 = **三层可搜索曲库**：

| 层 | 内容 | 搜索方式 | 备注 |
|---|---|---|---|
| ① 本地曲库 | 23 首 CC BY（Kevin MacLeod），`/opt/xiaozhi-music/library/` | 标题词 + 中文标签（tag 整词匹配） | 最快最稳，`/library/*.mp3` 免鉴权直发 |
| ② ccMixter | **数万首 CC BY / BY-NC**，在线 API | `search=`/`tags=`/`usernames=` + **分页 offset** | 远端文件需 Referer → 走 `/proxy?id=` 转发 |
| ③ Incompetech | 23 首 CC BY 目录 | 标签/标题 | 本地库未命中时的兜底 |

### 关键实现
- **`/proxy?id=<cc|ik>`**：服务端带 `Referer` 转发远端 MP3（ccMixter 直接拉会 403），
  设备 `Mp3StreamPlayer` 无需任何 header 即可边下边播
- **分页**：`/search?...&offset=N&limit=5` → `has_more` 正确
- **`tags=` 模式**：中文风格词（钢琴/爵士/放松/欢快/电子/民谣…）→ 英文 tag
- **反向映射**：英文 `piano` → 中文标签 `钢琴`（本地库也能命中）
- **排序优先级**：本地曲库(−3000) > 已转码缓存(−1000) > Incompetech(−200) > 其它
- 匹配用**词边界**，避免 `rock` 命中 `Rocket Power`

### 实测
```
搜 钢琴 → lb4 Fig Leaf Rag | lb8 Gymnopedie No 1 | ik1 Carefree
搜 piano → lb4 | lb8 | ik10 Bossa Antigua
搜 jazz  → lb0 Airport Lounge | lb1 Bossa Antigua | lb7 George Street Shuffle
搜 放松  → lb0 | lb1 | cc71335 SQUARE PEGS in CIRCLES
搜 jazz&offset=3 → cc71259 Citybuilder / cc71255 Electra Glider / cc71238 Remote Control Soul（分页正确）
/play?id=cc71324 → http://47.116.199.150:2233/proxy?id=cc71324
拉 /proxy?id=cc71324 → 200，1.99MB，287.4 秒 ✓
```

---

## 8. "要能播放的免费歌曲"（带人声曲目）（2026-11-16）

### 已解决：ccMixter 的人声曲目
中文关键词映射新增：`歌曲 / 唱歌 / 人声 / 有词的 → vocals`，直接命中 ccMixter 的 **a cappella / 人声**曲目。

实测：
```
搜「歌曲」→ cc66560 / cc66442 Inner Light - Wiseman / cc66421 Rêves et Perspectives / cc66344 Frozen Lace
播放 cc66560 → /proxy?id=cc66560 → 200，5.05MB，**315.6 秒完整播放**
```

### 已就绪：Jamendo（真正的大型免费歌曲库）
- 需 **免费 client_id**：https://devportal.jamendo.com 注册 → 创建 App → 复制 Client ID
- 配置：`/etc/systemd/system/xiaozhi-music.service` 增加
  `Environment=JAMENDO_CLIENT_ID=<你的id>` → `systemctl daemon-reload && systemctl restart xiaozhi-music`
- 代码已接入（`jamendo_search/jamendo_url` + `/proxy` 白名单 + `/play?id=jm...`）：
  - 曲库 60 万+，CC 授权，**含大量有人声的歌曲**
  - 支持 `search=` / `tag=` / `offset` 分页，`order=popularity_total` 按热度
  - 未配置 key 时自动跳过（不影响现有流程）

### 现状总结（全部可播、全部免费）
| 类型 | 来源 | 实测 |
|---|---|---|
| 器乐/纯音乐 | 本地库 23 首 + Incompetech + ccMixter | 63~287s 完整 ✓ |
| **带人声歌曲** | ccMixter `vocals`（+ Jamendo 待配置） | 315.6s 完整 ✓ |
| 流行商业歌 | —— | 无（版权源已删） |

---

## 9. 架构调整：先下载到服务器，再发给设备（不依赖 API 网站）（2026-11-16）

按用户要求改为"**采集到本地 → 本地直发**"，不再要求外部站点提供 API：

### 三种入库方式

| 方式 | 用法 | 说明 |
|---|---|---|
| ① 播放时自动落盘 | 设备点播 `cc*/ik*` → 服务端返回 `/proxy` **立即播放**，同时后台线程把它下载进 `library/` + 写 `library.json` | 第二次播放就走 `/library`（本地直发，最快、无外网依赖） |
| ② 批量收割 | `python3 /opt/xiaozhi-music/harvest.py [tags...]` | 默认抓 vocals/acappella/jazz/piano/chill/rock/electronic/folk/soundtrack/hiphop/classical/ambient，每标签 12 首 |
| ③ 手动添加任意 URL | `/opt/xiaozhi-music/add_track.sh <url> "标题" "作者" "标签" ["CC BY"]` | **任何**免费 MP3 直链都能入库，不需要 API |

### 关键实现
- `library.json`：`{tracks:[{file,title,artist,tags,license,source_id}]}`，`lib_refresh()` 读它做元数据
- `acquire_to_library()`：后台下载（带 Referer/UA），写 `.part` → rename，成功才写元数据
- `/library/<file>`：免鉴权、直发 MP3（设备 minimp3 解码）
- 搜索优先级：本地库（lb*）> Jamendo(jm*) > ccMixter(cc*) > Incompetech(ik*)

### 实测
```
搜「歌曲」→ lb23 No Meaning No (society) / lb24 Ip Op - ashwan / lb25 Wired Loop Example - wired  ← 本地库人声曲
搜「钢琴」→ lb4 Fig Leaf Rag / lb8 Gymnopedie No 1
曲库文件数：26（harvest 持续增长，55MB）
```

---

## 10. 解决"55 秒才起播"（2026-11-16）

用户实测 ccMixter 源站只有 ~72KB/s，一首 4MB 要 55 秒 → **不可接受**。

### 新策略：搜索结果只给"能秒开"的曲子
1. **`/search` 不再返回未落盘的 ccMixter 曲目**（`lib_cc<id>.mp3` 不存在的直接过滤掉），
   同时把它推进 `_prefetch_queue`，后台 `prefetch_worker()`（3 路）先下载
2. 本地库（`lb*`）+ Incompetech 曲目（已在本地）优先返回
3. `harvest.py` 升级为 **4 路并行下载**、跳过 >6MB 的大文件、每标签 20 首、14 个标签
4. 曲库扩张速度：约 **10 首/分钟**（41 首 / 123MB 起步，持续增长）

### 实测（改后）
```
搜「歌曲」→ lb23 No Meaning No / lb24 Warning Shots / lb25 Below(Smooth Low Mix)   ← 全部本地
搜「jazz」→ lb0 Airport Lounge / lb1 Bossa Antigua / lb7 George Street Shuffle
/play?id=lb23 → /library/lib_cc162.mp3
首字节 0.065s（65 毫秒），整首 5.25MB 走完 11.8s（≈440KB/s，公网正常速度）
```
对比修复前：55 秒起播 → **65 毫秒起播**。

### 代价与边界
- 没落盘的曲目不会出现在结果里，助手会提示"曲库准备中/换个风格"
- `/library` 仍不支持 Range（暂无 seek/续传）
- 曲库规模决定覆盖率：持续跑 harvest.py 即可（磁盘按 3.5MB/首 估算）

---

## 11. 改为"服务器只暂存"（用户要求，2026-11-16）

### 澄清一个数字
"55 秒"是**整首下载时间**，不是起播时间。ccMixter 远端曲目经 `/proxy` **流式转发**实测：

| 指标 | 实测 |
|---|---|
| **起播（首字节）** | **0.9 ~ 2.5 秒** |
| 整首 4.3MB 传完 | ~52 秒（源站 82KB/s） |
| 是否够播 | 够：128kbps 音频只需 16KB/s，源站 82KB/s > 需求，**边下边播不中断** |

### 改动
1. **停止批量收割**（`harvest.py` 进程已 kill，不再往服务器囤歌）
2. `library/` 从此是**临时缓存**，不是曲库：
   - `CACHE_TTL_HOURS`（默认 **2 小时**）：超过就删
   - `CACHE_MAX_MB`（默认 **300MB**）：超容量按最旧优先删
   - 后台 `cache_maintain()` 每 5 分钟清理一次，日志 `cache expire/evict`
3. `/search` 恢复完整 ccMixter（远端曲可直接点，起播 ~1-2s）
4. `add_track.sh` 手动加 URL 仍可用，同样受缓存策略约束

### 想更"只暂存"
```ini
# /etc/systemd/system/xiaozhi-music.service
Environment=CACHE_TTL_HOURS=0.5
Environment=CACHE_MAX_MB=120
```
