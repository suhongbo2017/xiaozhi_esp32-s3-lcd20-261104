#include "standby_page.h"

#include <esp_log.h>

#define TAG "StandbyPage"

#ifndef HAVE_LVGL

StandbyPage::StandbyPage(Display* display) : display_(display) {}
StandbyPage::~StandbyPage() = default;
void StandbyPage::CreateIfNeeded() {}
void StandbyPage::Destroy() {}
void StandbyPage::Show() {}
void StandbyPage::Hide() {}
bool StandbyPage::visible() const { return false; }
void StandbyPage::SetClock(const char*, const char*) {}
void StandbyPage::SetWeather(const Weather&) {}
void StandbyPage::SetStocks(const std::vector<StockItem>&) {}
void StandbyPage::SetHint(const char*) {}

#else /* HAVE_LVGL */

#include <cstdio>

#include "lvgl_theme.h"

namespace {

/* ---------------- 版式（240x320） ----------------
 * 顶部：大时钟（48px 数字）+ 日期
 * 中部：天气（图标 + 温度 + 描述）
 * 下部：最多 3 行指数/个股
 * 底部：提示文字
 */
constexpr int kPad = 10;
constexpr int kClockY = 26;
constexpr int kDateY = 96;
constexpr int kWeatherY = 128;
constexpr int kStockY = 196;
constexpr int kStockLineH = 34;

constexpr uint32_t kBgTop = 0x0B1622;
constexpr uint32_t kBgBottom = 0x04070C;
constexpr uint32_t kAccent = 0x00E5FF;
constexpr uint32_t kDim = 0x7E93A8;
constexpr uint32_t kLine = 0x1B2C3E;
constexpr uint32_t kUp = 0xFF5252;    /* 涨：红 */
constexpr uint32_t kDown = 0x4CD964;  /* 跌：绿 */
constexpr uint32_t kFlat = 0xC8D4E0;

const lv_font_t* TextFont(Display* display) {
    auto theme = dynamic_cast<LvglTheme*>(display->GetTheme());
    if (theme && theme->text_font()) {
        return theme->text_font()->font();
    }
    return nullptr;
}

/* emoji 字体：天气图标用（text_font 只含中英文字形，emoji 在独立字体里） */
const lv_font_t* EmojiFont(Display* display) {
    auto theme = dynamic_cast<LvglTheme*>(display->GetTheme());
    if (theme && theme->emoji_font()) {
        return theme->emoji_font()->font();
    }
    return nullptr;
}

const lv_font_t* IconFont(Display* display) {
    auto theme = dynamic_cast<LvglTheme*>(display->GetTheme());
    if (theme && theme->large_icon_font()) {
        return theme->large_icon_font()->font();
    }
    if (theme && theme->icon_font()) {
        return theme->icon_font()->font();
    }
    return nullptr;
}

void SetFont(lv_obj_t* obj, const lv_font_t* font) {
    if (font != nullptr) {
        lv_obj_set_style_text_font(obj, font, 0);
    }
}

/* 天气图标：用 emoji 字体（Noto Color Emoji，assets 里已有） */
const char* WeatherEmoji(int icon) {
    switch (icon) {
        case 0:
            return "\xF0\x9F\x8C\x9E";  /* 🌞 */
        case 1:
            return "\xE2\x9B\x85";      /* ⛅ */
        case 2:
            return "\xE2\x98\x81";      /* ☁ */
        case 3:
            return "\xF0\x9F\x8C\xA7";  /* 🌧 */
        case 4:
            return "\xE2\x9D\x84";      /* ❄ */
        case 5:
            return "\xE2\x9A\xA1";      /* ⚡ */
        case 6:
            return "\xF0\x9F\x8C\xAB";  /* 🌫 */
        default:
            return "\xE2\x98\x81";
    }
}

lv_obj_t* CreateLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    SetFont(label, font);
    lv_label_set_text(label, "");
    return label;
}

}  // namespace

StandbyPage::StandbyPage(Display* display) : display_(display) {}

StandbyPage::~StandbyPage() { Destroy(); }

bool StandbyPage::visible() const { return shown_; }

void StandbyPage::CreateIfNeeded() {
    if (root_ != nullptr || display_ == nullptr) {
        return;
    }
    const int w = display_->width();
    const int h = display_->height();

    root_ = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, w, h);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, lv_color_hex(kBgTop), 0);
    lv_obj_set_style_bg_grad_color(root_, lv_color_hex(kBgBottom), 0);
    lv_obj_set_style_bg_grad_dir(root_, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(root_);

    /* 大时钟：Montserrat 48（纯数字，切换成本最低） */
    clock_label_ = CreateLabel(root_, &lv_font_montserrat_48, 0xFFFFFF);
    lv_obj_set_width(clock_label_, w);
    lv_obj_set_style_text_align(clock_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(clock_label_, 0, kClockY);
    lv_label_set_text(clock_label_, "--:--");

    date_label_ = CreateLabel(root_, TextFont(display_), kDim);
    lv_obj_set_width(date_label_, w);
    lv_obj_set_style_text_align(date_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(date_label_, 0, kDateY);
    lv_label_set_text(date_label_, "");

    lv_obj_t* divider = lv_obj_create(root_);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, w - 2 * kPad, 1);
    lv_obj_set_pos(divider, kPad, kWeatherY - 8);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kLine), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

    /* 天气：emoji 图标 + 温度 + 描述 */
    const lv_font_t* emoji = EmojiFont(display_);
    weather_icon_ = CreateLabel(root_, emoji != nullptr ? emoji : TextFont(display_), 0xFFFFFF);
    lv_obj_set_pos(weather_icon_, kPad + 4, kWeatherY);
    lv_label_set_text(weather_icon_, "");

    weather_label_ = CreateLabel(root_, TextFont(display_), 0xFFFFFF);
    lv_obj_set_pos(weather_label_, kPad + 44, kWeatherY + 2);
    lv_obj_set_width(weather_label_, w - kPad * 2 - 48);
    lv_label_set_long_mode(weather_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(weather_label_, "天气加载中…");

    /* 指数/个股：名称 + 价格/涨跌幅 */
    for (int i = 0; i < 3; i++) {
        stock_name_[i] = CreateLabel(root_, TextFont(display_), 0xC8D4E0);
        lv_obj_set_pos(stock_name_[i], kPad, kStockY + i * kStockLineH);
        lv_obj_set_width(stock_name_[i], 118);
        lv_label_set_long_mode(stock_name_[i], LV_LABEL_LONG_DOT);
        lv_label_set_text(stock_name_[i], "");

        stock_price_[i] = CreateLabel(root_, TextFont(display_), kFlat);
        lv_obj_set_pos(stock_price_[i], kPad + 120, kStockY + i * kStockLineH);
        lv_obj_set_width(stock_price_[i], w - kPad * 2 - 120);
        lv_obj_set_style_text_align(stock_price_[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(stock_price_[i], "");
    }

    hint_label_ = CreateLabel(root_, TextFont(display_), kAccent);
    lv_obj_set_width(hint_label_, w);
    lv_obj_set_style_text_align(hint_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint_label_, 0, h - 34);
    lv_label_set_text(hint_label_, "说“你好小智”开始对话");
}

void StandbyPage::Destroy() {
    shown_ = false;
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    clock_label_ = nullptr;
    date_label_ = nullptr;
    weather_icon_ = nullptr;
    weather_label_ = nullptr;
    hint_label_ = nullptr;
    for (int i = 0; i < 3; i++) {
        stock_name_[i] = nullptr;
        stock_price_[i] = nullptr;
    }
}

void StandbyPage::Show() {
    if (display_ == nullptr || shown_) {
        return;
    }
    DisplayLockGuard lock(display_);
    CreateIfNeeded();
    if (root_ != nullptr) {
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
        shown_ = true;
    }
}

void StandbyPage::Hide() {
    if (display_ == nullptr || root_ == nullptr || !shown_) {
        shown_ = false;
        return;
    }
    DisplayLockGuard lock(display_);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    shown_ = false;
}

void StandbyPage::SetClock(const char* hhmm, const char* date_line) {
    if (display_ == nullptr || root_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (clock_label_ != nullptr && hhmm != nullptr) {
        lv_label_set_text(clock_label_, hhmm);
    }
    if (date_label_ != nullptr && date_line != nullptr) {
        lv_label_set_text(date_label_, date_line);
    }
}

void StandbyPage::SetWeather(const Weather& weather) {
    if (display_ == nullptr || root_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (!weather.valid) {
        if (weather_label_ != nullptr) {
            lv_label_set_text(weather_label_, "天气不可用");
        }
        if (weather_icon_ != nullptr) {
            lv_label_set_text(weather_icon_, "");
        }
        return;
    }
    char buf[64];
    if (weather_label_ != nullptr) {
        if (weather.humidity >= 0) {
            snprintf(buf, sizeof(buf), "%s  %.0f°C  湿度%d%%", weather.text.c_str(), weather.temp,
                     weather.humidity);
        } else {
            snprintf(buf, sizeof(buf), "%s  %.0f°C", weather.text.c_str(), weather.temp);
        }
        lv_label_set_text(weather_label_, buf);
    }
    if (weather_icon_ != nullptr) {
        lv_label_set_text(weather_icon_, WeatherEmoji(weather.icon));
    }
}

void StandbyPage::SetStocks(const std::vector<StockItem>& stocks) {
    if (display_ == nullptr || root_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    for (int i = 0; i < 3; i++) {
        if (stock_name_[i] == nullptr || stock_price_[i] == nullptr) {
            continue;
        }
        if (i >= static_cast<int>(stocks.size()) || !stocks[i].valid) {
            lv_label_set_text(stock_name_[i], "");
            lv_label_set_text(stock_price_[i], "");
            continue;
        }
        const auto& s = stocks[i];
        lv_label_set_text(stock_name_[i], s.name.c_str());
        char buf[48];
        if (s.change_pct >= 0) {
            snprintf(buf, sizeof(buf), "%.2f +%.2f%%", s.price, s.change_pct);
            lv_obj_set_style_text_color(stock_price_[i], lv_color_hex(kUp), 0);
        } else {
            snprintf(buf, sizeof(buf), "%.2f %.2f%%", s.price, s.change_pct);
            lv_obj_set_style_text_color(stock_price_[i], lv_color_hex(kDown), 0);
        }
        lv_label_set_text(stock_price_[i], buf);
    }
}

void StandbyPage::SetHint(const char* hint) {
    if (display_ == nullptr || root_ == nullptr || hint == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (hint_label_ != nullptr) {
        lv_label_set_text(hint_label_, hint);
    }
}

#endif /* HAVE_LVGL */
