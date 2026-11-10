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
