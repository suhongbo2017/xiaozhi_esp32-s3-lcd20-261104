#include "music_page.h"

#include "lvgl_theme.h"

#include <esp_log.h>

#include <cstdio>

#define TAG "MusicPage"

namespace {
constexpr uint32_t kCoverSize = 96;

const lv_font_t* TextFont(Display* display) {
    auto theme = dynamic_cast<LvglTheme*>(display->GetTheme());
    if (theme && theme->text_font()) {
        return theme->text_font()->font();
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

lv_color_t BgColor(Display* display) {
    auto theme = dynamic_cast<LvglTheme*>(display->GetTheme());
    return theme ? theme->background_color() : lv_color_hex(0x000000);
}

lv_color_t FgColor(Display* display) {
    auto theme = dynamic_cast<LvglTheme*>(display->GetTheme());
    return theme ? theme->text_color() : lv_color_hex(0xFFFFFF);
}

void SetFont(lv_obj_t* obj, const lv_font_t* font) {
    if (font != nullptr) {
        lv_obj_set_style_text_font(obj, font, 0);
    }
}
}  // namespace

MusicPage::MusicPage(Display* display) : display_(display) {}

MusicPage::~MusicPage() { Destroy(); }

void MusicPage::CreateIfNeeded() {
    if (root_ != nullptr || display_ == nullptr) {
        return;
    }

    const lv_color_t bg = BgColor(display_);
    const lv_color_t fg = FgColor(display_);
    const int w = display_->width();
    const int h = display_->height();

    root_ = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, w, h);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, bg, 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(root_);

    /* 封面占位（后续可替换为真实封面图）*/
    cover_ = lv_obj_create(root_);
    lv_obj_remove_style_all(cover_);
    lv_obj_set_size(cover_, kCoverSize, kCoverSize);
    lv_obj_set_style_radius(cover_, 12, 0);
    lv_obj_set_style_bg_color(cover_, fg, 0);
    lv_obj_set_style_bg_opa(cover_, LV_OPA_20, 0);
    lv_obj_align(cover_, LV_ALIGN_TOP_MID, 0, 12);

    cover_icon_ = lv_label_create(cover_);
    lv_label_set_text(cover_icon_, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(cover_icon_, fg, 0);
    SetFont(cover_icon_, IconFont(display_));
    lv_obj_center(cover_icon_);

    title_label_ = lv_label_create(root_);
    lv_label_set_long_mode(title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(title_label_, w - 16);
    lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title_label_, fg, 0);
    SetFont(title_label_, TextFont(display_));
    lv_label_set_text(title_label_, "");
    lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, kCoverSize + 20);

    artist_label_ = lv_label_create(root_);
    lv_label_set_long_mode(artist_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(artist_label_, w - 16);
    lv_obj_set_style_text_align(artist_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(artist_label_, fg, 0);
    lv_obj_set_style_text_opa(artist_label_, LV_OPA_70, 0);
    SetFont(artist_label_, TextFont(display_));
    lv_label_set_text(artist_label_, "");
    lv_obj_align_to(artist_label_, title_label_, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    /* 三行歌词：上一句 / 当前句 / 下一句 */
    lyric_prev_ = lv_label_create(root_);
    lv_label_set_long_mode(lyric_prev_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lyric_prev_, w - 12);
    lv_obj_set_style_text_align(lyric_prev_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lyric_prev_, fg, 0);
    lv_obj_set_style_text_opa(lyric_prev_, LV_OPA_50, 0);
    SetFont(lyric_prev_, TextFont(display_));
    lv_label_set_text(lyric_prev_, "");
    lv_obj_align(lyric_prev_, LV_ALIGN_TOP_MID, 0, kCoverSize + 76);

    lyric_cur_ = lv_label_create(root_);
    lv_label_set_long_mode(lyric_cur_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lyric_cur_, w - 12);
    lv_obj_set_style_text_align(lyric_cur_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lyric_cur_, fg, 0);
    SetFont(lyric_cur_, TextFont(display_));
    lv_label_set_text(lyric_cur_, "");
    lv_obj_align(lyric_cur_, LV_ALIGN_TOP_MID, 0, kCoverSize + 104);

    lyric_next_ = lv_label_create(root_);
    lv_label_set_long_mode(lyric_next_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lyric_next_, w - 12);
    lv_obj_set_style_text_align(lyric_next_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lyric_next_, fg, 0);
    lv_obj_set_style_text_opa(lyric_next_, LV_OPA_50, 0);
    SetFont(lyric_next_, TextFont(display_));
    lv_label_set_text(lyric_next_, "");
    lv_obj_align(lyric_next_, LV_ALIGN_TOP_MID, 0, kCoverSize + 150);

    /* 进度条 + 时间 */
    bar_ = lv_bar_create(root_);
    lv_obj_set_size(bar_, w - 32, 6);
    lv_bar_set_range(bar_, 0, 1000);
    lv_bar_set_value(bar_, 0, LV_ANIM_OFF);
    lv_obj_align(bar_, LV_ALIGN_BOTTOM_MID, 0, -28);

    time_label_ = lv_label_create(root_);
    lv_obj_set_style_text_color(time_label_, fg, 0);
    lv_obj_set_style_text_opa(time_label_, LV_OPA_70, 0);
    SetFont(time_label_, TextFont(display_));
    lv_label_set_text(time_label_, "00:00 / 00:00");
    lv_obj_align(time_label_, LV_ALIGN_BOTTOM_MID, 0, -6);

    ESP_LOGI(TAG, "music page created (%dx%d)", w, h);
}

void MusicPage::Destroy() {
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    cover_ = cover_icon_ = title_label_ = artist_label_ = nullptr;
    lyric_prev_ = lyric_cur_ = lyric_next_ = nullptr;
    bar_ = time_label_ = nullptr;
}

void MusicPage::Show(const char* title, const char* artist) {
    if (display_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    CreateIfNeeded();
    SetTitle(title, artist);
}

void MusicPage::SetTitle(const char* title, const char* artist) {
    if (display_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (root_ == nullptr) {
        return;
    }
    lv_label_set_text(title_label_, title != nullptr ? title : "");
    lv_label_set_text(artist_label_, artist != nullptr ? artist : "");
}

void MusicPage::SetLyric(const char* prev, const char* current, const char* next) {
    if (display_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (root_ == nullptr) {
        return;
    }
    lv_label_set_text(lyric_prev_, prev != nullptr ? prev : "");
    lv_label_set_text(lyric_cur_, current != nullptr ? current : "");
    lv_label_set_text(lyric_next_, next != nullptr ? next : "");
}

void MusicPage::SetProgress(uint32_t pos_ms, uint32_t dur_ms) {
    if (display_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (root_ == nullptr) {
        return;
    }
    if (dur_ms > 0) {
        uint32_t permille = static_cast<uint32_t>(static_cast<uint64_t>(pos_ms) * 1000 / dur_ms);
        lv_bar_set_value(bar_, static_cast<int32_t>(permille > 1000 ? 1000 : permille), LV_ANIM_OFF);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%02lu:%02lu / %02lu:%02lu",
             static_cast<unsigned long>(pos_ms / 60000), static_cast<unsigned long>((pos_ms / 1000) % 60),
             static_cast<unsigned long>(dur_ms / 60000), static_cast<unsigned long>((dur_ms / 1000) % 60));
    lv_label_set_text(time_label_, buf);
}

void MusicPage::Hide() {
    if (display_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    Destroy();
}
