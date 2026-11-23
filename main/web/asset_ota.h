#ifndef ASSET_OTA_H
#define ASSET_OTA_H

#include <string>

/**
 * 外观资源（字体/表情）安全热更新 —— 两阶段，避免"边用边写"导致崩溃/变砖
 *
 * 阶段 1（运行中）：把 assets.bin 下载到 **ota_1 分区**（4MB，非当前运行分区）暂存，
 *                   校验内部 checksum 后写 NVS 标记，然后重启。
 * 阶段 2（启动早期，任何 assets 使用者之前）：检测到标记 → 从 ota_1 读出暂存内容 →
 *                   擦除并写入 assets 分区 → 清除标记 → 再重启。
 *
 * 这样 assets 分区只在"无人使用"的启动早期被改写，旧资源在切换前始终完好；
 * 任一阶段掉电，最坏情况是暂存无效（开机校验失败→丢弃标记），不会变砖。
 */
namespace AssetOta {

/** 启动最早阶段调用（app_main 开头）：若有暂存任务则执行并重启，不会返回 */
void ApplyIfPending();

/** 下载资源包到暂存分区（运行中调用，成功返回 true，需调用方随后重启） */
bool StageFromUrl(const std::string& url);

}  // namespace AssetOta

#endif  // ASSET_OTA_H
