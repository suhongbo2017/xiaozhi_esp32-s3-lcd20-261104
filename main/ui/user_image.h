#ifndef USER_IMAGE_H
#define USER_IMAGE_H

#include "display.h"

#include <cstddef>
#include <cstdint>
#include <mutex>

#ifdef HAVE_LVGL
#ifdef __cplusplus
extern "C" {
#endif
#include <lvgl.h>
#ifdef __cplusplus
}
#endif
#endif

/**
 * 用户图片（开机图 / 待机背景图）
 *
 * 存储：assets 分区**尾部**预留 1MB（两个 448KB 槽位），
 *       不跟 assets.bin 抢空间（assets.bin 目前 ~1.7MB，8MB 分区足够）。
 *       资源包热更新时刻意只擦除 assets.bin 实际长度对应的扇区，保住这块用户区。
 *
 * 格式：槽位头部 32 字节 { magic, length, width, height, format }，后面是原始 JPEG 字节。
 *       浏览器端用 canvas 统一缩放到 240x320、JPEG 0.85（约 15~30KB）。
 *
 * 显示：解码一次成 RGB565（240x320 = 150KB，放 PSRAM），再交给 LVGL，
 *       避免 LVGL 反复解码 + 支持 mmap 无关的稳定指针。
 */
class UserImage {
public:
    enum Kind { kBoot = 0, kBackground = 1, kChatBackground = 2 };

    static UserImage& GetInstance();

    /** 定位分区/槽位；返回 false 表示该板型没有可用 assets 分区 */
    bool Init();

    bool Has(Kind kind);
    uint32_t length(Kind kind);

    /* ---- 写入（HTTP 分块调用） ---- */
    bool BeginWrite(Kind kind);
    bool WriteChunk(const uint8_t* data, size_t len);
    /** 写完提交头部（w/h 可为 0，交给解码时探测）；失败会自动清空槽位 */
    bool EndWrite(uint32_t width, uint32_t height);
    /** 取消/清空 */
    void Clear(Kind kind);

    /* ---- 读取（供网页回读预览） ---- */
    /** 读取最多 max_len 字节原始 JPEG；返回实际长度 */
    size_t Read(Kind kind, uint8_t* out, size_t max_len);

    /* ---- 显示 ---- */
    /** 待机页背景：成功时返回指向解码数据的 LVGL 描述符（生命周期由 UserImage 管理） */
    bool Decoded(Kind kind, uint8_t** rgb, size_t* rgb_len, uint32_t* width, uint32_t* height);

    /** 开机图：显示 seconds 秒后自动移除（在 Application::Initialize 里调用） */
    void ShowBootSplash(Display* display, int seconds);

private:
    UserImage() = default;

    bool SlotOffset(Kind kind, size_t* off, size_t* size);
    bool ReadHeader(Kind kind, uint32_t* len, uint32_t* w, uint32_t* h);

    std::mutex mutex_;
    void* part_ = nullptr;      /* const esp_partition_t* */
    size_t area_base_ = 0;
    size_t slot_size_ = 0;
    bool ready_ = false;
    Kind writing_ = kBoot;
    size_t written_ = 0;
    size_t write_limit_ = 0;
    uint32_t gen_ = 0;          /* 每次写入 +1，用于失效已解码缓存 */
    static constexpr int kSlotCount = 3;
    uint32_t cached_gen_[kSlotCount] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    uint8_t* cached_rgb_[kSlotCount] = {nullptr, nullptr, nullptr};
    size_t cached_len_[kSlotCount] = {0, 0, 0};
    uint32_t cached_w_[kSlotCount] = {0, 0, 0};
    uint32_t cached_h_[kSlotCount] = {0, 0, 0};
#ifdef HAVE_LVGL
    lv_image_dsc_t boot_dsc_ = {};
    lv_obj_t* splash_ = nullptr;
#endif
};

#endif  // USER_IMAGE_H
