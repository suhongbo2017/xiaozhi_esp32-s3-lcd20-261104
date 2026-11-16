#!/usr/bin/env python3
"""把免费 CC 曲目批量下载到服务器曲库（并行下载 + 跳过超大文件）。

用法:
    python3 /opt/xiaozhi-music/harvest.py                # 默认标签，每标签 20 首
    python3 /opt/xiaozhi-music/harvest.py vocals jazz    # 指定标签
"""
import concurrent.futures as cf
import http.client
import json
import os
import sys
import threading
import time
import urllib.parse

BASE = os.path.dirname(os.path.abspath(__file__))
LIB_DIR = os.path.join(BASE, "library")
LIB_JSON = os.path.join(LIB_DIR, "library.json")
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
os.makedirs(LIB_DIR, exist_ok=True)
LOCK = threading.Lock()

DEFAULT_TAGS = ["vocals", "acappella", "jazz", "piano", "chill", "rock", "electronic",
                "folk", "soundtrack", "hiphop", "classical", "ambient", "guitar", "pop"]
MAX_BYTES = 6 * 1024 * 1024     # 超过 6MB 的先跳过（下载太慢）
PER_TAG = 20
WORKERS = 4


def http_get(url, timeout=180, binary=False, referer="https://ccmixter.org/"):
    try:
        host = url.split("/")[2]
        path = "/" + "/".join(url.split("/")[3:])
        conn = http.client.HTTPSConnection(host, timeout=timeout) if url.startswith("https://") \
            else http.client.HTTPConnection(host, timeout=timeout)
        conn.request("GET", path, headers={"User-Agent": UA, "Referer": referer})
        r = conn.getresponse()
        data = r.read()
        conn.close()
        if r.status not in (200, 206):
            return None
        return data if binary else data.decode("utf-8", "replace")
    except Exception:
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


def fetch_one(item):
    """下载一首，返回 (fname, meta) 或 None"""
    sid, remote, title, artist, tag, lic = item
    fname = "lib_%s.mp3" % sid
    target = os.path.join(LIB_DIR, fname)
    if os.path.exists(target) and os.path.getsize(target) > 20000:
        return (fname, {"file": fname, "title": title, "artist": artist, "tags": tag,
                        "license": lic, "source_id": sid})
    data = http_get(remote, 240, binary=True)
    if not data or len(data) < 20000:
        return None
    if len(data) > MAX_BYTES:
        return None
    with open(target + ".part", "wb") as fh:
        fh.write(data)
    os.replace(target + ".part", target)
    print("  [%s] %s (%.1f MB)" % (tag, title[:32], len(data) / 1048576.0), flush=True)
    return (fname, {"file": fname, "title": title, "artist": artist, "tags": tag,
                    "license": lic, "source_id": sid})


def harvest_tag(tag):
    meta = load_meta()
    have = {t.get("source_id") for t in meta.values()}
    url = ("https://ccmixter.org/api/query?f=json&limit=%d&tags=%s&ord=rank"
           % (PER_TAG, urllib.parse.quote(tag)))
    raw = http_get(url, 30, referer="https://ccmixter.org/")
    if not raw:
        print("[%s] api failed" % tag, flush=True)
        return 0
    try:
        arr = json.loads(raw)
    except Exception:
        return 0
    jobs = []
    for it in arr if isinstance(arr, list) else []:
        sid = "cc%d" % int(it.get("upload_id", 0) or 0)
        if sid in have:
            continue
        files = it.get("files") or []
        mp3 = next((f for f in files if str(f.get("file_name", "")).lower().endswith(".mp3")), None)
        rel = (it.get("upload_extra") or {}).get("relative_dir") or ""
        if not mp3 or not rel:
            continue
        jobs.append((sid, "https://ccmixter.org/%s/%s" % (rel, mp3["file_name"]),
                     (it.get("upload_name") or "").strip(), (it.get("user_name") or "").strip(),
                     tag, (it.get("license_name") or "CC").strip()))
    got = 0
    if jobs:
        with cf.ThreadPoolExecutor(max_workers=WORKERS) as ex:
            for res in ex.map(fetch_one, jobs):
                if res:
                    fname, m = res
                    with LOCK:
                        meta = load_meta()
                        meta[fname] = m
                        save_meta(meta)
                    got += 1
    print("[%s] +%d" % (tag, got), flush=True)
    return got


if __name__ == "__main__":
    tags = sys.argv[1:] or DEFAULT_TAGS
    total = 0
    for t in tags:
        total += harvest_tag(t)
        print("== total %d, files %d" % (total, len([f for f in os.listdir(LIB_DIR) if f.endswith('.mp3')])), flush=True)
    print("DONE total=%d" % total, flush=True)
