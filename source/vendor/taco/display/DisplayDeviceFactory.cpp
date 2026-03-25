#include "vendor/taco/display/DisplayDeviceFactory.hpp"
#include "vendor/taco/display/TacoProDisplayDevice.hpp"
#include "vendor/taco/display/TacoDisplayDevice.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

std::unique_ptr<IDisplayDevice> DisplayDeviceFactory::create(const IDisplayVendorExtension& vendor) {
    const char* k = vendor.kind();

    if (std::strcmp(k, "tacopro") == 0) {
        const auto& ext = static_cast<const TacoProDisplayExtension&>(vendor);
        return std::make_unique<TacoProDisplayDevice>(ext);
    }

    if (std::strcmp(k, "taco") == 0) {
        const auto& ext = static_cast<const TacoDisplayExtension&>(vendor);
        return std::make_unique<TacoDisplayDevice>(ext);
    }

    throw std::invalid_argument(std::string("DisplayDeviceFactory: unknown vendor '") + k + "'");
}
