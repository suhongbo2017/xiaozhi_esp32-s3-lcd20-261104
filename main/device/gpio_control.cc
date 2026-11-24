#include "gpio_control.h"

#include "board.h"
#include "mcp_server.h"
#include "settings.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <cJSON.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#define TAG "GpioControl"

namespace {
constexpr int kMaxDevices = 4;
std::mutex g_mutex;
std::vector<GpioSwitchConfig> g_devices;
std::vector<bool> g_states;
bool g_ready = false;
bool g_tools_registered = false;

/** 通用危险脚（闪存 / USB / 串口 / 启动脚），与具体板型无关 */
struct FixedPin {
    int pin;
    const char* why;
};
const FixedPin kFixedReserved[] = {
    {0, "启动键(BOOT)"},
    {19, "USB D-"},
    {20, "USB D+"},
    {43, "串口 UART0 TX"},
    {44, "串口 UART0 RX"},
    {46, "S3 启动模式 strapping 脚"},
    /* ESP32-S3 的 flash/PSRAM 使用 GPIO26~GPIO37（封装内） */
    {26, "闪存"}, {27, "闪存"}, {28, "闪存"}, {29, "闪存"}, {30, "闪存"},
    {31, "闪存"}, {32, "闪存"}, {33, "闪存"}, {34, "闪存"}, {35, "闪存"},
    {36, "闪存"}, {37, "闪存"},
};

int FindIndex(const std::string& key) {
    /* 支持按名称或序号（1 开始）控制 */
    for (size_t i = 0; i < g_devices.size(); i++) {
        if (g_devices[i].name == key) {
            return static_cast<int>(i);
        }
    }
    if (!key.empty() && key.size() <= 2 && key[0] >= '1' && key[0] <= '9') {
        const int idx = key[0] - '1';
        if (idx >= 0 && idx < static_cast<int>(g_devices.size())) {
            return idx;
        }
    }
    return -1;
}

void ApplyLevel(int idx) {
    if (idx < 0 || idx >= static_cast<int>(g_devices.size())) {
        return;
    }
    const auto& dev = g_devices[idx];
    const int level = (g_states[idx] == dev.active_high) ? 1 : 0;
    gpio_set_level(static_cast<gpio_num_t>(dev.gpio), level);
}

bool ParseConfig(const char* json, std::vector<GpioSwitchConfig>* out, std::string* error) {
    cJSON* root = cJSON_Parse(json);
    if (!cJSON_IsArray(root)) {
        if (root != nullptr) {
            cJSON_Delete(root);
        }
        if (error != nullptr) {
            *error = "配置不是数组";
        }
        return false;
    }
    const int n = cJSON_GetArraySize(root);
    if (n > kMaxDevices) {
        cJSON_Delete(root);
        if (error != nullptr) {
            *error = "最多 4 路";
        }
        return false;
    }
    std::vector<GpioSwitchConfig> parsed;
    for (int i = 0; i < n; i++) {
        const cJSON* item = cJSON_GetArrayItem(root, i);
        const cJSON* name = cJSON_GetObjectItem(item, "name");
        const cJSON* gpio = cJSON_GetObjectItem(item, "gpio");
        const cJSON* active_high = cJSON_GetObjectItem(item, "active_high");
        const cJSON* initial_on = cJSON_GetObjectItem(item, "initial_on");
        if (!cJSON_IsString(name) || !cJSON_IsNumber(gpio)) {
            continue;
        }
        GpioSwitchConfig cfg;
        cfg.name = name->valuestring;
        cfg.gpio = gpio->valueint;
        cfg.active_high = cJSON_IsBool(active_high) ? cJSON_IsTrue(active_high) : true;
        cfg.initial_on = cJSON_IsBool(initial_on) ? cJSON_IsTrue(initial_on) : false;
        parsed.push_back(cfg);
    }
    cJSON_Delete(root);
    *out = std::move(parsed);
    return true;
}

std::string SerializeConfig(const std::vector<GpioSwitchConfig>& devices) {
    cJSON* arr = cJSON_CreateArray();
    for (const auto& d : devices) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", d.name.c_str());
        cJSON_AddNumberToObject(item, "gpio", d.gpio);
        cJSON_AddBoolToObject(item, "active_high", d.active_high);
        cJSON_AddBoolToObject(item, "initial_on", d.initial_on);
        cJSON_AddItemToArray(arr, item);
    }
    char* text = cJSON_PrintUnformatted(arr);
    std::string out = text != nullptr ? text : "[]";
    cJSON_free(text);
    cJSON_Delete(arr);
    return out;
}

void ConfigurePins() {
    for (size_t i = 0; i < g_devices.size(); i++) {
        const auto& dev = g_devices[i];
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << dev.gpio;
        cfg.mode = GPIO_MODE_INPUT_OUTPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE;
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config(%d) failed: %s", dev.gpio, esp_err_to_name(err));
            continue;
        }
        g_states[i] = dev.initial_on;
        ApplyLevel(static_cast<int>(i));
        ESP_LOGI(TAG, "gpio[%d] %s -> %s (active_%s, init %s)", dev.gpio, dev.name.c_str(),
                 g_states[i] ? "ON" : "OFF", dev.active_high ? "high" : "low",
                 dev.initial_on ? "on" : "off");
    }
}
}  // namespace

GpioControl& GpioControl::GetInstance() {
    static GpioControl instance;
    return instance;
}

bool GpioControl::IsReserved(int gpio, std::string* reason) const {
    for (const auto& fp : kFixedReserved) {
        if (fp.pin == gpio) {
            if (reason != nullptr) {
                *reason = fp.why;
            }
            return true;
        }
    }
    auto& board = Board::GetInstance();
    for (const auto& entry : board.ReservedGpios()) {
        if (entry.first == gpio) {
            if (reason != nullptr) {
                *reason = entry.second;
            }
            return true;
        }
    }
    return false;
}

std::vector<GpioControl::UsedPin> GpioControl::UsedPins() const {
    std::vector<UsedPin> used;
    for (const auto& fp : kFixedReserved) {
        used.push_back({fp.pin, fp.why});
    }
    auto& board = Board::GetInstance();
    for (const auto& entry : board.ReservedGpios()) {
        bool dup = false;
        for (const auto& u : used) {
            if (u.gpio == entry.first) {
                dup = true;   /* 固定表里已有（如 0 启动键），保留固定表说明 */
                break;
            }
        }
        if (!dup) {
            used.push_back({entry.first, entry.second + "（板载）"});
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& d : g_devices) {
            used.push_back({d.gpio, d.name + "（已配置）"});
        }
    }
    std::sort(used.begin(), used.end(), [](const UsedPin& a, const UsedPin& b) { return a.gpio < b.gpio; });
    return used;
}

std::vector<int> GpioControl::AvailablePins() const {
    /* ESP32-S3 有效 GPIO：0~21 与 26~48（22~25 不存在） */
    std::vector<UsedPin> used = UsedPins();
    std::vector<int> out;
    for (int pin = 0; pin <= 48; pin++) {
        if (pin >= 22 && pin <= 25) {
            continue;
        }
        bool taken = false;
        for (const auto& u : used) {
            if (u.gpio == pin) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            out.push_back(pin);
        }
    }
    return out;
}

void GpioControl::Initialize() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ready) {
        return;
    }
    Settings settings("gpio", false);
    const std::string json = settings.GetString("config", "[]");
    std::string error;
    if (!ParseConfig(json.c_str(), &g_devices, &error)) {
        ESP_LOGW(TAG, "bad gpio config: %s", error.c_str());
        g_devices.clear();
    }
    g_states.assign(g_devices.size(), false);
    ConfigurePins();
    g_ready = true;
    if (!g_devices.empty()) {
        ESP_LOGI(TAG, "loaded %d user gpio switch(es)", (int)g_devices.size());
    }
}

std::vector<GpioSwitchConfig> GpioControl::Devices() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_devices;
}

bool GpioControl::Save(const std::vector<GpioSwitchConfig>& devices, std::string* error) {
    if (devices.size() > static_cast<size_t>(kMaxDevices)) {
        if (error) {
            *error = "最多 4 路";
        }
        return false;
    }
    /* 校验：名称非空且不重复、引脚合法且未被占用 */
    std::vector<std::string> names;
    for (const auto& d : devices) {
        if (d.name.empty()) {
            if (error) {
                *error = "名称不能为空";
            }
            return false;
        }
        if (std::find(names.begin(), names.end(), d.name) != names.end()) {
            if (error) {
                *error = "名称重复：" + d.name;
            }
            return false;
        }
        names.push_back(d.name);
        if (d.gpio < 0 || d.gpio > 48) {
            if (error) {
                *error = "GPIO 范围 0~48：" + d.name;
            }
            return false;
        }
        std::string why;
        if (IsReserved(d.gpio, &why)) {
            if (error) {
                *error = "GPIO" + std::to_string(d.gpio) + " 是" + why + "，不能用";
            }
            return false;
        }
        for (const auto& other : devices) {
            if (&other != &d && other.gpio == d.gpio) {
                if (error) {
                    *error = "GPIO" + std::to_string(d.gpio) + " 重复配置";
                }
                return false;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        /* 旧配置的引脚恢复成输入（避免残留输出电平） */
        for (const auto& d : g_devices) {
            gpio_reset_pin(static_cast<gpio_num_t>(d.gpio));
        }
        g_devices = devices;
        g_states.assign(g_devices.size(), false);
        ConfigurePins();
        g_ready = true;
        Settings("gpio", true).SetString("config", SerializeConfig(g_devices));
    }
    ESP_LOGI(TAG, "saved %d gpio switch(es)", (int)devices.size());
    return true;
}

bool GpioControl::SetByState(const std::string& key, const std::string& state, std::string* message) {
    std::string s = state;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    bool want_on = false;
    bool toggle = false;
    if (s == "on" || s == "1" || s == "true" || s == "开" || s == "打开") {
        want_on = true;
    } else if (s == "off" || s == "0" || s == "false" || s == "关" || s == "关闭") {
        want_on = false;
    } else if (s == "toggle" || s == "switch" || s == "翻转") {
        toggle = true;
    } else {
        if (message) {
            *message = "state 只能是 on/off/toggle";
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    const int idx = FindIndex(key);
    if (idx < 0) {
        std::string names;
        for (const auto& d : g_devices) {
            names += (names.empty() ? "" : "、") + d.name;
        }
        if (message) {
            *message = names.empty() ? "还没有配置任何 GPIO 开关"
                                     : ("没有这个开关：" + key + "，可用：" + names);
        }
        return false;
    }
    const auto& dev = g_devices[idx];
    const bool target = toggle ? !g_states[idx] : want_on;
    g_states[idx] = target;
    ApplyLevel(idx);
    char buf[160];
    snprintf(buf, sizeof(buf), "%s 已%s（GPIO%d，%s电平）", dev.name.c_str(), target ? "打开" : "关闭",
             dev.gpio, dev.active_high ? "高" : "低");
    if (message) {
        *message = buf;
    }
    ESP_LOGI(TAG, "%s", message ? message->c_str() : buf);
    return true;
}

bool GpioControl::SetState(const std::string& name, bool on) {
    std::string msg;
    return SetByState(name, on ? "on" : "off", &msg);
}

bool GpioControl::GetState(const std::string& name, bool* on) const {
    std::lock_guard<std::mutex> lock(g_mutex);
    const int idx = FindIndex(name);
    if (idx < 0) {
        return false;
    }
    if (on != nullptr) {
        *on = g_states[idx];
    }
    return true;
}

std::string GpioControl::Describe() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_devices.empty()) {
        return "（未配置任何开关）";
    }
    std::string out;
    for (size_t i = 0; i < g_devices.size(); i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%d) %s [GPIO%d] = %s；", (int)(i + 1), g_devices[i].name.c_str(),
                 g_devices[i].gpio, g_states[i] ? "开" : "关");
        out += buf;
    }
    return out;
}

void GpioControl::RegisterMcpTools() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_tools_registered) {
        return;
    }
    g_tools_registered = true;

    auto& mcp = McpServer::GetInstance();
    mcp.AddTool(
        "self.gpio.list",
        "列出用户配置的 GPIO 开关及其当前状态（例如：打印机灯光 [GPIO3] = 关）。"
        "在控制开关之前先调用它确认名称。",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue { return GpioControl::GetInstance().Describe(); });

    mcp.AddTool(
        "self.gpio.set",
        "控制用户配置的 GPIO 开关（如打印机灯光、台灯、风扇）。"
        "device 传开关名称（或序号 1~4），state 传 on/off/toggle。",
        PropertyList({Property("device", kPropertyTypeString),
                      Property("state", kPropertyTypeString)}),
        [](const PropertyList& props) -> ReturnValue {
            std::string msg;
            const std::string device = props["device"].value<std::string>();
            const std::string state = props["state"].value<std::string>();
            if (!GpioControl::GetInstance().SetByState(device, state, &msg)) {
                return std::string("操作失败：") + msg;
            }
            return msg;
        });

    ESP_LOGI(TAG, "mcp tools registered (self.gpio.list / self.gpio.set)");
}
