# 音乐页交互：歌词横向滚动 + 实体切歌键

## 1. 歌词横向滚动（已实现）

- 当前歌词行改为 `LV_LABEL_LONG_SCROLL_CIRCULAR`（首尾相接的跑马灯），一轮 6 秒，循环间隔 24px。
- 短句仍然静止居中（LVGL 只有在文本超出宽度时才滚动），所以不会晃眼睛。
- 上一句 / 下一句仍是 `LONG_DOT`(省略号) 单行，保证三行永不重叠。
- 代码：`main/music/music_page.cc` → `CreateIfNeeded()` 中
  `lv_label_set_long_mode(lyric_cur_, LV_LABEL_LONG_SCROLL_CIRCULAR)`
  `lv_obj_set_style_anim_duration(lyric_cur_, 6000, 0)`（可调快慢）

## 2. 实体切歌键（已实现，默认关闭）

### 接线
```
按键一端 → GPIO（建议 14）     按键另一端 → GND
```
- 内部上拉，**按下 = 低电平**（`Button` 默认 `active_high=false`）
- 不需要外接上拉电阻；按键按下时把脚拉低即可
- **不要用**：0(BOOT)、19/20(USB)、26~37(Flash/PSRAM)、43/44(UART0)、45/46(strapping)、
  以及已占用的 1,2(I2C) 3,8,9,10,11(I2S) 4,5,6,7,15(LCD) 12(PA) 18(LAMP) 42 48(LED)

### 可用空闲脚
`13 / 14 / 16 / 17 / 21 / 38 / 39 / 40 / 41 / 42 / 47`

### 启用（改一行，重新 build + flash）
`main/boards/esp32s3-lcd20/config.h`
```c
#define MUSIC_NEXT_BUTTON_GPIO GPIO_NUM_14   /* 单击=下一首，双击=停止 */
#define MUSIC_PREV_BUTTON_GPIO GPIO_NUM_13   /* 单击=上一首（可选） */
```
默认是 `GPIO_NUM_NC`（不启用），所以现在烧的固件不会去初始化这两个脚。

### 行为
| 操作 | 动作 |
|---|---|
| 下一首键 单击 | `MusicPlayer::Next()`：播放中→下一首；空闲→从第一首开始 |
| 下一首键 双击 | `MusicPlayer::Stop()`：停止播放并收起音乐页 |
| 上一首键 单击 | `MusicPlayer::Prev()`：播放中→上一首；空闲→从第一首开始 |

### 代码路径
```
Button(GPIO) --OnClick--> Application::GetInstance().Schedule(lambda)
                              └─> MusicPlayer::Next()/Prev()/Stop()
```
- 按键回调运行在 button 组件任务里，**必须** 经 `Application::Schedule()` 投递到主任务，
  否则会跨任务操作音频队列和 LVGL（这是仓库既有的线程规则）。
- `MusicPlayer` 内部对"未播放时按切歌键"做了兜底（从第一首开始），不会静默失败。

### 想加"长按"？
`Button::OnLongPress()` 已可用，例如长按=`暂停/继续`、长按=`重启服务`，直接在
`InitializeMusicButtons()` 里加一个回调即可。
