#!/usr/bin/env python3
"""A 方案打磨：
1) Incompetech 曲库扩充到 22 首（全部 200 校验通过），带中文标签用于风格匹配
2) 风格/空查询时随机打散，避免每次都放同一首
3) 允许 song 为空（= 随便放首歌），走免费源
"""
import io

P = "music_server.py.new"
s = io.open(P, encoding="utf-8").read()

# ---------- 1) 替换曲库 ----------
start = s.index("# Incompetech 精选（CC-BY 4.0，作者 Kevin MacLeod），可直接播放")
end = s.index("def _cc_translate(kw):")
new_catalog = '''# Incompetech 精选（CC BY 4.0，作者 Kevin MacLeod），全部 URL 已 200 校验
# 字段：标题 / 中文风格标签（用于匹配"钢琴曲/轻音乐/搞笑…"这类查询）
IK_CATALOG = [
    ("Monkeys Spinning Monkeys", "欢快 搞笑 滑稽 背景 纯音乐"),
    ("Carefree", "轻快 欢快 尤克里里 纯音乐"),
    ("Sneaky Snitch", "滑稽 悬疑 喜剧 搞笑"),
    ("Wallpaper", "轻快 复古 纯音乐"),
    ("Fluffing a Duck", "俏皮 欢快 搞笑"),
    ("Investigations", "悬疑 探案 紧张"),
    ("Local Forecast", "轻松 商务 纯音乐"),
    ("Life of Riley", "欢快 民谣 纯音乐"),
    ("Fig Leaf Rag", "复古 拉格泰姆 钢琴"),
    ("Happy Alley", "开心 明亮 纯音乐"),
    ("Bossa Antigua", "波萨诺瓦 慵懒 放松 爵士"),
    ("Airport Lounge", "沙发音乐 放松 慵懒 爵士"),
    ("George Street Shuffle", "爵士 摇摆 轻快"),
    ("Deliberate Thought", "安静 思考 氛围 纯音乐"),
    ("Fretless", "舒缓 吉他 纯音乐"),
    ("Gymnopedie No 1", "古典 钢琴 安静 舒缓"),
    ("Hall of the Mountain King", "古典 紧张 戏剧"),
    ("Quirky Dog", "俏皮 搞笑 欢快"),
    ("Rocket Power", "动感 电子 欢快"),
    ("Sardana", "民族 欢快 舞曲"),
    ("Shiny Tech", "科技 电子 明亮"),
    ("Spazzmatica Polka", "幽默 快节奏 波尔卡"),
    ("Wagon Wheel", "民谣 乡村 轻松"),
]


'''
s = s[:start] + new_catalog + s[end:]

# ---------- 2) 风格查询：随机打散 + 允许空查询 ----------
s = s.replace('''    if not is_genre:
        log("ccmixter has no match for %r (specific song) -> let upper layer fall back" % (song,))
        return []''','''    if not is_genre:
        log("ccmixter has no match for %r (specific song) -> let upper layer fall back" % (song,))
        return []
    # 风格类查询：打散顺序，避免每次都放同一首
    import random as _random''', 1)

s = s.replace('''    log("ccmixter empty for %r -> incompetech catalog %d" % (kw, len(items)))
    return items[:limit]''','''    _random.shuffle(items)
    log("ccmixter empty for %r -> incompetech catalog %d (shuffled)" % (kw, len(items)))
    return items[:limit]''', 1)

# 关键词命中优先：把与查询标签匹配的曲目排前面
s = s.replace('''    for i, (title, tags) in enumerate(IK_CATALOG):
        if q and (q in title.lower() or any(t in tags for t in _cc_translate(q).split())):
            pass
        items.append({''','''    qtags = [t.strip() for t in _cc_translate(q).replace(",", " ").split() if t.strip()]
    matched, others = [], []
    for i, (title, tags) in enumerate(IK_CATALOG):
        hit = bool(qtags) and any(t in tags or t in title.lower() for t in qtags)
        entry = {''', 1)

s = s.replace('''            "provider": "incompetech",
            "score": 0,
            "_url": "https://incompetech.com/music/royalty-free/mp3-royaltyfree/%s.mp3"
                    % urllib.parse.quote(title),
        })
    _random.shuffle(items)''','''            "provider": "incompetech",
            "score": 0,
            "_url": "https://incompetech.com/music/royalty-free/mp3-royaltyfree/%s.mp3"
                    % urllib.parse.quote(title),
        }
        (matched if hit else others).append(entry)
    _random.shuffle(matched)
    _random.shuffle(others)
    items = matched + others''', 1)

# ---------- 3) /search 与 /stream_ogg 允许 song 为空（随便放首歌）----------
s = s.replace('''            if not song:
                self._send_json(400, {"error": "missing song"})
                return
            # 首选无版权限制的免费源（ccMixter / Incompetech）''','''            if not song and MUSIC_PROVIDER != "free":
                self._send_json(400, {"error": "missing song"})
                return
            # 首选无版权限制的免费源（ccMixter / Incompetech）
            # song 为空时 = "随便放首歌"，走内置 CC 曲库（随机）''', 1)

# /stream_ogg 里的空 song 检查
s = s.replace('''            else:
                if not song:
                    self._send_json(400, {"error": "missing song"})
                    return
                ordered = free_pick(free_search(song, artist, 6), song, artist)''','''            else:
                if not song and MUSIC_PROVIDER != "free":
                    self._send_json(400, {"error": "missing song"})
                    return
                ordered = free_pick(free_search(song, artist, 6), song, artist)''', 1)

# /stream_pcm 同理
s = s.replace('''            if not song:
                self._send_json(400, {"error": "missing song"})
                return
            ordered = free_pick(free_search(song, artist, 6), song, artist)''','''            if not song and MUSIC_PROVIDER != "free":
                self._send_json(400, {"error": "missing song"})
                return
            ordered = free_pick(free_search(song, artist, 6), song, artist)''', 1)

io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("catalog expanded:", len(s.split("IK_CATALOG = [")[1].split("]")[0].split("\n")) if "IK_CATALOG = [" in s else 0)
