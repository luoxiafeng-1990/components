#include "vendor/taco/display/TacoDisplayExtension.hpp"

std::unique_ptr<IDisplayVendorExtension> TacoDisplayExtension::clone() const {
    return std::make_unique<TacoDisplayExtension>(*this);
}

bool TacoDisplayExtension::validate(std::string& err) const {
    if (frame_width <= 0 || frame_height <= 0) {
        err = "taco: frame_width/frame_height must be > 0";
        return false;
    }
    return true;
}
