# P2：网页换外观资源（字体/表情）—— 安全两阶段热更新

## 现在能做什么

浏览器打开设备管理页 `http://<设备IP>:8080/` → **外观资源（字体/表情）** → 选一套 → **应用并重启**：

| 资源包 | 内容 | 大小 |
|---|---|---|
| `a20_e64`（默认/推荐） | 20px 字体 + 64px 表情 | 1.66 MB |
| `a16_e32` | 16px 字体 + 32px 表情（省空间） | 1.21 MB |
| `a30_e64` | 30px 字体 + 64px 表情（大字） | 2.99 MB |

资源包由 `scripts/build_default_assets.py` 生成，放在服务器 `assets_variants/`，
通过 `GET /assets/<name>.bin`（免鉴权）下发。

## 为什么不能"运行中直接写 assets"

第一版实现是：网页触发 → 下载到 PSRAM → 直接擦写 assets 分区。
结果**设备立刻进入重启循环**：LVGL 正在读 assets 里的字体文件，擦除瞬间读到非法数据 → 崩溃。

## 现在的两阶段设计（安全）

```
阶段1（运行中）: 下载 assets.bin → 写入 ota_1 分区（4MB 暂存区，非运行分区）
                  → 内部 checksum 校验 → NVS 标记 assets/pending=1 → 重启
阶段2（启动最早处）: app_main 里 nvs_init 之后、LVGL/assets 初始化之前
                  → 读 NVS 标记 → 从 ota_1 读出 → 校验 → 擦写 assets 分区 → 清标记 → 再重启
```

- assets 分区只在"无人使用"的启动早期改写，**旧资源在切换前始终完好**
- 任一阶段掉电：最坏是暂存包无效，开机校验失败自动丢弃标记 → 仍用旧资源启动，**不会变砖**
- 暂存头写在分区**尾部**（前面踩过一个坑：写在 offset 0 时写入返回 OK 但读回是 0，
  改成尾部 16 字节后稳定）

## 实测（全部走网页同一接口）

```
POST /api/assets {"url":".../a30_e64.bin"}
  → {"ok":true,"reboot":true}
  → 串口: stage header write=ESP_OK readback magic=41535354 len=2985897
          assets staged: 2985897 bytes, 24 files -> reboot to apply
  → 重启后: applying staged assets: 2985897 bytes
            assets applied (2985885 bytes), rebooting
  → 读 0x800000 头部: total=2985897  => 匹配 a30_e64 ✓
再切回 a20_e64 → total=1664265 => 匹配 a20_e64 ✓（两次切换都成功）
```

## 代码

| 文件 | 作用 |
|---|---|
| `main/web/asset_ota.{h,cc}` | 两阶段实现：`StageFromUrl()` + `ApplyIfPending()` |
| `main/main.cc` | `app_main` 里（NVS 初始化后、Application 之前）调用 `ApplyIfPending()` |
| `main/web/admin_server.cc` | `/api/assets` → StageFromUrl → 重启；UI 加资源选择器 |
| 服务端 | `/assets/<name>.bin` 免鉴权下发；`assets_variants/` 存放资源包 |

## 限制

- ota_1 被当作暂存区使用（它本来就是备用 app 槽，且在写入前会被整块擦除）
- 资源包必须 ≤ 4MB（assets 分区 8MB，但暂存区 4MB）；目前最大 2.99MB
- 术语：这仍属于"改配置/资源"，**不改变固件逻辑**；改逻辑仍需固件 OTA
