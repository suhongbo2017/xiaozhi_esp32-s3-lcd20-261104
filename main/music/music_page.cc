#include "music_page.h"

#include <esp_log.h>

#define TAG "MusicPage"

#ifndef HAVE_LVGL
/* 无 LVGL 的板型（OLED / 无屏）：音乐照常播放，只是没有页面 */

MusicPage::MusicPage(Display* display) : display_(display) {}
MusicPage::~MusicPage() = default;
void MusicPage::CreateIfNeeded() {}
void MusicPage::Destroy() {}
void MusicPage::Show(const char* title, const char* artist) { (void)title; (void)artist; }
void MusicPage::SetTitle(const char* title, const char* artist) { (void)title; (void)artist; }
void MusicPage::SetLyric(const char* prev, const char* current, const char* next) {
    (void)prev;
    (void)current;
    (void)next;
}
void MusicPage::SetProgress(uint32_t pos_ms, uint32_t dur_ms) { (void)pos_ms; (void)dur_ms; }
void MusicPage::Hide() {}

#else  /* HAVE_LVGL */

#include <cstdio>

#include "lvgl_theme.h"

namespace {

/* ---------------- 版式（240x320 适配） ---------------- */
constexpr int kCoverSize = 92;
constexpr int kCoverY = 10;
constexpr int kTitleY = 112;
constexpr int kArtistY = 138;
constexpr int kPrevY = 176;
constexpr int kCurY = 206;
constexpr int kNextY = 240;
constexpr int kBarH = 10;
constexpr int kBarBottom = 48;
constexpr int kPad = 10;

/* ---------------- 配色：深蓝渐变底 + 青色强调 ---------------- */
constexpr uint32_t kBgTop = 0x0E1E33;
constexpr uint32_t kBgBottom = 0x05090F;
constexpr uint32_t kAccent = 0x00E5FF; /* 当前歌词 / 进度条 */
constexpr uint32_t kCoverA = 0x1E88E5; /* 封面渐变起 */
constexpr uint32_t kCoverB = 0x7C4DFF; /* 封面渐变止 */
constexpr uint32_t kDim = 0x8FA6BF;    /* 次要文字 */
constexpr uint32_t kBarBg = 0x1B2C42;

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

void SetFont(lv_obj_t* obj, const lv_font_t* font) {
    if (font != nullptr) {
        lv_obj_set_style_text_font(obj, font, 0);
    }
}

/* 单行 + 超长省略：保证三行歌词永不重叠 */
lv_obj_t* MakeLine(lv_obj_t* parent, int width, uint32_t color, lv_opa_t opa, int y,
                   const lv_font_t* font) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    SetFont(label, font);
    lv_label_set_text(label, "");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

}  // namespace

MusicPage::MusicPage(Display* display) : display_(display) {}

MusicPage::~MusicPage() { Destroy(); }

void MusicPage::CreateIfNeeded() {
    if (root_ != nullptr || display_ == nullptr) {
        return;
    }

    const int w = display_->width();
    const int h = display_->height();
    const int line_w = w - 2 * kPad;

    /* 页面底：竖向渐变，深蓝 → 近黑 */
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

    /* 封面：蓝→紫渐变圆角块 + 音符图标 */
    cover_ = lv_obj_create(root_);
    lv_obj_remove_style_all(cover_);
    lv_obj_set_size(cover_, kCoverSize, kCoverSize);
    lv_obj_set_style_radius(cover_, 16, 0);
    lv_obj_set_style_bg_color(cover_, lv_color_hex(kCoverA), 0);
    lv_obj_set_style_bg_grad_color(cover_, lv_color_hex(kCoverB), 0);
    lv_obj_set_style_bg_grad_dir(cover_, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(cover_, LV_OPA_COVER, 0);
    lv_obj_align(cover_, LV_ALIGN_TOP_MID, 0, kCoverY);

    cover_icon_ = lv_label_create(cover_);
    lv_label_set_text(cover_icon_, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(cover_icon_, lv_color_hex(0xFFFFFF), 0);
    SetFont(cover_icon_, IconFont(display_));
    lv_obj_center(cover_icon_);

    /* 标题：单行跑马灯 */
    title_label_ = lv_label_create(root_);
    lv_obj_set_width(title_label_, line_w);
    lv_label_set_long_mode(title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_hex(0xFFFFFF), 0);
    SetFont(title_label_, TextFont(display_));
    lv_label_set_text(title_label_, "");
    lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, kTitleY);

    /* 歌手 + 三行歌词：固定 y、单行省略 */
    artist_label_ = MakeLine(root_, line_w, kDim, LV_OPA_90, kArtistY, TextFont(display_));
    lyric_prev_ = MakeLine(root_, line_w, kDim, LV_OPA_60, kPrevY, TextFont(display_));
    lyric_cur_ = MakeLine(root_, line_w, kAccent, LV_OPA_COVER, kCurY, TextFont(display_));
    lyric_next_ = MakeLine(root_, line_w, kDim, LV_OPA_60, kNextY, TextFont(display_));

    /* 进度条：加粗 10px、圆角、青色指示器 */
    bar_ = lv_bar_create(root_);
    lv_obj_remove_style_all(bar_);
    lv_obj_set_size(bar_, line_w, kBarH);
    lv_bar_set_range(bar_, 0, 1000);
    lv_bar_set_value(bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_, lv_color_hex(kBarBg), 0);
    lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar_, kBarH / 2, 0);
    lv_obj_set_style_bg_color(bar_, lv_color_hex(kAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_, kBarH / 2, LV_PART_INDICATOR);
    lv_obj_align(bar_, LV_ALIGN_BOTTOM_MID, 0, -kBarBottom);

    /* 时间 */
    time_label_ = lv_label_create(root_);
    lv_obj_set_style_text_color(time_label_, lv_color_hex(kDim), 0);
    SetFont(time_label_, TextFont(display_));
    lv_label_set_text(time_label_, "00:00 / 00:00");
    lv_obj_align(time_label_, LV_ALIGN_BOTTOM_MID, 0, -18);

    ESP_LOGI(TAG, "music page created (%dx%d, bar=%dpx)", w, h, kBarH);
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
    if (root_ == nullptr) {
        return;
    }
    lv_label_set_text(title_label_, title != nullptr ? title : "");
    lv_label_set_text(artist_label_, artist != nullptr ? artist : "");
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
             static_cast<unsigned long>(pos_ms / 60000),
             static_cast<unsigned long>((pos_ms / 1000) % 60),
             static_cast<unsigned long>(dur_ms / 60000),
             static_cast<unsigned long>((dur_ms / 1000) % 60));
    lv_label_set_text(time_label_, buf);
}

void MusicPage::Hide() {
    if (display_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    Destroy();
}

#endif  // HAVE_LVGL
