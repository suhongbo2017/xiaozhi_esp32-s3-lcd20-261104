#!/usr/bin/env python3
"""免费源优化：
1) 用 vkeys 的 quality 字段判断"是否只有试听"，不再做昂贵的 range 探测（搜索从 17s 降到几秒）
2) 播放时跳过只有试听的候选（VIP 原唱），自动选一首"免费完整版"（通常是同名翻唱）
3) 错误信息更明确（VIP/trial）
"""
import io

P = "music_server.py.new"
s = io.open(P, encoding="utf-8").read()

# ---------- 1) tencent detail -> 试听判断 ----------
s = s.replace('''def vkeys_is_playable(song_id):
    url = vkeys_play_url(song_id)
    return bool(url) and probe_range(url)''','''def vkeys_trial_only(song_id):
    """vkeys 的 quality 里带"试听"说明只有片段（VIP）；免费曲目是 320kbps/SQ"""
    d = _tx_detail(song_id) or {}
    q = str(d.get("quality") or "")
    if not d.get("url"):
        return True
    return ("试听" in q) or (not q and str(d.get("kbps", "")).replace("kbps", "").isdigit()
                             and int(str(d.get("kbps", "0")).replace("kbps", "") or 0) < 64)


def vkeys_is_playable(song_id):
    """playable = 有 url 且不是仅试听片段"""
    return not vkeys_trial_only(song_id)''', 1)

# ---------- 2) /search 的 playable 探测：tencent 用 quality 判断 ----------
s = s.replace('''            def _playable(c):
                cid = str(c.get("id", ""))
                if cid.startswith("tx"):
                    return vkeys_is_playable(cid)''','''            def _playable(c):
                cid = str(c.get("id", ""))
                if cid.startswith("tx"):
                    return vkeys_is_playable(cid)   # 仅查 detail(缓存)，不做 range 探测''', 1)

# ---------- 3) /stream_ogg 候选循环：跳过仅试听的 tx ----------
s = s.replace('''                for c in ordered:
                    cid = str(c.get("id", ""))
                    if cid.startswith("tx"):
                        p = vkeys_play_url(cid)
                    elif cid.startswith("kw"):
                        p = kuwo_play_url(cid)
                    else:
                        p = resolve_play_url(cid)''','''                trial_choice = None
                for c in ordered:
                    cid = str(c.get("id", ""))
                    if cid.startswith("tx"):
                        if vkeys_trial_only(cid):
                            # 记住这个试听候选，若后面没有完整版再退回它
                            if trial_choice is None:
                                trial_choice = (c, vkeys_play_url(cid))
                            continue
                        p = vkeys_play_url(cid)
                    elif cid.startswith("kw"):
                        p = kuwo_play_url(cid)
                    else:
                        p = resolve_play_url(cid)''', 1)

s = s.replace('''                    if p and probe_range(p):
                        chosen = (c, p)
                        break''','''                    if p and probe_range(p):
                        chosen = (c, p)
                        break
                if chosen is None and trial_choice and trial_choice[1]:
                    chosen = trial_choice   # 实在没有完整版，退回试听并打日志
                    log("only trial available for %r" % (song,))''', 1)

# ---------- 4) /stream_pcm 同样跳过试听 ----------
s = s.replace('''            for c in ordered:
                cid = str(c.get("id", ""))
                if cid.startswith("tx"):
                    purl = vkeys_play_url(cid)
                elif cid.startswith("kw"):
                    purl = kuwo_play_url(cid)
                else:
                    purl = resolve_play_url(cid)
                if purl and probe_range(purl):
                    chosen = {"info": c, "url": purl}
                    break''','''            for c in ordered:
                cid = str(c.get("id", ""))
                if cid.startswith("tx"):
                    if vkeys_trial_only(cid):
                        continue
                    purl = vkeys_play_url(cid)
                elif cid.startswith("kw"):
                    purl = kuwo_play_url(cid)
                else:
                    purl = resolve_play_url(cid)
                if purl and probe_range(purl):
                    chosen = {"info": c, "url": purl}
                    break''', 1)

# ---------- 5) 错误信息更明确 ----------
s = s.replace('''                self._send_json(503, {"error": "no_full_stream",
                                      "hint": "provider only returned a trial clip"})''','''                self._send_json(503, {"error": "no_full_stream",
                                      "hint": "only VIP trial clips available for this song; "
                                              "ask the user to pick another version"})''', 1)

io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("free-source optimization patched:", "vkeys_trial_only" in s)
