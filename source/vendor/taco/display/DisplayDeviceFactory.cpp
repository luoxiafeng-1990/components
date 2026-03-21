#include "vendor/taco/display/DisplayDeviceFactory.hpp"
#include "vendor/taco/display/TacoVODisplayDevice.hpp"
#include "vendor/taco/display/SharedFramebufferDevice.hpp"

std::unique_ptr<IDisplayDevice> DisplayDeviceFactory::create(const DisplayType& config) {
    switch (config.mode) {
        case DisplayType::TACO_VO:
            return std::make_unique<TacoVODisplayDevice>(config.taco_vo);
        case DisplayType::SHARED_FB:
        default:
            return std::make_unique<SharedFramebufferDevice>(config.taco_vo);
    }
}
