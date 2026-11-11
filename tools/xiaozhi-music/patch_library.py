#!/usr/bin/env python3
"""新增"本地 CC 曲库"源（自托管，最稳定）：
   - /opt/xiaozhi-music/library/*.mp3（CC BY / CC BY-NC，随仓库一起下载）
   - /library/<file> 免鉴权提供 MP3（设备 MP3 播放器不加 header）
   - /search 会包含曲库条目（id=lb<N>），/play?id=lbN 返回直链
"""
import io

P = "music_server.py.new"
s = io.open(P, encoding="utf-8").read()

LIB = r'''
# ============================================================
# 本地 CC 曲库（自托管，最稳）：/opt/xiaozhi-music/library/*.mp3
# 与 Incompetech 的 CC BY 曲目同源，但由本机直接提供，不依赖外部站点
# ============================================================
LIB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "library")
os.makedirs(LIB_DIR, exist_ok=True)
_LIB_ITEMS = []
_LIB_SCANNED_AT = 0.0

# 文件名 -> 中文标签（用于"钢琴曲/放松/搞笑"这类匹配）
_IK_TAGS = {
    "Monkeys_Spinning_Monkeys": "欢快 搞笑 滑稽 背景 纯音乐",
    "Carefree": "轻快 欢快 尤克里里 纯音乐",
    "Sneaky_Snitch": "滑稽 悬疑 喜剧 搞笑",
    "Wallpaper": "轻快 复古 纯音乐",
    "Fluffing_a_Duck": "俏皮 欢快 搞笑",
    "Investigations": "悬疑 探案 紧张",
    "Local_Forecast": "轻松 商务 纯音乐",
    "Life_of_Riley": "欢快 民谣 纯音乐",
    "Fig_Leaf_Rag": "复古 拉格泰姆 钢琴",
    "Happy_Alley": "开心 明亮 纯音乐",
    "Bossa_Antigua": "波萨诺瓦 慵懒 放松 爵士",
    "Airport_Lounge": "沙发音乐 放松 慵懒 爵士",
    "George_Street_Shuffle": "爵士 摇摆 轻快",
    "Deliberate_Thought": "安静 思考 氛围 纯音乐",
    "Fretless": "舒缓 吉他 纯音乐",
    "Gymnopedie_No_1": "古典 钢琴 安静 舒缓",
    "Hall_of_the_Mountain_King": "古典 紧张 戏剧",
    "Quirky_Dog": "俏皮 搞笑 欢快",
    "Rocket_Power": "动感 电子 欢快",
    "Sardana": "民族 欢快 舞曲",
    "Shiny_Tech": "科技 电子 明亮",
    "Spazzmatica_Polka": "幽默 快节奏 波尔卡",
    "Wagon_Wheel": "民谣 乡村 轻松",
}


def lib_refresh(force=False):
    """扫描本地曲库（60s 内只扫一次）"""
    global _LIB_ITEMS, _LIB_SCANNED_AT
    now = time.time()
    if not force and (now - _LIB_SCANNED_AT) < 60 and _LIB_ITEMS:
        return _LIB_ITEMS
    items = []
    try:
        names = sorted(f for f in os.listdir(LIB_DIR) if f.lower().endswith(".mp3"))
    except Exception:
        names = []
    for i, fn in enumerate(names):
        base = os.path.splitext(fn)[0]
        title = base.replace("_", " ")
        tags = _IK_TAGS.get(base, "")
        items.append({
            "id": "lb%d" % i,
            "name": title,
            "artist": "Kevin MacLeod",
            "album": "CC BY 4.0",
            "duration": 0,
            "provider": "library",
            "score": 0,
            "tags": tags,
            "_file": fn,
        })
    _LIB_ITEMS = items
    _LIB_SCANNED_AT = now
    if items:
        log("library: %d track(s)" % len(items))
    return items


def lib_search(song, artist="", limit=10):
    items = lib_refresh()
    if not items:
        return []
    q = (song or "").strip()
    if not q:
        import random as _random
        out = list(items)
        _random.shuffle(out)
        return out[:limit]
    ql = q.lower()
    qtags = [t.strip() for t in _cc_translate(q).replace(",", " ").split() if t.strip()]
    matched = []
    for it in items:
        hay = (it["name"] + " " + it.get("tags", "")).lower()
        if ql in hay or any(t in hay for t in qtags):
            matched.append(it)
    return matched[:limit]


def lib_url(song_id):
    try:
        idx = int(str(song_id)[2:])
    except Exception:
        return ""
    items = lib_refresh()
    if 0 <= idx < len(items):
        return "/library/" + urllib.parse.quote(items[idx]["_file"])
    return ""


def lib_meta(song_id):
    try:
        idx = int(str(song_id)[2:])
    except Exception:
        return {}
    items = lib_refresh()
    return items[idx] if 0 <= idx < len(items) else {}


'''

if "def lib_search(" not in s:
    s = s.replace("def _cc_translate(kw):", LIB + "def _cc_translate(kw):", 1)

# ---------- free_candidates 里加入本地曲库 ----------
s = s.replace('''def free_candidates(song, artist="", limit=6):
    """CC 候选：ccMixter 命中 + Incompetech 目录（目录兜底仅用于风格类查询）"""
    items = list(free_search(song, artist, limit))''','''def free_candidates(song, artist="", limit=6):
    """CC 候选：本地曲库 + ccMixter + Incompetech 目录"""
    items = list(lib_search(song, artist, limit))
    if items:
        return items[:max(limit, 6)]
    items = list(free_search(song, artist, limit))''', 1)

# ---------- /play：支持 lb ----------
s = s.replace('''            if not song_id.startswith(("cc", "ik")):
                self._send_json(400, {"error": "only_free_sources",
                                      "hint": "this build only serves CC-licensed music"})
                return''','''            if song_id.startswith("lb"):
                rel = lib_url(song_id)
                if not rel:
                    self._send_json(404, {"error": "not_found"})
                    return
                meta = lib_meta(song_id)
                host = self.headers.get("Host") or ("47.116.199.150:%d" % PORT)
                self._send_json(200, {
                    "id": song_id,
                    "title": meta.get("name", ""),
                    "artist": meta.get("artist", ""),
                    "album": meta.get("album", ""),
                    "audio_url": "http://%s%s" % (host, rel),
                    "license": meta.get("album", ""),
                })
                return
            if not song_id.startswith(("cc", "ik")):
                self._send_json(400, {"error": "only_free_sources",
                                      "hint": "this build only serves CC-licensed music"})
                return''', 1)

# ---------- /library/ 免鉴权 + 文件服务 ----------
s = s.replace('''        if AUTH_REQUIRED and not (check_auth(self.headers) or check_auth_query(params)):''','''        # 本地 CC 曲库：音频文件免鉴权（设备 MP3 播放器不带 header）
        if path.startswith("/library/"):
            fname = urllib.parse.unquote(path[len("/library/"):])
            if "/" in fname or "\\\\" in fname or not fname.lower().endswith(".mp3"):
                self._send_json(400, {"error": "bad name"})
                return
            full = os.path.join(LIB_DIR, fname)
            if not os.path.isfile(full):
                self._send_json(404, {"error": "not_found"})
                return
            try:
                size = os.path.getsize(full)
                self.send_response(200)
                self.send_header("Content-Type", "audio/mpeg")
                self.send_header("Content-Length", str(size))
                self.end_headers()
                with open(full, "rb") as fh:
                    while True:
                        chunk = fh.read(64 * 1024)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
            except Exception as e:
                sys.stderr.write("[music] library send failed: %r\\n" % (e,))
            return

        if AUTH_REQUIRED and not (check_auth(self.headers) or check_auth_query(params)):''', 1)

io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("local library source added")
