#!/usr/bin/env python3
"""服务端加固：
1) 音频时长校验：酷我未登录只给 ~11s 试听片段，转码后必须校验时长，过短则丢弃并回退网易云
2) 支持 NETEASE_COOKIE（MUSIC_U）：有登录态时网易云搜索/播放明显更好
3) 转码前先探源码时长（ffprobe），避免把试听片段当完整歌曲缓存
"""
import io

P = "music_server.py.new"
s = io.open(P, encoding="utf-8").read()

# ---------- 1) 网易云 cookie 支持 ----------
s = s.replace('NETEASE_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"',
              'NETEASE_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"\n'
              '# 网易云登录态（可选）：设置后搜索/播放质量明显提升\n'
              'NETEASE_COOKIE = os.environ.get("NETEASE_COOKIE", "").strip()', 1)

s = s.replace('''        conn.request("GET", path, headers={"User-Agent": NETEASE_UA, "Referer": "https://music.163.com", **headers})''',
              '''        hdrs = {"User-Agent": NETEASE_UA, "Referer": "https://music.163.com", **headers}
        if NETEASE_COOKIE:
            hdrs["Cookie"] = NETEASE_COOKIE if "=" in NETEASE_COOKIE else ("MUSIC_U=" + NETEASE_COOKIE)
        conn.request("GET", path, headers=hdrs)''', 1)

# ---------- 2) 时长探测 + 校验 ----------
helpers = '''

def media_duration_ms(path_or_url, is_url=False):
    """用 ffprobe 取时长（毫秒）；失败返回 0"""
    try:
        cmd = ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0"]
        if is_url:
            cmd = ["ffprobe", "-v", "error", "-user_agent", NETEASE_UA, "-headers",
                   "Referer: https://music.163.com\\r\\n", "-show_entries", "format=duration",
                   "-of", "csv=p=0", path_or_url]
        else:
            cmd.append(path_or_url)
        r = subprocess.run(cmd, capture_output=True, timeout=40)
        txt = r.stdout.decode("utf-8", "replace").strip().splitlines()
        return int(float(txt[0]) * 1000) if txt else 0
    except Exception:
        return 0


def log(msg):
    print("[music] " + msg, flush=True)

'''
if "def media_duration_ms" not in s:
    s = s.replace("\n_LRC_TIME = re.compile", helpers + "\n_LRC_TIME = re.compile", 1)

# ---------- 3) build_ogg 增加期望时长校验 ----------
s = s.replace("def build_ogg(song_id, audio_url):", "def build_ogg(song_id, audio_url, expected_ms=0):", 1)
s = s.replace('''    out = _ogg_path(song_id)
    if os.path.exists(out) and os.path.getsize(out) > 1024:
        return out
    with _OGG_LOCK:''','''    out = _ogg_path(song_id)
    if os.path.exists(out) and os.path.getsize(out) > 1024:
        # 已缓存：若明显短于期望时长，视为旧的试听片段，删除重转
        if expected_ms > 0:
            got = media_duration_ms(out)
            if got and got < expected_ms * 0.6:
                log("cache too short (%dms < %dms), re-transcoding %s" % (got, expected_ms, song_id))
                try:
                    os.remove(out)
                except Exception:
                    pass
            else:
                return out
        else:
            return out
    with _OGG_LOCK:''', 1)

s = s.replace('''        os.replace(tmp, out)
        return out''','''        # 校验时长：防止把试听片段当完整歌曲
        got = media_duration_ms(tmp)
        if expected_ms > 0 and got and got < expected_ms * 0.6:
            log("transcode too short for %s: %dms < %dms (trial clip?)" % (song_id, got, expected_ms))
            try:
                os.remove(tmp)
            except Exception:
                pass
            return None
        os.replace(tmp, out)
        log("transcoded %s -> %.1fs (%d bytes)" % (song_id, got / 1000.0, os.path.getsize(out)))
        return out''', 1)

# ---------- 4) /stream_ogg：酷我失败（试听片段）时回退网易云 ----------
s = s.replace('''            ogg = build_ogg(song_id, purl)
            if not ogg:
                self._send_json(500, {"error": "transcode_failed"})
                return''','''            expected = 0
            for (_k, (_t, cands)) in _KUWO_CACHE.items():
                if not isinstance(cands, list):
                    continue
                for c in cands:
                    if str(c.get("id")) == str(song_id):
                        expected = int(c.get("duration") or 0)
                        break
                if expected:
                    break

            ogg = build_ogg(song_id, purl, expected)
            if not ogg and song_id.startswith("kw"):
                # 酷我未登录只给 ~11s 试听 → 用同名歌去网易云找完整音源
                log("kuwo clip rejected, falling back to netease for %r" % (title or song,))
                for c in search_candidates(title or song, artist_name or artist, True):
                    p2 = resolve_play_url(c["id"])
                    if not (p2 and probe_range(p2)):
                        continue
                    ogg = build_ogg(str(c["id"]), p2, expected)
                    if ogg:
                        song_id = str(c["id"])
                        purl = p2
                        if not title:
                            title = c.get("name", "")
                        break
            if not ogg:
                self._send_json(503, {"error": "no_full_stream",
                                      "hint": "provider only returned a trial clip"})
                return''', 1)

io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("server hardening patched; has duration check:", "media_duration_ms" in s)
