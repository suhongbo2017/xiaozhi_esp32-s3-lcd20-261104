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
