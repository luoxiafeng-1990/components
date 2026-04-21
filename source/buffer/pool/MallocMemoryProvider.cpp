#include "buffer/pool/MallocMemoryProvider.hpp"
#include "vendor/contracts/MemoryProviderRegistry.hpp"
#include <cstdlib>
#include <cstring>

MallocMemoryProvider::MallocMemoryProvider(size_t default_alignment)
    : default_alignment_(default_alignment)
{}

MemoryBlock MallocMemoryProvider::allocate(size_t size, size_t alignment) {
    MemoryBlock blk{};
    size_t align = (alignment > 0) ? alignment : default_alignment_;
    void* ptr = nullptr;

    if (align > 0) {
        if (posix_memalign(&ptr, align, size) != 0) return blk;
    } else {
        ptr = malloc(size);
        if (!ptr) return blk;
    }

    memset(ptr, 0, size);
    blk.virt_addr = ptr;
    blk.size      = size;
    return blk;
}

void MallocMemoryProvider::deallocate(MemoryBlock& blk) {
    if (blk.virt_addr) {
        free(blk.virt_addr);
    }
    blk = {};
}

MemoryProviderCapabilities MallocMemoryProvider::getCapabilities() const {
    return {
        .supports_physical_address = false,
        .supports_dma              = false,
        .is_cache_coherent         = true,
        .default_alignment         = default_alignment_
    };
}

std::unique_ptr<IMemoryProvider> MallocMemoryProvider::clone() const {
    return std::make_unique<MallocMemoryProvider>(default_alignment_);
}

namespace {
    static const bool malloc_provider_registered = []() {
        MemoryProviderRegistry::instance().registerProvider(
            "malloc", []() { return std::make_unique<MallocMemoryProvider>(); });
        return true;
    }();
}
