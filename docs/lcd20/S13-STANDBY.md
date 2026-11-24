# P3-D：待机界面（时钟 / 日期 / 天气 / 指数）

## 用法

设备空闲（没有在对话、没有在放歌）时自动显示待机页；一说话或放歌就自动让位。

```
┌────────────────────────────┐
│         10:42              │  ← Montserrat 48（数字，切换零成本）
│      11月24日 周一          │  ← 主题 20px 中文字体
│  ─────────────────────────  │
│  ⛅  多云  10°C  湿度67%     │  ← emoji 图标来自 emoji_font
│                            │
│  上证指数      3538.95 +0.15%│  ← 涨红 / 跌绿
│  深证成指     10454.37 +1.93%│
│  创业板指      2201.33 -0.42%│
│                            │
│   说“你好小智”开始对话       │
└────────────────────────────┘
```

网页控制：`http://<设备IP>:8080/` → **待机页（时钟/天气/指数）**
- 开关：开启 / 关闭
- 城市：上海、北京、深圳、广州、杭州、成都、武汉、西安、南京、重庆、苏州、天津、长沙、青岛、厦门
- 指数/个股：`sh000001,sz399001,sz399006`（A股 sh/sz、港股 hk00700、美股 usNVDA，逗号分隔最多 4 个）

配置写 NVS（`standby` 命名空间），重启保持。

## 数据链路

```
设备 StandbyService（1s tick，独立任务）
   ├─ 时钟/日期：系统时间（localtime，激活时已由服务端下发北京时间）
   └─ 天气/行情：每 300s → GET /standby?city=..&symbols=..
                        ↓
        服务器 music_server.py
          ├─ 天气：open-meteo（免费、无需 key，城市 → 经纬度表）
          └─ 行情：腾讯 qt.gtimg.cn（复用 fetch_stock_quote）
          └─ 结果缓存 300s（按 city|symbols 分键）
```

服务端额外加了 city 的双编码兜底（UTF-8 / GBK），因为不同客户端发的中文参数编码不同
（实测 Git Bash 的 curl 发的是 GBK，浏览器是 UTF-8）。

## 实现要点（踩坑）

1. **不能只靠 `SetDeviceState` 的返回值判断空闲**：状态机拒绝 `Starting → Idle` 这类跳转，
   返回值 false 会让 `idle_` 永远停在 false → 页面不显示、数据也不拉。
   改为在 `HandleStateChangedEvent()` 里按 `state_machine_.GetState()` 的真实值下发。
2. **`visible()` 不能用 `root_ != nullptr` 判断**：`Hide()` 只是加 `LV_OBJ_FLAG_HIDDEN`，
   `root_` 仍在 → 每 500ms 都判定"状态变化"，日志刷屏且反复 Hide。加了 `shown_` 标志。
3. **天气图标必须用 `emoji_font()`**：`text_font` 是 CBin 的 Noto Sans 子集，没有 emoji 字形，
   用它会显示成方块。
4. **拉取不依赖页面可见**：数据拉取挪到 `if (want)` 之外，页面第一次 Show 时用缓存补一次
   （否则开机时页面还没创建，setter 全是空操作，数据白拉）。
5. **时钟只在分钟变化时写 LVGL**（`last_hhmm`/`last_date` 去重），避免每秒整屏重绘。
6. 页面与 MusicPage 同一套模式：`lv_screen_active()` 上建全屏容器 + `move_foreground`，
   `DisplayLockGuard` 保护；放音乐时 `MusicPlayer::playing()` 为真 → 自动让位给音乐页。

## 代码

| 文件 | 作用 |
|---|---|
| `main/ui/standby_page.{h,cc}` | LVGL 待机页（无 LVGL 板型走空实现） |
| `main/ui/standby_service.{h,cc}` | 1s tick 任务：时钟 / 每 300s 拉数据 / 显隐判断 / NVS 配置 |
| `main/music/music_api_client.*` | 新增 `Standby(city, symbols, &json)` |
| `main/application.cc` | `Initialize` 创建服务；`HandleStateChangedEvent` 下发 idle；网络连接后 `OnNetworkReady` |
| `main/web/admin_server.cc` | `/api/status` 增加 standby 字段；`/api/settings` 支持 standby/city/symbols；UI 新增卡片 |
| `main/boards/esp32s3-lcd20/config.json` | `CONFIG_LV_FONT_MONTSERRAT_48=y`（+24） |
| 服务端 `music_server.py` | `GET /standby`（免鉴权）+ 天气/行情抓取 + 300s 缓存 |

## 实测

```
串口：Standby: standby service started (enabled=1 city=上海 symbols=sh000001,sz399001,sz399006)
      Standby: standby updated: weather=多云 temp=10 stocks=3
      Standby: standby want=1 (enabled=1 idle=1 playing=0)     ← 空闲显示
      Standby: standby want=0 (enabled=0 idle=1 playing=0)     ← 网页关闭后隐藏
服务端：GET /standby?city=%E5%8C%97%E4%BA%AC&symbols=... 200
        [standby] city=北京 symbols=sh000001,sz399001,sz399006
切换城市：上海→北京 使 weather 从「多云 9.9°C」变为「晴 10°C」（证明参数生效）
固件体积：2.76MB → 2.86MB（73% of 3.94MB app 分区）
```

## 限制

- 城市表只有 15 个城市，其它城市回退到上海经纬度（加城市只需往 `CITY_COORDS` 加一行）
- 时间来自服务端下发的系统时间，长期运行会有秒级漂移（未接 SNTP）
- 指数最多 4 个、屏幕只显示前 3 行
- 天气数据每 5 分钟更新，服务端缓存同样 5 分钟（一天约 288 次外网请求）
