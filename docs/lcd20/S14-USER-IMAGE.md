# P4：开机图 / 待机背景图 + 网页设置分类卡片

## 网页改版（分类卡片 + tab）

`http://<设备IP>:8080/` 顶部新增 5 个分类 tab，竖屏手机上一屏一个类别：

| Tab | 内容 |
|---|---|
| **显示** | 主题、亮度、待机页开关、城市、指数/个股、**待机背景图** |
| **外观** | **开机图片**、字体/表情资源包 |
| **声音** | 音量、音乐播放时压低比例 |
| **灯光** | 状态灯模式、固定颜色 |
| **系统** | 音乐服务地址、重启设备、当前状态 JSON |

保存按钮固定在最下方，改动跨 tab 一起提交（一次 POST `/api/settings`）。

## 图片功能

| | 开机图 | 待机背景图 |
|---|---|---|
| 显示时机 | 开机后立即显示 **2 秒**（`esp_timer` 到点自动移除） | 待机页文字**下方**，带 43% 黑色遮罩保证可读 |
| 尺寸 | 网页端自动 cover 裁剪成 **240×320** | 同左 |
| 格式 | JPEG q0.85（约 15~30KB） | 同左 |
| 生效 | 上传后需重启 | 上传后立即生效 |

网页用 `canvas` 缩放 → `toBlob('image/jpeg',0.85)` → `POST /api/image?kind=boot|bg`（裸二进制，
不用 multipart/base64，35KB 以内很轻）。预览走 `GET /api/image?kind=..` 回读设备里的原图。

### 存储布局（关键设计）

16MB flash 已经全部分配完（2×4MB app + 8MB assets），没有多余分区，所以图片**复用 assets 分区尾部**：

```
assets 分区 (8MB)
├─ 0x000000 … ~0x1B0000   assets.bin（字体/emoji/srmodels，约 1.7MB）
├─ …                      空闲
├─ 0x700000 (size-1MB)    用户图片区起点
│   ├─ +0x00000  开机图槽（480KB）= 32B 头 + JPEG
│   └─ +0x78000  背景图槽（480KB）
└─ 0x800000
```

槽位头：`{magic "XZIM", length, width, height, format=1(JPEG), reserved}`。

**配套改动**：资源包热更新（S12 的两阶段流程）原来 `esp_partition_erase_range(assets, 0, size)`
会把整个 8MB 擦掉、连用户图片一起清空。现在只擦 `roundup(assets.bin 长度, 4KB)`，
实测换过一次字体包后，两张图的 MD5 完全不变。

### 解码

用项目自带的 `jpeg_to_image()`（软件解码兜底，RGB565），解码一次进 PSRAM（240×320 = 150KB），
再交给 LVGL 的 `lv_image_dsc_t`，避免 LVGL 反复解码、也不需要 mmap。

## 代码

| 文件 | 作用 |
|---|---|
| `main/ui/user_image.{h,cc}` | 槽位管理（读/写/清除/解码）+ 开机图显示（2s 定时移除） |
| `main/ui/standby_page.*` | 新增 `SetBackground(dsc)`：图片 + 遮罩层垫在所有文字下面 |
| `main/ui/standby_service.*` | `ReloadBackground()`：上传后重新解码并刷新 |
| `main/web/admin_server.cc` | `GET/POST /api/image`、`POST /api/reboot`；网页改版为 5 个 tab |
| `main/web/asset_ota.cc` | 擦除范围由「整分区」改为「assets.bin 实际长度」 |
| `main/application.cc` | 启动时 `Init()` + `ShowBootSplash(display, 2)` |

## 实测

```
上传背景图：POST /api/image?kind=bg 5337B -> {"ok":true}
            UserImage: image saved: kind=1 5337 bytes
            UserImage: background decoded: 240x320 (153600 bytes)
            Standby: standby background image applied (240x320)
回读校验：  GET /api/image?kind=bg -> 200 5337B，MD5 与上传文件一致 ✓
开机图：    上传 kind=boot -> 重启 ->
            UserImage: boot image decoded: 240x320
            UserImage: boot splash shown for 2 s (240x320)
资源包热更新后：boot/bg 两张图 MD5 均不变 ✓（说明尾部用户区被保住）
固件体积：2.86MB -> 2.97MB（app 分区 76%）
```

## 限制

- 只接受 JPEG（浏览器 canvas 统一产出 JPEG）；设备侧不做 PNG/透明通道
- 图片固定 240×320 cover 裁剪，超宽图会被裁掉两侧
- 开机图 2 秒硬编码（`ShowBootSplash(display, 2)`），要改需重新编译
- 用户图片区 1MB / 两槽各 480KB；上限 400KB（HTTP 侧校验）
- 待机背景只在待机页生效，音乐页/对话页不套用
