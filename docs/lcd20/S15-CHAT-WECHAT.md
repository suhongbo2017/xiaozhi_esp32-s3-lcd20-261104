# P5：聊天气泡改成微信风格

## 效果

```
┌──────────────────────────────────┐
│  样式预览 · 微信风格气泡           │  ← 居中灰色胶囊（系统消息）
│                                  │
│ ┌────────────────┐               │
│ │ 上海今天多云，12度 │  ╭──╮        │  ← 白色气泡 + AI 头像（左）
│ │ 湿度54%。纳指…   │  │AI│        │
│ └───────────────╯  ╰──╯        │
│                                  │
│               ┌──────────────┐   │
│      ╭──╮     │ 好的谢谢      │◁  │  ← 绿色气泡 + 尾巴 + 我头像（右）
│      │我│     └──────────────┘   │
│      ╰──╯                        │
└──────────────────────────────────┘
```

| 元素 | 我（user） | 小智（assistant） | 系统（system） |
|---|---|---|---|
| 对齐 | 右 | 左 | 居中 |
| 气泡色（浅色主题） | `#95EC69` 微信绿 | `#FFFFFF` 白 | `#DCDCDC` 灰 |
| 气泡色（深色主题） | `#3EB575` 深绿 | `#2C2C2E` 深灰 | `#1F1F1F` |
| 文字色 | 跟随主题（浅色黑/深色白） | 同左 | system_text_color |
| 头像 | 26px 绿圆 +「我」 | 26px 蓝圆 +「AI」 | 无 |
| 小尾巴 | 气泡右侧 45° 小方块 | 气泡左侧 | 无 |
| 圆角 | 10px（系统 8px） | 10px | 8px |
| 背景不透明度 | 100%（原来是 70%，会透出底图显得脏） | 100% | 100% |

聊天区底色同步改成微信的浅灰 `#EDEDED`（深色 `#111111`），气泡对比更明显。

## 关键前提：必须打开 `CONFIG_USE_WECHAT_MESSAGE_STYLE`

`lcd_display.cc` 里 **有两套 `SetChatMessage()` 实现**，由 Kconfig 选择：

```
#if CONFIG_USE_WECHAT_MESSAGE_STYLE      // 微信风格分支（line ~357 SetupUI / ~512 SetChatMessage）
   ...
#else                                     // 默认分支（line ~1054 SetChatMessage）
   ...
#endif
```

这块板子原来用的是默认分支，所以只改代码不生效（真机上"还是旧样式"就是这个原因，
一开始连日志都不打印）。现在在 `main/boards/esp32s3-lcd20/config.json` 里加了：

```json
"sdkconfig_append": [ "CONFIG_USE_WECHAT_MESSAGE_STYLE=y", ... ]
```

## 实现

`LcdDisplay::SetChatMessage()` 里每个消息现在是一个**行容器**（row）：

```
content_（flex column, 可滚动）
└─ row（flex row, 宽度=屏宽-2*pad, 对齐方式随 role）
   ├─ [assistant] 头像 + 气泡(内含 文本 + 尾巴)
   └─ [user]      气泡(内含 文本 + 尾巴) + 头像
```

关键点：

1. **气泡宽度**：先按 `LV_SIZE_CONTENT` 量出文本自然宽度，再夹到
   `max_bubble_w = 行宽 - (头像26 + 间距6) - 10`（240 屏约 **190px**，一行 9 个汉字），
   超出后 `LV_LABEL_LONG_WRAP` 自动换行。
2. **小尾巴**：10×10 小方块、圆角 2、`transform_rotation = 450`（45°），
   用 `LV_ALIGN_RIGHT_MID(+4)` / `LV_ALIGN_LEFT_MID(-4)` 压在气泡边缘，
   `lv_obj_move_background()` 垫到文字后面 → 露出的半个菱形就是尖角。
3. **头像**用文字（「我」「AI」）而不是图片：不占 assets、不需要额外字体，
   `LV_RADIUS_CIRCLE` 画圆。两者都在 basic 字库内，不会出现豆腐块。
4. 系统消息沿用「最后一条系统消息覆盖前一条」的折叠逻辑，只是把判定改成读行内第一个子对象的
   `user_data`（值仍是 `"user"/"assistant"/"system"`）。
5. 历史上限 `MAX_MESSAGES=20` 删除的是**行**对象（行才是 content_ 的直接子对象）。

## 自测入口

网页「系统」tab → **预览聊天气泡样式**（`POST /api/chat_test`）：
立刻往聊天区塞 4 条样例（系统/我/小智/我），不用动嘴就能看版式。
（下一次状态切换时会像普通消息一样被清屏。）

## 实测

```
POST /api/chat_test -> {"ok":true}
LcdDisplay: bubble[system]:    row=224x38  bubble=197x38  text=181px (max 208)
LcdDisplay: bubble[system]:    row=224x72  bubble=224x72  text=208px (max 208)   ← 长文本换行
LcdDisplay: bubble[user]:      row=224x103 bubble=186x103 text=170px (max 170)   ← 186+6+26=218 ≤ 224
LcdDisplay: bubble[assistant]: row=224x165 bubble=186x165 text=170px (max 170)
LcdDisplay: bubble[user]:      row=224x41  bubble=96x41   text=80px             ← 短句自适应
```

调试日志已降级成 `ESP_LOGD`（默认不打印）。

踩坑记录：一开始忘了给气泡设 `LV_SIZE_CONTENT`，LVGL 用默认尺寸，
日志显示 `bubble=130x130`（所有气泡都变成方块），补上宽高自适应后才正常。

## 限制

- 没有时间戳、没有「对方正在输入」、没有长按菜单（微信那些交互在触摸屏上才有意义，
  这块 2.0 寸板没有触摸）
- 头像是文字圆标，不是真实头像图片（要真头像得走 S14 的图片存储，另加一张 26×26 的图）
- 气泡尾巴是方块的 45° 旋转，边缘会有轻微抗锯齿缺失（LVGL 小尺寸旋转，肉眼基本看不出）
