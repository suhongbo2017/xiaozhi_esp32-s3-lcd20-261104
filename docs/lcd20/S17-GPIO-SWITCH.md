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
  GPIO 号   [GPIO1 ▼]        ← 下拉框，只列"当前可用"的引脚
  触发电平  [高电平点亮 / 低电平点亮（继电器）]
  初始状态  [关闭 / 打开]
  [添加这一路]

已被占用的引脚（说明栏，逐行列出，含原因）
  GPIO0  · 启动键(BOOT)
  GPIO3  · 麦克风时钟（板载）
  GPIO4  · 屏幕 SPI CLK（板载）
  ...
  GPIO12 · 打印机灯光（已配置）
```

**下拉框是"活的"**：`/api/gpio` 每次都返回 `available`（当前可选的脚）和 `used`（全部占用脚+原因），
所以加一路之后那个脚立刻从下拉里消失、并出现在说明栏里标为「（已配置）」；删掉又回到下拉里。

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

### 冲突检测的三层来源

```cpp
Board::ReservedGpios()   // 新增虚函数：返回 {引脚, 用途} 对，各板自己声明
  └─ esp32s3-lcd20 覆写 14 项：0 启动键 / 3 麦克风时钟 / 4,5,6,7,15 屏 SPI /
     8,9,10 喇叭 I2S / 11 麦克风数据 / 18 灯带 / 45 背光 / 48 状态灯
kFixedReserved[]         // 通用危险脚：USB 19,20 / 闪存+PSRAM 26~37 / UART 43,44 / strapping 46
已配置的开关              // UsedPins() 里以「名称（已配置）」参与
```

`AvailablePins()` = ESP32-S3 有效脚（0~21、26~48，**22~25 不存在**）减去上面三层。

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
GET  /api/gpio
  -> {"devices":[],"reserved":[0,3,...],
      "available":[1,2,12,13,14,16,17,21,38,39,40,41,42,47],
      "used":[{"gpio":0,"reason":"启动键(BOOT)"},{"gpio":3,"reason":"麦克风时钟（板载）"},...]}
加 GPIO12 之后：available 里 12 消失，used 里出现 {"gpio":12,"reason":"打印机灯光（已配置）"} ✓
删掉之后：12 回到 available ✓
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
