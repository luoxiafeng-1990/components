#include "vendor/taco/display/TacoProDisplayExtension.hpp"

std::unique_ptr<IDisplayVendorExtension> TacoProDisplayExtension::clone() const {
    return std::make_unique<TacoProDisplayExtension>(*this);
}

bool TacoProDisplayExtension::validate(std::string& err) const {
    if (display_pp_channel != -1 && display_pp_channel != 0 && display_pp_channel != 1) {
        err = "TacoProDisplayExtension: display_pp_channel must be -1, 0, or 1";
        return false;
    }
    return true;
}
