#ifndef MUSIC_API_CLIENT_H
#define MUSIC_API_CLIENT_H

#include "music_player.h"   /* MusicLyricLine */

#include <cstdint>
#include <string>
#include <vector>

/**
 * MusicApiClient —— 对接自建音乐/行情服务（/opt/xiaozhi-music/music_server.py）
 *
 * 服务端接口（已实测）：
 *   GET /search?song=&artist=&offset=&limit=      -> {total, items:[{index,id,name,artist,album,playable}]}
 *   GET /play?id=                                 -> {id,title,artist,audio_url,lyric_url}
 *   GET /stream_pcm?song=&artist=                 -> {artist,title,audio_url,lyric_url}
 *   GET /stream_ogg?id= / ?song=&artist=          -> OGG/Opus 二进制（24kHz 单声道，供本机播放）
 *   GET /lyric_json/<id>                          -> {id, lines:[{start_ms,text}]}
 *   GET /stock/quote?code=                        -> 行情 JSON
 *   GET /stock/search?q=                          -> 搜索结果 JSON
 *
 * 鉴权（服务端 check_auth）：
 *   X-MAC-Address / X-Chip-ID / X-Timestamp / X-Key
 *   X-Key = sha256("<mac>:<chip>:<timestamp>:<secret>")[:32]
 *   其中 timestamp 用开机 uptime 秒即可（服务端不校验时钟偏差）
 */
class MusicApiClient {
public:
    struct SongItem {
        int index = 0;
        std::string id;
        std::string name;
        std::string artist;
        std::string album;
        bool playable = false;
    };

    static MusicApiClient& GetInstance();

    /* ---- 搜索 / 详情 ---- */
    /** 在线搜索；成功返回 true，结果写入 out（并附带服务端原始 JSON 供 MCP 返回） */
    bool Search(const std::string& song, const std::string& artist, int offset,
                std::vector<SongItem>* out, std::string* raw_json = nullptr);

    /** 取某首歌的歌词（LRC 已由服务端解析成时间轴） */
    bool GetLyrics(const std::string& song_id, std::vector<MusicLyricLine>* out);

    /* ---- 播放地址（供 NotifyPlayer 拉流）---- */
    std::string StreamUrlById(const std::string& song_id) const;
    std::string StreamUrlByName(const std::string& song, const std::string& artist) const;

    /* ---- 股票 ---- */
    bool StockQuote(const std::string& code, std::string* out_json);
    bool StockSearch(const std::string& keyword, std::string* out_json);

    /* ---- 配置 ---- */
    bool configured() const { return !base_url_.empty(); }
    const std::string& base_url() const { return base_url_; }

private:
    MusicApiClient();

    /** 带鉴权头的 GET，返回 HTTP 状态码；成功时 body 写入 out */
    int HttpGet(const std::string& path_and_query, std::string* body, int timeout_ms = 8000);

    std::string base_url_;
    std::string secret_;
    std::string chip_id_;
    std::string mac_;
};

#endif  // MUSIC_API_CLIENT_H
