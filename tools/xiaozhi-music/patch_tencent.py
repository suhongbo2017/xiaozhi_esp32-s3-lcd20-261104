#!/usr/bin/env python3
"""接入 vkeys 公共 API 的腾讯音乐源（免费、无需 cookie、搜索准确、可拿完整音频+歌词）。

- 搜索: https://api.vkeys.cn/v2/music/tencent?word=<song artist>&num=10
- 详情/播放: https://api.vkeys.cn/v2/music/tencent?id=<id>   -> data.url / quality / kbps
- 歌词: https://api.vkeys.cn/v2/music/tencent/lyric?id=<id>  -> data.lrc (LRC 文本)
id 统一加 "tx" 前缀（如 tx102065756），设备端把 id 当不透明字符串。
"""
import io

P = "music_server.py.new"
s = io.open(P, encoding="utf-8").read()

VKEYS = r'''
# ============================================================
# 腾讯音乐源（vkeys 公共 API，免费、免登录）
# 搜索命中官方原唱；免费曲目给 320kbps/SQ；VIP 曲目给 25kbps 试听但仍是完整时长。
# ============================================================
VKEYS_API = "https://api.vkeys.cn/v2/music/tencent"
_VKEYS_CACHE = {}
_VKEYS_TTL = 30 * 60


def _http_get_text_utf8(url, timeout=15):
    """vkeys 返回 UTF-8 JSON；不能走 GBK 优先的解码逻辑"""
    try:
        host = url.split("/")[2]
        path = "/" + "/".join(url.split("/")[3:])
        if url.startswith("https://"):
            conn = http.client.HTTPSConnection(host, timeout=timeout)
        else:
            conn = http.client.HTTPConnection(host, timeout=timeout)
        conn.request("GET", path, headers={"User-Agent": NETEASE_UA, "Accept": "application/json"})
        r = conn.getresponse()
        body = r.read()
        conn.close()
        return body.decode("utf-8", "replace") if r.status == 200 else ""
    except Exception:
        return ""


def _interval_to_ms(text):
    """'4分59秒' -> 299000"""
    try:
        m = re.match(r"(?:(\d+)\s*分)?\s*(\d+)\s*秒?", str(text or ""))
        if not m:
            return 0
        return (int(m.group(1) or 0) * 60 + int(m.group(2))) * 1000
    except Exception:
        return 0


def vkeys_search(song, artist="", limit=10):
    key = "tx:" + song.strip().lower() + "|" + artist.strip().lower()
    hit = _VKEYS_CACHE.get(key)
    if hit and time.time() - hit[0] < _VKEYS_TTL:
        return hit[1]
    kw = song + (" " + artist if artist else "")
    raw = _http_get_text_utf8(VKEYS_API + "?word=%s&num=%d" % (urllib.parse.quote(kw), limit))
    if not raw:
        return []
    try:
        data = json.loads(raw).get("data") or []
    except Exception:
        return []
    if not isinstance(data, list):
        data = [data]
    out = []
    for it in data:
        sid = it.get("id")
        if sid is None:
            continue
        out.append({
            "id": "tx%d" % int(sid),
            "name": _clean(it.get("song") or it.get("name") or it.get("title")),
            "artist": _clean(it.get("singer") or it.get("artist")),
            "album": _clean(it.get("album")),
            "duration": _interval_to_ms(it.get("interval") or it.get("duration")),
            "provider": "tencent",
            "score": 0,
        })
    if out:
        _VKEYS_CACHE[key] = (time.time(), out)
    log("tencent search %r -> %d item(s)" % (kw, len(out)))
    return out


def _tx_detail(song_id):
    tid = str(song_id)
    if tid.startswith("tx"):
        tid = tid[2:]
    key = "txid:" + tid
    hit = _VKEYS_CACHE.get(key)
    if hit and time.time() - hit[0] < 600:
        return hit[1]
    raw = _http_get_text_utf8(VKEYS_API + "?id=%s" % tid)
    if not raw:
        return None
    try:
        d = json.loads(raw)
        data = d.get("data")
        if isinstance(data, list):
            data = data[0] if data else None
        if isinstance(data, dict):
            _VKEYS_CACHE[key] = (time.time(), data)
            return data
    except Exception:
        pass
    return None


def vkeys_play_url(song_id):
    d = _tx_detail(song_id)
    if not d:
        return ""
    url = d.get("url") or ""
    if not url.startswith("http"):
        return ""
    info = _tx_detail(song_id) or {}
    log("tencent url id=%s quality=%s kbps=%s" % (song_id, info.get("quality"), info.get("kbps")))
    return url


def vkeys_is_playable(song_id):
    url = vkeys_play_url(song_id)
    return bool(url) and probe_range(url)


def vkeys_lyric_lrc(song_id):
    tid = str(song_id)
    if tid.startswith("tx"):
        tid = tid[2:]
    raw = _http_get_text_utf8(VKEYS_API + "/lyric?id=%s" % tid)
    if not raw:
        return ""
    try:
        d = json.loads(raw)
        data = d.get("data") or {}
        if isinstance(data, dict):
            return data.get("lrc") or data.get("lyric") or ""
    except Exception:
        pass
    return ""


def vkeys_pick(candidates, song, artist):
    def norm(x):
        return (x or "").lower().replace(" ", "")

    title, wanted = norm(song), norm(artist)

    def rank(c):
        name, ar = norm(c["name"]), norm(c["artist"])
        r = 0
        if title and name == title:
            r -= 100
        elif title and title in name:
            r -= 50
        if wanted and wanted in ar:
            r -= 30
        if "live" in name or "伴奏" in name:
            r += 20
        return r

    return sorted(candidates, key=rank)


'''

if "def vkeys_search(" not in s:
    s = s.replace("def check_auth_query(params):", VKEYS + "def check_auth_query(params):", 1)

# ---- /search：优先腾讯(vkeys) ----
s = s.replace('''            # 首选酷我（官方版本命中率高），无结果再退回网易云
            ordered = kuwo_pick(kuwo_search(song, artist), song, artist)''','''            # 首选腾讯(vkeys 公共 API)：搜索准确、免费曲目给高码率
            ordered = vkeys_pick(vkeys_search(song, artist, max(limit + 3, 6)), song, artist)
            # 其次酷我（官方版本命中率高），最后退回网易云
            if not ordered:
                ordered = kuwo_pick(kuwo_search(song, artist), song, artist)''', 1)

# 可播放性探测加入 tencent
s = s.replace('''            def _playable(c):
                cid = str(c.get("id", ""))
                if c.get("provider") == "kuwo" or cid.startswith("kw"):
                    return kuwo_is_playable(cid)
                return is_playable(cid)''','''            def _playable(c):
                cid = str(c.get("id", ""))
                if cid.startswith("tx"):
                    return vkeys_is_playable(cid)
                if c.get("provider") == "kuwo" or cid.startswith("kw"):
                    return kuwo_is_playable(cid)
                return is_playable(cid)''', 1)

# ---- /play：支持 tx ----
s = s.replace('''            if song_id.startswith("kw"):
                purl = kuwo_play_url(song_id)''','''            if song_id.startswith("tx"):
                purl = vkeys_play_url(song_id)
                if not (purl and probe_range(purl)):
                    self._send_json(404, {"error": "not_playable"})
                    return
                d = _tx_detail(song_id) or {}
                self._send_json(200, {
                    "id": song_id,
                    "title": _clean(d.get("song") or ""),
                    "artist": _clean(d.get("singer") or ""),
                    "audio_url": purl,
                    "lyric_url": f"{BASE_URL}/lyric_json/{song_id}",
                    "quality": d.get("quality"), "kbps": d.get("kbps"),
                })
                return
            if song_id.startswith("kw"):
                purl = kuwo_play_url(song_id)''', 1)

# ---- /stream_ogg：支持 tx + 时长/回退链 ----
s = s.replace('''            if song_id:
                if song_id.startswith("kw"):
                    purl = kuwo_play_url(song_id)
                else:
                    purl = resolve_play_url(song_id)''','''            if song_id:
                if song_id.startswith("tx"):
                    purl = vkeys_play_url(song_id)
                elif song_id.startswith("kw"):
                    purl = kuwo_play_url(song_id)
                else:
                    purl = resolve_play_url(song_id)''', 1)

s = s.replace('''            else:
                if not song:
                    self._send_json(400, {"error": "missing song"})
                    return
                ordered = kuwo_pick(kuwo_search(song, artist), song, artist)''','''            else:
                if not song:
                    self._send_json(400, {"error": "missing song"})
                    return
                ordered = vkeys_pick(vkeys_search(song, artist, 6), song, artist)
                if not ordered:
                    ordered = kuwo_pick(kuwo_search(song, artist), song, artist)''', 1)

s = s.replace('''                for c in ordered:
                    if str(c.get("id", "")).startswith("kw"):
                        p = kuwo_play_url(c["id"])
                    else:
                        p = resolve_play_url(c["id"])''','''                for c in ordered:
                    cid = str(c.get("id", ""))
                    if cid.startswith("tx"):
                        p = vkeys_play_url(cid)
                    elif cid.startswith("kw"):
                        p = kuwo_play_url(cid)
                    else:
                        p = resolve_play_url(cid)''', 1)

# 期望时长：从 vkeys 缓存里补
s = s.replace('''            expected = 0
            for (_k, (_t, cands)) in _KUWO_CACHE.items():''','''            expected = 0
            for (_k, (_t, cands)) in _VKEYS_CACHE.items():
                if not isinstance(cands, list):
                    continue
                for c in cands:
                    if str(c.get("id")) == str(song_id):
                        expected = int(c.get("duration") or 0)
                        break
                if expected:
                    break
            for (_k, (_t, cands)) in _KUWO_CACHE.items():''', 1)

# ---- /stream_pcm：支持 tx ----
s = s.replace('''            ordered = kuwo_pick(kuwo_search(song, artist), song, artist)
            if not ordered:
                ordered = search_candidates(song, artist)''','''            ordered = vkeys_pick(vkeys_search(song, artist, 6), song, artist)
            if not ordered:
                ordered = kuwo_pick(kuwo_search(song, artist), song, artist)
            if not ordered:
                ordered = search_candidates(song, artist)''', 1)

s = s.replace('''            for c in ordered:
                if str(c.get("id", "")).startswith("kw"):
                    purl = kuwo_play_url(c["id"])
                else:
                    purl = resolve_play_url(c["id"])''','''            for c in ordered:
                cid = str(c.get("id", ""))
                if cid.startswith("tx"):
                    purl = vkeys_play_url(cid)
                elif cid.startswith("kw"):
                    purl = kuwo_play_url(cid)
                else:
                    purl = resolve_play_url(cid)''', 1)

# ---- /lyric_json：支持 tx ----
s = s.replace('''        m = re.match(r"^/lyric_json/(kw)?(\\d+)$", path)
        if m:
            if m.group(1):
                lrc = kuwo_lyric_lrc(m.group(2))
            else:
                lrc = fetch_lyric(m.group(2))
            self._send_json(200, {"id": (m.group(1) or "") + m.group(2), "lines": lrc_to_json(lrc)})
            return''','''        m = re.match(r"^/lyric_json/(tx|kw)?(\\d+)$", path)
        if m:
            if m.group(1) == "tx":
                lrc = vkeys_lyric_lrc(m.group(2))
            elif m.group(1) == "kw":
                lrc = kuwo_lyric_lrc(m.group(2))
            else:
                lrc = fetch_lyric(m.group(2))
            self._send_json(200, {"id": (m.group(1) or "") + m.group(2), "lines": lrc_to_json(lrc)})
            return''', 1)

io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("vkeys/tencent provider patched:", "def vkeys_search(" in s)
