#include "asset_ota.h"

#include "board.h"
#include "settings.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

#include <cstring>
#include <vector>

#define TAG "AssetOta"

namespace {
constexpr const char* kStagingLabel = "ota_1";
constexpr size_t kChunk = 32 * 1024;
constexpr uint32_t kMagic = 0x41535354;  /* "ASST" */

struct StageHeader {
    uint32_t magic;
    uint32_t length;
    uint32_t checksum;   /* assets.bin 内部 checksum（sum(data)&0xFFFF） */
};

bool FindStaging(const esp_partition_t** out, const esp_partition_t** assets) {
    *out = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, kStagingLabel);
    *assets = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    return *out != nullptr && *assets != nullptr;
}

uint32_t AssetsChecksum(const uint8_t* buf, size_t len, uint32_t* data_len) {
    uint32_t files = 0, checksum = 0, dlen = 0;
    memcpy(&files, buf, 4);
    memcpy(&checksum, buf + 4, 4);
    memcpy(&dlen, buf + 8, 4);
    if (files == 0 || dlen == 0 || dlen + 12 > len) {
        return 0;
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < dlen; i++) {
        sum += buf[12 + i];
    }
    *data_len = dlen;
    return (sum & 0xFFFF) == (checksum & 0xFFFF) ? (checksum & 0xFFFF) : 0;
}
}  // namespace

namespace AssetOta {

void ApplyIfPending() {
    Settings provisional("assets", false);
    const int pending = provisional.GetInt("pending", 0);
    if (pending == 0) {
        return;
    }

    const esp_partition_t* staging = nullptr;
    const esp_partition_t* assets = nullptr;
    if (!FindStaging(&staging, &assets)) {
        ESP_LOGE(TAG, "partitions not found, clearing marker");
        Settings("assets", true).SetInt("pending", 0);
        return;
    }

    StageHeader hdr = {};
    const size_t hdr_off = staging->size - 16;
    if (esp_partition_read(staging, hdr_off, &hdr, sizeof(hdr)) != ESP_OK || hdr.magic != kMagic ||
        hdr.length < 12 || hdr.length > assets->size) {
        ESP_LOGE(TAG, "no valid staging blob (magic=%08x len=%u), clearing marker", (unsigned)hdr.magic,
                 (unsigned)hdr.length);
        Settings("assets", true).SetInt("pending", 0);
        return;
    }

    ESP_LOGW(TAG, "applying staged assets: %u bytes", (unsigned)hdr.length);
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(hdr.length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        ESP_LOGE(TAG, "no psram for staged assets");
        return;   /* 下次启动再试 */
    }
    if (esp_partition_read(staging, 0, buf, hdr.length) != ESP_OK) {
        ESP_LOGE(TAG, "read staging failed");
        heap_caps_free(buf);
        return;
    }
    uint32_t dlen = 0;
    const uint32_t ck = AssetsChecksum(buf, hdr.length, &dlen);
    if (ck == 0 || ck != hdr.checksum) {
        ESP_LOGE(TAG, "staged assets checksum invalid, aborting");
        heap_caps_free(buf);
        Settings("assets", true).SetInt("pending", 0);
        return;
    }

    /* 只擦 assets.bin 实际占用（4KB 对齐），保留分区尾部 1MB 用户图片区 */
    size_t erase_len = (static_cast<size_t>(hdr.length) + 0xFFF) & ~static_cast<size_t>(0xFFF);
    if (erase_len > assets->size) {
        erase_len = assets->size;
    }
    esp_err_t err = esp_partition_erase_range(assets, 0, erase_len);
    if (err == ESP_OK) {
        err = esp_partition_write(assets, 0, buf, hdr.length);
    }
    heap_caps_free(buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write assets failed: %s", esp_err_to_name(err));
        return;   /* 保留标记，下次启动重试 */
    }
    ESP_LOGW(TAG, "assets applied (%u bytes, %u files), rebooting", (unsigned)dlen, 0u);
    Settings("assets", true).SetInt("pending", 0);
    esp_restart();
}

bool StageFromUrl(const std::string& url) {
    const esp_partition_t* staging = nullptr;
    const esp_partition_t* assets = nullptr;
    if (!FindStaging(&staging, &assets)) {
        ESP_LOGE(TAG, "partitions not found");
        return false;
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http) {
        return false;
    }
    http->SetTimeout(60000);
    http->SetHeader("Accept", "application/octet-stream");
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "open failed: %d", http->GetLastError());
        return false;
    }
    const int len = http->GetBodyLength();
    if (len < 12 || static_cast<size_t>(len) > staging->size - sizeof(StageHeader) ||
        static_cast<size_t>(len) > assets->size) {
        ESP_LOGE(TAG, "bad length %d", len);
        http->Close();
        return false;
    }

    /* 分块边下边写：先写占位头，最后回写真实头 */
    const size_t hdr_off = staging->size - 16;
    if (esp_partition_erase_range(staging, 0, staging->size) != ESP_OK) {
        ESP_LOGE(TAG, "erase staging failed");
        http->Close();
        return false;
    }

    uint32_t written = 0;
    uint32_t sum = 0;
    uint32_t files = 0, stored_ck = 0, dlen = 0;
    bool header_ok = false;
    std::vector<uint8_t> tail;
    auto* chunk = static_cast<uint8_t*>(heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (chunk == nullptr) {
        http->Close();
        return false;
    }
    while (written < static_cast<uint32_t>(len)) {
        int want = static_cast<int>(std::min(kChunk, static_cast<size_t>(len - written)));
        int n = http->Read(reinterpret_cast<char*>(chunk), want);
        if (n <= 0) {
            break;
        }
        /* 头部校验（前 12 字节） */
        if (!header_ok) {
            tail.insert(tail.end(), chunk, chunk + n);
            if (tail.size() >= 12) {
                memcpy(&files, tail.data(), 4);
                memcpy(&stored_ck, tail.data() + 4, 4);
                memcpy(&dlen, tail.data() + 8, 4);
                if (files == 0 || dlen == 0 || dlen + 12 > static_cast<uint32_t>(len)) {
                    ESP_LOGE(TAG, "bad assets header");
                    break;
                }
                header_ok = true;
            }
        }
        if (esp_partition_write(staging, written, chunk, n) != ESP_OK) {
            ESP_LOGE(TAG, "staging write failed at %u", (unsigned)written);
            break;
        }
        /* 统计校验和（跳过前 12 字节头） */
        for (int i = 0; i < n; i++) {
            uint32_t pos = written + i;
            if (pos >= 12 && pos < 12 + dlen) {
                sum += chunk[i];
            }
        }
        written += n;
    }
    heap_caps_free(chunk);
    http->Close();

    if (written != static_cast<uint32_t>(len) || !header_ok || (sum & 0xFFFF) != (stored_ck & 0xFFFF)) {
        ESP_LOGE(TAG, "staging incomplete/invalid (%u/%d, ck calc=0x%04x stored=0x%04x)",
                 (unsigned)written, len, (unsigned)(sum & 0xFFFF), (unsigned)(stored_ck & 0xFFFF));
        Settings("assets", true).SetInt("pending", 0);
        return false;
    }

    StageHeader hdr = {.magic = kMagic, .length = written, .checksum = stored_ck & 0xFFFF};
    esp_err_t werr = esp_partition_write(staging, hdr_off, &hdr, sizeof(hdr));
    StageHeader check = {};
    esp_partition_read(staging, hdr_off, &check, sizeof(check));
    ESP_LOGW(TAG, "stage header write=%s readback magic=%08x len=%u", esp_err_to_name(werr),
             (unsigned)check.magic, (unsigned)check.length);
    if (werr != ESP_OK || check.magic != kMagic || check.length != written) {
        return false;
    }
    Settings("assets", true).SetInt("pending", 1);
    ESP_LOGW(TAG, "assets staged: %u bytes, %u files -> reboot to apply", (unsigned)written,
             (unsigned)files);
    return true;
}

}  // namespace AssetOta
