# P6：唤醒词设置上网页 + 去掉 EMOJI 表情

## 网页新增（声音 tab）

| 项 | 说明 | 生效 |
|---|---|---|
| **唤醒词开关** | 开启 / 关闭 | 立即（开=马上开始监听，关=立刻停掉 WakeNet） |
| **灵敏度** | 0.40 ~ 0.99 滑杆（默认 0.50） | 立即下发到 AFE |
| 当前唤醒词 | 只读显示，来自模型（本板 `wn9_nihaoxiaozhi_tts` → **你好小智**） | — |

> 关闭唤醒词后只能用 **BOOT 键**开始/结束说话，页面上有黄字提示。
> 想换唤醒词本身需要换 WakeNet 模型（assets 里的 srmodels），不是运行时可改的。

配置写 NVS：`wake/enabled`、`wake/threshold`（整数百分比），重启保持。

## 实现

### 1. 开关：由 Application 统一裁决

原来 `application.cc` 里有 6 处 `audio_service_.EnableWakeWordDetection(true)`，现在全部改成：

```cpp
audio_service_.EnableWakeWordDetection(WakeWordEnabled() && audio_service_.IsAfeWakeWord());
```

`WakeWordEnabled()` 读成员变量（NVS 在 `Initialize()` 里读一次），
`SetWakeWordEnabled()` 写 NVS + 立刻 `EnableWakeWordDetection(enabled)`。
这样"空闲重新开监听""打断后恢复监听"等所有路径都会尊重开关。

### 2. 灵敏度：走 AFE 的 `set_wakenet_threshold`

`esp_afe_sr_iface_t` 提供 `set_wakenet_threshold(afe, index=1, 0.4~0.9999)`。
但**不能在 HTTP 任务里直接调**——那个 AFE 句柄正被 ProcessingTask 的 fetch 用着。
所以沿用项目已有的"延迟下发"机制：

```
AudioEngine::SetWakeWordThreshold(t)        // 新虚函数，默认 false
  └─ AfeAudioEngine: pending_threshold_.store(t); afe_control_dirty_ = true;
                     xEventGroupSetBits(kAfeActive)   // 唤醒处理任务
        └─ ProcessingTask → ApplyAfeControls() 里 set_wakenet_threshold(...)
```

`AudioService` 只做转发；`Application::SetWakeWordThreshold()` 负责夹到 0.4~0.99 并存 NVS。

### 3. 唤醒词名称只读展示

`AudioEngine::GetWakeWords()`（默认返回空 vector，AfeAudioEngine 覆写）→
`AudioService::GetWakeWords()`（为空时回退到"最近一次检测到的词"）→ `/api/status` 的 `wake_words`。

## 去掉 EMOJI 表情

微信气泡模式自带每条消息的头像，原来居中的「AI 机器人图标 + 表情大图」就多余了：

- `SetupUI()`（wechat 分支）不再创建 `emoji_image_` / `emoji_label_`
- `SetEmotion()` 在 `emoji_image_ == nullptr` 时**静默返回**
  （application 每次状态切换都会调 `SetEmotion("neutral")`，不静默会刷警告）
- `SetChatMessage()`、`ClearChatMessages()`、`SetTheme()` 里对这两个指针的访问全部判空

**踩坑**：一开始只改了创建和 SetEmotion，漏了 `SetTheme()` 里
`lv_obj_set_style_text_color(emoji_label_, ...)` → 开机 `AdminServer::ApplyPersisted()`
切主题时空指针崩溃，**设备直接重启循环**。backtrace 定位：

```
0x420e627f lv_obj_set_local_style_prop      (lv_obj_style.c:359)
0x420e69b8 lv_obj_set_style_text_color      (lv_obj_style_gen.c:635)
0x420161e8 LcdDisplay::SetTheme             (lcd_display.cc:1235)
0x4202a978 ApplyTheme                       (admin_server.cc:709)
0x4202aa86 AdminServer::ApplyPersisted      (admin_server.cc:874)
0x4202fb69 Application::Initialize          (application.cc:168)
```

## 实测

```
I (10720) AfeAudioEngine: wakenet model=wn9_nihaoxiaozhi_tts words=你好小智
GET /api/status -> {"wake_enabled":1,"wake_threshold":50,"wake_words":"你好小智",...}

POST {"wake_threshold":70}
  Application: wake word threshold -> 0.70 (applied)
  AfeAudioEngine: wakenet threshold -> 0.700        ← 真正下发到 AFE
POST {"wake_enabled":0}  -> Application: wake word disabled
POST {"wake_enabled":1}  -> Application: wake word enabled
```

固件 2.98MB / 3.94MB（76%）。

## 限制

- 唤醒词**内容**不可运行时切换（WakeNet 模型限定），换词要换 `srmodels` 资源
- 阈值只对 WakeNet9 有效（`set_wakenet_threshold` 只支持 index 1/2 的 wakenet）
- 关闭唤醒词后没有任何语音入口，只能按键；如果将来加"语音全双工"需要重新设计
