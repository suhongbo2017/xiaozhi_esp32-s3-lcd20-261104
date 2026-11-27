#ifndef QR_PAGE_H
#define QR_PAGE_H

#include "display.h"

#include <string>

#ifdef HAVE_LVGL
#ifdef __cplusplus
extern "C" {
#endif
#include <lvgl.h>
#ifdef __cplusplus
}
#endif
#endif

/**
 * 二维码页：把局域网管理页地址（http://<ip>:8080/）显示成二维码，
 * 手机（微信扫一扫/相机）扫一下就能打开管理页面，不用手输 IP。
 *
 * 触发：
 *  - BOOT 键双击：显示/隐藏
 *  - 网页「系统 → 显示二维码到屏幕」
 *  - 自动隐藏（默认 60 秒）
 */
class QrPage {
public:
    static QrPage& GetInstance();

    /** 显示二维码；url 为空时自动用当前设备 IP 拼 http://<ip>:8080/ */
    bool Show(Display* display, const std::string& url = "", int seconds = 60);
    void Hide();
    bool visible() const;

private:
    QrPage() = default;

    Display* display_ = nullptr;
    int hide_timer_s_ = 0;
#ifdef HAVE_LVGL
    lv_obj_t* root_ = nullptr;
    lv_obj_t* qr_ = nullptr;
    lv_obj_t* url_label_ = nullptr;
#endif
};

#endif  // QR_PAGE_H
