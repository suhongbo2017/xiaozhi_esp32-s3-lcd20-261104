#ifndef STANDBY_PAGE_H
#define STANDBY_PAGE_H

#include "display.h" /* display.h 内部按需定义 HAVE_LVGL 并引入 lvgl.h */

#include <cstdint>
#include <string>
#include <vector>

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
 * 待机页（240x320）：大时钟 + 日期 + 天气 + 指数/个股。
 *
 * 设计取舍（与 MusicPage 一致）：
 *  - 直接在**新建的 lv_screen** 上绘制，并挂到 display 的 screen 之上；
 *    不修改 LcdDisplay 内部布局，其它板型/无屏板走空实现；
 *  - 时钟数字用 LVGL 内置 Montserrat 48（切换零成本），中文用主题 text_font（CBin）；
 *  - 所有 LVGL 调用都在 DisplayLockGuard 保护下执行；
 *  - 只在 idle 状态显示，进入监听/说话/播放音乐时立即隐藏。
 */
class StandbyPage {
public:
    struct StockItem {
        std::string name;
        float price = 0;
        float change = 0;
        float change_pct = 0;
        bool valid = false;
    };
    struct Weather {
        std::string text;   /* 多云 / 小雨 ... */
        float temp = 0;
        int humidity = -1;
        int icon = 2;       /* 0=晴 1=多云 2=阴 3=雨 4=雪 5=雷 6=雾 */
        bool valid = false;
    };

    explicit StandbyPage(Display* display);
    ~StandbyPage();

    void CreateIfNeeded();
    void Destroy();

    void Show();
    void Hide();
    bool visible() const;

    /** 由服务（每秒/每次拉取后）调用，页面不可见时也安全 */
    void SetClock(const char* hhmm, const char* date_line);
    void SetWeather(const Weather& weather);
    void SetStocks(const std::vector<StockItem>& stocks);
    void SetHint(const char* hint);

private:
    Display* display_ = nullptr;
    bool shown_ = false;
#ifdef HAVE_LVGL
    lv_obj_t* root_ = nullptr;
    lv_obj_t* clock_label_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* weather_icon_ = nullptr;
    lv_obj_t* weather_label_ = nullptr;
    lv_obj_t* stock_name_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* stock_price_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* hint_label_ = nullptr;
#endif
};

#endif  // STANDBY_PAGE_H
