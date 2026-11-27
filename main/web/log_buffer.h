#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include <string>

/**
 * 日志环形缓冲：把 esp_log 的输出同时存进内存，通过 GET /api/log 读取。
 *
 * 背景：这块板子的 USB Serial/JTAG 偶尔会失效（串口一个字都收不到，但设备还在跑），
 * 出问题时没法抓日志。有了这个就能用手机网页直接看最近日志。
 *
 * 实现：挂钩 esp_log_set_vprintf，格式化后 ①写入 8KB 环形缓冲（PSRAM）②照常输出。
 */
namespace LogBuffer {

/** 在 app_main 最开头调用（越早越好，能抓到启动日志） */
void Init();

/** 取最近 n 行日志（按输出顺序） */
std::string Tail(int n);

}  // namespace LogBuffer

#endif  // LOG_BUFFER_H
