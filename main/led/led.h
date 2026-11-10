#ifndef _LED_H_
#define _LED_H_

#include <cstdint>

class Led {
public:
    virtual ~Led() = default;
    // Set the led state based on the device state
    virtual void OnStateChanged() = 0;

    /* 可选能力：可控彩灯（WS2812）等实现之；默认不支持 */
    virtual bool SupportsColor() const { return false; }
    /** 状态灯自动模式开关（关闭后 OnStateChanged 不再点灯，仅响应手动控制） */
    virtual void SetStatusEnabled(bool enabled) { (void)enabled; }
    virtual bool StatusEnabled() const { return true; }
    virtual void SetColor(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
    virtual void TurnOn() {}
    virtual void TurnOff() {}
};


class NoLed : public Led {
public:
    virtual void OnStateChanged() override {}
};

#endif // _LED_H_
