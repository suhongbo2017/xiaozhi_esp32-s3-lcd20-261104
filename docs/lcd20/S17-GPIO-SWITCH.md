# P7：网页自定义 GPIO 开关（语音可控制）

## 用法

网页新增 **「开关」** tab：

```
我的开关
  打印机灯光          [关]  [翻转]  [删]
  GPIO12 · 高电平 · 关
  可以用语音控制，例如说「打开打印机灯光」

新增 / 编辑
  名称      [打印机灯光]
  GPIO 号   [12]
  触发电平  [高电平点亮 / 低电平点亮（继电器）]
  初始状态  [关闭 / 打开]
  [添加这一路]
```

加完就能动嘴：**"打开打印机灯光" / "把打印机灯光关了" / "翻转一下台灯"**。
大模型会先调 `self.gpio.list` 拿到开关名，再调 `self.gpio.set`。

## ⚠️ 本板 GPIO3 已被占用

这块板 **GPIO3 是 PDM 麦克风时钟**（`AUDIO_PDM_MIC_GPIO_CLK`），配成灯会把麦克风搞坏。
填 GPIO3 会被直接拒绝：

```
POST /api/gpio {"devices":[{"name":"打印机灯光","gpio":3}]}
  -> {"ok":false,"error":"GPIO3 是本板硬件占用，不能用"}
```

**本板可用引脚**：`1, 2, 12, 13, 14, 16, 17, 21, 38, 39, 40, 41, 42, 47`

被占用的：

| 类别 | 引脚 |
|---|---|
| 板载硬件 | 0 启动键、3 麦克风时钟、4 屏 CLK、5 屏 MOSI、6 屏 RST、7 屏 DC、8/9/10 喇叭 I2S、11 麦克风数据、15 屏 CS、18 灯带、45 背光、48 状态灯 |
| 固定禁用 | 19/20 USB、26~37 闪存+PSRAM、43/44 串口、46 启动模式 strapping |

## 实现

### 配置与持久化

NVS 命名空间 `gpio`，键 `config` 存 JSON 数组：

```json
[{"name":"打印机灯光","gpio":12,"active_high":true,"initial_on":false}]
```

最多 4 路。保存时逐条校验（名称非空/不重复、0~48、未被占用、GPIO 不重复），
**全部通过才落盘并立即 `gpio_config()` + 应用初始电平**；旧配置的引脚会 `gpio_reset_pin()`。

### 冲突检测的两层来源

```cpp
Board::ReservedGpios()      // 新增虚函数，各板自己声明硬件占用的脚
  └─ esp32s3-lcd20 覆写：0,3,4,5,6,7,8,9,10,11,15,18,45,48
kFixedReserved[]            // 通用危险脚：USB/UART/闪存/strapping
```

核心层只依赖 `Board` 接口，不 include 具体板子的 `config.h`（符合分层要求）。

### 语音控制（MCP）

```cpp
self.gpio.list                 -> "1) 打印机灯光 [GPIO12] = 开；"
self.gpio.set(device, state)   -> device 支持名称或序号，state: on/off/toggle
```

工具在 `Application::Initialize()` 里注册一次，描述里说明"先 list 再 set"，
所以**改配置不需要重启**、也不需要重新注册工具。

## 实测

```
GET  /api/gpio -> {"devices":[],"reserved":[0,3,4,5,6,7,8,9,10,11,15,18,45,48]}
POST /api/gpio {devices:[{name:"打印机灯光",gpio:3}]}
     -> {"ok":false,"error":"GPIO3 是本板硬件占用，不能用"}          ← 挡住危险脚
POST /api/gpio {devices:[{name:"打印机灯光",gpio:12,initial_on:true}]}
     -> {"ok":true}  → 重启后 gpio[12] 打印机灯光 -> ON (init on)     ← 持久化生效
POST {"name":"打印机灯光","state":"on"}     -> 打印机灯光 已打开（GPIO12，高电平）
POST {"name":"打印机灯光","state":"toggle"} -> 打印机灯光 已关闭（GPIO12，高电平）
POST {"name":"1","state":"off"}            -> 支持用序号控制
POST {"name":"日光灯","state":"on"}        -> 没有这个开关：日光灯，可用：打印机灯光
串口: GpioControl: mcp tools registered (self.gpio.list / self.gpio.set)
```

固件 3.00MB / 3.94MB（76%）。

## 限制

- 只做**开关输出**（高/低电平），没有 PWM/调光、没有输入型（按钮/传感器）配置
- 最多 4 路；名称建议短一点（语音识别+模型匹配都更稳）
- `active_high` 决定实际电平：继电器模块多为低电平触发，选错会"开=关"
- 修改配置会立即重配引脚，如果那个脚上接了别的外设会立刻断掉
