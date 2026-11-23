#include "admin_server.h"

#include "audio_codec.h"
#include "asset_ota.h"
#include "board.h"
#include "display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "music/music_api_client.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <cstring>
#include <string>

#define TAG "AdminServer"

namespace {

httpd_handle_t g_server = nullptr;

constexpr const char* kHtml = R"HTML(<!doctype html>
<html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>小智设备管理</title>
<style>
:root{color-scheme:dark}
body{margin:0;font:15px/1.6 -apple-system,Segoe UI,Roboto,sans-serif;background:#0e1e33;color:#e8f1ff}
.wrap{max-width:560px;margin:0 auto;padding:20px 16px 48px}
h1{font-size:20px;margin:8px 0 4px}
.sub{color:#8fa6bf;font-size:13px;margin-bottom:18px}
.card{background:#13273f;border:1px solid #1e3a58;border-radius:14px;padding:16px;margin:14px 0}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px;margin:12px 0}
label{color:#b9cde4}
input[type=range]{width:100%}
input[type=text]{flex:1;background:#0b1a2a;border:1px solid #1e3a58;color:#e8f1ff;border-radius:10px;padding:10px}
select{background:#0b1a2a;border:1px solid #1e3a58;color:#e8f1ff;border-radius:10px;padding:8px 10px}
input[type=color]{width:46px;height:32px;border:none;background:none}
button{width:100%;padding:13px;border:none;border-radius:12px;background:#00a2ff;color:#04121f;font-weight:700;font-size:16px;margin-top:6px}
button:active{transform:scale(.99)}
.ok{color:#37e39b}.warn{color:#ffcf5c}
.tip{color:#8fa6bf;font-size:12px;margin-top:6px}
.val{color:#00e5ff;font-variant-numeric:tabular-nums}
pre{background:#0b1a2a;border-radius:10px;padding:10px;font-size:12px;overflow:auto;max-height:150px}
</style></head><body><div class="wrap">
<h1>小智设备管理</h1>
<div class="sub" id="dev">连接中…</div>

<div class="card">
  <div class="row"><label>主题</label>
    <select id="theme"><option value="dark">深色</option><option value="light">浅色</option></select>
  </div>
  <div class="row"><label>屏幕亮度 <span class="val" id="bv">-</span></label></div>
  <input type="range" id="brightness" min="0" max="100" step="1">
  <div class="row"><label>音量 <span class="val" id="vv">-</span></label></div>
  <input type="range" id="volume" min="0" max="100" step="1">
  <div class="row"><label>音乐播放时音量比例 <span class="val" id="dv">-</span>%</label></div>
  <input type="range" id="duck" min="30" max="100" step="5">
  <div class="tip">播放音乐时自动压低音量，方便用唤醒词打断</div>
</div>

<div class="card">
  <div class="row"><label>状态灯</label>
    <select id="led_mode"><option value="on">跟随状态</option><option value="off">常灭</option><option value="color">固定颜色</option></select>
  </div>
  <div class="row"><label>固定颜色</label><input type="color" id="led_color" value="#00b3ff"></div>
  <div class="tip">唤醒词开关需要改音频生命周期，留到 P2</div>
</div>

<div class="card">
  <div class="row"><label>音乐服务地址</label></div>
  <input type="text" id="api" placeholder="http://47.116.199.150:2233">
  <div class="tip">改完保存立即生效（无需重启）</div>
</div>

<div class="card">
  <div class="row"><label>外观资源（字体 / 表情）</label>
    <select id="variant">
      <option value="a20_e64" selected>20px 字体 + 64px 表情（推荐）</option>
      <option value="a16_e32">16px 字体 + 32px 表情（省空间）</option>
      <option value="a30_e64">30px 字体 + 64px 表情（大字）</option>
    </select>
  </div>
  <button id="applyAssets" style="background:#ff9f43">应用并重启</button>
  <div class="tip warn" id="amsg"></div>
  <div class="tip">会下载资源包写入设备 assets 分区并自动重启（约 5~20 秒）</div>
</div>

<button id="save">保存并应用</button>
<div class="tip ok" id="msg"></div>
<div class="card" style="opacity:.85">
  <div class="tip">当前状态</div><pre id="status">-</pre>
</div>
</div>
<script>
const $ = id => document.getElementById(id);
let cur = {};
function fill(s){
  cur = s;
  $('dev').textContent = (s.board||'设备') + ' · ' + location.host + ' · ' + (s.mac||'');
  $('theme').value = s.theme || 'dark';
  $('brightness').value = s.brightness ?? 80; $('bv').textContent = $('brightness').value;
  $('volume').value = s.volume ?? 70; $('vv').textContent = $('volume').value;
  $('duck').value = s.duck ?? 60; $('dv').textContent = $('duck').value;
  $('led_mode').value = s.led_mode || 'on';

  if (s.led_color) $('led_color').value = s.led_color;
  if (s.api) $('api').value = s.api;
  $('status').textContent = JSON.stringify(s, null, 1);
}
async function load(){
  try{ const r = await fetch('/api/status'); fill(await r.json()); }
  catch(e){ $('msg').textContent='读取失败：'+e; }
}
['brightness','volume','duck'].forEach(id=>{
  $(id).addEventListener('input',()=>{ $(id[0]+'v').textContent = $(id).value; });
});
$('save').onclick = async () => {
  $('msg').textContent = '保存中…';
  const body = {
    theme: $('theme').value,
    brightness: +$('brightness').value,
    volume: +$('volume').value,
    duck: +$('duck').value,
    led_mode: $('led_mode').value,
    led_color: $('led_color').value,
    api: $('api').value.trim()
  };
  try{
    const r = await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    const j = await r.json();
    $('msg').className = 'tip ' + (j.ok?'ok':'warn');
    $('msg').textContent = j.ok ? '已应用 ✓' : ('失败：'+(j.error||''));
    load();
  }catch(e){ $('msg').textContent='请求失败：'+e; }
};
$('applyAssets').onclick = async () => {
  const v = $('variant').value;
  if(!confirm('将把「'+v+'」资源包写入设备并重启，确定吗？')) return;
  $('amsg').textContent = '下载中，请稍候…（设备会自动重启）';
  try{
    const r = await fetch('/api/assets',{method:'POST',headers:{'Content-Type':'application/json'},
      body: JSON.stringify({url:'http://47.116.199.150:2233/assets/'+v+'.bin'})});
    const j = await r.json();
    $('amsg').textContent = j.ok ? '已开始写入，设备即将重启…' : ('失败：'+(j.error||''));
  }catch(e){ $('amsg').textContent = '请求失败：'+e; }
};
load();
</script></body></html>)HTML";

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
    cJSON_AddStringToObject(root, "led_mode",
                             led_settings.GetString("mode",
                                                     led_settings.GetInt("status_enabled", 1)
                                                         ? "on" : "off").c_str());
    cJSON_AddStringToObject(root, "led_color", "#00b3ff");
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

esp_err_t HandleRoot(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kHtml, HTTPD_RESP_USE_STRLEN);
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
    auto* led_color = cJSON_GetObjectItem(root, "led_color");
    if (auto* led = board.GetLed()) {
        if (cJSON_IsString(led_mode)) {
            std::string mode = led_mode->valuestring;
            if (mode != "on" && mode != "off" && mode != "color") {
                mode = "on";
            }
            Settings("led", true).SetString("mode", mode);
            if (mode == "off") {
                led->SetStatusEnabled(false);
                led->TurnOff();
            } else if (mode == "color") {
                uint8_t r = 0, g = 0, b = 255;
                if (cJSON_IsString(led_color)) {
                    HexToRgb(led_color->valuestring, &r, &g, &b);
                }
                led->SetStatusEnabled(false);
                led->SetColor(r, g, b);
                led->TurnOn();
            } else {
                led->SetStatusEnabled(true);
            }
            ESP_LOGI(TAG, "led mode -> %s", mode.c_str());
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
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    if (httpd_start(&g_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start admin server");
        g_server = nullptr;
        return false;
    }
    httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = HandleRoot, .user_ctx = nullptr};
    httpd_uri_t uri_status = {.uri = "/api/status", .method = HTTP_GET, .handler = HandleStatus, .user_ctx = nullptr};
    httpd_uri_t uri_settings = {.uri = "/api/settings", .method = HTTP_POST, .handler = HandleSettings, .user_ctx = nullptr};
    httpd_uri_t uri_assets = {.uri = "/api/assets", .method = HTTP_POST, .handler = HandleAssetsApply, .user_ctx = nullptr};
    httpd_register_uri_handler(g_server, &uri_root);
    httpd_register_uri_handler(g_server, &uri_status);
    httpd_register_uri_handler(g_server, &uri_settings);
    httpd_register_uri_handler(g_server, &uri_assets);
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
