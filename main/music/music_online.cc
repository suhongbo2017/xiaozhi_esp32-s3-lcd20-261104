#include "music_online.h"

#include "music_api_client.h"
#include "mcp_server.h"

#include <esp_log.h>

#include <cJSON.h>

#define TAG "MusicOnline"

namespace music_online {

void RegisterTools(MusicPlayer& player) {
    auto& mcp = McpServer::GetInstance();

    /* ---- 在线搜索（旧固件语义：song_name / artist_name / offset） ---- */
    mcp.AddTool(
        "self.music.search_song",
        "搜索在线歌曲。参数 song_name=歌名，artist_name=歌手（可空），offset=分页偏移（默认0）。"
        "返回 JSON：{total, items:[{index,id,name,artist,album,playable,trial}]}。"
        "playable=true 时把 items[].id 传给 self.music.play_song_by_id 播放；"
        "trial=true 表示该版本只有约 60 秒试听（VIP 曲），**仍然可以播放**，"
        "请直接播放并告知用户这是试听片段，不要拒绝或说无法播放。",
        PropertyList({
            Property("song_name", kPropertyTypeString),
            Property("artist_name", kPropertyTypeString, ""),
            Property("offset", kPropertyTypeInteger, 0),
        }),
        [&player](const PropertyList& props) -> ReturnValue {
            std::string json;
            if (!player.SearchOnline(props["song_name"].value<std::string>(),
                                     props["artist_name"].value<std::string>(),
                                     props["offset"].value<int>(), &json)) {
                return std::string("{\"error\":\"search_failed\"}");
            }
            return json;
        });

    /* ---- 播放：支持 在线歌名 / URL 直推 / 本地关键词 / 序号 ---- */
    mcp.AddTool(
        "self.music.play_song",
        "播放歌曲。优先 song_name(+artist_name) 在线搜索并播放第一首可播版本（VIP 曲目会播 60 秒试听）；"
        "也可传 url 直接播放音频流，或传 keyword/index 从本地列表选曲。"
        "如果 song_name 为空，则随机播放一首免费的纯音乐（CC 授权）。",
        PropertyList({
            Property("song_name", kPropertyTypeString, ""),
            Property("artist_name", kPropertyTypeString, ""),
            Property("url", kPropertyTypeString, ""),
            Property("keyword", kPropertyTypeString, ""),
            Property("index", kPropertyTypeInteger, -1),
        }),
        [&player](const PropertyList& props) -> ReturnValue {
            auto url = props["url"].value<std::string>();
            if (!url.empty()) {
                MusicTrack track;
                track.id = "adhoc";
                track.url = url;
                track.title = props["song_name"].value<std::string>();
                if (track.title.empty()) {
                    track.title = "网络音乐";
                }
                track.artist = props["artist_name"].value<std::string>();
                return player.PlayTrack(track);
            }
            auto song = props["song_name"].value<std::string>();
            if (!song.empty()) {
                return player.PlayOnline(song, props["artist_name"].value<std::string>());
            }
            auto keyword = props["keyword"].value<std::string>();
            if (!keyword.empty()) {
                return player.PlayByKeyword(keyword);
            }
            int index = props["index"].value<int>();
            return player.PlayIndex(index >= 0 ? static_cast<size_t>(index) : 0);
        });

    /* ---- 按 id 播放（旧固件语义：song_id） ---- */
    mcp.AddTool(
        "self.music.play_song_by_id",
        "按歌曲 id 播放（id 来自 self.music.search_song 的 items[].id）。会自动取歌词并显示。"
        "若该曲是 VIP（trial=true）会播放约 60 秒试听片段，这是正常行为。",
        PropertyList({Property("song_id", kPropertyTypeString)}),
        [&player](const PropertyList& props) -> ReturnValue {
            return player.PlayOnlineById(props["song_id"].value<std::string>());
        });

    /* ---- 显示模式：兼容旧值 spectrum/lyrics 与现值 lyric/cover/hidden ---- */
    mcp.AddTool(
        "self.music.set_display_mode",
        "设置音乐页显示模式：lyrics（歌词滚动，默认）/ spectrum（频谱，暂等同封面模式）/ cover（仅封面+进度）/ hidden（隐藏音乐页）。",
        PropertyList({Property("mode", kPropertyTypeString)}),
        [&player](const PropertyList& props) -> ReturnValue {
            auto mode = props["mode"].value<std::string>();
            int value;
            if (mode == "lyrics" || mode == "lyric") {
                value = 0;
            } else if (mode == "spectrum" || mode == "cover") {
                value = 1;
            } else if (mode == "hidden") {
                value = 2;
            } else {
                return false;
            }
            return player.SetDisplayMode(value);
        });

    ESP_LOGI(TAG, "online music tools registered (api=%s)",
             MusicApiClient::GetInstance().base_url().c_str());
}

void CacheFromSearchJson(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return;
    }
    cJSON* items = cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(items)) {
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, items) {
            auto id = cJSON_GetObjectItem(it, "id");
            auto name = cJSON_GetObjectItem(it, "name");
            auto artist = cJSON_GetObjectItem(it, "artist");
            if (cJSON_IsString(id) && cJSON_IsString(name)) {
                MusicPlayer::GetInstance().RememberSong(
                    id->valuestring, name->valuestring,
                    cJSON_IsString(artist) ? artist->valuestring : "", 0);
            }
        }
    }
    cJSON_Delete(root);
}

}  // namespace music_online
