#include "vendor/taco/display/TacoProDisplayExtension.hpp"

std::unique_ptr<IDisplayVendorExtension> TacoProDisplayExtension::clone() const {
    return std::make_unique<TacoProDisplayExtension>(*this);
}

bool TacoProDisplayExtension::validate(std::string& err) const {
    if (screen_width <= 0 || screen_height <= 0) {
        err = "tacopro: screen_width/screen_height must be > 0";
        return false;
    }
    return true;
}
