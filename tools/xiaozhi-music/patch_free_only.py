#!/usr/bin/env python3
"""去掉所有版权受限音源（腾讯/酷我/网易云），只保留无版权限制的免费源：
   - ccMixter（CC BY / CC BY-NC）—— 搜索 + 直链
   - Incompetech / Kevin MacLeod（CC BY）—— 内置曲库
设备端新架构直接解 MP3，所以 /play 只返回免费源的 MP3 直链。
"""
import io

P = "music_server.py.new"
s = io.open(P, encoding="utf-8").read()

# ---------- /search：只走免费源 ----------
old = '''            # 首选无版权限制的免费源（ccMixter / Incompetech）
            # song 为空时 = "随便放首歌"，走内置 CC 曲库（随机）
            ordered = free_pick(free_candidates(song, artist, max(limit + 3, 6)), song, artist)
            # 其次腾讯(vkeys 公共 API)
            if not ordered or MUSIC_PROVIDER == "tencent":
                ordered = vkeys_pick(vkeys_search(song, artist, max(limit + 3, 6)), song, artist)'''
new = '''            # 只使用无版权限制的免费源（ccMixter / Incompetech）
            # song 为空时 = "随便放首歌"，走内置 CC 曲库（随机）
            ordered = free_pick(free_candidates(song, artist, max(limit + 3, 6)), song, artist)'''
assert old in s
s = s.replace(old, new, 1)

# /search 里去掉后续的 tencent/kuwo/netease 回退
old = '''            if not ordered:
                ordered = kuwo_pick(kuwo_search(song, artist), song, artist)
            if not ordered:
                ordered = search_candidates(song, artist)
            if not ordered:
                self._send_json(404, {"error": "song not found"})
                return
            page = ordered[offset:offset + limit]'''
new = '''            if not ordered:
                self._send_json(404, {"error": "song not found",
                                      "hint": "only free/CC catalog is available"})
                return
            page = ordered[offset:offset + limit]'''
assert old in s
s = s.replace(old, new, 1)

# /search 的可播放探测：只处理 cc/ik
old = '''            def _playable(c):
                cid = str(c.get("id", ""))
                if cid.startswith(("cc", "ik")):
                    return True   # CC 授权，直接可完整播放（实际拉流时还会校验）
                if cid.startswith("tx"):'''
new = '''            def _playable(c):
                cid = str(c.get("id", ""))
                if cid.startswith(("cc", "ik")):
                    return True   # CC 授权，直接可完整播放
                return True
                if cid.startswith("tx"):'''
s = s.replace(old, new, 1)

# /search 的 trial 计算不再需要（tx 专属）
s = s.replace('''                trial = False
                if cid.startswith("tx"):
                    try:
                        trial = vkeys_trial_only(cid)
                    except Exception:
                        trial = False''','''                trial = False''', 1)

# ---------- /play：只接受 cc/ik ----------
start = s.index('        if path == "/play":')
end = s.index('        m = re.match(r"^/lyric_json/(tx|kw)?(\\d+)$", path)')
play_new = '''        if path == "/play":
            song_id = (params.get("id") or [""])[0]
            if not song_id:
                self._send_json(400, {"error": "missing id"})
                return
            if not song_id.startswith(("cc", "ik")):
                self._send_json(400, {"error": "only_free_sources",
                                      "hint": "this build only serves CC-licensed music"})
                return
            purl = free_url(song_id)
            if not purl:
                self._send_json(404, {"error": "not_found"})
                return
            meta = free_meta(song_id)
            self._send_json(200, {
                "id": song_id,
                "title": meta.get("name", ""),
                "artist": meta.get("artist", ""),
                "album": meta.get("album", ""),
                "duration_ms": meta.get("duration", 0),
                "audio_url": purl,
                "license": meta.get("album", ""),
            })
            return

'''
s = s[:start] + play_new + s[end:]

# ---------- /lyric_json：免费源没有歌词 ----------
s = s.replace('''        m = re.match(r"^/lyric_json/(tx|kw)?(\\d+)$", path)
        if m:
            if m.group(1) == "tx":
                lrc = vkeys_lyric_lrc(m.group(2))
            elif m.group(1) == "kw":
                lrc = kuwo_lyric_lrc(m.group(2))
            else:
                lrc = fetch_lyric(m.group(2))
            self._send_json(200, {"id": (m.group(1) or "") + m.group(2), "lines": lrc_to_json(lrc)})
            return''','''        m = re.match(r"^/lyric_json/(cc|ik)?(\\d*)$", path)
        if m:
            # 免费源（CC 曲目）没有 LRC 歌词
            self._send_json(200, {"id": (m.group(1) or "") + (m.group(2) or ""), "lines": []})
            return''', 1)

# ---------- /stream_ogg 与 /stream_pcm：只保留免费源 ----------
start = s.index('        if path == "/stream_ogg":')
end = s.index('        m = re.match(r"^/lyric_json/(cc|ik)?', start)
stream_new = '''        if path == "/stream_ogg":
            # 兼容旧设备：只支持免费源（cc/ik）转 OGG；新固件直接播 MP3，不走这里
            song_id = (params.get("id") or [""])[0]
            song = (params.get("song") or [""])[0]
            artist = (params.get("artist") or [""])[0]
            title = ""
            artist_name = ""
            if not song_id:
                ordered = free_pick(free_candidates(song, artist, 6), song, artist)
                if not ordered:
                    self._send_json(404, {"error": "song not found"})
                    return
                song_id = str(ordered[0]["id"])
                title, artist_name = ordered[0].get("name", ""), ordered[0].get("artist", "")
            purl = free_url(song_id)
            if not purl:
                self._send_json(404, {"error": "not_found"})
                return
            meta = free_meta(song_id)
            title = title or meta.get("name", "")
            artist_name = artist_name or meta.get("artist", "")
            waited = 0
            while _OGG_BUILDING.get(str(song_id)) and waited < 90:
                time.sleep(1.0)
                waited += 1
            ogg = build_ogg(song_id, purl, 0)
            if not ogg:
                self._send_json(503, {"error": "transcode_failed"})
                return
            try:
                with open(ogg, "rb") as fh:
                    data = fh.read()
            except Exception:
                self._send_json(500, {"error": "cache_read_failed"})
                return
            self.send_response(200)
            self.send_header("Content-Type", "audio/ogg")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("X-Song-Title", urllib.parse.quote(title or ""))
            self.send_header("X-Song-Artist", urllib.parse.quote(artist_name or ""))
            self.end_headers()
            self.wfile.write(data)
            return

        if path == "/stream_pcm":
            song = (params.get("song") or [""])[0]
            artist = (params.get("artist") or [""])[0]
            ordered = free_pick(free_candidates(song, artist, 6), song, artist)
            if not ordered:
                self._send_json(404, {"error": "song not found"})
                return
            c = ordered[0]
            purl = free_url(c["id"])
            if not purl:
                self._send_json(404, {"error": "not_found"})
                return
            self._send_json(200, {
                "artist": c.get("artist", ""), "title": c.get("name", ""),
                "audio_url": purl, "lyric_url": "",
            })
            return

'''
s = s[:start] + stream_new + s[end:]

# ---------- 预热只处理免费源 ----------
s = s.replace('''        sid = str(song_id)
        if sid.startswith(("cc", "ik")):
            u = free_url(sid)
            if u:
                build_ogg(sid, u, 0)
                log("prewarm free done %s" % sid)
            return
        purl = resolve_play_url(song_id)''','''        sid = str(song_id)
        u = free_url(sid)
        if not u:
            return
        build_ogg(sid, u, 0)
        log("prewarm free done %s" % sid)
        return
        purl = resolve_play_url(song_id)''', 1)

io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("copyrighted providers removed; free-only dispatch")
