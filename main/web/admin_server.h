#ifndef ADMIN_SERVER_H
#define ADMIN_SERVER_H

/**
 * 设备管理网页（P1：运行期配置，WiFi 下发，立即生效）
 *
 *   GET  /                 -> 单页管理界面（内嵌 HTML/CSS/JS，不依赖外网）
 *   GET  /api/status       -> 当前配置 JSON
 *   POST /api/settings     -> 应用并持久化（NVS）
 *
 * 可改项：主题(light/dark)、亮度、音量、音乐 ducking、状态灯(颜色/关闭)、
 *         音乐服务地址、唤醒词开关。
 *
 * 端口 8080（80 端口留给配网页 ConfigServer，避免冲突）。
 */
class AdminServer {
public:
    /** 启动管理网页（幂等；网络未就绪也能监听，有 IP 后即可访问） */
    static bool Start();

    /** 读取持久化配置并应用（开机调用：主题/亮度/音量等） */
    static void ApplyPersisted();
};

#endif  // ADMIN_SERVER_H
