# P8：状态灯（WS2812 / GPIO48）三模式修复

## 现象（用户反馈）

1. 网页「灯光」选「常灭」无效
2. 选「固定颜色」应用后 LED **也不亮**
3. 但**放音乐时灯会自己亮**（不是用户要求的）

## 根因：原来根本没有"固定颜色"这个模式

`main/led/single_led.cc` 里只有一个布尔 `status_enabled_`：

```cpp
void SingleLed::OnStateChanged() {
    if (!status_enabled_) { TurnOff(); return; }   // ← 一旦关掉，强制熄灭
    ... 按设备状态刷颜色
}
```

所以网页的"固定颜色"实际执行的是：

```cpp
led->SetStatusEnabled(false);   // 禁止跟随状态
led->SetColor(r, g, b);
led->TurnOn();                  // 亮一瞬间
```

下一次设备状态变化（空闲→监听→说话，放音乐也会触发）就走到 `!status_enabled_` 分支 →
**TurnOff()**。于是"应用颜色后不亮 / 亮一下就灭"，而"放音乐时自己亮"其实是 Follow 模式在刷状态色。

另外 `/api/status` 的 `led_color` 是**硬编码 `#00b3ff`**，所以网页取色器也永远显示错。

## 修复：Led 加真正的三态模式

```cpp
enum class Mode { Follow, Off, Fixed };
virtual void SetMode(Mode);          // 立即生效
virtual Mode GetMode() const;
virtual void SetFixedColor(r,g,b);
virtual void GetFixedColor(...);
```

`SingleLed::OnStateChanged()` 开头先判模式：

```cpp
if (mode_ == Mode::Off)   { TurnOff(); return; }                 // 常灭：谁说都不亮
if (mode_ == Mode::Fixed) { SetColor(fixed_); TurnOn(); return; } // 固定色：状态变化不改
... 原 Follow 逻辑
```

- NVS：`led/mode`（`on|off|color`）+ `led/color`（`#RRGGBB`），**兼容旧键** `status_enabled`
- `SetStatusEnabled(bool)` 保留，映射为 `SetMode(Follow/Off)`，旧调用不失效
- 模式切换立即 `ApplyMode()`，不用等下次状态变化
- 其它板型的 `CircularStrip` / `GpioLed` 不覆写即走基类默认实现，行为不变

## 网页与语音对齐

| 入口 | 改动 |
|---|---|
| `GET /api/status` | `led_mode` / `led_color` 从 **Led 对象**读真实值（去掉硬编码） |
| `POST /api/settings` | 直接 `SetFixedColor()` + `SetMode()`，NVS 由 Led 统一写，不再两边各写一套键 |
| `self.led.turn_off` | → `Mode::Off`（**持久**，之后状态变化不复活） |
| `self.led.set_color` | → `SetFixedColor` + `Mode::Fixed` |
| `self.led.turn_on` | → `Mode::Follow`（恢复自动） |
| `self.led.test` | 自检时临时切 `Fixed`，**结束后恢复用户原来的模式**（原来会把灯永久关掉） |

## 实测

```
POST {"led_mode":"color","led_color":"#FF0000"}  → SingleLed: led mode -> fixed color / AdminServer: led -> color (mode=2)
POST {"led_mode":"off"}                          → SingleLed: led mode -> off        / AdminServer: led -> off (mode=1)
POST {"led_mode":"color","led_color":"#00FF00"}  → SingleLed: led mode -> fixed color
POST {"led_mode":"on"}                           → SingleLed: led mode -> follow     / AdminServer: led -> on (mode=0)

GET /api/status → {"led_mode":"color","led_color":"#FF0000"}
重启后            → {"led_mode":"color","led_color":"#FF0000"}   ← NVS 持久化 ✓
```

## 需要人工确认的两项（我做不了）

1. 设为 `#FF0000` 时灯**是不是真的红**（RMT 写 WS2812 只能看实物）
2. 设为固定色/常灭后**开口说一句话**（触发 空闲→监听→说话→空闲），灯**是否保持不变**
   —— 这是本次修复的核心目的

## 排查过程中搞清楚的事实

- 板载状态灯 = **WS2812 单颗**，接 **GPIO48**（`BUILTIN_LED_GPIO`），走 `led_strip` + RMT
- 用户实测把 GPIO38 配成开关点不亮 → 排除 38，确认是 48
- `LAMP_GPIO = GPIO18` 是另一个"板载灯执行器"（继电器/MOS），暴露 `self.lamp.*`，别和状态灯混
