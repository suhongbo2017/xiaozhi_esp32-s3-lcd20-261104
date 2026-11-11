#include "mp3_stream_player.h"

#include "audio_service.h"
#include "board.h"
#include "http.h"
#include "protocol.h"

#include "minimp3/minimp3.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <memory>

#define TAG "Mp3Stream"

namespace {
constexpr size_t kReadChunk = 4096;
constexpr uint32_t kTaskStack = 8192;
constexpr UBaseType_t kTaskPrio = 3;
constexpr int kHttpTimeoutMs = 20000;
}  // namespace

Mp3StreamPlayer::Mp3StreamPlayer(AudioService& audio_service) : audio_service_(audio_service) {}
Mp3StreamPlayer::~Mp3StreamPlayer() { Stop(); }

bool Mp3StreamPlayer::Start(const std::string& url, uint32_t playback_id, FinishedCallback finished) {
    if (url.empty() || running_) {
        return false;
    }
    url_ = url;
    playback_id_ = playback_id;
    position_ms_ = 0;
    finished_ = std::move(finished);
    cancel_ = false;
    running_ = true;
    if (xTaskCreate(TaskEntry, "mp3_stream", kTaskStack, this, kTaskPrio, (TaskHandle_t*)&task_) != pdPASS) {
        running_ = false;
        ESP_LOGE(TAG, "failed to create mp3 task");
        return false;
    }
    ESP_LOGI(TAG, "mp3 start: %s", url_.c_str());
    return true;
}

void Mp3StreamPlayer::Stop() {
    if (!running_ && !task_alive_) {
        return;
    }
    cancel_ = true;
    /* 等任务自行退出（最多 3s），避免任务里还持有 HTTP 指针 */
    for (int i = 0; i < 30 && task_alive_; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    running_ = false;
    ESP_LOGI(TAG, "mp3 stopped");
}

void Mp3StreamPlayer::TaskEntry(void* arg) {
    auto* self = static_cast<Mp3StreamPlayer*>(arg);
    self->task_alive_ = true;
    self->Task();
    self->task_alive_ = false;
    self->running_ = false;
    vTaskDelete(nullptr);
}

void Mp3StreamPlayer::Task() {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(2);
    if (!http) {
        ESP_LOGE(TAG, "no http client");
        if (finished_) finished_(false);
        return;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "audio/mpeg, audio/mp3, */*");
    http->SetHeader("Accept-Encoding", "identity");
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    if (!http->Open("GET", url_)) {
        ESP_LOGE(TAG, "open failed: %d", http->GetLastError());
        if (finished_) finished_(false);
        return;
    }

    mp3dec_t dec;
    mp3dec_init(&dec);
    std::vector<uint8_t> buf;
    buf.reserve(kReadChunk * 2);
    std::vector<int16_t> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
    bool any_frame = false;
    bool ok = true;
    int idle_reads = 0;

    while (!cancel_) {
        uint8_t chunk[kReadChunk];
        int n = http->Read((char*)chunk, sizeof(chunk));
        if (n < 0) {
            ESP_LOGW(TAG, "read error: %d", http->GetLastError());
            ok = any_frame;   /* 已播过一部分就算正常结束（长连接断开） */
            break;
        }
        if (n == 0) {
            if (++idle_reads > 2) {
                break;        /* 流结束 */
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        idle_reads = 0;
        buf.insert(buf.end(), chunk, chunk + n);

        size_t off = 0;
        while (off + 4 <= buf.size()) {
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(&dec, buf.data() + off, (int)(buf.size() - off), pcm.data(), &info);
            if (info.frame_bytes == 0) {
                break;        /* 数据不够，继续读 */
            }
            off += info.frame_bytes;
            if (samples <= 0) {
                continue;
            }
            any_frame = true;

            /* 单声道化 */
            int channels = info.channels > 0 ? info.channels : 1;
            std::vector<int16_t> mono;
            if (channels == 2) {
                mono.resize(samples);
                for (int i = 0; i < samples; ++i) {
                    int32_t l = pcm[i * 2];
                    int32_t r = pcm[i * 2 + 1];
                    mono[i] = static_cast<int16_t>((l + r) / 2);
                }
            } else {
                mono.assign(pcm.begin(), pcm.begin() + samples);
            }

            /* 打包成 raw_pcm 包（sample_rate = 解码后的源采样率，交给 AudioService 重采样） */
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = info.hz;
            packet->frame_duration = info.hz > 0 ? samples * 1000 / info.hz : 20;
            packet->playback_id = playback_id_;
            packet->media_position_ms = position_ms_;
            packet->raw_pcm = true;
            packet->payload.resize(mono.size() * sizeof(int16_t));
            memcpy(packet->payload.data(), mono.data(), packet->payload.size());

            position_ms_ += packet->frame_duration;
            if (!audio_service_.PushPacketToDecodeQueue(std::move(packet), true)) {
                ESP_LOGW(TAG, "decode queue full, stop");
                cancel_ = true;
                break;
            }
        }
        if (off > 0) {
            buf.erase(buf.begin(), buf.begin() + off);
        }
        if (buf.size() > kReadChunk * 4) {
            ESP_LOGW(TAG, "buffer overflow without frame sync, drop");
            buf.clear();
            mp3dec_init(&dec);
        }
    }

    http->Close();
    ESP_LOGI(TAG, "mp3 finished: ok=%d frames=%d pos=%lums", ok, any_frame, (unsigned long)position_ms_);
    if (finished_) {
        finished_(ok && any_frame);
    }
}
