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
void StandbyPage::SetBackground(const lv_image_dsc_t*) {}

#else /* HAVE_LVGL */

#include <cstdio>

#include "lvgl_theme.h"

namespace {

/* ---------------- 版式（240x320） ----------------
 * 顶部：大时钟（48px 数字）+ 日期
 * 中部：天气（温度 + 描述，整行居中）
 * 下部：最多 3 行指数/个股
 * 底部：提示文字
 */
constexpr int kPad = 10;
constexpr int kClockY = 14;
constexpr int kDateY = 78;
constexpr int kWeatherY = 118;
constexpr int kStockY = 170;
constexpr int kStockLineH = 32;

/* 三列：名称 | 价格(右对齐) | 涨跌幅(右对齐)，宽度加起来不超 240-2*kPad */
constexpr int kNameW = 48;         /* “纳指/标普/上证” 2 字 = 40px */
constexpr int kPriceX = kPad + kNameW + 2;
constexpr int kPriceW = 96;
constexpr int kPctX = kPriceX + kPriceW + 4;
constexpr int kPctW = 240 - kPad - kPctX;

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

void SetFont(lv_obj_t* obj, const lv_font_t* font) {
    if (font != nullptr) {
        lv_obj_set_style_text_font(obj, font, 0);
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

    /* 天气：整行居中（图标字体缺字会显示方块，这里只用文字，居中靠 text_align） */
    weather_label_ = CreateLabel(root_, TextFont(display_), 0xFFFFFF);
    lv_obj_set_pos(weather_label_, 0, kWeatherY);
    lv_obj_set_width(weather_label_, w);
    lv_obj_set_style_text_align(weather_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(weather_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(weather_label_, "天气加载中…");

    /* 指数/个股：名称 + 价格/涨跌幅 */
    for (int i = 0; i < 3; i++) {
        const int y = kStockY + i * kStockLineH;

        stock_name_[i] = CreateLabel(root_, TextFont(display_), 0xC8D4E0);
        lv_obj_set_pos(stock_name_[i], kPad, y);
        lv_obj_set_width(stock_name_[i], kNameW);
        lv_label_set_long_mode(stock_name_[i], LV_LABEL_LONG_CLIP);
        lv_label_set_text(stock_name_[i], "");

        stock_price_[i] = CreateLabel(root_, &lv_font_montserrat_24, kFlat);
        lv_obj_set_pos(stock_price_[i], kPriceX, y - 3);
        lv_obj_set_width(stock_price_[i], kPriceW);
        lv_obj_set_style_text_align(stock_price_[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(stock_price_[i], "");

        stock_pct_[i] = CreateLabel(root_, &lv_font_montserrat_14, kFlat);
        lv_obj_set_pos(stock_pct_[i], kPctX, y + 4);
        lv_obj_set_width(stock_pct_[i], kPctW);
        lv_obj_set_style_text_align(stock_pct_[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(stock_pct_[i], "");
    }

    hint_label_ = CreateLabel(root_, TextFont(display_), kAccent);
    lv_obj_set_width(hint_label_, w);
    lv_obj_set_style_text_align(hint_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint_label_, 0, h - 30);
    lv_label_set_text(hint_label_, "说“你好小智”开始对话");
}

void StandbyPage::Destroy() {
    shown_ = false;
    bg_image_ = nullptr;
    bg_dim_ = nullptr;
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    clock_label_ = nullptr;
    date_label_ = nullptr;
    weather_label_ = nullptr;
    hint_label_ = nullptr;
    for (int i = 0; i < 3; i++) {
        stock_name_[i] = nullptr;
        stock_price_[i] = nullptr;
        stock_pct_[i] = nullptr;
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
            lv_label_set_text(weather_label_, "天气暂无数据");
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
            if (stock_pct_[i] != nullptr) {
                lv_label_set_text(stock_pct_[i], "");
            }
            continue;
        }
        const auto& s = stocks[i];
        lv_label_set_text(stock_name_[i], s.name.c_str());

        char price[24];
        if (s.price >= 1000.0f) {
            snprintf(price, sizeof(price), "%.0f", s.price);   /* 万点级不显示小数，省宽度 */
        } else {
            snprintf(price, sizeof(price), "%.2f", s.price);
        }
        lv_label_set_text(stock_price_[i], price);

        char pct[16];
        snprintf(pct, sizeof(pct), "%+.2f%%", s.change_pct);
        if (stock_pct_[i] != nullptr) {
            lv_label_set_text(stock_pct_[i], pct);
            const uint32_t color = s.change_pct > 0.0001f ? kUp : (s.change_pct < -0.0001f ? kDown : kFlat);
            lv_obj_set_style_text_color(stock_pct_[i], lv_color_hex(color), 0);
        }
        const uint32_t pcolor = s.change_pct > 0.0001f ? kUp : (s.change_pct < -0.0001f ? kDown : kFlat);
        lv_obj_set_style_text_color(stock_price_[i], lv_color_hex(pcolor), 0);
    }
}

void StandbyPage::SetBackground(const lv_image_dsc_t* dsc) {
    if (display_ == nullptr || root_ == nullptr ) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (dsc == nullptr) {
        if (bg_dim_ != nullptr) {
            lv_obj_delete(bg_dim_);
            bg_dim_ = nullptr;
        }
        if (bg_image_ != nullptr) {
            lv_obj_delete(bg_image_);
            bg_image_ = nullptr;
        }
        return;
    }
    if (bg_image_ == nullptr) {
        bg_image_ = lv_image_create(root_);
        lv_obj_remove_style_all(bg_image_);
        lv_obj_set_size(bg_image_, display_->width(), display_->height());
    }
    if (bg_dim_ == nullptr) {
        /* 半透明黑遮罩：图片上压白字要看得清 */
        bg_dim_ = lv_obj_create(root_);
        lv_obj_remove_style_all(bg_dim_);
        lv_obj_set_size(bg_dim_, display_->width(), display_->height());
        lv_obj_set_style_bg_color(bg_dim_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(bg_dim_, 110, 0);   /* 0~255，约 43% */
        lv_obj_clear_flag(bg_dim_, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_image_set_src(bg_image_, dsc);
    lv_obj_set_pos(bg_image_, 0, 0);
    lv_obj_set_pos(bg_dim_, 0, 0);
    /* 顺序：图片(最底) -> 遮罩 -> 文字 */
    lv_obj_move_background(bg_dim_);
    lv_obj_move_background(bg_image_);
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
