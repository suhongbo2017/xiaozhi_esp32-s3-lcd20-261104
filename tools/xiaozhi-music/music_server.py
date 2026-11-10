#!/usr/bin/env python3
"""Self-hosted music service for the xiaozhi ESP32 music feature.

Implements the upstream protocol (what the ESP32 firmware expects):

  GET /stream_pcm?song=<name>&artist=<name>
      -> 200 JSON {artist, title, audio_url, lyric_url}
         audio_url:  resolved http(s) MP3 URL (Range-capable, no redirects)
         lyric_url:  http(s) URL returning LRC text

  GET /lyric/<song_key>.lrc
      -> LRC text (content-type: text/plain; charset=utf-8)

Song resolution: queries NetEase Cloud Music public API, follows the
play URL redirect to a final CDN URL (the ESP32 HTTP client does NOT
follow 30x redirects), and picks the first song whose stream answers
a Range request with 206/200.

Auth: requests from the firmware carry
  X-MAC-Address, X-Chip-ID, X-Timestamp, X-Key
The timestamp freshness and X-Key SHA-256 are verified when
AUTH_REQUIRED=1 (default). Set AUTH_REQUIRED=0 to disable.

Zero third-party dependencies: stdlib only. Run with python3.
"""
import hashlib
import html as _html
import ast as _ast
import hmac
import http.client
import json
import os
import re
import sys
import time
import urllib.parse
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "0.0.0.0"
PORT = int(os.environ.get("PORT", "2233"))
BASE_URL = os.environ.get("BASE_URL", f"http://{PORT}:{PORT}")  # overridden below
AUTH_REQUIRED = os.environ.get("AUTH_REQUIRED", "1") == "1"
AUTH_SECRET = os.environ.get("AUTH_SECRET", "your-esp32-secret-key-2024")
NETEASE_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
# 网易云登录态（可选）：设置后搜索/播放质量明显提升
NETEASE_COOKIE = os.environ.get("NETEASE_COOKIE", "").strip()
_TIMEOUT = 10


def public_ip_url():
    try:
        conn = http.client.HTTPSConnection("api.ipify.org", timeout=5)
        conn.request("GET", "/", headers={"User-Agent": NETEASE_UA})
        r = conn.getresponse()
        ip = r.read().decode().strip()
        conn.close()
        return f"http://{ip}:{PORT}"
    except Exception:
        return None


def https_get_json(host, path, headers):
    """Standard-library HTTPS GET returning parsed JSON (or None).
    Uses http.client directly: urllib requests get fingerprinted by
    NetEase and return a captcha hash instead of results."""
    conn = http.client.HTTPSConnection(host, timeout=_TIMEOUT)
    try:
        hdrs = {"User-Agent": NETEASE_UA, "Referer": "https://music.163.com", **headers}
        if NETEASE_COOKIE:
            hdrs["Cookie"] = NETEASE_COOKIE if "=" in NETEASE_COOKIE else ("MUSIC_U=" + NETEASE_COOKIE)
        conn.request("GET", path, headers=hdrs)
        r = conn.getresponse()
        body = r.read()
        code = r.status
        ctype = r.getheader("Content-Type") or ""
        conn.close()
        if code != 200:
            return None
        return body.decode("utf-8", "replace")
    except Exception:
        try:
            conn.close()
        except Exception:
            pass
        return None


def netease_search(song, artist, limit=20):
    s = song
    if artist:
        s += " " + artist
    songs = []
    # 优先 cloudsearch 接口，失败退回 legacy web search
    for path_fmt in (
        "/api/cloudsearch/pc?csrf_token=&type=1&offset=0&total=true&limit=%d&s=%s",
        "/api/search/get/web?csrf_token=&type=1&offset=0&total=true&limit=%d&s=%s",
    ):
        path = path_fmt % (limit, urllib.parse.quote(s))
        raw = https_get_json("music.163.com", path, {})
        if not isinstance(raw, str):
            continue
        try:
            data = json.loads(raw)
        except Exception:
            continue
        songs = (data.get("result") or {}).get("songs") or []
        if songs:
            break
        songs = []
    out = []
    for it in songs:
        # cloudsearch 用 ar，legacy 用 artists
        raw_artists = it.get("artists")
        if raw_artists is None:
            raw_artists = it.get("ar") or []
        artist_list = [a.get("name", "") or "" for a in raw_artists]
        artists = ", ".join(artist_list)
        out.append({
            "id": it.get("id"),
            "name": it.get("name"),
            "artist": artists,
            "primary_artist": artist_list[0] if artist_list else "",
            "album": (it.get("album") or {}).get("name", ""),
            "duration": it.get("duration"),
            "score": it.get("score") or 0,  # 网易云相关度评分，用于排序
        })
    return out


# 关键词过滤：排除明显的翻唱/现场/伴奏版本，保留卡拉OK原生版
_BAD_TITLE_WORDS = ("live", "现场", "伴奏", "karaoke", "remaster", "cover",
                    "翻唱", "重制", "舞蹈版", "演唱会", "mv", "(官方")


def pick_song(candidates, song, artist):
    """从网易云搜索结果中排序出最可能正确的候选（播放性探测前的排序）。

    规则（按优先级）：
    1. 歌手硬约束：指定了歌手时，候选必须包含该歌手；全部不匹配则放宽
       （LLM 可能把歌手识别错，不能因为歌手参数错误而选不到歌）。
    2. 剔除明显劣质标题版本（live/伴奏/翻唱...）。
    3. 歌名精确匹配 > 歌名包含 > 其余；同级按网易云相关度 score 降序，再按原顺序。
    """
    def is_bad_title(c):
        name = (c.get("name") or "").lower()
        return any(w in name for w in _BAD_TITLE_WORDS)

    wanted = []
    if artist:
        wanted = [a.strip().lower() for a in artist.replace("/", " ").replace("、", " ").replace("·", " ").split() if a.strip()]

    def artist_hit(c):
        if not wanted:
            return True
        primary = (c.get("primary_artist") or "").lower()
        joined = (c.get("artist") or "").lower()
        return any(w in primary or primary.endswith(w) or w in joined for w in wanted)

    pool = [c for c in candidates if artist_hit(c) and not is_bad_title(c)]
    if not pool:
        pool = [c for c in candidates if not is_bad_title(c)]
    if not pool:
        pool = candidates

    song_l = song.strip().lower()

    def rank(c):
        name = (c.get("name") or "").strip().lower()
        if name == song_l:
            return 3
        if song_l and song_l in name:
            return 2
        return 1

    def key(c, idx):
        return (-rank(c), -(c.get("score") or 0), idx)

    ordered = sorted(range(len(pool)), key=lambda i: key(pool[i], i))
    return [pool[i] for i in ordered]


# 搜索结果缓存：(song, artist) -> (time, ordered_candidates)
# 保障翻页时列表稳定、同一首歌重复搜索不重复打网易云。
_SEARCH_CACHE = {}
_SEARCH_TTL = 30 * 60


def search_candidates(song, artist="", force=False):
    key = (song.strip().lower(), artist.strip().lower())
    now = time.time()
    hit = _SEARCH_CACHE.get(key)
    if hit and not force and now - hit[0] < _SEARCH_TTL:
        return hit[1]
    candidates = netease_search(song, artist)
    if not candidates and artist:
        candidates = netease_search(song, "")
    ordered = pick_song(candidates, song, artist)
    _SEARCH_CACHE[key] = (now, ordered)
    print(f"[music] search fresh: song={song!r} artist={artist!r} hits={len(ordered)}", flush=True)
    return ordered


def resolve_play_url(song_id):
    """Return final MP3 URL or None. The ESP32 cannot follow redirects,
    so we resolve the 302 Location ourselves (http.client does not follow)."""
    path = "/song/media/outer/url?id=%s.mp3" % song_id
    conn = http.client.HTTPSConnection("music.163.com", timeout=_TIMEOUT)
    try:
        conn.request("GET", path, headers={"User-Agent": NETEASE_UA, "Referer": "https://music.163.com"})
        r = conn.getresponse()
        r.read()
        code = r.status
        loc = r.getheader("Location")
        conn.close()
        if code in (200, 302) and loc and loc.startswith("http"):
            return loc
        if code in (200, 302):
            return None
        return None
    except Exception:
        try:
            conn.close()
        except Exception:
            pass
        return None


def probe_range(url):
    """Confirm the URL really serves MP3 audio (not an HTML 404 page).

    The ESP32 sends Range: bytes=0- and needs a playable MP3 stream.
    NetEase redirects VIP/nonexistent songs to music.163.com/404 which
    answers 200 with an HTML page — so we additionally check the
    Content-Type and the first bytes (MP3 magic: ID3 tag or 0xFF frame sync).
    """
    u = urllib.parse.urlparse(url)
    conn_cls = http.client.HTTPSConnection if u.scheme == "https" else http.client.HTTPConnection
    conn = conn_cls(u.hostname, u.port or (443 if u.scheme == "https" else 80), timeout=_TIMEOUT)
    try:
        conn.request("GET", u.path + (("?" + u.query) if u.query else ""),
                     headers={"User-Agent": NETEASE_UA, "Range": "bytes=0-2"})
        r = conn.getresponse()
        data = r.read()
        code = r.status
        ctype = (r.getheader("Content-Type") or "").lower()
        conn.close()
        if code not in (200, 206):
            return False
        # 只接受音频类型
        if "audio" in ctype or "octet-stream" in ctype or "mp3" in ctype:
            # 再校验前几个字节是 MP3 魔数（ID3 或 0xFF 帧同步）
            if len(data) >= 3:
                if data[:3] == b"ID3":
                    return True
                if data[0] == 0xFF and (data[1] & 0xE0) == 0xE0:
                    return True
            return False
        return False
    except Exception:
        try:
            conn.close()
        except Exception:
            pass
        return False


def fetch_lyric(song_id):
    raw = https_get_json("music.163.com", "/api/song/lyric?id=%s&lv=1&kv=1&tv=-1" % song_id, {})
    if not isinstance(raw, str):
        return ""
    try:
        data = json.loads(raw)
    except Exception:
        return ""
    if data.get("code") != 200:
        return ""
    return (data.get("lrc") or {}).get("lyric") or ""


# 可播放性缓存：song_id -> True/False，避免同一个 id 反复探测
_PLAY_CACHE = {}
_PLAY_CACHE_TTL = 10 * 60


def is_playable(song_id):
    now = time.time()
    hit = _PLAY_CACHE.get(song_id)
    if hit and now - hit[0] < _PLAY_CACHE_TTL:
        return hit[1]
    purl = resolve_play_url(song_id)
    ok = bool(purl and probe_range(purl))
    _PLAY_CACHE[song_id] = (now, ok)
    return ok


def sha256_hex(text):
    return hashlib.sha256(text.encode()).hexdigest()



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


def build_ogg(song_id, audio_url, expected_ms=0):
    """MP3 -> OGG/Opus（24kHz 单声道）。返回文件路径，失败返回 None。"""
    out = _ogg_path(song_id)
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
        # 校验时长：防止把试听片段当完整歌曲
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


def _prewarm_ogg(song_id):
    """后台把某首歌转成 OGG，避免用户点播时等转码"""
    try:
        if os.path.exists(_ogg_path(song_id)) and os.path.getsize(_ogg_path(song_id)) > 1024:
            return
        purl = resolve_play_url(song_id)
        if purl and probe_range(purl):
            build_ogg(song_id, purl)
            print("[music] prewarm done id=%s" % song_id, file=sys.stderr)
    except Exception as e:
        print("[music] prewarm failed id=%s: %s" % (song_id, e), file=sys.stderr)



def media_duration_ms(path_or_url, is_url=False):
    """用 ffprobe 取时长（毫秒）；失败返回 0"""
    try:
        cmd = ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0"]
        if is_url:
            cmd = ["ffprobe", "-v", "error", "-user_agent", NETEASE_UA, "-headers",
                   "Referer: https://music.163.com\r\n", "-show_entries", "format=duration",
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
            "market": parts[0].strip().strip('"'),
            "code": parts[1],
            "name": parts[2],
            "type": parts[4] if len(parts) > 4 else "",
        })
        if len(out) >= 10:
            break
    return out



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
    key = "kw:" + song.strip().lower() + "|" + artist.strip().lower()
    cached = _KUWO_CACHE.get(key)
    if cached and time.time() - cached[0] < _KUWO_TTL:
        return cached[1]
    kw = song + (" " + artist if artist else "")
    raw = _http_get_text(KUWO_SEARCH % (urllib.parse.quote(kw), limit))
    if not raw:
        return []
    body = raw.strip().replace("&nbsp;", " ")
    data = {}
    # 酷我返回 "Python dict 字面量"（单引号/None 等），优先 literal_eval
    try:
        data = _ast.literal_eval(body)
    except Exception as e1:
        sys.stderr.write("[music] kuwo literal_eval failed: %r\n" % (e1,))
        try:
            data = json.loads(body.replace("\'", "'"))
        except Exception as e2:
            sys.stderr.write("[music] kuwo json failed: %r\n" % (e2,))
            data = {}
    if not isinstance(data, dict):
        data = {}
    items = data.get("abslist") or []
    out = []
    for it in items:
        try:
            rid = _rid_of(it.get("MUSICRID", ""))
        except Exception:
            continue
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
    if out:                      # 空结果不缓存，避免一次抖动导致 30 分钟搜不到
        _KUWO_CACHE[key] = (time.time(), out)
    sys.stderr.write("[music] kuwo search %r -> %d item(s)\n" % (kw, len(out)))
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


def check_auth_query(params):
    """同一套密钥，但允许通过 URL query 传递（设备播放器拉流时加不了 header）"""
    def get(name):
        v = params.get(name) or params.get(name.upper()) or []
        return (v[0] if isinstance(v, list) and v else v) or ""

    mac = get("mac") or get("X-MAC-Address")
    chip = get("chip") or get("X-Chip-ID")
    ts = get("ts") or get("X-Timestamp")
    key = get("key") or get("X-Key")
    if not (mac and chip and ts and key):
        return False
    expect = sha256_hex(f"{mac}:{chip}:{ts}:{AUTH_SECRET}")[:32]
    return hmac.compare_digest(expect.lower(), str(key).lower())


def check_auth(headers):
    """Implements the firmware's key scheme:
    X-Key = sha256(<mac>:<chipid>:<timestamp>:<secret>), hex, first 16 bytes.
    """
    def get(name):
        low = name.lower()
        for k, v in headers.items():
            if k.lower() == low:
                return v
        return ""

    mac = get("X-MAC-Address")
    chip = get("X-Chip-ID")
    ts = get("X-Timestamp")
    key = get("X-Key") or get("X-Dynamic-Key")
    if not (mac and chip and ts and key):
        return False
    # The firmware sends uptime seconds (boot-relative), not Unix epoch, so
    # a clock-skew window cannot be enforced. Authenticate purely on the
    # dynamic key; it already binds mac/chip/timestamp to the secret.
    expect = sha256_hex(f"{mac}:{chip}:{ts}:{AUTH_SECRET}")[:32]
    return hmac.compare_digest(expect.lower(), key.lower())


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[music] %s\n" % (fmt % args))

    def _send_json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_text(self, code, text, ctype):
        body = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        params = urllib.parse.parse_qs(parsed.query)

        if AUTH_REQUIRED and not (check_auth(self.headers) or check_auth_query(params)):
            self._send_json(401, {"error": "unauthorized"})
            return

        # ---- 新增：直接给设备送 OGG/Opus（设备端只解码 Opus）----
        if path == "/stream_ogg":
            song_id = (params.get("id") or [""])[0]
            song = (params.get("song") or [""])[0]
            artist = (params.get("artist") or [""])[0]
            title = ""
            artist_name = ""
            purl = None
            if song_id:
                if song_id.startswith("kw"):
                    purl = kuwo_play_url(song_id)
                else:
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
                        break
                if not chosen:
                    self._send_json(404, {"error": "no playable stream"})
                    return
                c, purl = chosen
                song_id = str(c["id"])
                title = c.get("name", "")
                artist_name = c.get("artist", "")
            expected = 0
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
        m = re.match(r"^/lyric_json/(kw)?(\d+)$", path)
        if m:
            if m.group(1):
                lrc = kuwo_lyric_lrc(m.group(2))
            else:
                lrc = fetch_lyric(m.group(2))
            self._send_json(200, {"id": (m.group(1) or "") + m.group(2), "lines": lrc_to_json(lrc)})
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

        if path == "/stream_pcm":
            song = (params.get("song") or [""])[0]
            artist = (params.get("artist") or [""])[0]
            print(f"[music] stream_pcm song={song!r} artist={artist!r}", flush=True)
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
            chosen_by = "kuwo"
            for c in ordered:
                if str(c.get("id", "")).startswith("kw"):
                    purl = kuwo_play_url(c["id"])
                else:
                    purl = resolve_play_url(c["id"])
                if purl and probe_range(purl):
                    chosen = {"info": c, "url": purl}
                    break
            if not chosen:
                self._send_json(404, {"error": "no playable stream"})
                return
            song_key = str(chosen["info"]["id"])
            payload = {
                "artist": chosen["info"]["artist"],
                "title": chosen["info"]["name"],
                "audio_url": chosen["url"],
                "lyric_url": f"{BASE_URL}/lyric/{song_key}.lrc",
            }
            print(f"[music] stream_pcm chosen: title={chosen['info']['name']!r} "
                  f"artist={chosen['info']['artist']!r} id={song_key} by={chosen_by}", flush=True)
            self._send_json(200, payload)
            return

        # 候选列表：一次返回最多 PAGE（默认5）个，供用户语音选择
        if path == "/search":
            song = (params.get("song") or [""])[0]
            artist = (params.get("artist") or [""])[0]
            try:
                offset = int((params.get("offset") or ["0"])[0])
                limit = int((params.get("limit") or ["5"])[0])
            except ValueError:
                self._send_json(400, {"error": "bad offset/limit"})
                return
            limit = max(1, min(limit, 10))
            offset = max(0, offset)
            if not song:
                self._send_json(400, {"error": "missing song"})
                return
            # 首选酷我（官方版本命中率高），无结果再退回网易云
            ordered = kuwo_pick(kuwo_search(song, artist), song, artist)
            if not ordered:
                ordered = search_candidates(song, artist)
            if not ordered:
                self._send_json(404, {"error": "song not found"})
                return
            page = ordered[offset:offset + limit]
            # 并发探测每个候选是否可播放（有缓存），标记 VIP/无法播放的
            def _playable(c):
                cid = str(c.get("id", ""))
                if c.get("provider") == "kuwo" or cid.startswith("kw"):
                    return kuwo_is_playable(cid)
                return is_playable(cid)

            with ThreadPoolExecutor(max_workers=min(len(page), 5)) as ex:
                playable_map = dict(zip(
                    [str(c["id"]) for c in page],
                    list(ex.map(_playable, page))))
            items = []
            for i, c in enumerate(page):
                cid = str(c["id"])
                items.append({
                    "index": offset + i + 1,   # 1-based 序号，给用户报"第几个"
                    "id": cid,
                    "name": c.get("name", ""),
                    "artist": c.get("artist", ""),
                    "album": c.get("album", ""),
                    "duration_ms": c.get("duration", 0),
                    "playable": playable_map.get(cid, False),
                })
            # 预热：后台转码第一个可播放候选（点播时即可秒开）
            for it in items:
                if it.get("playable"):
                    threading.Thread(target=_prewarm_ogg, args=(it["id"],), daemon=True).start()
                    break
            print(f"[music] search song={song!r} artist={artist!r} offset={offset} "
                  f"total={len(ordered)} items={len(items)}", flush=True)
            self._send_json(200, {
                "song": song,
                "total": len(ordered),
                "offset": offset,
                "count": len(items),
                "has_more": offset + len(items) < len(ordered),
                "items": items,
            })
            return

        # 按网易云 song_id 直接取可播放地址（选中后调用）
        if path == "/play":
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
            purl = resolve_play_url(song_id)
            # 必须真实可播放（Range + 音频魔数校验通过），不退回无效 URL
            if not (purl and probe_range(purl)):
                print(f"[music] play id={song_id} NOT playable (VIP or broken)", flush=True)
                self._send_json(404, {"error": "not_playable"})
                return
            # 从搜索缓存里找回 title/artist，便于设备显示歌名
            title = ""
            artist = ""
            for (_key, (_ts, cands)) in _SEARCH_CACHE.items():
                for c in cands:
                    if str(c.get("id")) == song_id:
                        title = c.get("name", "")
                        artist = c.get("artist", "")
                        break
                if title:
                    break
            print(f"[music] play id={song_id} by=range_ok", flush=True)
            self._send_json(200, {
                "id": song_id,
                "title": title,
                "artist": artist,
                "audio_url": purl,
                "lyric_url": f"{BASE_URL}/lyric/{song_id}.lrc",
            })
            return

        m = re.match(r"^/lyric/(\d+)\.lrc$", path)
        if m:
            lyric = fetch_lyric(m.group(1))
            print(f"[music] lyric id={m.group(1)} len={len(lyric)}", flush=True)
            self._send_text(200, lyric or "[00:00.00]暂无歌词", "text/plain; charset=utf-8")
            return

        if path in ("/health", "/"):
            self._send_json(200, {"status": "ok", "auth": AUTH_REQUIRED})
            return

        self._send_json(404, {"error": "not found"})


if __name__ == "__main__":
    if not os.environ.get("BASE_URL"):
        resolved = public_ip_url()
        BASE_URL = resolved or f"http://localhost:{PORT}"
    srv = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"[music] listening on {HOST}:{PORT}  base_url={BASE_URL}  auth={'on' if AUTH_REQUIRED else 'off'}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass