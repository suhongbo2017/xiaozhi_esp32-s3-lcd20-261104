#include "music_player.h"

#include "application.h"
#include "board.h"
#include "mcp_server.h"
#include "music_api_client.h"
#include "music_online.h"
#include "music_page.h"
#include "notify/notify_player.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#define TAG "MusicPlayer"

MusicPlayer& MusicPlayer::GetInstance() {
    static MusicPlayer instance;
    return instance;
}

/* ------------------------------------------------------------------ */
/* MCP 工具                                                            */
/* ------------------------------------------------------------------ */
void MusicPlayer::Initialize() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tools_registered_) {
            return;
        }
        tools_registered_ = true;
    }

    music_online::RegisterTools(*this);

    ESP_LOGI(TAG, "music MCP tools registered");

    /* ---- bring-up 自检：CONFIG_MUSIC_SELFTEST_URL 非空时，开机 15s 后自动播一次 ---- */
    if (strlen(CONFIG_MUSIC_SELFTEST_URL) > 0) {
        esp_timer_create_args_t args = {
            .callback = [](void*) {
                MusicTrack track;
                track.id = "selftest";
                track.title = "自检音轨";
                track.artist = "LCD20 bring-up";
                track.url = CONFIG_MUSIC_SELFTEST_URL;
                track.duration_ms = 24000;
                /* 合成歌词时间轴：验证"到点切行"和 UI 渲染 */
                track.lyrics = {
                    {0, "第一句：链路自检开始"},
                    {8000, "第二句：HTTP 取流 + Opus 解码"},
                    {16000, "第三句：歌词与进度同步"},
                };
                /* 必须在主任务里操作状态机/音频服务 */
                Application::GetInstance().Schedule([track]() {
                    ESP_LOGI(TAG, "self-test: playing %s", track.url.c_str());
                    MusicPlayer::GetInstance().SetPlaylist({track});
                    MusicPlayer::GetInstance().PlayIndex(0);
                });
            },
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "music_selftest",
            .skip_unhandled_events = true,
        };
        esp_timer_handle_t timer = nullptr;
        if (esp_timer_create(&args, &timer) == ESP_OK) {
            esp_timer_start_once(timer, 15 * 1000 * 1000);
            ESP_LOGW(TAG, "music self-test armed: %s", CONFIG_MUSIC_SELFTEST_URL);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 曲库                                                                */
/* ------------------------------------------------------------------ */
void MusicPlayer::SetPlaylist(std::vector<MusicTrack> tracks) {
    std::lock_guard<std::mutex> lock(mutex_);
    playlist_ = std::move(tracks);
    ESP_LOGI(TAG, "playlist set: %u track(s)", static_cast<unsigned>(playlist_.size()));
}

void MusicPlayer::AddTrack(const MusicTrack& track) {
    std::lock_guard<std::mutex> lock(mutex_);
    playlist_.push_back(track);
}

std::vector<MusicTrack> MusicPlayer::GetPlaylist() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playlist_;
}

/* ------------------------------------------------------------------ */
/* 播放控制                                                            */
/* ------------------------------------------------------------------ */
bool MusicPlayer::PlayIndex(size_t index) {
    MusicTrack track;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= playlist_.size()) {
            ESP_LOGW(TAG, "play index %u out of range (%u tracks)", static_cast<unsigned>(index),
                     static_cast<unsigned>(playlist_.size()));
            return false;
        }
        track = playlist_[index];
        current_index_ = index;
    }
    return PlayTrack(track);
}

bool MusicPlayer::PlayById(const std::string& id) {
    MusicTrack track;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(playlist_.begin(), playlist_.end(),
                               [&id](const MusicTrack& t) { return t.id == id; });
        if (it == playlist_.end()) {
            ESP_LOGW(TAG, "no track with id '%s'", id.c_str());
            return false;
        }
        track = *it;
        current_index_ = static_cast<size_t>(std::distance(playlist_.begin(), it));
    }
    return PlayTrack(track);
}

bool MusicPlayer::PlayByKeyword(const std::string& keyword) {
    MusicTrack track;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(playlist_.begin(), playlist_.end(), [&keyword](const MusicTrack& t) {
            return t.title.find(keyword) != std::string::npos ||
                   t.artist.find(keyword) != std::string::npos || t.id == keyword;
        });
        if (it == playlist_.end()) {
            ESP_LOGW(TAG, "no track matching '%s'", keyword.c_str());
            return false;
        }
        track = *it;
        current_index_ = static_cast<size_t>(std::distance(playlist_.begin(), it));
    }
    return PlayTrack(track);
}

bool MusicPlayer::PlayTrack(const MusicTrack& track) {
    if (track.url.empty()) {
        ESP_LOGW(TAG, "track '%s' has no url", track.title.c_str());
        return false;
    }

    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle) {
        ESP_LOGW(TAG, "device busy (state=%d), music ignored", static_cast<int>(app.GetDeviceState()));
        return false;
    }

    /* 歌词转成 NotifyPlayer 的时间轴字幕，交给它做“到点回调” */
    std::vector<NotifySubtitle> subtitles;
    subtitles.reserve(track.lyrics.size());
    for (const auto& line : track.lyrics) {
        subtitles.push_back(NotifySubtitle{line.start_ms, line.text});
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = track;
        playing_ = true;
        lyric_index_ = -1;
    }

    if (!app.StartMusic(track.url, subtitles)) {
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = false;
        ESP_LOGE(TAG, "failed to start music '%s'", track.title.c_str());
        return false;
    }

    EnsurePage();
    ESP_LOGI(TAG, "playing '%s' - '%s'", track.title.c_str(), track.artist.c_str());
    return true;
}

void MusicPlayer::Stop() {
    bool was_playing;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        was_playing = playing_;
        playing_ = false;
        lyric_index_ = -1;
    }
    if (was_playing) {
        Application::GetInstance().StopMusic();
        ESP_LOGI(TAG, "stopped");
    }
    HidePage();
}

/* ------------------------------------------------------------------ */
/* 在线音乐（自建 music_server.py）                                    */
/* ------------------------------------------------------------------ */
void MusicPlayer::RememberSong(const std::string& id, const std::string& title,
                               const std::string& artist, uint32_t duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : song_cache_) {
        if (kv.first == id) {
            kv.second = SongMeta{title, artist, duration_ms};
            return;
        }
    }
    song_cache_.emplace_back(id, SongMeta{title, artist, duration_ms});
    if (song_cache_.size() > 20) {
        song_cache_.erase(song_cache_.begin());
    }
}

bool MusicPlayer::SearchOnline(const std::string& song, const std::string& artist, int offset,
                               std::string* out_json) {
    std::vector<MusicApiClient::SongItem> items;
    auto& api = MusicApiClient::GetInstance();
    if (!api.Search(song, artist, offset, &items, out_json)) {
        return false;
    }
    for (const auto& it : items) {
        /* 服务端 /search 不含时长，先用 0；播放时再补 */
        RememberSong(it.id, it.name, it.artist, 0);
    }
    return true;
}

bool MusicPlayer::PlayOnlineById(const std::string& song_id) {
    if (song_id.empty()) {
        return false;
    }
    auto& api = MusicApiClient::GetInstance();
    MusicTrack track;
    track.id = song_id;
    track.url = api.StreamUrlById(song_id);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& kv : song_cache_) {
            if (kv.first == song_id) {
                track.title = kv.second.title;
                track.artist = kv.second.artist;
                track.duration_ms = kv.second.duration_ms;
                break;
            }
        }
    }
    if (track.title.empty()) {
        track.title = "网络音乐";
    }
    std::vector<MusicLyricLine> lyrics;
    if (api.GetLyrics(song_id, &lyrics)) {
        track.lyrics = std::move(lyrics);
    }
    /* 服务端没给时长：用歌词最后一行 + 20s 估个大致值，仅用于进度条 */
    if (track.duration_ms == 0 && !track.lyrics.empty()) {
        track.duration_ms = track.lyrics.back().start_ms + 20000;
    }
    ESP_LOGI(TAG, "play online id=%s title='%s' lyrics=%u", song_id.c_str(), track.title.c_str(),
             static_cast<unsigned>(track.lyrics.size()));
    return PlayTrack(track);
}

bool MusicPlayer::PlayOnline(const std::string& song, const std::string& artist) {
    if (song.empty()) {
        return false;
    }
    std::vector<MusicApiClient::SongItem> items;
    auto& api = MusicApiClient::GetInstance();
    if (!api.Search(song, artist, 0, &items, nullptr)) {
        ESP_LOGW(TAG, "online search failed for '%s'", song.c_str());
        return false;
    }
    const MusicApiClient::SongItem* chosen = nullptr;
    for (const auto& it : items) {
        RememberSong(it.id, it.name, it.artist, 0);
        if (chosen == nullptr && it.playable) {
            chosen = &it;
        }
    }
    if (chosen == nullptr) {
        chosen = &items.front(); /* 都不可播放也试一次（服务端会再判） */
    }
    ESP_LOGI(TAG, "online play: '%s - %s' (id=%s, playable=%d)", chosen->name.c_str(),
             chosen->artist.c_str(), chosen->id.c_str(), chosen->playable);
    return PlayOnlineById(chosen->id);
}

bool MusicPlayer::Next() {
    size_t index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playlist_.empty()) {
            return false;
        }
        if (!playing_ || current_index_ == SIZE_MAX) {
            index = 0;                                   /* 未播放：从第一首开始 */
        } else {
            index = (current_index_ + 1) % playlist_.size();
        }
    }
    return PlayIndex(index);
}

bool MusicPlayer::Prev() {
    size_t index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playlist_.empty()) {
            return false;
        }
        if (!playing_ || current_index_ == SIZE_MAX) {
            index = 0;
        } else {
            index = (current_index_ + playlist_.size() - 1) % playlist_.size();
        }
    }
    return PlayIndex(index);
}

bool MusicPlayer::playing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_;
}

int MusicPlayer::display_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return display_mode_;
}

std::string MusicPlayer::CurrentTitle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_.title;
}

bool MusicPlayer::SetDisplayMode(int mode) {
    if (mode < 0 || mode > 2) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        display_mode_ = mode;
    }
    if (mode == 2) {
        HidePage();
    } else if (playing()) {
        EnsurePage();
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 音乐页                                                              */
/* ------------------------------------------------------------------ */
void MusicPlayer::EnsurePage() {
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }

    MusicTrack track;
    int mode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (display_mode_ == 2) {
            return;
        }
        track = current_;
        mode = display_mode_;
        if (!page_initialized_) {
            page_ = new MusicPage(display);
            page_initialized_ = true;
        }
    }

    auto* page = static_cast<MusicPage*>(page_);
    if (page == nullptr) {
        return;
    }
    page->Show(track.title.c_str(), track.artist.c_str());
    if (mode == 0) {
        page->SetLyric("", track.lyrics.empty() ? "" : "", "");
    }
    page->SetProgress(0, track.duration_ms);
}

void MusicPlayer::HidePage() {
    void* page = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        page = page_;
    }
    if (page != nullptr) {
        static_cast<MusicPage*>(page)->Hide();
    }
}

void MusicPlayer::UpdateLyricLocked(uint32_t position_ms, bool force) {
    if (current_.lyrics.empty()) {
        return;
    }
    int index = -1;
    for (size_t i = 0; i < current_.lyrics.size(); ++i) {
        if (current_.lyrics[i].start_ms <= position_ms) {
            index = static_cast<int>(i);
        } else {
            break;
        }
    }
    if (!force && index == lyric_index_) {
        return;
    }
    lyric_index_ = index;
    ESP_LOGI(TAG, "lyric[%d] @%lu ms: %s", index, static_cast<unsigned long>(position_ms),
             (index >= 0) ? current_.lyrics[index].text.c_str() : "(前奏)");

    auto* page = static_cast<MusicPage*>(page_);
    if (page == nullptr || display_mode_ == 2) {
        return;
    }
    if (display_mode_ == 1) {
        /* 封面模式只更新进度，不显示歌词 */
        page->SetProgress(position_ms, current_.duration_ms);
        return;
    }

    const char* prev = (index > 0) ? current_.lyrics[index - 1].text.c_str() : "";
    const char* cur = (index >= 0) ? current_.lyrics[index].text.c_str() : "";
    const char* next = (index >= 0 && static_cast<size_t>(index) + 1 < current_.lyrics.size())
                           ? current_.lyrics[index + 1].text.c_str()
                           : "";
    page->SetLyric(prev, cur, next);
    page->SetProgress(position_ms, current_.duration_ms);
}

/* ------------------------------------------------------------------ */
/* 引擎事件                                                            */
/* ------------------------------------------------------------------ */
void MusicPlayer::OnProgress(uint32_t playback_id, uint32_t media_position_ms) {
    (void)playback_id;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!playing_) {
        return;
    }
    UpdateLyricLocked(media_position_ms, false);
}

void MusicPlayer::OnFinished(uint32_t playback_id, bool success) {
    (void)playback_id;
    ESP_LOGI(TAG, "playback finished (%s)", success ? "ok" : "failed");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = false;
        lyric_index_ = -1;
    }
    /* 自动播下一首：列表里有多首才继续，单曲播完就收尾（避免无限循环） */
    size_t track_count;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        track_count = playlist_.size();
    }
    if (track_count > 1 && Next()) {
        return;
    }
    HidePage();
    /* 播完/失败都要把设备状态从 notifying 收回 idle（否则后续请求全被 device busy 拒绝） */
    Application::GetInstance().Schedule([]() {
        Application::GetInstance().StopMusic();
    });
}

void MusicPlayer::OnEngineStopped() {
    bool was_playing;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        was_playing = playing_;
        playing_ = false;
        lyric_index_ = -1;
    }
    if (was_playing) {
        ESP_LOGI(TAG, "engine stopped externally (e.g. wake word)");
        HidePage();
    }
}
