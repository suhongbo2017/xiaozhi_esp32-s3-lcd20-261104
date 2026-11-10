#!/usr/bin/env python3
"""Patch /opt/xiaozhi-music/music_server.py with additive endpoints.

New endpoints (existing ones untouched):
  GET /stream_ogg?song=<name>&artist=<name>   -> OGG/Opus binary (24kHz mono)
  GET /stream_ogg?id=<song_id>                -> same, resolved from search cache
  GET /lyric_json/<song_id>                   -> {"id":..,"lines":[{start_ms,text}]}
  GET /stock/quote?code=<code>                -> Tencent quote JSON
  GET /stock/search?q=<keyword>               -> Tencent smartbox results
"""
import io
import sys

SRC = "music_server.py"
DST = "music_server.py.new"

src = io.open(SRC, encoding="utf-8").read()

HELPERS = r'''
# ============================================================
# 扩展能力（2026-11 增加）：OGG/Opus 转码、歌词 JSON、股票行情
# 说明：仅新增端点，原有 /search /play /stream_pcm /lyric 行为不变
# ============================================================
import subprocess
import threading

CACHE_DIR = os.environ.get(
    "CACHE_DIR",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "cache"),
)
os.makedirs(CACHE_DIR, exist_ok=True)
_OGG_LOCK = threading.Lock()
_OGG_BUILDING = {}


def _ogg_path(song_id):
    return os.path.join(CACHE_DIR, "%s.ogg" % song_id)


def build_ogg(song_id, audio_url):
    """MP3 -> OGG/Opus（24kHz 单声道）。返回文件路径，失败返回 None。"""
    out = _ogg_path(song_id)
    if os.path.exists(out) and os.path.getsize(out) > 1024:
        return out
    with _OGG_LOCK:
        if os.path.exists(out) and os.path.getsize(out) > 1024:
            return out
        if _OGG_BUILDING.get(song_id):
            return None
        _OGG_BUILDING[song_id] = True
    tmp = out + ".part"
    try:
        cmd = [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-headers",
            "Referer: https://music.163.com\r\nUser-Agent: %s\r\n" % NETEASE_UA,
            "-i", audio_url,
            "-vn", "-ac", "1", "-ar", "24000",
            "-c:a", "libopus", "-b:a", "32k", "-application", "audio",
            "-f", "ogg", tmp,
        ]
        r = subprocess.run(cmd, capture_output=True, timeout=180)
        if r.returncode != 0 or not os.path.exists(tmp) or os.path.getsize(tmp) < 1024:
            sys.stderr.write(
                "[music] ffmpeg failed: %s\n" % r.stderr.decode("utf-8", "replace")[:400]
            )
            if os.path.exists(tmp):
                os.remove(tmp)
            return None
        os.replace(tmp, out)
        return out
    except Exception as e:
        sys.stderr.write("[music] transcode error: %s\n" % e)
        if os.path.exists(tmp):
            try:
                os.remove(tmp)
            except Exception:
                pass
        return None
    finally:
        _OGG_BUILDING[song_id] = False


_LRC_TIME = re.compile(r"\[(\d+):(\d+)(?:[.:](\d+))?\]")


def lrc_to_json(lrc_text):
    """LRC 文本 -> [{start_ms, text}]"""
    out = []
    for line in (lrc_text or "").splitlines():
        stamps = list(_LRC_TIME.finditer(line))
        if not stamps:
            continue
        text = _LRC_TIME.sub("", line).strip()
        if not text:
            continue
        for m in stamps:
            mm, ss, frac = m.group(1), m.group(2), m.group(3) or "0"
            ms = int(mm) * 60000 + int(ss) * 1000
            if frac:
                ms += int(frac) * (10 if len(frac) == 2 else 1)
            out.append({"start_ms": ms, "text": text})
    out.sort(key=lambda x: x["start_ms"])
    return out


def _http_get_text(url, timeout=8):
    """极简 HTTP/HTTPS GET（股票行情用，GBK 解码）"""
    try:
        host = url.split("/")[2]
        path = "/" + "/".join(url.split("/")[3:])
        if url.startswith("https://"):
            conn = http.client.HTTPSConnection(host, timeout=timeout)
        else:
            conn = http.client.HTTPConnection(host, timeout=timeout)
        conn.request("GET", path, headers={"User-Agent": NETEASE_UA, "Referer": "https://finance.qq.com"})
        r = conn.getresponse()
        body = r.read()
        conn.close()
        if r.status != 200:
            return ""
        for enc in ("gbk", "utf-8"):
            try:
                return body.decode(enc)
            except Exception:
                continue
        return body.decode("utf-8", "replace")
    except Exception:
        return ""


def normalize_stock_code(code):
    """sh600519 / 600519.SS / 600519 / 0700.HK / AAPL -> 腾讯行情代码"""
    c = (code or "").strip().upper().replace(" ", "")
    if not c:
        return ""
    if c.startswith("US"):
        return "us" + c[2:]
    if c.startswith(("SH", "SZ", "HK")):
        return c.lower()
    if c.endswith(".SS") or c.endswith(".SH"):
        return "sh" + c[:-3]
    if c.endswith(".SZ"):
        return "sz" + c[:-3]
    if c.endswith(".HK"):
        return "hk" + c[:-3].zfill(5)
    if c.isdigit():
        if len(c) == 6:
            return ("sh" if c[0] in "5689" else "sz") + c
        if len(c) <= 5:
            return "hk" + c.zfill(5)
    return "us" + c


def fetch_stock_quote(code):
    tcode = normalize_stock_code(code)
    if not tcode:
        return None
    text = _http_get_text("https://qt.gtimg.cn/q=%s" % tcode)
    if not text or "=" not in text:
        return None
    payload = text.split("=", 1)[1].strip().strip(";").strip('"')
    f = payload.split("~")
    if len(f) < 45:
        return None

    def num(idx):
        try:
            return float(f[idx])
        except Exception:
            return None

    def txt(idx):
        try:
            return f[idx]
        except Exception:
            return ""

    return {
        "code": tcode,
        "name": txt(1),
        "price": num(3),
        "prev_close": num(4),
        "open": num(5),
        "high": num(33),
        "low": num(34),
        "change": num(31),
        "change_pct": num(32),
        "volume_lots": num(36),
        "amount_wan": num(37),
        "turnover_pct": num(38),
        "pe": num(39),
        "market_cap_yi": num(45) if len(f) > 45 else None,
        "pb": num(46) if len(f) > 46 else None,
        "time": txt(30),
    }


def fetch_stock_search(keyword):
    text = _http_get_text(
        "https://smartbox.gtimg.cn/s3/?q=%s&t=all" % urllib.parse.quote(keyword or "")
    )
    if not text:
        return []
    body = text
    try:
        body = text.split("=", 1)[1].rstrip(";").strip()
        body = body.encode("utf-8", "replace").decode("unicode_escape")
    except Exception:
        pass
    out = []
    for item in body.split("^"):
        parts = item.split("~")
        if len(parts) < 5:
            continue
        out.append({
            "market": parts[0],
            "code": parts[1],
            "name": parts[2],
            "type": parts[4] if len(parts) > 4 else "",
        })
        if len(out) >= 10:
            break
    return out


'''

BRANCHES = r'''        # ---- 新增：直接给设备送 OGG/Opus（设备端只解码 Opus）----
        if path == "/stream_ogg":
            song_id = (params.get("id") or [""])[0]
            song = (params.get("song") or [""])[0]
            artist = (params.get("artist") or [""])[0]
            title = ""
            artist_name = ""
            purl = None
            if song_id:
                purl = resolve_play_url(song_id)
                if not (purl and probe_range(purl)):
                    self._send_json(404, {"error": "not_playable"})
                    return
                for (_k, (_ts, cands)) in _SEARCH_CACHE.items():
                    for c in cands:
                        if str(c.get("id")) == song_id:
                            title = c.get("name", "")
                            artist_name = c.get("artist", "")
                            break
                    if title:
                        break
            else:
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
                        break
                if not chosen:
                    self._send_json(404, {"error": "no playable stream"})
                    return
                c, purl = chosen
                song_id = str(c["id"])
                title = c.get("name", "")
                artist_name = c.get("artist", "")
            ogg = build_ogg(song_id, purl)
            if not ogg:
                self._send_json(500, {"error": "transcode_failed"})
                return
            try:
                with open(ogg, "rb") as fh:
                    data = fh.read()
            except Exception:
                self._send_json(500, {"error": "cache_read_failed"})
                return
            print("[music] stream_ogg id=%s title=%r bytes=%d" % (song_id, title, len(data)), flush=True)
            self.send_response(200)
            self.send_header("Content-Type", "audio/ogg")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("X-Song-Title", urllib.parse.quote(title or ""))
            self.send_header("X-Song-Artist", urllib.parse.quote(artist_name or ""))
            self.end_headers()
            self.wfile.write(data)
            return

        # ---- 新增：歌词 JSON ----
        m = re.match(r"^/lyric_json/(\d+)$", path)
        if m:
            lrc = fetch_lyric(m.group(1))
            self._send_json(200, {"id": m.group(1), "lines": lrc_to_json(lrc)})
            return

        # ---- 新增：股票行情 / 搜索 ----
        if path == "/stock/quote":
            code = (params.get("code") or params.get("ticker") or [""])[0]
            info = fetch_stock_quote(code)
            if not info:
                self._send_json(404, {"error": "quote_not_found", "code": code})
                return
            print("[stock] quote %s -> %s %s" % (code, info.get("name"), info.get("price")), flush=True)
            self._send_json(200, info)
            return

        if path == "/stock/search":
            kw = (params.get("q") or params.get("keyword") or [""])[0]
            if not kw:
                self._send_json(400, {"error": "missing q"})
                return
            self._send_json(200, {"keyword": kw, "items": fetch_stock_search(kw)})
            return

        if path == "/stream_pcm":'''

assert "def check_auth(headers):" in src
src = src.replace("def check_auth(headers):", HELPERS + "def check_auth(headers):", 1)

anchor = '        if path == "/stream_pcm":'
assert anchor in src
src = src.replace(anchor, BRANCHES, 1)

io.open(DST, "w", encoding="utf-8", newline="\n").write(src)
print("patched ->", DST, "lines:", src.count("\n"))
