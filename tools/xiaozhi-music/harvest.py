#!/usr/bin/env python3
"""把免费 CC 曲目批量下载到服务器曲库（不走实时 API 播放）。

用法（在服务器上）:
    python3 /opt/xiaozhi-music/harvest.py             # 用默认标签抓取
    python3 /opt/xiaozhi-music/harvest.py vocals jazz # 指定标签

每首下载后写入 library.json（title/artist/tags/license/source_id），
之后 /search 可以直接搜到、/play 走 /library 本地直发。
"""
import http.client
import json
import os
import sys
import time
import urllib.parse

BASE = os.path.dirname(os.path.abspath(__file__))
LIB_DIR = os.path.join(BASE, "library")
LIB_JSON = os.path.join(LIB_DIR, "library.json")
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
os.makedirs(LIB_DIR, exist_ok=True)

DEFAULT_TAGS = ["vocals", "acappella", "jazz", "piano", "chill", "rock", "electronic",
                "folk", "soundtrack", "hiphop", "classical", "ambient"]


def get(url, timeout=60, binary=False):
    try:
        host = url.split("/")[2]
        path = "/" + "/".join(url.split("/")[3:])
        conn = http.client.HTTPSConnection(host, timeout=timeout) if url.startswith("https://") \
            else http.client.HTTPConnection(host, timeout=timeout)
        conn.request("GET", path, headers={"User-Agent": UA, "Referer": "https://ccmixter.org/"})
        r = conn.getresponse()
        data = r.read()
        conn.close()
        if r.status not in (200, 206):
            return None
        return data if binary else data.decode("utf-8", "replace")
    except Exception as e:
        print("  [get] %s -> %r" % (url[:60], e), flush=True)
        return None


def load_meta():
    try:
        with open(LIB_JSON, encoding="utf-8") as fh:
            return {t["file"]: t for t in (json.load(fh).get("tracks") or []) if t.get("file")}
    except Exception:
        return {}


def save_meta(meta):
    with open(LIB_JSON, "w", encoding="utf-8") as fh:
        json.dump({"tracks": list(meta.values())}, fh, ensure_ascii=False, indent=1)


def harvest_tag(tag, per_tag=12):
    meta = load_meta()
    have_ids = {t.get("source_id") for t in meta.values()}
    url = ("https://ccmixter.org/api/query?f=json&limit=%d&tags=%s&ord=rank"
           % (per_tag, urllib.parse.quote(tag)))
    raw = get(url, 30)
    if not raw:
        print("[%s] api failed" % tag, flush=True)
        return 0
    try:
        arr = json.loads(raw)
    except Exception as e:
        print("[%s] parse failed %r" % (tag, e), flush=True)
        return 0
    n = 0
    for it in arr if isinstance(arr, list) else []:
        sid = "cc%d" % int(it.get("upload_id", 0) or 0)
        if sid in have_ids:
            continue
        files = it.get("files") or []
        mp3 = next((f for f in files if str(f.get("file_name", "")).lower().endswith(".mp3")), None)
        rel = (it.get("upload_extra") or {}).get("relative_dir") or ""
        if not mp3 or not rel:
            continue
        remote = "https://ccmixter.org/%s/%s" % (rel, mp3["file_name"])
        title = (it.get("upload_name") or "").strip()
        artist = (it.get("user_name") or "").strip()
        lic = (it.get("license_name") or "CC").strip()
        fname = "lib_%s.mp3" % sid
        target = os.path.join(LIB_DIR, fname)
        if not (os.path.exists(target) and os.path.getsize(target) > 1024):
            data = get(remote, 180, binary=True)
            if not data or len(data) < 20000:
                print("  [%s] download failed %s" % (tag, title[:24]), flush=True)
                continue
            with open(target, "wb") as fh:
                fh.write(data)
            print("  [%s] %s (%.1f MB)" % (tag, title[:30], len(data) / 1048576.0), flush=True)
        meta[fname] = {
            "file": fname, "title": title, "artist": artist,
            "tags": tag, "license": lic, "source_id": sid,
        }
        have_ids.add(sid)
        save_meta(meta)
        n += 1
    return n


if __name__ == "__main__":
    tags = sys.argv[1:] or DEFAULT_TAGS
    total = 0
    for t in tags:
        got = harvest_tag(t)
        total += got
        print("[%s] +%d (total %d)" % (t, got, total), flush=True)
        time.sleep(1)
    print("DONE total=%d files=%d" % (total, len(os.listdir(LIB_DIR))), flush=True)
