#include "vendor/contracts/MemoryProviderRegistry.hpp"
#include <stdexcept>

MemoryProviderRegistry& MemoryProviderRegistry::instance() {
    static MemoryProviderRegistry reg;
    return reg;
}

void MemoryProviderRegistry::registerProvider(std::string kind, Factory f) {
    std::lock_guard<std::mutex> lk(mu_);
    factories_[std::move(kind)] = std::move(f);
}

std::unique_ptr<IMemoryProvider> MemoryProviderRegistry::create(
    const std::string& kind) const
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = factories_.find(kind);
    if (it == factories_.end()) return nullptr;
    return it->second();
}

bool MemoryProviderRegistry::hasProvider(const std::string& kind) const {
    std::lock_guard<std::mutex> lk(mu_);
    return factories_.count(kind) > 0;
}
