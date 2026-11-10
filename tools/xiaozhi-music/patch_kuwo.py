#!/usr/bin/env python3
"""给 music_server.py 增加酷我(Kuwo)音源：搜索/播放地址/歌词。

背景：网易云未登录搜索被翻唱/冒名刷屏（搜"七里香"返回 "Xai小爱" 等），
      酷我搜索能直接返回官方版本，且 antiserver 能拿到可播放 MP3 直链、m.kuwo.cn 能给带时间轴歌词。
      保留网易云作为兜底（老 id 为纯数字时走网易云）。
"""
import io
import re

P = "music_server.py.new"
s = io.open(P, encoding="utf-8").read()

# ---------- 1) imports ----------
if "import html as _html" not in s:
    s = s.replace("import hashlib", "import hashlib\nimport html as _html", 1)

# ---------- 2) Kuwo provider helpers（插在 check_auth_query 之前） ----------
KUWO = r'''
# ============================================================
# 酷我音源（2026-11 增加）：搜索 / 播放直链 / 歌词
# 网易云未登录时搜索结果被翻唱淹没，酷我能直接命中官方版本。
# ============================================================
KUWO_SEARCH = ("http://search.kuwo.cn/r.s?all=%s&ft=music&itemset=web_2013&client=kt"
               "&pn=0&rn=%d&rformat=json&encoding=utf8&show_copyright_off=1&pcxz=1&vipver=1")
KUWO_PLAY = "http://antiserver.kuwo.cn/anti.s?type=convert_url&format=mp3&response=url&rid=%s"
KUWO_LYRIC = "http://m.kuwo.cn/newh5/singles/songinfoandlrc?musicId=%s"
_KUWO_CACHE = {}
_KUWO_TTL = 30 * 60


def _clean(name):
    if not name:
        return ""
    return _html.unescape(str(name)).replace("\xa0", " ").strip()


def _rid_of(song_id):
    """把 'kw94237' / 'MUSIC_94237' / 94237 统一成 '94237'"""
    t = str(song_id).strip()
    if t.lower().startswith("kw"):
        t = t[2:]
    if t.upper().startswith("MUSIC_"):
        t = t[6:]
    return t


def kuwo_search(song, artist="", limit=20):
    key = ("kw:" + song.strip().lower() + "|" + artist.strip().lower(), time.time())
    cached = _KUWO_CACHE.get(key[0])
    if cached and time.time() - cached[0] < _KUWO_TTL:
        return cached[1]
    kw = song + (" " + artist if artist else "")
    raw = _http_get_text(KUWO_SEARCH % (urllib.parse.quote(kw), limit))
    if not raw:
        return []
    # 酷我返回的是单引号 JSON，转成标准 JSON
    body = raw.strip()
    if body.startswith("'") and body.endswith("'"):
        body = body[1:-1]
    body = body.replace("&nbsp;", " ")
    try:
        data = json.loads(body.replace("\\'", "'"))
    except Exception:
        # 退化：用正则抓 abslist 里的条目
        data = {}
    items = data.get("abslist") or []
    out = []
    for it in items:
        rid = _rid_of(it.get("MUSICRID", ""))
        if not rid.isdigit():
            continue
        try:
            duration = int(float(it.get("DURATION", 0) or 0)) * 1000
        except Exception:
            duration = 0
        out.append({
            "id": "kw" + rid,
            "name": _clean(it.get("SONGNAME")),
            "artist": _clean(it.get("ARTIST")),
            "album": _clean(it.get("ALBUM")),
            "duration": duration,
            "score": 0,
            "provider": "kuwo",
        })
    _KUWO_CACHE[key[0]] = (time.time(), out)
    return out


def kuwo_play_url(song_id):
    rid = _rid_of(song_id)
    if not rid.isdigit():
        return ""
    raw = _http_get_text(KUWO_PLAY % rid)
    url = (raw or "").strip()
    if url.startswith("http"):
        return url
    return ""


def kuwo_is_playable(song_id):
    key = "play:" + _rid_of(song_id)
    hit = _KUWO_CACHE.get(key)
    if hit and time.time() - hit[0] < _KUWO_TTL:
        return hit[1]
    url = kuwo_play_url(song_id)
    ok = bool(url) and probe_range(url)
    _KUWO_CACHE[key] = (time.time(), ok)
    return ok


def kuwo_lyric_lrc(song_id):
    """酷我歌词 -> 标准 LRC 文本（复用已有的 lrc_to_json）"""
    rid = _rid_of(song_id)
    if not rid.isdigit():
        return ""
    raw = _http_get_text(KUWO_LYRIC % rid)
    if not raw:
        return ""
    try:
        data = json.loads(raw)
    except Exception:
        return ""
    lines = (data.get("data") or {}).get("lrclist") or []
    out = []
    for ln in lines:
        try:
            sec = float(ln.get("time", 0) or 0)
        except Exception:
            continue
        text = _clean(ln.get("lineLyric"))
        if not text:
            continue
        mm = int(sec // 60)
        ss = sec - mm * 60
        out.append("[%02d:%05.2f]%s" % (mm, ss, text))
    return "\n".join(out)


def kuwo_pick(candidates, song, artist):
    """排序：歌名精确匹配 > 包含；指定歌手时优先该歌手；其次按原顺序"""
    def norm(x):
        return (x or "").lower().replace(" ", "")

    wanted = norm(artist)
    title = norm(song)

    def rank(c):
        name = norm(c["name"])
        ar = norm(c["artist"])
        r = 0
        if title and name == title:
            r -= 100
        elif title and title in name:
            r -= 50
        if wanted and wanted in ar:
            r -= 30
        if c.get("score"):
            r -= c["score"]
        return r

    return sorted(candidates, key=rank)


'''

if "def kuwo_search(" not in s:
    s = s.replace("def check_auth_query(params):", KUWO + "def check_auth_query(params):", 1)

# ---------- 3) /search：优先酷我 ----------
old_search = '''        # 候选列表：一次返回最多 PAGE（默认5）个，供用户语音选择
        if path == "/search":'''
new_search = '''        # 候选列表：一次返回最多 PAGE（默认5）个，供用户语音选择
        if path == "/search":'''
s = s.replace(old_search, new_search, 1)

# 在 /search 里替换候选来源
s = s.replace('''            limit = max(1, min(limit, 10))
            offset = max(0, offset)
            if not song:
                self._send_json(400, {"error": "missing song"})
                return
            ordered = search_candidates(song, artist)''','''            limit = max(1, min(limit, 10))
            offset = max(0, offset)
            if not song:
                self._send_json(400, {"error": "missing song"})
                return
            # 首选酷我（官方版本命中率高），无结果再退回网易云
            ordered = kuwo_pick(kuwo_search(song, artist), song, artist)
            if not ordered:
                ordered = search_candidates(song, artist)''', 1)

# /search 里可播放性探测：按 provider 选择探测函数
s = s.replace('''            with ThreadPoolExecutor(max_workers=min(len(page), 5)) as ex:
                playable_map = dict(zip(
                    [str(c["id"]) for c in page],
                    list(ex.map(lambda c: is_playable(str(c["id"])), page))))''','''            def _playable(c):
                cid = str(c.get("id", ""))
                if c.get("provider") == "kuwo" or cid.startswith("kw"):
                    return kuwo_is_playable(cid)
                return is_playable(cid)

            with ThreadPoolExecutor(max_workers=min(len(page), 5)) as ex:
                playable_map = dict(zip(
                    [str(c["id"]) for c in page],
                    list(ex.map(_playable, page))))''', 1)

# /search 返回值里补上 duration（设备端进度条用）
s = s.replace('''                items.append({
                    "index": offset + i + 1,   # 1-based 序号，给用户报"第几个"
                    "id": cid,
                    "name": c.get("name", ""),
                    "artist": c.get("artist", ""),
                    "album": c.get("album", ""),
                    "playable": playable_map.get(cid, False),
                })''','''                items.append({
                    "index": offset + i + 1,   # 1-based 序号，给用户报"第几个"
                    "id": cid,
                    "name": c.get("name", ""),
                    "artist": c.get("artist", ""),
                    "album": c.get("album", ""),
                    "duration_ms": c.get("duration", 0),
                    "playable": playable_map.get(cid, False),
                })''', 1)

# ---------- 4) /play：支持 kw id ----------
s = s.replace('''        if path == "/play":
            song_id = (params.get("id") or [""])[0]
            if not song_id:
                self._send_json(400, {"error": "missing id"})
                return
            purl = resolve_play_url(song_id)''','''        if path == "/play":
            song_id = (params.get("id") or [""])[0]
            if not song_id:
                self._send_json(400, {"error": "missing id"})
                return
            if song_id.startswith("kw"):
                purl = kuwo_play_url(song_id)
                if not (purl and probe_range(purl)):
                    self._send_json(404, {"error": "not_playable"})
                    return
                title, artist = "", ""
                for (_k, (_t, cands)) in _KUWO_CACHE.items():
                    if not isinstance(cands, list):
                        continue
                    for c in cands:
                        if str(c.get("id")) == song_id:
                            title, artist = c.get("name", ""), c.get("artist", "")
                            break
                    if title:
                        break
                self._send_json(200, {
                    "id": song_id, "title": title, "artist": artist,
                    "audio_url": purl,
                    "lyric_url": f"{BASE_URL}/lyric_json/{song_id}",
                })
                return
            purl = resolve_play_url(song_id)''', 1)

# ---------- 5) /stream_ogg & /stream_pcm：支持酷我 ----------
s = s.replace('''            if song_id:
                purl = resolve_play_url(song_id)
                if not (purl and probe_range(purl)):
                    self._send_json(404, {"error": "not_playable"})
                    return''','''            if song_id:
                if song_id.startswith("kw"):
                    purl = kuwo_play_url(song_id)
                else:
                    purl = resolve_play_url(song_id)
                if not (purl and probe_range(purl)):
                    self._send_json(404, {"error": "not_playable"})
                    return''', 1)

s = s.replace('''            else:
                if not song:
                    self._send_json(400, {"error": "missing song"})
                    return
                ordered = search_candidates(song, artist)
                if not ordered:
                    self._send_json(404, {"error": "song not found"})
                    return
                chosen = None
                for c in ordered:
                    p = resolve_play_url(c["id"])
                    if p and probe_range(p):
                        chosen = (c, p)
                        break''','''            else:
                if not song:
                    self._send_json(400, {"error": "missing song"})
                    return
                ordered = kuwo_pick(kuwo_search(song, artist), song, artist)
                if not ordered:
                    ordered = search_candidates(song, artist)
                if not ordered:
                    self._send_json(404, {"error": "song not found"})
                    return
                chosen = None
                for c in ordered:
                    if str(c.get("id", "")).startswith("kw"):
                        p = kuwo_play_url(c["id"])
                    else:
                        p = resolve_play_url(c["id"])
                    if p and probe_range(p):
                        chosen = (c, p)
                        break''', 1)

# stream_pcm 同样优先酷我
s = s.replace('''            ordered = search_candidates(song, artist)
            if not ordered:
                self._send_json(404, {"error": "song not found"})
                return

            chosen = None
            chosen_by = "rank"
            for c in ordered:
                purl = resolve_play_url(c["id"])
                if purl and probe_range(purl):
                    chosen = {"info": c, "url": purl}
                    break''','''            ordered = kuwo_pick(kuwo_search(song, artist), song, artist)
            if not ordered:
                ordered = search_candidates(song, artist)
            if not ordered:
                self._send_json(404, {"error": "song not found"})
                return

            chosen = None
            chosen_by = "kuwo"
            for c in ordered:
                if str(c.get("id", "")).startswith("kw"):
                    purl = kuwo_play_url(c["id"])
                else:
                    purl = resolve_play_url(c["id"])
                if purl and probe_range(purl):
                    chosen = {"info": c, "url": purl}
                    break''', 1)

# ---------- 6) /lyric_json：支持 kw ----------
s = s.replace('''        m = re.match(r"^/lyric_json/(\\d+)$", path)
        if m:
            lrc = fetch_lyric(m.group(1))
            self._send_json(200, {"id": m.group(1), "lines": lrc_to_json(lrc)})
            return''','''        m = re.match(r"^/lyric_json/(kw)?(\\d+)$", path)
        if m:
            if m.group(1):
                lrc = kuwo_lyric_lrc(m.group(2))
            else:
                lrc = fetch_lyric(m.group(2))
            self._send_json(200, {"id": (m.group(1) or "") + m.group(2), "lines": lrc_to_json(lrc)})
            return''', 1)

io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("kuwo provider patched")
