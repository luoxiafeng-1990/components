#include "vendor/taco/display/TacoDisplayExtension.hpp"

std::unique_ptr<IDisplayVendorExtension> TacoDisplayExtension::clone() const {
    return std::make_unique<TacoDisplayExtension>(*this);
}

bool TacoDisplayExtension::validate(std::string& err) const {
    if (display_pp_channel != -1 && display_pp_channel != 0 && display_pp_channel != 1) {
        err = "TacoDisplayExtension: display_pp_channel must be -1, 0, or 1";
        return false;
    }
    return true;
}
