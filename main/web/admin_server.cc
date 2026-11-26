#include "admin_server.h"

#include "audio_codec.h"
#include "application.h"
#include "device/gpio_control.h"
#include "mcp_server.h"
#include "asset_ota.h"
#include "ui/standby_service.h"
#include "ui/user_image.h"
#include "board.h"
#include "display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "music/music_api_client.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <driver/gpio.h>
#include <led_strip.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstring>
#include <string>

#define TAG "AdminServer"

namespace {

httpd_handle_t g_server = nullptr;

/* 网页内容已拆到 main/web/ui/{index.html,style.css,app.js}，由 CMake EMBED_FILES 打包 */

const char* LedModeToString(int mode) {
    switch (mode) {
        case 1: return "off";
        case 2: return "color";
        default: return "on";
    }
}

int HexToRgb(const std::string& hex, uint8_t* r, uint8_t* g, uint8_t* b) {
    if (hex.size() != 7 || hex[0] != '#') {
        return 0;
    }
    unsigned int v = 0;
    if (sscanf(hex.c_str() + 1, "%06x", &v) != 1) {
        return 0;
    }
    *r = (v >> 16) & 0xFF;
    *g = (v >> 8) & 0xFF;
    *b = v & 0xFF;
    return 1;
}

std::string BuildStatusJson() {
    auto& board = Board::GetInstance();
    Settings ui("ui", false);
    Settings music("music", false);
    Settings led_settings("led", false);

    auto* display = board.GetDisplay();
    std::string theme = "dark";
    if (display != nullptr && display->GetTheme() != nullptr) {
        theme = display->GetTheme()->name();
    }
    int brightness = 80;
    if (auto* bl = board.GetBacklight()) {
        brightness = bl->brightness();
    }
    int volume = 70;
    if (auto* codec = board.GetAudioCodec()) {
        volume = codec->output_volume();
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "board", BOARD_NAME);
    cJSON_AddStringToObject(root, "version", SystemInfo::GetUserAgent().c_str());
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "theme", theme.c_str());
    cJSON_AddNumberToObject(root, "brightness", brightness);
    cJSON_AddNumberToObject(root, "volume", volume);
    cJSON_AddNumberToObject(root, "duck", music.GetInt("duck_volume_percent", CONFIG_MUSIC_DUCK_VOLUME_PERCENT));
    cJSON_AddStringToObject(root, "api", music.GetString("api_url", CONFIG_MUSIC_API_URL).c_str());
    /* 状态灯：以 Led 对象为准（NVS 只是持久化副本） */
    const char* led_mode_str = "on";
    char led_color_hex[8] = "#00b3ff";
    if (auto* led = board.GetLed()) {
        switch (led->GetMode()) {
            case Led::Mode::Off:
                led_mode_str = "off";
                break;
            case Led::Mode::Fixed:
                led_mode_str = "color";
                break;
            default:
                led_mode_str = "on";
                break;
        }
        uint8_t lr = 0, lg = 0, lb = 255;
        led->GetFixedColor(&lr, &lg, &lb);
        snprintf(led_color_hex, sizeof(led_color_hex), "#%02X%02X%02X", lr, lg, lb);
    }
    cJSON_AddNumberToObject(root, "chat_style", Settings("ui", false).GetInt("chat_style", 0));
    cJSON_AddStringToObject(root, "led_mode", led_mode_str);
    cJSON_AddStringToObject(root, "led_color", led_color_hex);
    auto& app = Application::GetInstance();
    cJSON_AddNumberToObject(root, "wake_enabled", app.WakeWordEnabled() ? 1 : 0);
    cJSON_AddNumberToObject(root, "wake_threshold", (int)(app.WakeWordThreshold() * 100 + 0.5f));
    std::string wake_words;
    for (auto& w : app.GetWakeWords()) {
        if (!wake_words.empty()) {
            wake_words += " / ";
        }
        wake_words += w;
    }
    cJSON_AddStringToObject(root, "wake_words", wake_words.c_str());

    auto& standby = StandbyService::GetInstance();
    cJSON_AddNumberToObject(root, "standby", standby.enabled() ? 1 : 0);
    cJSON_AddStringToObject(root, "standby_city", standby.city().c_str());
    cJSON_AddStringToObject(root, "standby_symbols", standby.symbols().c_str());
    char* out = cJSON_PrintUnformatted(root);
    std::string json = out ? out : "{}";
    cJSON_free(out);
    cJSON_Delete(root);
    return json;
}


/* ---------------- 外观资源包（字体/表情）安全热更新 ----------------
 * 不在运行中直接改写 assets（会与 LVGL 读取冲突导致崩溃/变砖），
 * 改为：下载到 ota_1 暂存 -> 重启 -> 启动早期由 AssetOta::ApplyIfPending() 写入。
 */

esp_err_t HandleAssetsApply(httpd_req_t* req) {
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"empty\"}");
        return ESP_FAIL;
    }
    buf[len] = 0;
    cJSON* root = cJSON_Parse(buf);
    if (root == nullptr) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad json\"}");
        return ESP_FAIL;
    }
    auto* url_item = cJSON_GetObjectItem(root, "url");
    std::string url = cJSON_IsString(url_item) ? url_item->valuestring : "";
    cJSON_Delete(root);
    if (url.size() < 10) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad url\"}");
        return ESP_FAIL;
    }
    if (!AssetOta::StageFromUrl(url)) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"stage_failed\"}");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    /* 让响应先发出去，再重启 */
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}


/* ---------------- 用户图片（开机图 / 待机背景） ----------------
 * GET  /api/image?kind=boot|bg        回读原始 JPEG（网页预览用）
 * POST /api/image?kind=boot|bg        直接上传 JPEG 字节（网页用 canvas 缩放到 240x320）
 * POST /api/image?kind=boot|bg&clear=1 清除
 */
/* 调试用：往聊天区塞几条示例消息，方便在不动嘴的情况下检查气泡样式 */
esp_err_t HandleChatTest(httpd_req_t* req) {
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no display\"}");
        return ESP_FAIL;
    }
    display->SetChatMessage("system", "样式预览 · 微信风格气泡");
    display->SetChatMessage("user", "今天天气怎么样？帮我看看纳指涨了没有");
    display->SetChatMessage("assistant", "上海今天多云，12 度，湿度 54%。纳指 24089，涨 6.51%，标普 7237，涨 4.73%。");
    display->SetChatMessage("user", "好的谢谢");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t HandleReboot(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

esp_err_t HandleImageGet(httpd_req_t* req) {
    char query[64] = {0};
    if (httpd_req_get_url_query_len(req) > 0) {
        httpd_req_get_url_query_str(req, query, sizeof(query));
    }
    char kind_str[16] = {0};
    if (httpd_query_key_value(query, "kind", kind_str, sizeof(kind_str)) != ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no kind\"}");
        return ESP_FAIL;
    }
    auto kind = strcmp(kind_str, "bg") == 0     ? UserImage::kBackground
                : strcmp(kind_str, "chat") == 0 ? UserImage::kChatBackground
                                                : UserImage::kBoot;
    auto& img = UserImage::GetInstance();
    const uint32_t len = img.length(kind);
    if (len == 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        buf = static_cast<uint8_t*>(malloc(len));
    }
    if (buf == nullptr) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    const size_t got = img.Read(kind, buf, len);
    if (got == 0) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, reinterpret_cast<const char*>(buf), got);
    free(buf);
    return err;
}

esp_err_t HandleImagePost(httpd_req_t* req) {
    char query[128] = {0};
    if (httpd_req_get_url_query_len(req) > 0) {
        httpd_req_get_url_query_str(req, query, sizeof(query));
    }
    char kind_str[16] = {0};
    char clear_str[8] = {0};
    if (httpd_query_key_value(query, "kind", kind_str, sizeof(kind_str)) != ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no kind\"}");
        return ESP_FAIL;
    }
    auto kind = strcmp(kind_str, "bg") == 0     ? UserImage::kBackground
                : strcmp(kind_str, "chat") == 0 ? UserImage::kChatBackground
                                                : UserImage::kBoot;
    auto& img = UserImage::GetInstance();

    if (httpd_query_key_value(query, "clear", clear_str, sizeof(clear_str)) == ESP_OK &&
        clear_str[0] == '1') {
        img.Clear(kind);
        if (kind == UserImage::kBackground) {
            StandbyService::GetInstance().ReloadBackground();
        } else if (kind == UserImage::kChatBackground) {
            if (auto* d = Board::GetInstance().GetDisplay()) {
                d->ApplyChatStyle();
            }
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"cleared\":true}");
        return ESP_OK;
    }

    const int total = req->content_len;
    if (total < 64 || total > 400 * 1024) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad size\"}");
        return ESP_FAIL;
    }
    if (!img.BeginWrite(kind)) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"erase failed\"}");
        return ESP_FAIL;
    }
    auto* chunk = static_cast<uint8_t*>(heap_caps_malloc(8192, MALLOC_CAP_SPIRAM));
    if (chunk == nullptr) {
        chunk = static_cast<uint8_t*>(malloc(8192));
    }
    if (chunk == nullptr) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int received = 0;
    bool ok = true;
    while (received < total) {
        int want = total - received;
        if (want > 8192) {
            want = 8192;
        }
        int n = httpd_req_recv(req, reinterpret_cast<char*>(chunk), want);
        if (n <= 0) {
            ok = false;
            break;
        }
        if (!img.WriteChunk(chunk, n)) {
            ok = false;
            break;
        }
        received += n;
    }
    free(chunk);

    if (!ok || !img.EndWrite(0, 0)) {
        img.Clear(kind);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"image_too_large_or_invalid\"}");
        return ESP_FAIL;
    }
    if (kind == UserImage::kBackground) {
        StandbyService::GetInstance().ReloadBackground();
    } else if (kind == UserImage::kChatBackground) {
        if (auto* d = Board::GetInstance().GetDisplay()) {
            d->ApplyChatStyle();
        }
    }
    ESP_LOGI(TAG, "image uploaded: kind=%s %d bytes", kind_str, received);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}


/* ---------------- 用户自定义 GPIO 开关 ----------------
 * GET  /api/gpio  -> {"devices":[{name,gpio,active_high,initial_on,on}], "reserved":[...]}
 * POST /api/gpio  {"devices":[{name,gpio,active_high,initial_on}]}  保存配置
 * POST /api/gpio  {"name":"打印机灯光","state":"on|off|toggle"}      即时控制
 */
/* 诊断：当前注册给大模型的 MCP 工具列表 */
esp_err_t HandleMcpTools(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(root, "tools");
    for (const auto& name : McpServer::GetInstance().GetToolNames()) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(name.c_str()));
    }
    cJSON_AddNumberToObject(root, "count", cJSON_GetArraySize(arr));
    char* out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out != nullptr ? out : "{}");
    cJSON_free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

/* 诊断：直接用 led_strip 驱动指定 GPIO 上的 WS2812，绕过 SingleLed
 * POST /api/led/probe?pin=48&r=64&g=0&b=0   （默认绿色）
 */
esp_err_t HandleLedProbe(httpd_req_t* req) {
    char query[128] = {0};
    if (httpd_req_get_url_query_len(req) > 0) {
        httpd_req_get_url_query_str(req, query, sizeof(query));
    }
    auto get_int = [&query](const char* key, int def) {
        char val[16] = {0};
        if (httpd_query_key_value(query, key, val, sizeof(val)) != ESP_OK) {
            return def;
        }
        return atoi(val);
    };
    const int pin = get_int("pin", 48);
    const int r = get_int("r", 0), g = get_int("g", 64), b = get_int("b", 0);

    led_strip_config_t cfg = {};
    cfg.strip_gpio_num = static_cast<gpio_num_t>(pin);
    cfg.max_leds = 1;
    cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    cfg.led_model = LED_MODEL_WS2812;
    led_strip_rmt_config_t rmt = {};
    rmt.resolution_hz = 10 * 1000 * 1000;
    led_strip_handle_t strip = nullptr;
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &strip);
    esp_err_t set_err = ESP_FAIL, ref_err = ESP_FAIL;
    if (err == ESP_OK && strip != nullptr) {
        set_err = led_strip_set_pixel(strip, 0, r, g, b);
        ref_err = led_strip_refresh(strip);
    }
    ESP_LOGW(TAG, "led probe GPIO%d rgb=(%d,%d,%d) new=%s set=%s refresh=%s", pin, r, g, b,
             esp_err_to_name(err), esp_err_to_name(set_err), esp_err_to_name(ref_err));
    if (strip != nullptr) {
        led_strip_del(strip);
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK && ref_err == ESP_OK);
    cJSON_AddNumberToObject(root, "pin", pin);
    cJSON_AddStringToObject(root, "new", esp_err_to_name(err));
    cJSON_AddStringToObject(root, "set", esp_err_to_name(set_err));
    cJSON_AddStringToObject(root, "refresh", esp_err_to_name(ref_err));
    char* out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out != nullptr ? out : "{}");
    cJSON_free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

/* 诊断：在指定 GPIO 上闪 N 次（每次 120ms），用于"数闪几下"定位真实 LED 引脚
 * POST /api/led/blink?pin=48&count=3
 */
esp_err_t HandleLedBlink(httpd_req_t* req) {
    char query[128] = {0};
    if (httpd_req_get_url_query_len(req) > 0) {
        httpd_req_get_url_query_str(req, query, sizeof(query));
    }
    auto get_int = [&query](const char* key, int def) {
        char val[16] = {0};
        if (httpd_query_key_value(query, key, val, sizeof(val)) != ESP_OK) {
            return def;
        }
        return atoi(val);
    };
    const int pin = get_int("pin", 48);
    const int count = get_int("count", 1);

    led_strip_config_t cfg = {};
    cfg.strip_gpio_num = static_cast<gpio_num_t>(pin);
    cfg.max_leds = 1;
    cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    cfg.led_model = LED_MODEL_WS2812;
    led_strip_rmt_config_t rmt = {};
    rmt.resolution_hz = 10 * 1000 * 1000;
    led_strip_handle_t strip = nullptr;
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &strip);
    int done = 0;
    if (err == ESP_OK && strip != nullptr) {
        for (int i = 0; i < count; i++) {
            led_strip_set_pixel(strip, 0, 96, 96, 96);   /* 白色，够亮 */
            led_strip_refresh(strip);
            vTaskDelay(pdMS_TO_TICKS(160));
            led_strip_clear(strip);
            vTaskDelay(pdMS_TO_TICKS(160));
            done++;
        }
        led_strip_del(strip);
    }
    ESP_LOGW(TAG, "led blink GPIO%d x%d (err=%s)", pin, done, esp_err_to_name(err));
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(root, "pin", pin);
    cJSON_AddNumberToObject(root, "blinks", done);
    char* out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out != nullptr ? out : "{}");
    cJSON_free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

/* 诊断：把某个引脚当"普通 GPIO"输出电平（测试是否其实是单色 LED / 继电器）
 * POST /api/led/gpio?pin=18&level=1&ms=3000
 */
esp_err_t HandleLedGpio(httpd_req_t* req) {
    char query[128] = {0};
    if (httpd_req_get_url_query_len(req) > 0) {
        httpd_req_get_url_query_str(req, query, sizeof(query));
    }
    auto get_int = [&query](const char* key, int def) {
        char val[16] = {0};
        if (httpd_query_key_value(query, key, val, sizeof(val)) != ESP_OK) {
            return def;
        }
        return atoi(val);
    };
    const int pin = get_int("pin", 18);
    const int level = get_int("level", 1) ? 1 : 0;
    int ms = get_int("ms", 3000);
    if (ms < 100) {
        ms = 100;
    }
    if (ms > 15000) {
        ms = 15000;
    }

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = GPIO_MODE_INPUT_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) {
        gpio_set_level(static_cast<gpio_num_t>(pin), level);
    }
    ESP_LOGW(TAG, "led gpio test: pin=%d level=%d ms=%d err=%s", pin, level, ms,
             esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(ms));
    if (err == ESP_OK) {
        gpio_set_level(static_cast<gpio_num_t>(pin), level ? 0 : 1);
        vTaskDelay(pdMS_TO_TICKS(300));
        gpio_reset_pin(static_cast<gpio_num_t>(pin));
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(root, "pin", pin);
    cJSON_AddNumberToObject(root, "level", level);
    cJSON_AddNumberToObject(root, "ms", ms);
    char* out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out != nullptr ? out : "{}");
    cJSON_free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t HandleGpio(httpd_req_t* req) {
    auto& gpio = GpioControl::GetInstance();
    httpd_resp_set_type(req, "application/json");

    if (req->method == HTTP_GET) {
        cJSON* root = cJSON_CreateObject();
        cJSON* arr = cJSON_AddArrayToObject(root, "devices");
        for (const auto& d : gpio.Devices()) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", d.name.c_str());
            cJSON_AddNumberToObject(item, "gpio", d.gpio);
            cJSON_AddBoolToObject(item, "active_high", d.active_high);
            cJSON_AddBoolToObject(item, "initial_on", d.initial_on);
            bool on = false;
            gpio.GetState(d.name, &on);
            cJSON_AddBoolToObject(item, "on", on);
            cJSON_AddItemToArray(arr, item);
        }
        auto* reserved = cJSON_AddArrayToObject(root, "reserved");
        for (const auto& entry : Board::GetInstance().ReservedGpios()) {
            cJSON_AddItemToArray(reserved, cJSON_CreateNumber(entry.first));
        }
        /* 说明栏：所有已占用引脚 + 原因 */
        auto* used = cJSON_AddArrayToObject(root, "used");
        for (const auto& u : gpio.UsedPins()) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "gpio", u.gpio);
            cJSON_AddStringToObject(item, "reason", u.reason.c_str());
            cJSON_AddItemToArray(used, item);
        }
        /* 下拉框：当前还能选的引脚 */
        auto* avail = cJSON_AddArrayToObject(root, "available");
        for (int pin : gpio.AvailablePins()) {
            cJSON_AddItemToArray(avail, cJSON_CreateNumber(pin));
        }
        char* out = cJSON_PrintUnformatted(root);
        httpd_resp_sendstr(req, out != nullptr ? out : "{}");
        cJSON_free(out);
        cJSON_Delete(root);
        return ESP_OK;
    }

    char buf[1024];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"empty\"}");
        return ESP_FAIL;
    }
    buf[len] = 0;
    cJSON* root = cJSON_Parse(buf);
    if (root == nullptr) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad json\"}");
        return ESP_FAIL;
    }

    /* 即时控制：{"name":..,"state":..} */
    auto* ctl_name = cJSON_GetObjectItem(root, "name");
    auto* ctl_state = cJSON_GetObjectItem(root, "state");
    if (cJSON_IsString(ctl_name) && cJSON_IsString(ctl_state)) {
        std::string msg;
        bool ok = gpio.SetByState(ctl_name->valuestring, ctl_state->valuestring, &msg);
        cJSON_Delete(root);
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", ok);
        cJSON_AddStringToObject(resp, "message", msg.c_str());
        char* out = cJSON_PrintUnformatted(resp);
        httpd_resp_sendstr(req, out != nullptr ? out : "{\"ok\":false}");
        cJSON_free(out);
        cJSON_Delete(resp);
        return ESP_OK;
    }

    /* 保存配置 */
    auto* devices = cJSON_GetObjectItem(root, "devices");
    std::vector<GpioSwitchConfig> cfgs;
    if (cJSON_IsArray(devices)) {
        const int n = cJSON_GetArraySize(devices);
        for (int i = 0; i < n; i++) {
            const cJSON* it = cJSON_GetArrayItem(devices, i);
            const cJSON* name = cJSON_GetObjectItem(it, "name");
            const cJSON* pin = cJSON_GetObjectItem(it, "gpio");
            if (!cJSON_IsString(name) || !cJSON_IsNumber(pin)) {
                continue;
            }
            if (name->valuestring[0] == 0) {
                continue;
            }
            GpioSwitchConfig cfg;
            cfg.name = name->valuestring;
            cfg.gpio = pin->valueint;
            const cJSON* ah = cJSON_GetObjectItem(it, "active_high");
            const cJSON* io = cJSON_GetObjectItem(it, "initial_on");
            cfg.active_high = cJSON_IsBool(ah) ? cJSON_IsTrue(ah) : true;
            cfg.initial_on = cJSON_IsBool(io) ? cJSON_IsTrue(io) : false;
            cfgs.push_back(cfg);
        }
    }
    cJSON_Delete(root);

    std::string error;
    bool ok = gpio.Save(cfgs, &error);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ok);
    if (!ok) {
        cJSON_AddStringToObject(resp, "error", error.c_str());
    }
    char* out = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, out != nullptr ? out : "{\"ok\":false}");
    cJSON_free(out);
    cJSON_Delete(resp);
    return ESP_OK;
}

/* 内嵌的网页文件（CMake 里通过 EMBED_FILES 打包，运行时直接从 flash 发送） */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t style_css_start[]  asm("_binary_style_css_start");
extern const uint8_t style_css_end[]    asm("_binary_style_css_end");
extern const uint8_t app_js_start[]     asm("_binary_app_js_start");
extern const uint8_t app_js_end[]       asm("_binary_app_js_end");

esp_err_t HandleRoot(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, reinterpret_cast<const char*>(index_html_start),
                            index_html_end - index_html_start - 1);
}

esp_err_t HandleStyle(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/css; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    return httpd_resp_send(req, reinterpret_cast<const char*>(style_css_start),
                            style_css_end - style_css_start - 1);
}

esp_err_t HandleAppJs(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    return httpd_resp_send(req, reinterpret_cast<const char*>(app_js_start),
                            app_js_end - app_js_start - 1);
}

esp_err_t HandleStatus(httpd_req_t* req) {
    auto json = BuildStatusJson();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), json.size());
}

void ApplyTheme(const std::string& name) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    auto* theme = LvglThemeManager::GetInstance().GetTheme(name);
    if (theme != nullptr) {
        display->SetTheme(theme);
        Settings("ui", true).SetString("theme", name);
        ESP_LOGI(TAG, "theme -> %s", name.c_str());
    }
}

esp_err_t HandleSettings(httpd_req_t* req) {
    char buf[1024];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"empty body\"}");
        return ESP_FAIL;
    }
    buf[len] = '\0';
    cJSON* root = cJSON_Parse(buf);
    if (root == nullptr) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad json\"}");
        return ESP_FAIL;
    }

    auto& board = Board::GetInstance();

    auto* theme = cJSON_GetObjectItem(root, "theme");
    if (cJSON_IsString(theme)) {
        ApplyTheme(theme->valuestring);
    }

    auto* brightness = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(brightness)) {
        if (auto* bl = board.GetBacklight()) {
            bl->SetBrightness(static_cast<uint8_t>(brightness->valueint), true);
            ESP_LOGI(TAG, "brightness -> %d", brightness->valueint);
        }
    }

    auto* volume = cJSON_GetObjectItem(root, "volume");
    if (cJSON_IsNumber(volume)) {
        if (auto* codec = board.GetAudioCodec()) {
            codec->SetOutputVolume(volume->valueint);
            Settings("audio", true).SetInt("output_volume", volume->valueint);
            ESP_LOGI(TAG, "volume -> %d", volume->valueint);
        }
    }

    auto* duck = cJSON_GetObjectItem(root, "duck");
    if (cJSON_IsNumber(duck)) {
        Settings("music", true).SetInt("duck_volume_percent", duck->valueint);
        ESP_LOGI(TAG, "duck -> %d%%", duck->valueint);
    }

    auto* api = cJSON_GetObjectItem(root, "api");
    if (cJSON_IsString(api) && strlen(api->valuestring) > 7) {
        Settings("music", true).SetString("api_url", api->valuestring);
        MusicApiClient::GetInstance().Reload();
        ESP_LOGI(TAG, "music api -> %s", api->valuestring);
    }

    auto* led_mode = cJSON_GetObjectItem(root, "led_mode");
    auto* wake_enabled = cJSON_GetObjectItem(root, "wake_enabled");
    auto* wake_threshold = cJSON_GetObjectItem(root, "wake_threshold");
    if (cJSON_IsNumber(wake_enabled) || cJSON_IsNumber(wake_threshold)) {
        auto& app = Application::GetInstance();
        if (cJSON_IsNumber(wake_threshold)) {
            app.SetWakeWordThreshold(wake_threshold->valueint / 100.0f);
        }
        if (cJSON_IsNumber(wake_enabled)) {
            app.SetWakeWordEnabled(wake_enabled->valueint != 0);
        }
        ESP_LOGI(TAG, "wake -> %d threshold=%d", (int)app.WakeWordEnabled(),
                 (int)(app.WakeWordThreshold() * 100 + 0.5f));
    }

    auto* standby = cJSON_GetObjectItem(root, "standby");
    auto* standby_city = cJSON_GetObjectItem(root, "standby_city");
    auto* standby_symbols = cJSON_GetObjectItem(root, "standby_symbols");
    if (cJSON_IsNumber(standby) || cJSON_IsString(standby_city) || cJSON_IsString(standby_symbols)) {
        auto& svc = StandbyService::GetInstance();
        if (cJSON_IsNumber(standby)) {
            svc.SetEnabled(standby->valueint != 0);
        }
        if (cJSON_IsString(standby_city) && standby_city->valuestring[0]) {
            svc.SetCity(standby_city->valuestring);
        }
        if (cJSON_IsString(standby_symbols) && standby_symbols->valuestring[0]) {
            svc.SetSymbols(standby_symbols->valuestring);
        }
        svc.ReloadConfig();
        ESP_LOGI(TAG, "standby -> %d city=%s symbols=%s", (int)svc.enabled(), svc.city().c_str(),
                 svc.symbols().c_str());
    }

    auto* chat_style = cJSON_GetObjectItem(root, "chat_style");
    if (cJSON_IsNumber(chat_style)) {
        int idx = chat_style->valueint;
        if (idx < 0 || idx > 3) {
            idx = 0;
        }
        Settings("ui", true).SetInt("chat_style", idx);
        if (auto* display = board.GetDisplay()) {
            display->ApplyChatStyle();
        }
        ESP_LOGI(TAG, "chat style -> %d", idx);
    }

    auto* led_color = cJSON_GetObjectItem(root, "led_color");
    if (auto* led = board.GetLed()) {
        /* 先同步固定颜色，再切模式；两边都由 Led 自己写 NVS，避免键不一致 */
        if (cJSON_IsString(led_color)) {
            uint8_t r = 0, g = 0, b = 255;
            HexToRgb(led_color->valuestring, &r, &g, &b);
            led->SetFixedColor(r, g, b);
        }
        if (cJSON_IsString(led_mode)) {
            std::string mode = led_mode->valuestring;
            if (mode == "off") {
                led->SetMode(Led::Mode::Off);
            } else if (mode == "color") {
                led->SetMode(Led::Mode::Fixed);
            } else {
                led->SetMode(Led::Mode::Follow);
            }
            ESP_LOGI(TAG, "led -> %s (mode=%d)", mode.c_str(), (int)led->GetMode());
        }
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

}  // namespace

bool AdminServer::Start() {
    if (g_server != nullptr) {
        return true;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.ctrl_port = 32769;
    config.max_uri_handlers = 20;   /* 现在有 11 个 URI（GET/POST 分开算），默认 8 不够 */
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    if (httpd_start(&g_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start admin server");
        g_server = nullptr;
        return false;
    }
    httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = HandleRoot, .user_ctx = nullptr};
    httpd_uri_t uri_style = {.uri = "/style.css", .method = HTTP_GET, .handler = HandleStyle, .user_ctx = nullptr};
    httpd_uri_t uri_app = {.uri = "/app.js", .method = HTTP_GET, .handler = HandleAppJs, .user_ctx = nullptr};
    httpd_uri_t uri_status = {.uri = "/api/status", .method = HTTP_GET, .handler = HandleStatus, .user_ctx = nullptr};
    httpd_uri_t uri_settings = {.uri = "/api/settings", .method = HTTP_POST, .handler = HandleSettings, .user_ctx = nullptr};
    httpd_uri_t uri_assets = {.uri = "/api/assets", .method = HTTP_POST, .handler = HandleAssetsApply, .user_ctx = nullptr};
    httpd_uri_t uri_reboot = {.uri = "/api/reboot", .method = HTTP_POST, .handler = HandleReboot, .user_ctx = nullptr};
    httpd_uri_t uri_chat_test = {.uri = "/api/chat_test", .method = HTTP_POST, .handler = HandleChatTest, .user_ctx = nullptr};
    httpd_uri_t uri_led_gpio = {.uri = "/api/led/gpio", .method = HTTP_POST, .handler = HandleLedGpio, .user_ctx = nullptr};
    httpd_uri_t uri_led_blink = {.uri = "/api/led/blink", .method = HTTP_POST, .handler = HandleLedBlink, .user_ctx = nullptr};
    httpd_uri_t uri_led_probe = {.uri = "/api/led/probe", .method = HTTP_POST, .handler = HandleLedProbe, .user_ctx = nullptr};
    httpd_uri_t uri_mcp_tools = {.uri = "/api/mcp_tools", .method = HTTP_GET, .handler = HandleMcpTools, .user_ctx = nullptr};
    httpd_uri_t uri_gpio_get = {.uri = "/api/gpio", .method = HTTP_GET, .handler = HandleGpio, .user_ctx = nullptr};
    httpd_uri_t uri_gpio_post = {.uri = "/api/gpio", .method = HTTP_POST, .handler = HandleGpio, .user_ctx = nullptr};
    httpd_uri_t uri_image_get = {.uri = "/api/image", .method = HTTP_GET, .handler = HandleImageGet, .user_ctx = nullptr};
    httpd_uri_t uri_image_post = {.uri = "/api/image", .method = HTTP_POST, .handler = HandleImagePost, .user_ctx = nullptr};
    auto reg = [](httpd_uri_t* uri) {
        esp_err_t err = httpd_register_uri_handler(g_server, uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uri->uri, esp_err_to_name(err));
        }
    };
    reg(&uri_root);
    reg(&uri_style);
    reg(&uri_app);
    reg(&uri_status);
    reg(&uri_settings);
    reg(&uri_assets);
    reg(&uri_reboot);
    reg(&uri_chat_test);
    reg(&uri_led_gpio);
    reg(&uri_led_blink);
    reg(&uri_led_probe);
    reg(&uri_mcp_tools);
    reg(&uri_gpio_get);
    reg(&uri_gpio_post);
    reg(&uri_image_get);
    reg(&uri_image_post);
    ESP_LOGI(TAG, "admin web: http://<device-ip>:8080/");
    return true;
}

void AdminServer::ApplyPersisted() {
    Settings ui("ui", false);
    auto theme = ui.GetString("theme", "");
    if (!theme.empty()) {
        ApplyTheme(theme);
    }
}
