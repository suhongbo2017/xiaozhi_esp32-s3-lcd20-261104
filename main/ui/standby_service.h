#ifndef STANDBY_SERVICE_H
#define STANDBY_SERVICE_H

#include "standby_page.h"

#include <atomic>
#include <string>
#include <vector>

class Display;

/**
 * 待机页服务：时钟 / 日期 / 天气 / 指数行情
 *
 * 设计：
 *  - 独立 FreeRTOS 任务（1s tick）：只在"空闲且未在放音乐"时显示待机页；
 *  - 天气/行情每 5 分钟拉一次（服务端有 5 分钟缓存，设备侧不额外压服务器）；
 *  - 时间来自系统时间（激活/OTA 时已由服务端下发，localtime 即北京时间）；
 *  - 配置存 NVS：standby_enabled / standby_city / standby_symbols，可网页改。
 */
class StandbyService {
public:
    static StandbyService& GetInstance();

    /** Application::Initialize 里调用（display 已就绪） */
    void Initialize(Display* display);

    /** 网络可用（连接成功）后调用：允许拉取天气/行情 */
    void OnNetworkReady();

    /** 是否处于"应该显示待机页"的状态（idle=true，其它=false） */
    void SetIdle(bool idle);

    /** 网页改配置后调用：重载 NVS 并立即刷一次 */
    void ReloadConfig();

    /* ---- 配置读写（供 AdminServer 用） ---- */
    bool enabled() const { return enabled_.load(); }
    void SetEnabled(bool enabled);
    std::string city() const;
    void SetCity(const std::string& city);
    std::string symbols() const;
    void SetSymbols(const std::string& symbols);

private:
    StandbyService() = default;
    ~StandbyService() = default;

    void LoadConfig();
    static void TaskEntry(void* arg);
    void Tick();

    Display* display_ = nullptr;
    std::atomic<bool> idle_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> net_ready_{false};
    std::atomic<bool> dirty_{true};
    std::string city_ = "上海";
    std::string symbols_ = "usIXIC,usINX,sh000001";
    volatile bool started_ = false;

    /* 最近一次拉取的数据：页面首次显示时补一次（页面未创建时 setter 是空操作） */
    StandbyPage::Weather last_weather_;
    std::vector<StandbyPage::StockItem> last_stocks_;
    bool have_weather_ = false;
};

#endif  // STANDBY_SERVICE_H
