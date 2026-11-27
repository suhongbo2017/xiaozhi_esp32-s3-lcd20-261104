#include "qr_page.h"

#include "board.h"
#include "mcp_server.h"
#include "qrcodegen.h"   /* 复制自 esp_emote_gfx（MIT），符号由该组件提供 */

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstring>

#include <wifi_manager.h>

#define TAG "QrPage"

namespace {
constexpr int kQrSize = 200;      /* 240x320 屏上 200px 的码最好扫 */
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kBlack = 0x0000;

esp_timer_handle_t g_hide_timer = nullptr;
uint8_t* g_pixels = nullptr;      /* kQrSize*kQrSize RGB565，放 PSRAM */
uint8_t g_qr[qrcodegen_BUFFER_LEN_MAX];
uint8_t g_tmp[qrcodegen_BUFFER_LEN_MAX];

void HideTimerCb(void* arg) {
    QrPage::GetInstance().Hide();
}

/* 把 qrcodegen 的模块矩阵画到 RGB565 缓冲里（白底黑块） */
void RenderToBuffer(const std::string& text, int* out_modules) {
    if (g_pixels == nullptr) {
        return;
    }
    const size_t stride = kQrSize * 2;
    for (int y = 0; y < kQrSize; y++) {
        auto* row = reinterpret_cast<uint16_t*>(g_pixels + y * stride);
        for (int x = 0; x < kQrSize; x++) {
            row[x] = kWhite;
        }
    }

    const bool ok = qrcodegen_encodeText(text.c_str(), g_tmp, g_qr, qrcodegen_Ecc_MEDIUM,
                                         qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                         qrcodegen_Mask_AUTO, true);
    if (!ok) {
        ESP_LOGE(TAG, "qrcode encode failed for '%s'", text.c_str());
        return;
    }
    const int modules = qrcodegen_getSize(g_qr);
    int box = kQrSize / modules;
    if (box < 1) {
        box = 1;
    }
    const int used = box * modules;
    const int pad = (kQrSize - used) / 2;   /* 居中 */

    for (int my = 0; my < modules; my++) {
        for (int mx = 0; mx < modules; mx++) {
            if (!qrcodegen_getModule(g_qr, mx, my)) {
                continue;
            }
            const int px0 = pad + mx * box;
            const int py0 = pad + my * box;
            for (int dy = 0; dy < box; dy++) {
                auto* row = reinterpret_cast<uint16_t*>(g_pixels + (py0 + dy) * stride);
                for (int dx = 0; dx < box; dx++) {
                    row[px0 + dx] = kBlack;
                }
            }
        }
    }
    if (out_modules != nullptr) {
        *out_modules = modules;
    }
}
}  // namespace

QrPage& QrPage::GetInstance() {
    static QrPage instance;
    return instance;
}

bool QrPage::visible() const {
#ifdef HAVE_LVGL
    return root_ != nullptr;
#else
    return false;
#endif
}

#ifdef HAVE_LVGL
bool QrPage::Show(Display* display, const std::string& url, int seconds) {
    if (display == nullptr) {
        return false;
    }

    std::string target = url;
    if (target.empty()) {
        std::string ip = WifiManager::GetInstance().GetIpAddress();
        if (ip.empty()) {
            ESP_LOGW(TAG, "no ip yet, cannot build admin url");
            return false;
        }
        target = "http://" + ip + ":8080/";
    }

    DisplayLockGuard lock(display);
    display_ = display;

    if (g_pixels == nullptr) {
        g_pixels = static_cast<uint8_t*>(heap_caps_malloc(kQrSize * kQrSize * 2,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (g_pixels == nullptr) {
            ESP_LOGE(TAG, "no psram for qr buffer");
            return false;
        }
    }

    if (root_ == nullptr) {
        const int w = display->width();
        const int h = display->height();

        root_ = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(root_);
        lv_obj_set_size(root_, w, h);
        lv_obj_set_pos(root_, 0, 0);
        lv_obj_set_style_bg_color(root_, lv_color_hex(0xF2F4F7), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(root_, 0, 0);
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

        qr_ = lv_image_create(root_);
        lv_obj_align(qr_, LV_ALIGN_TOP_MID, 0, 12);

        url_label_ = lv_label_create(root_);
        lv_label_set_text(url_label_, "");
        lv_obj_set_width(url_label_, w - 16);
        lv_obj_set_style_text_align(url_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(url_label_, lv_color_hex(0x1B2B3D), 0);
        lv_obj_align(url_label_, LV_ALIGN_BOTTOM_MID, 0, -30);

        lv_obj_t* tip = lv_label_create(root_);
        lv_label_set_text(tip, "手机扫一扫打开设置页");
        lv_obj_set_style_text_color(tip, lv_color_hex(0x5C7490), 0);
        lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -10);
    }

    int modules = 0;
    RenderToBuffer(target, &modules);

    /* 直接把 RGB565 缓冲包成 image descriptor（和待机背景图同一套做法，稳定） */
    static lv_image_dsc_t dsc = {};
    dsc = {};
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc.header.w = kQrSize;
    dsc.header.h = kQrSize;
    dsc.data_size = kQrSize * kQrSize * 2;
    dsc.data = g_pixels;
    lv_image_set_src(qr_, &dsc);
    lv_obj_align(qr_, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_invalidate(qr_);
    lv_label_set_text(url_label_, target.c_str());
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);

    if (g_hide_timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = HideTimerCb;
        args.arg = nullptr;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "qr_hide";
        args.skip_unhandled_events = true;
        esp_timer_create(&args, &g_hide_timer);
    }
    esp_timer_stop(g_hide_timer);
    if (seconds > 0) {
        esp_timer_start_once(g_hide_timer, static_cast<uint64_t>(seconds) * 1000000ULL);
    }

    ESP_LOGI(TAG, "QR page shown: %s (%d modules, %d s)", target.c_str(), modules, seconds);
    return true;
}

void QrPage::Hide() {
    if (root_ == nullptr) {
        return;
    }
    if (g_hide_timer != nullptr) {
        esp_timer_stop(g_hide_timer);
    }
    if (display_ != nullptr) {
        DisplayLockGuard lock(display_);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "QR page hidden");
}

#else

bool QrPage::Show(Display* display, const std::string& url, int seconds) {
    (void)display;
    (void)url;
    (void)seconds;
    return false;
}
void QrPage::Hide() {}

#endif

namespace {
/* 语音入口：说"怎么设置/显示二维码"也能调出来 */
struct QrMcpRegistrar {
    QrMcpRegistrar() {
        McpServer::GetInstance().AddTool(
            "self.screen.show_qrcode",
            "在设备屏幕上显示一个二维码，手机扫一下就能打开设备的网页设置页面"
            "（http://设备IP:8080/）。"
            "当用户说「显示二维码」「二维码」「怎么设置」「怎么修改设置」「设置页面在哪」"
            "「手机怎么连」「扫个码」时调用本工具。"
            "Show a QR code on the screen that opens the device's web settings page.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto* display = Board::GetInstance().GetDisplay();
                if (!QrPage::GetInstance().Show(display, "", 120)) {
                    return std::string("设备还没拿到 IP，暂时无法生成二维码");
                }
                return std::string("二维码已显示在屏幕上，用手机扫一扫就能打开设置页");
            });
    }
};
}  // namespace

void QrPageRegisterMcpTool() {
    static QrMcpRegistrar registrar;
}
