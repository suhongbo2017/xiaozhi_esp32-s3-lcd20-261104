#ifndef _LED_H_
#define _LED_H_

#include <cstdint>

class Led {
public:
    /** 状态灯工作模式 */
    enum class Mode {
        Follow = 0,   /* 跟随设备状态（空闲灭/监听红/说话绿…） */
        Off = 1,      /* 常灭：任何状态变化都不点灯 */
        Fixed = 2,    /* 固定颜色：任何状态变化都保持这个颜色 */
    };

    virtual ~Led() = default;
    // Set the led state based on the device state
    virtual void OnStateChanged() = 0;

    /* 可选能力：可控彩灯（WS2812）等实现之；默认不支持 */
    virtual bool SupportsColor() const { return false; }

    /** 设置模式并立即生效（子类覆写；默认只映射 Follow/Off） */
    virtual void SetMode(Mode mode) { SetStatusEnabled(mode == Mode::Follow); }
    virtual Mode GetMode() const { return StatusEnabled() ? Mode::Follow : Mode::Off; }

    /** 固定颜色模式下的颜色（子类覆写保存） */
    virtual void SetFixedColor(uint8_t r, uint8_t g, uint8_t b) { SetColor(r, g, b); }
    virtual void GetFixedColor(uint8_t* r, uint8_t* g, uint8_t* b) const {
        if (r) *r = 0;
        if (g) *g = 0;
        if (b) *b = 0;
    }

    /** 状态灯自动模式开关（旧的兼容接口，等价于 SetMode(Follow/Off)） */
    virtual void SetStatusEnabled(bool enabled) { (void)enabled; }
    virtual bool StatusEnabled() const { return true; }
    virtual void SetColor(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
    /** 重新初始化底层驱动（诊断接口临时占用引脚后，用它把驱动接回来） */
    virtual void Reinit() {}

    virtual void TurnOn() {}
    virtual void TurnOff() {}
};


class NoLed : public Led {
public:
    virtual void OnStateChanged() override {}
};

#endif // _LED_H_
