# P1：设备管理网页（WiFi 下发，立即生效）

## 用法

设备联网后，在同一局域网的手机/电脑打开：

```
http://192.168.102.26:8080/
```
（IP 从串口日志 `WifiStation: Got IP: …` 或路由器里看；端口 **8080**，80 留给配网页）

## 可改项（保存即生效，写入 NVS，重启保持）

| 项 | 说明 | 生效方式 |
|---|---|---|
| 主题 | dark / light | `display->SetTheme()` 立刻换 |
| 屏幕亮度 | 0~100 | `PwmBacklight::SetBrightness()`（渐变） |
| 音量 | 0~100 | `AudioCodec::SetOutputVolume()` + NVS |
| 音乐 ducking 比例 | 30~100% | 播放音乐时自动压低到该比例（方便唤醒词打断） |
| 状态灯 | 跟随状态 / 常灭 / 固定颜色 | `Led::SetStatusEnabled/SetColor` |
| 音乐服务地址 | 默认 `http://47.116.199.150:2233` | `MusicApiClient::Reload()` **无需重启** |

## 接口

```
GET  /               单页管理界面（内嵌 HTML/CSS/JS，4.9KB，无外网依赖）
GET  /api/status     {"board","version","mac","theme","brightness","volume","duck","api","led_mode","led_color"}
POST /api/settings   {"theme":"dark","brightness":70,"volume":65,"duck":60,"led_mode":"on|off|color","led_color":"#00b3ff","api":"http://..."}
                     -> {"ok":true}
```

## 实现要点（踩坑记录）

1. **`httpd_start()` 必须在 lwIP 初始化之后**。最初放在 `Application::Initialize()` 里，直接
   `assert failed: tcpip_send_msg_wait_sem (Invalid mbox)` → 开机重启循环。
   **改为在网络 `NetworkEvent::Connected` 回调里启动**，正常。
2. `display->GetTheme()->name().c_str()` 返回悬空指针（临时对象）→ 状态里出现乱码，
   改为先拷贝到 `std::string`。
3. 唤醒词开关从 UI 移除：需要改 AudioService 生命周期（状态变化会重新 enable），留到 P2，避免"看着能改其实没生效"。

## 代码

| 文件 | 作用 |
|---|---|
| `main/web/admin_server.h/.cc` | HTTP 服务 + 内嵌单页 + `/api/status` `/api/settings` |
| `main/music/music_api_client.*` | 新增 `Reload()`（网页改地址后热加载） |
| `main/application.cc` | 网络连上后启动管理页；ducking 比例改为读 NVS（可网页调） |
| `main/CMakeLists.txt` | 新增 `web/admin_server.cc`、include `web`、`PRIV_REQUIRES esp_http_server` |

## 实测

```
GET  /                -> 200，4936 字节 HTML
GET  /api/status      -> {"theme":"dark","brightness":70,"volume":65,...}
POST /api/settings    -> {"ok":true}     （亮度 45→70、音量 55→65 立即生效并持久化）
```
