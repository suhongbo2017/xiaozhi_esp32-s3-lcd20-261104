# P9：管理网页重构（三文件 + 自动保存 + 聊天样式预设 + 屏幕模拟）

## 为什么要重写

原来整页 26KB 塞在 `admin_server.cc` 的一个 `R"HTML(...)"` 常量里，改一处要翻 800 行 C++；
而且用户反馈的 5 个问题都出在这页：

| 反馈 | 原因 | 改法 |
|---|---|---|
| 「外观」和「显示」看不懂 | 名字起错：一个是运行参数、一个是静态素材 | 改成 **屏幕 / 图片 / 声音 / 控制 / 系统**，每个 tab 下面带一行小字说明 |
| 说明文字铺一长条 | 没有容器约束 | `<details>` 折叠 + `.note-body` 限高 110px 可滚动 |
| 图片显示太大像像素低 | 预览撑满宽度（≈300px 宽） | 缩略图固定 **72×96（3:4）**，点击弹新窗口按 240×320 原尺寸看 |
| 开关卡片太大 | 每张 ≈110px 高 | 压到 **单行**：名称 + GPIO 信息 + iOS 开关 + 删除文字按钮 |
| 气泡样式只有一个 | 硬编码 | **4 个预设** + 网页内 CSS 实时预览 + 设备屏幕模拟 |

## 目录结构（新）

```
main/web/ui/index.html    10.9KB   结构
main/web/ui/style.css      8.6KB   样式
main/web/ui/app.js        21.2KB   逻辑（自动保存 / 裁图 / 预览）
main/web/admin_server.cc           只留 API 与路由（26KB HTML 常量已删除）
```

CMake 用 `EMBED_FILES` 打包（项目里原本就有 `EMBED_FILES ${LANG_SOUNDS}` 的先例），
运行时 `httpd_resp_send()` 直接从 flash 分块发送，**不占 RAM**。

```cmake
set(WEB_UI_FILES "web/ui/index.html" "web/ui/style.css" "web/ui/app.js")
idf_component_register(SRCS ${SOURCES} EMBED_FILES ... ${WEB_UI_FILES} ...)
```

```cpp
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
...
httpd_resp_send(req, (const char*)index_html_start, index_html_end - index_html_start - 1);
```
（`EMBED_FILES` 会在末尾补 1 字节 NUL，发送时减掉）

## 自动保存

所有控件**改了就存**，不再有底部"保存并应用"按钮：

```js
queueSave({brightness: 72})        // 350ms 防抖，多次修改合并成一次请求
  → POST /api/settings {brightness: 72}
  → toast「已保存 ✓」/ 失败变红并提示离线
```
滑杆用 `input` 事件（拖动结束 350ms 后落一次盘）、下拉/开关用 `change` 且 `delay:0` 立即提交。
**关键**：合并 + 防抖，避免拖一次滑杆写几十次 NVS（NVS 写一次 10~20ms 且有寿命）。

## 聊天样式预设（4 个）

固件 `LcdDisplay::SetChatMessage()` 里改成样式参数表：

| # | 名称 | 我方气泡 | 对方气泡 | 圆角 | 头像 | 尾巴 | 聊天底 |
|---|---|---|---|---|---|---|---|
| 0 | 微信浅色 | #95EC69 | #FFFFFF | 10 | 有 | 有 | #EDEDED |
| 1 | 微信深色 | #3EB575 | #2C2C2E | 10 | 有 | 有 | #111111 |
| 2 | iMessage | #0A84FF | #E9E9EB | 18 | 无 | 无 | #000000 |
| 3 | 极简 | 透明+#95EC69 字 | 透明+#D8DEE9 字 | 0 | 无 | 无 | #EDEDED |

- 存 NVS `ui/chat_style`，`chat_style_` **缓存成成员**（每条消息不再读 NVS）
- 网页点一下就 `POST {chat_style:N}` → 固件 `ApplyChatStyle()` **立即生效**（改聊天区底色 + 重设气泡参数）
- 网页里每个预设都用 CSS 画了一张 mini 预览（3 条示例气泡），不用位图 → 不会"像素低"

## 自定义聊天背景图（新增）

复用 S14 的图片存储，扩到 **3 个槽**：

```
assets 分区尾部 1.5MB
├─ +0x00000  开机图      480KB
├─ +0x78000  待机背景    480KB
└─ +0xF0000  聊天背景    480KB   ← 新增
```

`POST /api/image?kind=chat` 上传，`LcdDisplay::ApplyChatStyle()` 里：

```cpp
lv_obj_set_style_bg_image_src(content_, &chat_bg_dsc, 0);
lv_obj_set_style_bg_image_opa(content_, 150, 0);   // 约 59%，压暗保证文字可读
```

## 设备屏幕模拟预览

网页按「主题 + 聊天样式 + 待机页设置」用 CSS 画一个 210×280 的模拟屏：
- 待机页开着 → 显示时钟 / 日期 / 天气 / 指数（指数行用你配的代码个数）
- 待机页关着 → 显示 4 条示例气泡（按选中的样式上色）

**这是 CSS 模拟，不是设备截图**，页面里有说明。

## 实测

```
GET /            -> 200 10986B   （原 26KB，现在拆成 3 个文件）
GET /style.css   -> 200  8563B
GET /app.js      -> 200 21245B
POST /api/settings {"chat_style":2,"brightness":72} -> ok
  LcdDisplay: chat style -> 2 / AdminServer: chat style -> 2
POST /api/image?kind=chat (5337B) -> ok
  UserImage: image saved: kind=2 5337 bytes (240x320)
  UserImage: chat background decoded: 240x320 (153600 bytes)
  LcdDisplay: chat background image applied (240x320)
Playwright：4 个样式磁贴、屏幕模拟、GPIO 列表(1 路)、下拉(13 个可用脚) 均正常，0 个 JS 报错
自动保存：网页把亮度改成 66 -> 设备 /api/status 读到 66 ✓
```

## 踩坑

1. 全局 `button{width:100%}` 把 tab 按钮也撑满 → 5 个 tab 只看得见 1 个。
   修法：`.tabs button{flex:1 1 0;width:auto;min-width:0}`
2. 图片预览用 `object-fit:cover` 撑满宽框 → 240×320 的竖图被裁成一条。
   修法：缩略图固定 72×96（3:4）+ 点击看原图。
3. 旧 HTML 常量删掉前占了 24KB flash，删完 app 体积回落。

## 体积

```
app: 3.02MB / 3.94MB (77%)     网页 41KB 内嵌，不占 RAM
```
