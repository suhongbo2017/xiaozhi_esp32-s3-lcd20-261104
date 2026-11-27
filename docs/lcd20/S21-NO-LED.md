# P10：把板载 WS2812 从功能中移除

用户决定：板载状态灯（WS2812 / GPIO48）不再作为功能提供。

## 改动

| 位置 | 改动 |
|---|---|
| `boards/esp32s3-lcd20/esp32s3_lcd20_board.cc` | `GetLed()` 改为返回 **`NoLed`**（所有 Led 调用变空操作，不创建 RMT、不碰 GPIO48） |
| 同上 | 构造函数里新增 `ClearOnboardWs2812Once()`：开机给灯发**一帧全 0** 把它熄掉，然后 `led_strip_del` 释放引脚 —— WS2812 会一直保持最后收到的颜色，不主动清就会一直亮着 |
| 同上 | 诊断流程里的「LED 自检」（红→绿→蓝）整段删除 |
| `web/admin_server.cc` | 删除 `/api/led/test`、`/api/led/diag`；`/api/status` 去掉 `led_mode`/`led_color`，改为 `"led_supported": false`；`POST /api/settings` 不再处理 LED |
| `web/ui/index.html` | 「控制」页删掉「状态灯」卡片，Tab 副标题改为「我的开关」 |
| `web/ui/app.js` | 删除 LED 绑定与自检函数 |
| MCP 工具 | `self.led.*` 因为 `NoLed::SupportsColor() == false` **自动不再注册**（实测工具总数 25，无 led 相关） |

## 效果

- 开机时灯被熄一次，之后**永不点亮**（不管对话/放音乐/调设置）
- GPIO48 仍然保留在"已占用引脚"清单里，避免被配成用户开关后给 WS2812 发乱数据
- LAMP（GPIO18，`self.lamp.*`）不受影响，仍是独立功能

## 排障记录（为什么最终删掉）

1. 一开始以为"关灯无效"是模式问题 → 加了 `Follow/Off/Fixed` 三态（S19），网页能切、状态能存，但灯不灭
2. 进一步发现：为定位引脚加的调试接口 `/api/led/probe|blink|gpio` 会**把 GPIO 从 RMT 手里抢走**
   （`gpio_config` / `gpio_reset_pin`），之后 `led_strip_refresh()` 全部返回 OK 但波形发不出去，
   灯就永久卡在最后一次成功写入的颜色（白/红）
3. 重启后 `SingleLed` 重建 RMT 能恢复 → 说明**软件链路是通的**
4. 但用户实测：`TurnOn rgb=(255,0,0) set=ESP_OK refresh=ESP_OK` 之后灯**仍不变成红色**，
   只有开机第一次那帧白色生效 → 属于"软件报成功、硬件无响应"，与灯珠型号/走线/电平有关，
   短期无法在软件侧确认
5. 用户决定：不值得继续投入，**从功能中移除**

> 教训：诊断类接口只要会改 GPIO 归属（`gpio_config` / `gpio_reset_pin` / 临时 `led_strip`），
> 就必须在结束后把原驱动 `Reinit()` 回来；否则会把好好的功能"测坏"。
