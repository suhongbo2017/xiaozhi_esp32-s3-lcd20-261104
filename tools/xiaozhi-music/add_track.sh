#!/bin/bash
# 手动把任意免费音乐 URL 加进服务器曲库： ./add_track.sh <url> "标题" "作者" "标签" ["授权"]
set -e
URL="$1"; TITLE="${2:-}"; ARTIST="${3:-}"; TAGS="${4:-}"
LIC="${5:-CC}"
DIR=/opt/xiaozhi-music/library
mkdir -p "$DIR"
NAME="lib_manual_$(date +%s)_$(basename "${URL%%\?*}" | tr -cd 'a-zA-Z0-9._-' | cut -c1-40)"
[ -z "$NAME" ] && NAME="lib_manual_$(date +%s).mp3"
case "$NAME" in *.mp3) ;; *) NAME="$NAME.mp3";; esac
curl -sL -m 600 -A "Mozilla/5.0" -H "Referer: $URL" -o "$DIR/$NAME" "$URL"
SZ=$(stat -c%s "$DIR/$NAME")
if [ "$SZ" -lt 20000 ]; then echo "[!] 下载失败或文件过小 ($SZ bytes)"; rm -f "$DIR/$NAME"; exit 1; fi
python3 - "$DIR" "$NAME" "$TITLE" "$ARTIST" "$TAGS" "$LIC" <<'PY'
import json, os, sys
d, name, title, artist, tags, lic = sys.argv[1:7]
p = os.path.join(d, "library.json")
meta = {}
if os.path.exists(p):
    try:
        meta = {t["file"]: t for t in json.load(open(p, encoding="utf-8")).get("tracks", []) if t.get("file")}
    except Exception:
        meta = {}
meta[name] = {"file": name, "title": title or name, "artist": artist or "CC Music",
              "tags": tags, "license": lic, "source_id": "manual"}
json.dump({"tracks": list(meta.values())}, open(p, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print("added:", name)
PY
echo "完成：$(ls -lh "$DIR/$NAME" | awk '{print $5, $9}')"
