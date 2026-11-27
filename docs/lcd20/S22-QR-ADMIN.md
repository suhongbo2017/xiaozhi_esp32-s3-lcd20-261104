# P11：设备屏幕显示「管理页二维码」（扫一扫进设置）

## 用法

三种方式把管理页地址显示成二维码（`http://<设备IP>:8080/`）：

| 触发 | 说明 |
|---|---|
| **BOOT 键双击** | 显示/隐藏（再双击隐藏） |
| 网页「系统 → 二维码显示到屏幕」 | `POST /api/qr?seconds=120` |
| 语音 | 说"怎么设置/显示个二维码" → 调 `self.screen.show_qrcode` |

显示 200×200 的码（29 模块，ECC Medium），下面一行小字是完整 URL，默认 60~180 秒后自动消失。

## 实现

### 为什么自己画码而不用 LVGL 的 lv_qrcode

打开 `CONFIG_LV_USE_QRCODE=y` 后链接报错：

```
multiple definition of `qrcodegen_makeEci'
  lvgl/src/libs/qrcode/qrcodegen.c
  espressif2022__esp_emote_gfx/src/lib/qrcode/qrcodegen.c   ← 这个组件也内置了一份
```

`esp_emote_gfx` 是 EmoteDisplay 用的管理组件（vendor 目录，不能改），它 `GLOB` 了 `src/*.c` 把
qrcodegen 一起编进去了。于是改用**它已经链接好的 qrcodegen 符号**，只把声明头拷到
`main/ui/qrcodegen.h`（Nayuki qrcodegen，MIT），自己负责把模块矩阵画成像素。

### 画码流程

```cpp
qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 40, qrcodegen_Mask_AUTO, true);
int n = qrcodegen_getSize(qr);          // 29
int box = kQrSize / n;                  // 200/29 = 6px/模块
// 先把 200x200 的 RGB565 缓冲刷白，再把暗模块填黑
lv_canvas_set_buffer(canvas, pixels, 200, 200, LV_COLOR_FORMAT_RGB565);
```

- 缓冲 200×200×2 = **80KB，放 PSRAM**（`heap_caps_malloc(MALLOC_CAP_SPIRAM)`）
- 编解码缓冲 `qrcodegen_BUFFER_LEN_MAX` × 2 用静态数组（约 7.8KB，不能放栈）
- 每模块 6×6 像素，29 模块刚好占满 174px，四周自动留白

### 与待机页的冲突

待机页每 500ms 判断要不要显示，`Show()` 里会 `lv_obj_move_foreground()` → **会把二维码盖住**。
修法：`StandbyService` 的 want 条件加上 `&& !QrPage::GetInstance().visible()`。

## 实测

```
POST /api/qr?seconds=120 -> {"ok":true}
QrPage: QR page shown: http://192.168.102.26:8080/ (29 modules, 120 s)
/api/mcp_tools -> 26 个工具，含 self.screen.show_qrcode
```

## 微信扫码的注意事项

1. 手机必须和设备在**同一个 WiFi**（二维码里是内网地址 `192.168.x.x`）
2. 微信扫一扫可能提示"非官方网页"，点继续即可；用系统相机扫更顺
3. 想要"扫完自动进公众号/小程序绑定"，需要公网 H5 + 公众号（路线 C，未实施）

## 代码

| 文件 | 作用 |
|---|---|
| `main/ui/qr_page.{h,cc}` | 二维码页（canvas 绘制）+ MCP 工具 `self.screen.show_qrcode` |
| `main/ui/qrcodegen.h` | 从 esp_emote_gfx 拷来的声明头（MIT，Nayuki） |
| `main/web/admin_server.cc` | `POST /api/qr?seconds=N` |
| `main/web/ui/{index.html,app.js}` | 「系统 → 二维码显示到屏幕」按钮 |
| `main/boards/esp32s3-lcd20/esp32s3_lcd20_board.cc` | BOOT 双击显示/隐藏 |
| `main/ui/standby_service.cc` | 二维码显示时待机页让位 |
