#ifndef DISPLAY_DEVICE_FACTORY_HPP
#define DISPLAY_DEVICE_FACTORY_HPP

#include "vendor/contracts/IDisplayDevice.hpp"
#include "productionline/worker/WorkerConfig.hpp"

#include <memory>

/**
 * DisplayDeviceFactory - 显示设备工厂
 *
 * 根据 DisplayType 配置创建对应的 IDisplayDevice 实现。
 * 遵循开闭原则：新增显示后端只需修改此工厂。
 *
 * 当前支持：
 * - TACO_VO:     TacoVODisplayDevice（旧 taco-vo 实现）
 * - SHARED_FB:   SharedFramebufferDevice（SharedDisplayContext + BufferPool 多通道，默认）
 */
class DisplayDeviceFactory {
public:
    using DisplayType = WorkerConfig::ConsumerTypeConfig::DisplayType;

    static std::unique_ptr<IDisplayDevice> create(const DisplayType& config);
};

#endif // DISPLAY_DEVICE_FACTORY_HPP
