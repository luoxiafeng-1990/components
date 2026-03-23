#ifndef DISPLAY_DEVICE_FACTORY_HPP
#define DISPLAY_DEVICE_FACTORY_HPP

#include "vendor/contracts/IDisplayDevice.hpp"
#include "vendor/contracts/DisplayVendorExtension.hpp"

#include <memory>

/**
 * DisplayDeviceFactory - 显示设备工厂
 *
 * 根据 IDisplayVendorExtension::kind() 创建对应的 IDisplayDevice 实现。
 *
 * 当前支持：
 * - "tacopro" → TacoProDisplayDevice
 * - "taco"    → TacoDisplayDevice
 */
class DisplayDeviceFactory {
public:
    static std::unique_ptr<IDisplayDevice> create(const IDisplayVendorExtension& vendor);
};

#endif // DISPLAY_DEVICE_FACTORY_HPP
