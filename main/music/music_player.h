#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

/** 一行歌词 */
struct MusicLyricLine {
    uint32_t start_ms = 0;
    std::string text;
};

/** 一首曲目 */
struct MusicTrack {
    std::string id;
    std::string title;
    std::string artist;
    std::string url;        /* 音频流地址（OGG/Opus，复用 NotifyPlayer 管线） */
    std::string cover_url;  /* 预留：封面图地址 */
    uint32_t duration_ms = 0;
    std::vector<MusicLyricLine> lyrics;
};

/**
 * MusicPlayer — 音乐闭环的业务层。
 *
 * 播放引擎复用现有 NotifyPlayer（HTTP 流 + Opus 解码 + 时间轴字幕），
 * 因此本类只负责：
 *   - 曲库（playlist）与选曲
 *   - 歌词时间轴 → 音乐页（prev/current/next 三行）
 *   - 4 个 MCP 工具（与旧固件同名，便于替换后行为一致）
 *
 * 生命周期（state、音频处理开关）仍由 Application 负责：
 *   MusicPlayer::Play* → Application::StartMusic(...)
 *   引擎事件（进度/结束/被打断）→ MusicPlayer::OnProgress/OnFinished/OnEngineStopped
 */
class MusicPlayer {
public:
    static MusicPlayer& GetInstance();

    /** 注册 MCP 工具；可重复调用（内部只注册一次） */
    void Initialize();

    /* ---- 曲库 ---- */
    void SetPlaylist(std::vector<MusicTrack> tracks);
    void AddTrack(const MusicTrack& track);
    std::vector<MusicTrack> GetPlaylist() const;

    /* ---- 播放控制 ---- */
    bool PlayIndex(size_t index);
    bool PlayById(const std::string& id);
    bool PlayByKeyword(const std::string& keyword);
    /** 直接播放一首（可临时覆盖歌词/标题，用于 URL 直推） */
    bool PlayTrack(const MusicTrack& track);
    void Stop();
    bool Next();

    /* ---- 状态 ---- */
    bool playing() const;
    int display_mode() const;          /* 0=lyric 1=cover 2=hidden */
    bool SetDisplayMode(int mode);
    std::string CurrentTitle() const;

    /* ---- 引擎回调（由 Application 调用） ---- */
    void OnProgress(uint32_t playback_id, uint32_t media_position_ms);
    void OnFinished(uint32_t playback_id, bool success);
    void OnEngineStopped();

private:
    MusicPlayer() = default;

    void EnsurePage();
    void UpdateLyricLocked(uint32_t position_ms, bool force);
    void HidePage();

    mutable std::mutex mutex_;
    std::vector<MusicTrack> playlist_;
    MusicTrack current_;
    size_t current_index_ = SIZE_MAX;
    uint32_t playback_id_ = 0;
    bool playing_ = false;
    int display_mode_ = 0;
    int lyric_index_ = -1;
    bool page_initialized_ = false;
    bool tools_registered_ = false;
    void* page_ = nullptr;   /* std::unique_ptr<MusicPage>，避免头文件互相包含 */
};

#endif  // MUSIC_PLAYER_H
