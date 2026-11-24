#include "standby_service.h"

#include "standby_page.h"

#include "music/music_api_client.h"
#include "music/music_player.h"
#include "settings.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cJSON.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

#define TAG "Standby"

namespace {
constexpr uint32_t kFetchIntervalS = 300;   /* 天气/行情刷新间隔（服务端也缓存 300s） */
constexpr const char* kWeekday[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

std::unique_ptr<StandbyPage> g_page;
uint32_t g_last_fetch_s = 0;

std::string FormatDate(const struct tm& t) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d月%d日 %s", t.tm_mon + 1, t.tm_mday, kWeekday[t.tm_wday % 7]);
    return buf;
}
}  // namespace

StandbyService& StandbyService::GetInstance() {
    static StandbyService instance;
    return instance;
}

void StandbyService::LoadConfig() {
    Settings settings("standby", false);
    enabled_ = settings.GetInt("enabled", 1) != 0;
    city_ = settings.GetString("city", "上海");
    symbols_ = settings.GetString("symbols", "usIXIC,usINX,sh000001");
    if (city_.empty()) {
        city_ = "上海";
    }
    if (symbols_.empty()) {
        symbols_ = "usIXIC,usINX,sh000001";
    }
}

void StandbyService::Initialize(Display* display) {
    if (started_ || display == nullptr) {
        return;
    }
    display_ = display;
    LoadConfig();
    g_page = std::make_unique<StandbyPage>(display);
    dirty_ = true;
    started_ = true;
    xTaskCreate(TaskEntry, "standby", 6144, this, 3, nullptr);
    ESP_LOGI(TAG, "standby service started (enabled=%d city=%s symbols=%s)", (int)enabled_.load(),
             city_.c_str(), symbols_.c_str());
}

void StandbyService::OnNetworkReady() {
    net_ready_ = true;
    dirty_ = true;
}

void StandbyService::SetIdle(bool idle) {
    if (idle_.exchange(idle) != idle) {
        dirty_ = true;
    }
}

void StandbyService::ReloadConfig() {
    LoadConfig();
    dirty_ = true;
    g_last_fetch_s = 0;   /* 立即重新拉一次 */
}

void StandbyService::SetEnabled(bool enabled) {
    Settings("standby", true).SetInt("enabled", enabled ? 1 : 0);
    enabled_ = enabled;
    dirty_ = true;
}

std::string StandbyService::city() const {
    return city_;
}

void StandbyService::SetCity(const std::string& city) {
    if (city.empty()) {
        return;
    }
    Settings("standby", true).SetString("city", city);
    city_ = city;
    dirty_ = true;
    g_last_fetch_s = 0;
}

std::string StandbyService::symbols() const {
    return symbols_;
}

void StandbyService::SetSymbols(const std::string& symbols) {
    Settings("standby", true).SetString("symbols", symbols);
    symbols_ = symbols;
    dirty_ = true;
    g_last_fetch_s = 0;
}

void StandbyService::TaskEntry(void* arg) {
    static_cast<StandbyService*>(arg)->Tick();
}

void StandbyService::Tick() {
    uint32_t last_clock_s = 0;
    char last_hhmm[16] = "";
    char last_date[48] = "";
    while (true) {
        const uint32_t now_s = static_cast<uint32_t>(esp_log_timestamp() / 1000);

        /* 只在"空闲 + 已启用 + 没在放音乐"时显示 */
        const bool want = enabled_.load() && idle_.load() && !MusicPlayer::GetInstance().playing();

        if (dirty_.exchange(false) || want != g_page->visible()) {
            ESP_LOGI(TAG, "standby want=%d (enabled=%d idle=%d playing=%d)", (int)want,
                     (int)enabled_.load(), (int)idle_.load(),
                     (int)MusicPlayer::GetInstance().playing());
            if (want) {
                g_page->Show();
                last_clock_s = 0;
                if (have_weather_) {
                    g_page->SetWeather(last_weather_);
                }
                if (!last_stocks_.empty()) {
                    g_page->SetStocks(last_stocks_);
                }
                if (g_last_fetch_s == 0) {
                    dirty_ = true;
                }
            } else {
                g_page->Hide();
            }
        }

        if (want) {
            /* 时钟：每秒刷新（只在文本变化时写 LVGL 也无妨，开销很小） */
            if (now_s != last_clock_s) {
                last_clock_s = now_s;
                time_t t = time(nullptr);
                struct tm tm_now = {};
                localtime_r(&t, &tm_now);
                char hhmm[16];
                const char* date_line = "校时中…";
                if (tm_now.tm_year >= 2025 - 1900) {
                    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
                    date_line = FormatDate(tm_now).c_str();
                } else {
                    snprintf(hhmm, sizeof(hhmm), "--:--");
                }
                /* 只在分钟变化时写 LVGL，避免每秒重绘 */
                if (strcmp(hhmm, last_hhmm) != 0 || strcmp(date_line, last_date) != 0) {
                    snprintf(last_hhmm, sizeof(last_hhmm), "%s", hhmm);
                    snprintf(last_date, sizeof(last_date), "%s", date_line);
                    g_page->SetClock(hhmm, date_line);
                }
            }

        }

        {
            /* 天气/行情：每 5 分钟（或配置变化后）拉一次；与页面是否可见无关 */
            const bool need_fetch = net_ready_.load() && (g_last_fetch_s == 0 ||
                                                          now_s - g_last_fetch_s >= kFetchIntervalS);
            if (need_fetch) {
                g_last_fetch_s = now_s;   /* 先记时间，失败也不至于每次 tick 都重试 */
                std::string json;
                if (MusicApiClient::GetInstance().Standby(city_, symbols_, &json)) {
                    cJSON* root = cJSON_Parse(json.c_str());
                    if (root != nullptr) {
                        StandbyPage::Weather weather;
                        const cJSON* w = cJSON_GetObjectItem(root, "weather");
                        if (cJSON_IsObject(w)) {
                            const cJSON* text = cJSON_GetObjectItem(w, "text");
                            const cJSON* temp = cJSON_GetObjectItem(w, "temp");
                            const cJSON* hum = cJSON_GetObjectItem(w, "humidity");
                            const cJSON* icon = cJSON_GetObjectItem(w, "icon");
                            weather.valid = true;
                            weather.text = cJSON_IsString(text) ? text->valuestring : "";
                            weather.temp = cJSON_IsNumber(temp) ? (float)temp->valuedouble : 0;
                            weather.humidity = cJSON_IsNumber(hum) ? hum->valueint : -1;
                            weather.icon = cJSON_IsNumber(icon) ? icon->valueint : 2;
                        }
                        last_weather_ = weather;
                        have_weather_ = true;
                        g_page->SetWeather(weather);

                        std::vector<StandbyPage::StockItem> stocks;
                        const cJSON* arr = cJSON_GetObjectItem(root, "stocks");
                        const int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
                        for (int i = 0; i < n && i < 3; i++) {
                            const cJSON* it = cJSON_GetArrayItem(arr, i);
                            StandbyPage::StockItem s;
                            const cJSON* short_name = cJSON_GetObjectItem(it, "short");
                            const cJSON* name = cJSON_GetObjectItem(it, "name");
                            const cJSON* price = cJSON_GetObjectItem(it, "price");
                            const cJSON* pct = cJSON_GetObjectItem(it, "change_pct");
                            if (cJSON_IsString(short_name) && short_name->valuestring[0]) {
                                s.name = short_name->valuestring;
                            } else {
                                s.name = cJSON_IsString(name) ? name->valuestring : "";
                            }
                            s.price = cJSON_IsNumber(price) ? (float)price->valuedouble : 0;
                            s.change_pct = cJSON_IsNumber(pct) ? (float)pct->valuedouble : 0;
                            s.valid = !s.name.empty();
                            stocks.push_back(s);
                        }
                        last_stocks_ = stocks;
                        g_page->SetStocks(stocks);
                        ESP_LOGI(TAG, "standby updated: weather=%s temp=%.0f stocks=%d",
                                 weather.text.c_str(), weather.temp, (int)stocks.size());
                        cJSON_Delete(root);
                    }
                } else {
                    ESP_LOGW(TAG, "standby fetch failed");
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
