#ifndef MUSIC_PAGE_H
#define MUSIC_PAGE_H

#include "display.h"  /* 注意：display.h 内部按需定义 HAVE_LVGL 并引入 lvgl.h */

#include <cstdint>

#ifdef HAVE_LVGL
#ifdef __cplusplus
extern "C" {
#endif
#include <lvgl.h>
#ifdef __cplusplus
}
#endif
#endif /* HAVE_LVGL */

/**
 * 音乐播放页（240x320 友好）。
 *
 * 设计取舍：
 *  - 直接挂在当前 LVGL screen 上（全屏容器 + move_foreground），
 *    不改动 LcdDisplay 的内部布局，避免影响其它板型；
 *  - 文字一律使用主题里的 text_font（CBin 动态字形），因此中文歌词可以正常显示；
 *  - 所有 LVGL 调用都在 DisplayLockGuard 保护下执行。
 */
class MusicPage {
public:
    explicit MusicPage(Display* display);
    ~MusicPage();

    void Show(const char* title, const char* artist);
    void SetLyric(const char* prev, const char* current, const char* next);
    void SetProgress(uint32_t pos_ms, uint32_t dur_ms);
    void SetTitle(const char* title, const char* artist);
    void Hide();
#ifdef HAVE_LVGL
    bool visible() const { return root_ != nullptr; }
#else
    bool visible() const { return false; }
#endif

private:
    void CreateIfNeeded();
    void Destroy();

    Display* display_ = nullptr;
#ifdef HAVE_LVGL
    lv_obj_t* root_ = nullptr;
    lv_obj_t* cover_ = nullptr;
    lv_obj_t* cover_icon_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* artist_label_ = nullptr;
    lv_obj_t* lyric_prev_ = nullptr;
    lv_obj_t* lyric_cur_ = nullptr;
    lv_obj_t* lyric_next_ = nullptr;
    lv_obj_t* bar_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
#endif
};

#endif  // MUSIC_PAGE_H
