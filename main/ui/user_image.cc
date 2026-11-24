#include "user_image.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_timer.h>

#include "jpg/jpeg_to_image.h"

#include <cstring>

#define TAG "UserImage"

namespace {
constexpr uint32_t kMagic = 0x4D495A58;      /* "XZIM" */
constexpr size_t kAreaSize = 0x100000;       /* assets 分区尾部预留 1MB */
constexpr size_t kSlotSize = 0x78000;        /* 每槽 480KB */
constexpr size_t kHdrSize = 32;
constexpr size_t kMaxImage = kSlotSize - kHdrSize;

struct ImageHeader {
    uint32_t magic;
    uint32_t length;
    uint32_t width;
    uint32_t height;
    uint32_t format;   /* 1 = JPEG */
    uint32_t reserved[3];
};

void HideSplashCb(void* arg) {
    auto* obj = static_cast<lv_obj_t**>(arg);
    if (obj != nullptr && *obj != nullptr) {
        lv_obj_delete(*obj);
        *obj = nullptr;
    }
    heap_caps_free(arg);
}
}  // namespace

UserImage& UserImage::GetInstance() {
    static UserImage instance;
    return instance;
}

bool UserImage::SlotOffset(Kind kind, size_t* off, size_t* size) {
    if (!ready_) {
        return false;
    }
    const size_t idx = (kind == kBackground) ? 1 : 0;
    *off = area_base_ + idx * kSlotSize;
    *size = kSlotSize;
    return true;
}

bool UserImage::Init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ready_) {
        return true;
    }
    auto* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (part == nullptr || part->size < kAreaSize + 0x100000) {
        ESP_LOGW(TAG, "assets partition too small for user images");
        return false;
    }
    part_ = const_cast<esp_partition_t*>(part);
    area_base_ = part->size - kAreaSize;
    slot_size_ = kSlotSize;
    ready_ = true;
    ESP_LOGI(TAG, "user image area: offset 0x%06x (%u KB per slot)", (unsigned)area_base_,
             (unsigned)(kSlotSize / 1024));
    return true;
}

bool UserImage::ReadHeader(Kind kind, uint32_t* len, uint32_t* w, uint32_t* h) {
    size_t off = 0, size = 0;
    if (!SlotOffset(kind, &off, &size)) {
        return false;
    }
    ImageHeader hdr = {};
    if (esp_partition_read(static_cast<const esp_partition_t*>(part_), off, &hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    if (hdr.magic != kMagic || hdr.length == 0 || hdr.length > kMaxImage) {
        return false;
    }
    if (len != nullptr) {
        *len = hdr.length;
    }
    if (w != nullptr) {
        *w = hdr.width;
    }
    if (h != nullptr) {
        *h = hdr.height;
    }
    return true;
}

bool UserImage::Has(Kind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ReadHeader(kind, nullptr, nullptr, nullptr);
}

uint32_t UserImage::length(Kind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t len = 0;
    return ReadHeader(kind, &len, nullptr, nullptr) ? len : 0;
}

bool UserImage::BeginWrite(Kind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t off = 0, size = 0;
    if (!SlotOffset(kind, &off, &size)) {
        return false;
    }
    esp_err_t err = esp_partition_erase_range(static_cast<const esp_partition_t*>(part_), off, size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase slot failed: %s", esp_err_to_name(err));
        return false;
    }
    writing_ = kind;
    written_ = 0;
    write_limit_ = kMaxImage;
    return true;
}

bool UserImage::WriteChunk(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t off = 0, size = 0;
    if (!SlotOffset(writing_, &off, &size) || len == 0) {
        return false;
    }
    if (written_ + len > write_limit_) {
        ESP_LOGE(TAG, "image too large (%u + %u > %u)", (unsigned)written_, (unsigned)len,
                 (unsigned)write_limit_);
        return false;
    }
    esp_err_t err = esp_partition_write(static_cast<const esp_partition_t*>(part_),
                                        off + kHdrSize + written_, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write image failed: %s", esp_err_to_name(err));
        return false;
    }
    written_ += len;
    return true;
}

bool UserImage::EndWrite(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t off = 0, size = 0;
    if (!SlotOffset(writing_, &off, &size) || written_ < 64) {
        return false;
    }
    ImageHeader hdr = {};
    hdr.magic = kMagic;
    hdr.length = static_cast<uint32_t>(written_);
    hdr.width = width;
    hdr.height = height;
    hdr.format = 1;   /* JPEG */
    esp_err_t err = esp_partition_write(static_cast<const esp_partition_t*>(part_), off, &hdr, sizeof(hdr));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write header failed: %s", esp_err_to_name(err));
        return false;
    }
    gen_++;
    ESP_LOGI(TAG, "image saved: kind=%d %u bytes", (int)writing_, (unsigned)written_);
    written_ = 0;
    return true;
}

void UserImage::Clear(Kind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t off = 0, size = 0;
    if (!SlotOffset(kind, &off, &size)) {
        return;
    }
    ImageHeader hdr = {};
    if (esp_partition_erase_range(static_cast<const esp_partition_t*>(part_), off, 4096) == ESP_OK) {
        esp_partition_write(static_cast<const esp_partition_t*>(part_), off, &hdr, sizeof(hdr));
    }
    gen_++;
    ESP_LOGI(TAG, "image cleared: kind=%d", (int)kind);
}

size_t UserImage::Read(Kind kind, uint8_t* out, size_t max_len) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t off = 0, size = 0;
    uint32_t len = 0;
    if (!SlotOffset(kind, &off, &size) || !ReadHeader(kind, &len, nullptr, nullptr) || out == nullptr) {
        return 0;
    }
    if (len > max_len) {
        return 0;
    }
    if (esp_partition_read(static_cast<const esp_partition_t*>(part_), off + kHdrSize, out, len) != ESP_OK) {
        return 0;
    }
    return len;
}

bool UserImage::Decoded(Kind kind, uint8_t** rgb, size_t* rgb_len, uint32_t* width, uint32_t* height) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int idx = (kind == kBackground) ? 1 : 0;
    if (cached_rgb_[idx] != nullptr && cached_gen_[idx] == gen_) {
        if (rgb) {
            *rgb = cached_rgb_[idx];
        }
        if (rgb_len) {
            *rgb_len = cached_len_[idx];
        }
        if (width) {
            *width = cached_w_[idx];
        }
        if (height) {
            *height = cached_h_[idx];
        }
        return true;
    }

    size_t off = 0, size = 0;
    uint32_t len = 0;
    if (!SlotOffset(kind, &off, &size) || !ReadHeader(kind, &len, nullptr, nullptr)) {
        return false;
    }
    auto* jpeg = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (jpeg == nullptr) {
        jpeg = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_8BIT));
    }
    if (jpeg == nullptr) {
        ESP_LOGE(TAG, "no memory for %u byte image", (unsigned)len);
        return false;
    }
    if (esp_partition_read(static_cast<const esp_partition_t*>(part_), off + kHdrSize, jpeg, len) != ESP_OK) {
        heap_caps_free(jpeg);
        return false;
    }

    uint8_t* out = nullptr;
    size_t out_len = 0, w = 0, h = 0, stride = 0;
    esp_err_t err = jpeg_to_image(jpeg, len, &out, &out_len, &w, &h, &stride);
    heap_caps_free(jpeg);
    if (err != ESP_OK || out == nullptr) {
        ESP_LOGW(TAG, "jpeg decode failed: %s", esp_err_to_name(err));
        return false;
    }

    if (cached_rgb_[idx] != nullptr) {
        heap_caps_free(cached_rgb_[idx]);
    }
    cached_rgb_[idx] = out;
    cached_len_[idx] = out_len;
    cached_w_[idx] = static_cast<uint32_t>(w);
    cached_h_[idx] = static_cast<uint32_t>(h);
    cached_gen_[idx] = gen_;
    ESP_LOGI(TAG, "%s decoded: %ux%u (%u bytes)", kind == kBackground ? "background" : "boot image",
             (unsigned)w, (unsigned)h, (unsigned)out_len);

    if (rgb) {
        *rgb = out;
    }
    if (rgb_len) {
        *rgb_len = out_len;
    }
    if (width) {
        *width = cached_w_[idx];
    }
    if (height) {
        *height = cached_h_[idx];
    }
    return true;
}

void UserImage::ShowBootSplash(Display* display, int seconds) {
    if (display == nullptr) {
        return;
    }
    uint8_t* rgb = nullptr;
    size_t len = 0;
    uint32_t w = 0, h = 0;
    if (!Decoded(kBoot, &rgb, &len, &w, &h)) {
        return;
    }

#ifdef HAVE_LVGL
    DisplayLockGuard guard(display);
    boot_dsc_ = {};
    boot_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    boot_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    boot_dsc_.header.w = static_cast<uint32_t>(w);
    boot_dsc_.header.h = static_cast<uint32_t>(h);
    boot_dsc_.data_size = len;
    boot_dsc_.data = rgb;

    lv_obj_t* img = lv_image_create(lv_screen_active());
    lv_image_set_src(img, &boot_dsc_);
    lv_obj_center(img);
    lv_obj_move_foreground(img);

    auto* holder = static_cast<lv_obj_t**>(heap_caps_malloc(sizeof(lv_obj_t*), MALLOC_CAP_8BIT));
    if (holder == nullptr) {
        lv_obj_delete(img);
        return;
    }
    *holder = img;
    esp_timer_handle_t timer = nullptr;
    esp_timer_create_args_t args = {};
    args.callback = HideSplashCb;
    args.arg = holder;
    args.name = "splash_hide";
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, static_cast<uint64_t>(seconds > 0 ? seconds : 2) * 1000000ULL);
    }
    ESP_LOGI(TAG, "boot splash shown for %d s (%ux%u)", seconds, (unsigned)w, (unsigned)h);
#else
    (void)seconds;
#endif
}
