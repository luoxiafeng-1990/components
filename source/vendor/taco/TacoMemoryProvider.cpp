#include "vendor/taco/memory/TacoMemoryProvider.hpp"
#include "vendor/contracts/MemoryProviderRegistry.hpp"
#include <log4cplus/loggingmacros.h>
#include <cstring>

extern "C" {
#include "ta_sys_api.h"
}

TacoMemoryProvider::TacoMemoryProvider(const std::string& zone_name)
    : zone_name_(zone_name)
    , logger_(log4cplus::Logger::getInstance(
          LOG4CPLUS_TEXT("components.MemoryProvider.Taco")))
{}

MemoryBlock TacoMemoryProvider::allocate(size_t size, size_t /*alignment*/) {
    MemoryBlock blk{};

    uint32_t blk_id = taco_sys_get_block(TACO_INVALID_POOLID, size, zone_name_.c_str());
    if (blk_id == 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "taco_sys_get_block failed (size=%zu)", size);
        return blk;
    }

    uint64_t phys = taco_sys_handle2_phys_addr(blk_id);
    void* virt = taco_sys_mmap_noncache(phys, static_cast<uint32_t>(size));
    if (!virt) {
        LOG4CPLUS_ERROR_FMT(logger_, "taco_sys_mmap_noncache failed (blk_id=%u)", blk_id);
        taco_sys_release_block(blk_id);
        return blk;
    }

    memset(virt, 0, size);

    blk.virt_addr = virt;
    blk.phys_addr = phys;
    blk.size      = size;
    blk.handle    = blk_id;
    return blk;
}

void TacoMemoryProvider::deallocate(MemoryBlock& blk) {
    if (blk.virt_addr) {
        taco_sys_munmap(blk.virt_addr, static_cast<uint32_t>(blk.size));
    }
    if (blk.handle != 0) {
        taco_sys_release_block(blk.handle);
    }
    blk = {};
}

MemoryProviderCapabilities TacoMemoryProvider::getCapabilities() const {
    return {
        .supports_physical_address = true,
        .supports_dma              = true,
        .is_cache_coherent         = false,
        .default_alignment         = 4096
    };
}

std::unique_ptr<IMemoryProvider> TacoMemoryProvider::clone() const {
    return std::make_unique<TacoMemoryProvider>(zone_name_);
}

namespace {
    static const bool taco_provider_registered = []() {
        MemoryProviderRegistry::instance().registerProvider(
            "taco", []() { return std::make_unique<TacoMemoryProvider>(); });
        return true;
    }();
}
