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
