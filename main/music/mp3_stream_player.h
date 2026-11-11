#ifndef MP3_STREAM_PLAYER_H
#define MP3_STREAM_PLAYER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class AudioService;

/**
 * Mp3StreamPlayer —— 设备端 MP3 直放（架构重做）
 *
 * 设计要点（替代"服务端 ffmpeg 转 OGG/Opus + NotifyPlayer"那条链路）：
 *   1. HTTP 拉 MP3 流（Range/分块读），不做整曲下载
 *   2. minimp3 逐帧解码 → int16 PCM
 *   3. 单声道化 + 重采样到 codec 输出采样率
 *   4. 以 raw_pcm 包塞进 AudioService 的播放队列（跳过 Opus 解码）
 *
 * 好处：服务端不再需要 ffmpeg 转码（原来首播要 20~180s，还会超时 503），
 *       任何能给出完整 MP3 直链的源都能"边下边播"，且支持任意时长。
 */
class Mp3StreamPlayer {
public:
    using FinishedCallback = std::function<void(bool success)>;

    Mp3StreamPlayer(AudioService& audio_service);
    ~Mp3StreamPlayer();

    /** 开始播放；url 必须是 http(s) 的 MP3 直链 */
    bool Start(const std::string& url, uint32_t playback_id, FinishedCallback finished);

    void Stop();

    bool active() const { return running_; }
    uint32_t position_ms() const { return position_ms_; }

private:
    static void TaskEntry(void* arg);
    void Task();

    AudioService& audio_service_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
    volatile bool task_alive_ = false;
    void* task_ = nullptr;
    std::string url_;
    uint32_t playback_id_ = 0;
    uint32_t position_ms_ = 0;
    FinishedCallback finished_;
};

#endif  // MP3_STREAM_PLAYER_H
