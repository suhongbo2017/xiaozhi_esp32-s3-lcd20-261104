#include "music_api_client.h"

#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <psa/crypto.h>

#include <cJSON.h>

#include <cstdio>
#include <cstring>

#define TAG "MusicApi"

namespace {
constexpr const char* kDefaultBaseUrl = CONFIG_MUSIC_API_URL;
constexpr const char* kDefaultSecret = CONFIG_MUSIC_API_SECRET;

std::string UrlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::string Sha256Hex32(const std::string& text) {
    /* mbedTLS 4.x 移除了 legacy API，改用 PSA */
    static bool psa_ready = false;
    if (!psa_ready) {
        psa_crypto_init();
        psa_ready = true;
    }
    unsigned char digest[32];
    size_t out_len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t*>(text.data()), text.size(),
                         digest, sizeof(digest), &out_len) != PSA_SUCCESS || out_len != 32) {
        return std::string();
    }
    char hex[65];
    for (int i = 0; i < 32; ++i) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    hex[64] = '\0';
    return std::string(hex, 32); /* 服务端只比较前 32 个字符 */
}
}  // namespace

MusicApiClient& MusicApiClient::GetInstance() {
    static MusicApiClient instance;
    return instance;
}

MusicApiClient::MusicApiClient() {
    /* 优先级：NVS("music") > Kconfig 默认值 */
    Settings settings("music", false);
    base_url_ = settings.GetString("api_url", kDefaultBaseUrl);
    secret_ = settings.GetString("api_secret", kDefaultSecret);
    chip_id_ = "ESP32";
    mac_ = SystemInfo::GetMacAddress();

    while (!base_url_.empty() && base_url_.back() == '/') {
        base_url_.pop_back();
    }
    ESP_LOGI(TAG, "music API: %s (mac=%s)", base_url_.c_str(), mac_.c_str());
}

int MusicApiClient::HttpGet(const std::string& path_and_query, std::string* body, int timeout_ms) {
    if (base_url_.empty()) {
        return -1;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return -1;
    }
    auto http = network->CreateHttp(1);
    if (!http) {
        return -1;
    }

    const int64_t uptime_s = esp_timer_get_time() / 1000000;
    char ts[24];
    snprintf(ts, sizeof(ts), "%lld", static_cast<long long>(uptime_s));
    const std::string key = Sha256Hex32(mac_ + ":" + chip_id_ + ":" + ts + ":" + secret_);

    http->SetTimeout(timeout_ms);
    http->SetHeader("X-MAC-Address", mac_);
    http->SetHeader("X-Chip-ID", chip_id_);
    http->SetHeader("X-Timestamp", ts);
    http->SetHeader("X-Key", key);
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "application/json");

    const std::string url = base_url_ + path_and_query;
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "open failed: %s (err=%d)", url.c_str(), http->GetLastError());
        return -1;
    }
    const int status = http->GetStatusCode();
    if (status == 200 && body != nullptr) {
        *body = http->ReadAll();
    }
    http->Close();
    return status;
}

bool MusicApiClient::Search(const std::string& song, const std::string& artist, int offset,
                            std::vector<SongItem>* out, std::string* raw_json) {
    if (!out) {
        return false;
    }
    out->clear();

    std::string path = "/search?song=" + UrlEncode(song);
    if (!artist.empty()) {
        path += "&artist=" + UrlEncode(artist);
    }
    path += "&offset=" + std::to_string(offset < 0 ? 0 : offset) + "&limit=5";

    std::string body;
    int status = HttpGet(path, &body, 15000);
    if (status != 200) {
        ESP_LOGW(TAG, "search failed: http %d", status);
        return false;
    }
    if (raw_json) {
        *raw_json = body;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }
    cJSON* items = cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(items)) {
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, items) {
            SongItem item;
            item.index = cJSON_GetObjectItem(it, "index") ? cJSON_GetObjectItem(it, "index")->valueint : 0;
            auto id = cJSON_GetObjectItem(it, "id");
            auto name = cJSON_GetObjectItem(it, "name");
            auto ar = cJSON_GetObjectItem(it, "artist");
            auto album = cJSON_GetObjectItem(it, "album");
            auto playable = cJSON_GetObjectItem(it, "playable");
            item.id = cJSON_IsString(id) ? id->valuestring : "";
            item.name = cJSON_IsString(name) ? name->valuestring : "";
            item.artist = cJSON_IsString(ar) ? ar->valuestring : "";
            item.album = cJSON_IsString(album) ? album->valuestring : "";
            item.playable = cJSON_IsTrue(playable);
            out->push_back(std::move(item));
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "search '%s' -> %u item(s)", song.c_str(), static_cast<unsigned>(out->size()));
    return !out->empty();
}

bool MusicApiClient::GetLyrics(const std::string& song_id, std::vector<MusicLyricLine>* out) {
    if (!out) {
        return false;
    }
    out->clear();
    std::string body;
    int status = HttpGet("/lyric_json/" + UrlEncode(song_id), &body, 12000);
    if (status != 200) {
        ESP_LOGW(TAG, "lyric failed: http %d", status);
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }
    cJSON* lines = cJSON_GetObjectItem(root, "lines");
    if (cJSON_IsArray(lines)) {
        cJSON* ln = nullptr;
        cJSON_ArrayForEach(ln, lines) {
            auto t = cJSON_GetObjectItem(ln, "start_ms");
            auto text = cJSON_GetObjectItem(ln, "text");
            if (!cJSON_IsNumber(t) || !cJSON_IsString(text)) {
                continue;
            }
            MusicLyricLine line;
            line.start_ms = static_cast<uint32_t>(t->valuedouble);
            line.text = text->valuestring;
            out->push_back(std::move(line));
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "lyrics id=%s -> %u line(s)", song_id.c_str(), static_cast<unsigned>(out->size()));
    return !out->empty();
}

std::string MusicApiClient::SignedQuery() const {
    const int64_t uptime_s = esp_timer_get_time() / 1000000;
    char ts[24];
    snprintf(ts, sizeof(ts), "%lld", static_cast<long long>(uptime_s));
    const std::string key = Sha256Hex32(mac_ + ":" + chip_id_ + ":" + ts + ":" + secret_);
    return "&mac=" + UrlEncode(mac_) + "&chip=" + UrlEncode(chip_id_) + "&ts=" + ts + "&key=" + key;
}

std::string MusicApiClient::StreamUrlById(const std::string& song_id) const {
    if (base_url_.empty()) {
        return std::string();
    }
    /* NotifyPlayer 拉流时不会带自定义 header，所以把鉴权放到 query 里 */
    return base_url_ + "/stream_ogg?id=" + UrlEncode(song_id) + SignedQuery();
}

std::string MusicApiClient::StreamUrlByName(const std::string& song, const std::string& artist) const {
    if (base_url_.empty()) {
        return std::string();
    }
    std::string url = base_url_ + "/stream_ogg?song=" + UrlEncode(song);
    if (!artist.empty()) {
        url += "&artist=" + UrlEncode(artist);
    }
    return url + SignedQuery();
}

bool MusicApiClient::StockQuote(const std::string& code, std::string* out_json) {
    std::string body;
    int status = HttpGet("/stock/quote?code=" + UrlEncode(code), &body, 12000);
    if (status != 200) {
        ESP_LOGW(TAG, "stock quote failed: http %d", status);
        return false;
    }
    if (out_json) {
        *out_json = body;
    }
    return true;
}

bool MusicApiClient::StockSearch(const std::string& keyword, std::string* out_json) {
    std::string body;
    int status = HttpGet("/stock/search?q=" + UrlEncode(keyword), &body, 12000);
    if (status != 200) {
        ESP_LOGW(TAG, "stock search failed: http %d", status);
        return false;
    }
    if (out_json) {
        *out_json = body;
    }
    return true;
}
