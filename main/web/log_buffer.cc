#include "log_buffer.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#define TAG "LogBuffer"

namespace {

constexpr size_t kSize = 8 * 1024;   /* 环形缓冲大小 */

char* g_buf = nullptr;               /* PSRAM */
size_t g_head = 0;                   /* 写指针 */
size_t g_used = 0;                   /* 已用字节（<= kSize） */
vprintf_like_t g_prev = nullptr;
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

void Append(const char* data, size_t len) {
    if (g_buf == nullptr || len == 0) {
        return;
    }
    if (len > kSize) {                 /* 单行超长：只留尾部 */
        data += (len - kSize);
        len = kSize;
    }
    const size_t first = (len < kSize - g_head) ? len : (kSize - g_head);
    memcpy(g_buf + g_head, data, first);
    if (len > first) {
        memcpy(g_buf, data + first, len - first);
    }
    g_head = (g_head + len) % kSize;
    g_used = (g_used + len > kSize) ? kSize : (g_used + len);
}

int LogHook(const char* fmt, va_list args) {
    char line[256];

    va_list copy;
    va_copy(copy, args);
    const int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    if (n > 0) {
        portENTER_CRITICAL_SAFE(&g_lock);
        Append(line, strnlen(line, sizeof(line)));
        portEXIT_CRITICAL_SAFE(&g_lock);
    }

    if (g_prev != nullptr) {
        return g_prev(fmt, args);
    }
    return vprintf(fmt, args);
}

}  // namespace

namespace LogBuffer {

void Init() {
    if (g_buf != nullptr) {
        return;
    }
    g_buf = static_cast<char*>(heap_caps_malloc(kSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_buf == nullptr) {
        g_buf = static_cast<char*>(heap_caps_malloc(kSize, MALLOC_CAP_8BIT));
    }
    if (g_buf == nullptr) {
        return;
    }
    g_head = 0;
    g_used = 0;
    g_prev = esp_log_set_vprintf(LogHook);   /* 之后所有日志都会经过钩子 */
}

std::string Tail(int n) {
    if (g_buf == nullptr || n <= 0) {
        return {};
    }
    std::string all;
    portENTER_CRITICAL_SAFE(&g_lock);
    all.reserve(g_used);
    const size_t start = (g_head + kSize - g_used) % kSize;
    for (size_t i = 0; i < g_used; i++) {
        all.push_back(g_buf[(start + i) % kSize]);
    }
    portEXIT_CRITICAL_SAFE(&g_lock);

    if (n <= 0) {
        return all;
    }
    /* 从尾部往前数 n 个换行 */
    int lines = 0;
    size_t pos = all.size();
    while (pos > 0) {
        pos--;
        if (all[pos] == '\n') {
            if (++lines > n) {
                pos++;
                break;
            }
        }
    }
    return all.substr(pos);
}

}  // namespace LogBuffer
