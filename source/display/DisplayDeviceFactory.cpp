#include "display/DisplayDeviceFactory.hpp"
#include "display/LinuxFramebufferDevice.hpp"
#include "display/TacoVODisplayDevice.hpp"

std::unique_ptr<IDisplayDevice> DisplayDeviceFactory::create(const DisplayType& config) {
    switch (config.mode) {
        case DisplayType::TACO_VO:
            return std::make_unique<TacoVODisplayDevice>(config.taco_vo);
        case DisplayType::FRAMEBUFFER:
        default:
            return std::make_unique<LinuxFramebufferDevice>();
    }
}
