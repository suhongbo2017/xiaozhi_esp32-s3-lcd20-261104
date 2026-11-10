# 板载 WS2812 状态灯说明（2026-11-10）

## 它为什么会"自己亮"

SKU `esp32s3-lcd20` 的板载灯 = **单颗 WS2812（GPIO48）**，由 `SingleLed` 当**状态指示灯**用：

| 设备状态 | 灯效 |
|---|---|
| `starting` | 蓝色 100ms 快闪 |
| `wifi_configuring` | 蓝色 500ms 慢闪 |
| `activating` | 绿色 500ms 慢闪 |
| `connecting` | 蓝色常亮 |
| `listening` | **红色常亮**（检测到人声时更亮） |
| `speaking` / `notifying`（含音乐播放） | **绿色常亮** |
| `idle` | 熄灭 |
| `upgrading` | 绿色 100ms 快闪 |

所以"说句话之后灯亮着"通常是**正常**的：对话中处于 listening（红）/ speaking（绿）；
如果 VAD 一直没判定静音，设备停在 listening，灯就一直是红色。

## 为什么说"关闭LED灯"没关掉它

当时只有 `self.lamp.*`（GPIO18 台灯/继电器执行器）三个工具，没有板载灯工具，
所以大模型把"LED灯"理解成了台灯 → 调 `self.lamp.turn_off`，板载灯自然没变，
随后它回答"没有控制板载LED灯的功能"。

## 本次新增：self.led.*（3 个工具，工具总数 18 → 23）

| 工具 | 参数 | 说明 |
|---|---|---|
| `self.led.set_color` | `r,g,b` (0-255) | 设置颜色并点亮 |
| `self.led.turn_on` | – | 低亮度点亮 |
| `self.led.turn_off` | – | 熄灭 |

实现：`Led` 接口新增 `SupportsColor()/SetColor()/TurnOn()/TurnOff()`（默认不支持），
`SingleLed` 实现之；`mcp_server.cc` 在 LED 支持颜色时才注册这三个工具（其它板型不受影响）。

**注意**：板载灯仍是状态灯，手动设置的颜色会在**下一次状态变化**（开始/结束对话、播放音乐）时被覆盖。

---

## 为什么 turn_off 后"看起来没关掉"（2026-11-10 二修）

串口实录：
```
I (1206038) MCP tools/call: self.led.turn_off     ← 工具确实执行了，灯也真的灭了
I (1206578) << 好啦，彩灯已经关掉啰。
I (1209168) StateMachine: State: speaking -> listening   ← 紧接着状态变化
                                              → OnStateChanged() 把灯重新点亮成红色
I (1215798) MCP tools/call: self.led.turn_off     ← 第二次同样
I (1218658) StateMachine: State: speaking -> listening   ← 又被点亮
```
即：灯被熄灭后不到 1 秒，状态从 `speaking` 回到 `listening`，状态灯逻辑立刻把它点亮成红色，
用户感知就是"没关掉"。

### 修复：状态灯自动模式开关（持久化）

| 行为 | 说明 |
|---|---|
| `self.led.turn_off` / `set_color` / `turn_on` | 调用时**自动关闭"状态灯自动模式"**，之后设备状态变化不再点灯 |
| `self.led.set_status_enabled {enabled:true/false}` | 显式恢复/关闭自动模式 |
| 持久化 | NVS `namespace="led", key="status_enabled"`，重启后保持 |
| 日志 | `SingleLed: status LED auto mode -> off` |

实现：`Led` 接口新增 `SetStatusEnabled/StatusEnabled`（默认无操作），`SingleLed` 用成员变量 +
NVS 保存；`OnStateChanged()` 在关闭时直接 `TurnOff()` 并返回。

工具总数 18 → **24**。
