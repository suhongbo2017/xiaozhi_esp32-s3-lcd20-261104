#!/usr/bin/env python3
"""增加"无版权限制"的免费音源（默认优先）：
  1) ccMixter  —— CC-BY / CC-BY-NC，搜索 API + 直链（需要 Referer）
  2) Incompetech(Kevin MacLeod) —— CC-BY，内置精选曲目直链（无需搜索服务）
中文关键词会映射成英文标签（钢琴→piano 等），因为这两个源的元数据是英文。

id 前缀：cc<upload_id>（ccMixter）、ik<index>（Incompetech）
"""
import io

P = "music_server.py.new"
s = io.open(P, encoding="utf-8").read()

FREE = r'''
# ============================================================
# 无版权限制的免费音源（2026-11 增加，默认优先）
#   ccMixter:   CC-BY / CC-BY-NC，搜索 API，直链需 Referer: https://ccmixter.org/
#   Incompetech: CC-BY (Kevin MacLeod)，内置精选曲目直链
# 这些源没有版权限制（CC 授权，署名即可），可完整播放、无试听限制。
# ============================================================
import math

CC_API = "https://ccmixter.org/api/query?f=json&limit=%d&search=%s"
_CC_CACHE = {}
_CC_TTL = 30 * 60

# 中文关键词 -> 英文标签（两个源的元数据都是英文）
CC_ZH_MAP = [
    ("钢琴", "piano"), ("吉他", "guitar"), ("小提琴", "violin"), ("萨克斯", "saxophone"),
    ("爵士", "jazz"), ("摇滚", "rock"), ("电子", "electronic"), ("嘻哈", "hiphop"),
    ("说唱", "rap"), ("鼓", "drums"), ("贝斯", "bass"),
    ("轻音乐", "instrumental"), ("纯音乐", "instrumental"), ("器乐", "instrumental"),
    ("放松", "chill"), ("舒缓", "chill"), ("安静", "ambient"), ("氛围", "ambient"),
    ("欢快", "upbeat"), ("开心", "happy"), ("悲伤", "sad"), ("浪漫", "romantic"),
    ("电影", "music_for_film"), ("背景", "background"), ("播客", "podcast"),
    ("流行", "pop"), ("民谣", "folk"), ("古典", "classical"),
]

# Incompetech 精选（CC-BY 4.0，作者 Kevin MacLeod），可直接播放
IK_CATALOG = [
    ("Monkeys Spinning Monkeys", "欢快 搞笑 背景"),
    ("Carefree", "轻快 尤克里里"),
    ("Sneaky Snitch", "滑稽 悬疑 喜剧"),
    ("Wallpaper", "轻快 复古"),
    ("Fluffing a Duck", "俏皮 欢快"),
    ("Investigations", "悬疑 探案"),
    ("Local Forecast", "轻松 商务"),
    ("Life of Riley", "欢快 民谣"),
    ("Fig Leaf Rag", "复古 拉格泰姆"),
    ("Happy Alley", "开心 明亮"),
    ("Bossa Antigua", "波萨诺瓦 慵懒"),
    ("Airport Lounge", "沙发音乐 放松"),
]


def _cc_translate(kw):
    """中文关键词映射成英文标签；没有命中就原样返回"""
    for zh, en in CC_ZH_MAP:
        if zh in kw:
            return en
    return kw


def _parse_ps(text):
    """'5:36' -> 336000 ms"""
    try:
        parts = [int(x) for x in str(text or "").split(":")]
        if len(parts) == 2:
            return (parts[0] * 60 + parts[1]) * 1000
        if len(parts) == 3:
            return (parts[0] * 3600 + parts[1] * 60 + parts[2]) * 1000
    except Exception:
        pass
    return 0


def free_search(song, artist="", limit=10):
    """ccMixter 搜索；无结果时用 Incompetech 精选目录兜底"""
    kw = _cc_translate((song or "").strip())
    key = "cc:" + kw.lower() + "|" + artist.strip().lower()
    hit = _CC_CACHE.get(key)
    if hit and time.time() - hit[0] < _CC_TTL:
        return hit[1]
    url = CC_API % (max(limit, 6), urllib.parse.quote(kw))
    raw = _http_get_text_utf8(url)
    out = []
    if raw:
        try:
            arr = json.loads(raw)
        except Exception:
            arr = []
        for it in arr if isinstance(arr, list) else []:
            files = it.get("files") or []
            mp3 = None
            for f in files:
                if str(f.get("file_name", "")).lower().endswith(".mp3"):
                    mp3 = f
                    break
            rel = (it.get("upload_extra") or {}).get("relative_dir") or ""
            if mp3 is None or not rel:
                continue
            lic = it.get("license_name") or ""
            out.append({
                "id": "cc%d" % int(it.get("upload_id")),
                "name": _clean(it.get("upload_name")),
                "artist": _clean(it.get("user_name")),
                "album": ("%s / %s" % (lic, _clean(it.get("user_name")))).strip(" /"),
                "duration": _parse_ps(((mp3.get("file_format_info") or {}).get("ps"))),
                "provider": "ccmixter",
                "score": 0,
                "_url": "https://ccmixter.org/%s/%s" % (rel, mp3["file_name"]),
            })
    if out:
        _CC_CACHE[key] = (time.time(), out)
        log("ccmixter search %r -> %d item(s)" % (kw, len(out)))
        return out

    # 兜底：Incompetech 精选（按标题/标签做简单匹配，匹配不到就返回全部）
    q = (song or "").strip().lower()
    items = []
    for i, (title, tags) in enumerate(IK_CATALOG):
        if q and (q in title.lower() or any(t in tags for t in _cc_translate(q).split())):
            pass
        items.append({
            "id": "ik%d" % i,
            "name": title,
            "artist": "Kevin MacLeod",
            "album": "CC BY 4.0",
            "duration": 0,
            "provider": "incompetech",
            "score": 0,
            "_url": "https://incompetech.com/music/royalty-free/mp3-royaltyfree/%s.mp3"
                    % urllib.parse.quote(title),
        })
    log("ccmixter empty for %r -> incompetech catalog %d" % (kw, len(items)))
    return items[:limit]


def free_url(song_id):
    sid = str(song_id)
    if sid.startswith("cc"):
        for (_k, (_t, items)) in _CC_CACHE.items():
            for it in items:
                if str(it.get("id")) == sid:
                    return it.get("_url") or ""
        return ""
    if sid.startswith("ik"):
        try:
            idx = int(sid[2:])
            title = IK_CATALOG[idx][0]
        except Exception:
            return ""
        return ("https://incompetech.com/music/royalty-free/mp3-royaltyfree/%s.mp3"
                % urllib.parse.quote(title))
    return ""


def free_is_playable(song_id):
    url = free_url(song_id)
    return bool(url) and probe_range(url)


def free_meta(song_id):
    sid = str(song_id)
    if sid.startswith("cc"):
        for (_k, (_t, items)) in _CC_CACHE.items():
            for it in items:
                if str(it.get("id")) == sid:
                    return it
    if sid.startswith("ik"):
        try:
            idx = int(sid[2:])
            return {"name": IK_CATALOG[idx][0], "artist": "Kevin MacLeod", "duration": 0}
        except Exception:
            pass
    return {}


def free_pick(items, song, artist):
    def norm(x):
        return (x or "").lower().replace(" ", "")

    title = norm(song)

    def rank(c):
        name = norm(c["name"])
        r = 0
        if title and name == title:
            r -= 100
        elif title and title in name:
            r -= 50
        return r

    return sorted(items, key=rank)


'''

if "def free_search(" not in s:
    s = s.replace("def check_auth_query(params):", FREE + "def check_auth_query(params):", 1)

# ---------- build_ogg：按域名选择 Referer ----------
s = s.replace('''        cmd = [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-headers",
            "Referer: https://music.163.com\\r\\nUser-Agent: %s\\r\\n" % NETEASE_UA,''','''        referer = "https://music.163.com"
        if "ccmixter" in audio_url:
            referer = "https://ccmixter.org/"
        elif "incompetech" in audio_url:
            referer = "https://incompetech.com/"
        elif "qqmusic" in audio_url or "stream.qq" in audio_url:
            referer = "https://y.qq.com/"
        elif "kuwo" in audio_url:
            referer = "https://www.kuwo.cn/"
        cmd = [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-headers",
            "Referer: %s\\r\\nUser-Agent: %s\\r\\n" % (referer, NETEASE_UA),''', 1)

# ---------- /search：免费源优先 ----------
s = s.replace('''            # 首选腾讯(vkeys 公共 API)：搜索准确、免费曲目给高码率
            ordered = vkeys_pick(vkeys_search(song, artist, max(limit + 3, 6)), song, artist)''','''            # 首选无版权限制的免费源（ccMixter / Incompetech）
            ordered = free_pick(free_search(song, artist, max(limit + 3, 6)), song, artist)
            # 其次腾讯(vkeys 公共 API)
            if not ordered or MUSIC_PROVIDER == "tencent":
                ordered = vkeys_pick(vkeys_search(song, artist, max(limit + 3, 6)), song, artist)''', 1)

s = s.replace('''            def _playable(c):
                cid = str(c.get("id", ""))
                if cid.startswith("tx"):''','''            def _playable(c):
                cid = str(c.get("id", ""))
                if cid.startswith(("cc", "ik")):
                    return True   # CC 授权，直接可完整播放（实际拉流时还会校验）
                if cid.startswith("tx"):''', 1)

# ---------- /play：支持 cc/ik ----------
s = s.replace('''            if song_id.startswith("tx"):
                purl = vkeys_play_url(song_id)''','''            if song_id.startswith(("cc", "ik")):
                purl = free_url(song_id)
                if not (purl and probe_range(purl)):
                    self._send_json(404, {"error": "not_playable"})
                    return
                meta = free_meta(song_id)
                self._send_json(200, {
                    "id": song_id,
                    "title": meta.get("name", ""),
                    "artist": meta.get("artist", ""),
                    "audio_url": purl,
                    "lyric_url": f"{BASE_URL}/lyric_json/{song_id}",
                    "license": meta.get("album", ""),
                })
                return
            if song_id.startswith("tx"):
                purl = vkeys_play_url(song_id)''', 1)

# ---------- /stream_ogg：支持 cc/ik ----------
s = s.replace('''            if song_id:
                if song_id.startswith("tx"):
                    purl = vkeys_play_url(song_id)''','''            if song_id:
                if song_id.startswith(("cc", "ik")):
                    purl = free_url(song_id)
                elif song_id.startswith("tx"):
                    purl = vkeys_play_url(song_id)''', 1)

s = s.replace('''                ordered = vkeys_pick(vkeys_search(song, artist, 6), song, artist)
                if not ordered:
                    ordered = kuwo_pick(kuwo_search(song, artist), song, artist)''','''                ordered = free_pick(free_search(song, artist, 6), song, artist)
                if not ordered or MUSIC_PROVIDER == "tencent":
                    ordered = vkeys_pick(vkeys_search(song, artist, 6), song, artist)
                if not ordered:
                    ordered = kuwo_pick(kuwo_search(song, artist), song, artist)''', 1)

s = s.replace('''                for c in ordered:
                    cid = str(c.get("id", ""))
                    if cid.startswith("tx"):
                        if vkeys_trial_only(cid):''','''                for c in ordered:
                    cid = str(c.get("id", ""))
                    if cid.startswith(("cc", "ik")):
                        p = free_url(cid)
                    elif cid.startswith("tx"):
                        if vkeys_trial_only(cid):''', 1)

# ---------- /stream_pcm：支持 cc/ik ----------
s = s.replace('''            ordered = vkeys_pick(vkeys_search(song, artist, 6), song, artist)
            if not ordered:
                ordered = kuwo_pick(kuwo_search(song, artist), song, artist)
            if not ordered:
                ordered = search_candidates(song, artist)''','''            ordered = free_pick(free_search(song, artist, 6), song, artist)
            if not ordered or MUSIC_PROVIDER == "tencent":
                ordered = vkeys_pick(vkeys_search(song, artist, 6), song, artist)
            if not ordered:
                ordered = kuwo_pick(kuwo_search(song, artist), song, artist)
            if not ordered:
                ordered = search_candidates(song, artist)''', 1)

s = s.replace('''            for c in ordered:
                cid = str(c.get("id", ""))
                if cid.startswith("tx"):
                    if vkeys_trial_only(cid):
                        continue
                    purl = vkeys_play_url(cid)''','''            for c in ordered:
                cid = str(c.get("id", ""))
                if cid.startswith(("cc", "ik")):
                    purl = free_url(cid)
                elif cid.startswith("tx"):
                    if vkeys_trial_only(cid):
                        continue
                    purl = vkeys_play_url(cid)''', 1)

# ---------- 配置项 ----------
s = s.replace('NETEASE_COOKIE = os.environ.get("NETEASE_COOKIE", "").strip()',
              'NETEASE_COOKIE = os.environ.get("NETEASE_COOKIE", "").strip()\n'
              '# 音源策略：free(默认,无版权限制) | tencent | auto\n'
              'MUSIC_PROVIDER = os.environ.get("MUSIC_PROVIDER", "free").strip().lower()', 1)

io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("free providers patched:", "def free_search(" in s)
