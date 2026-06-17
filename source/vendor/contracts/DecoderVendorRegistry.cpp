#include "vendor/contracts/DecoderVendorRegistry.hpp"

DecoderVendorRegistry& DecoderVendorRegistry::instance() {
    static DecoderVendorRegistry registry;
    return registry;
}

void DecoderVendorRegistry::registerVendor(std::string kind, Vendor f) {
    std::lock_guard<std::mutex> lock(mu_);
    factories_[std::move(kind)] = std::move(f);
}

std::unique_ptr<IDecoderVendorExtension> DecoderVendorRegistry::create(const std::string& kind) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = factories_.find(kind);
    if (it == factories_.end()) {
        return nullptr;
    }
    return it->second();
}
