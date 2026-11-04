# 自建 OTA 服务器（XIAOZHI_ESP32-S3_LCD20）

零依赖（Python 3 标准库），实现 `main/ota.cc`（xiaozhi 2.4.2）的 OTA 契约。

## 契约（从固件源码反推）

```
POST /ota/            （无 system info 时退化为 GET）
Headers: Device-Id: <MAC> | Client-Id: <board UUID> | User-Agent | Accept-Language
         Activation-Version: 1|2 | Content-Type: application/json
Body:    board.GetSystemInfoJson()

200 →
{
  "firmware": { "version": "2.5.0", "url": "http://HOST/fw/xxx.bin", "force": 0 },
  "mqtt":      { ... },     // 可选，写入设备 NVS "mqtt"
  "websocket": { ... }      // 可选，写入设备 NVS "websocket"
}

GET /fw/<file>        固件下载，必须带 Content-Length
```

版本比较（`Ota::ParseVersion` + `IsNewVersionAvailable`）：按 `.` 分段、每段取**前导数字**、逐段比较，段数多者更大。
所以要用**纯数字点分版本**：`2.5.0` > `2.4.2`；而 `2.4.2-lcd20` 会被解析成 `[2,4,2]`，等于 `2.4.2`（不会触发升级）。

## 运行

```bash
python tools/ota-server/server.py --port 8080
# 日志：控制台 + tools/ota-server/device_requests.jsonl
# 健康检查：curl http://127.0.0.1:8080/healthz
```

## 发版流程

```bash
# 1) 构建产物放到 firmware 目录（app 镜像，不是整片 flash）
cp build/xiaozhi.bin tools/ota-server/firmware/xiaozhi_lcd20_2.5.0.bin
# 2) 改 releases.json 的 version / file（force=1 为强制升级）
# 3) 设备端 ota_url 指向本服务
```

## 设备端如何指向自建 OTA

| 方式 | 说明 |
|---|---|
| 编译期 | `CONFIG_OTA_URL="http://<PC-IP>:8080/ota/"`（`main/Kconfig.projbuild` 顶部 `OTA_URL`） |
| 运行期 | NVS `settings["wifi"]["ota_url"]`（优先级更高，烧一次即可切换，不用重编译） |

> 注意：`ota.cc` 要求响应 **HTTP 200**；局域网 http 即可（无需 https）。若要公网部署，建议前置 nginx/caddy 做 TLS。

## 限制

- 只分发**整包 app 镜像**（写 `ota_0`/`ota_1`，各 4MB，当前镜像 2.77MB，余量 1.26MB）。
- **assets 分区不参与 OTA**（换字体/emoji/内置音乐需整片烧 `assets.bin`）。
- 设备已开 `BOOTLOADER_APP_ROLLBACK_ENABLE`：新固件若启动失败（未调用 `esp_ota_mark_app_valid_cancel_rollback`）会自动回滚到上一个可用分区。
