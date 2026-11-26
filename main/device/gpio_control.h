#ifndef GPIO_CONTROL_H
#define GPIO_CONTROL_H

#include <string>
#include <vector>

/**
 * 用户自定义 GPIO 开关（例如把某个脚配成"打印机灯光"）
 *
 * - 配置存 NVS：命名空间 "gpio"，键 "config"（JSON 数组）
 * - 最多 4 路，每路：名称 / GPIO 号 / 有效电平 / 初始状态
 * - 引脚安全：板子自用脚 + flash/USB/UART 等一律拒绝（含冲突提示）
 * - 语音控制：注册 MCP 工具 self.gpio.list / self.gpio.set，大模型可直接调用
 */
struct GpioSwitchConfig {
    std::string name;
    int gpio = -1;
    bool active_high = true;    /* 高电平点亮；继电器低电平触发就设 false */
    bool initial_on = false;
};

class GpioControl {
public:
    static GpioControl& GetInstance();

    /** 读 NVS 并配置引脚（Application::Initialize 调用） */
    void Initialize();

    /** 当前配置（不含实时状态） */
    std::vector<GpioSwitchConfig> Devices() const;

    /** 保存配置：先校验（名称/引脚/冲突），全部通过才落盘并立即应用 */
    bool Save(const std::vector<GpioSwitchConfig>& devices, std::string* error);

    /** 按名称控制；state 支持 "on"/"off"/"toggle"/"1"/"0" */
    bool SetByState(const std::string& name_or_index, const std::string& state, std::string* message);
    bool SetState(const std::string& name, bool on);
    bool GetState(const std::string& name, bool* on) const;

    /** 形如 "打印机灯光(GPIO3)=开; 台灯(GPIO12)=关"，给大模型看的 */
    std::string Describe() const;

    /** 注册通用工具（幂等）：self.gpio.list / self.gpio.set */
    void RegisterMcpTools();

    /** 按当前配置重建"每路独立工具"：self.gpio.sw{n}_on / _off / _state */
    void RebuildPerDeviceTools();

    /** 引脚是否被板子占用；reason 返回原因 */
    bool IsReserved(int gpio, std::string* reason) const;

    struct UsedPin {
        int gpio;
        std::string reason;
    };
    /** 所有已占用引脚（板载 + 固定禁用 + 已配置开关），供网页列"说明栏" */
    std::vector<UsedPin> UsedPins() const;
    /** 当前可选的引脚（ESP32-S3 有效脚 - 已占用） */
    std::vector<int> AvailablePins() const;

private:
    GpioControl() = default;
};

#endif  // GPIO_CONTROL_H
