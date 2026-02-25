#include "display/DisplayDeviceFactory.hpp"
#include "display/LinuxFramebufferDevice.hpp"
#include "display/TacoVODisplayDevice.hpp"
#include "display/SharedFramebufferDevice.hpp"

std::unique_ptr<IDisplayDevice> DisplayDeviceFactory::create(const DisplayType& config) {
    switch (config.mode) {
        case DisplayType::TACO_VO:
            return std::make_unique<TacoVODisplayDevice>(config.taco_vo);
        case DisplayType::SHARED_FB:
            return std::make_unique<SharedFramebufferDevice>(config.taco_vo);
        case DisplayType::FRAMEBUFFER:
        default:
            return std::make_unique<LinuxFramebufferDevice>();
    }
}
