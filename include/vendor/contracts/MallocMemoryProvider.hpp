#ifndef MALLOC_MEMORY_PROVIDER_HPP
#define MALLOC_MEMORY_PROVIDER_HPP

#include "vendor/contracts/IMemoryProvider.hpp"

/**
 * @brief 标准堆内存提供者（posix_memalign / free）
 *
 * IMemoryProvider 的默认实现，平台无关。
 */
class MallocMemoryProvider : public IMemoryProvider {
public:
    explicit MallocMemoryProvider(size_t default_alignment = 64);

    MemoryBlock allocate(size_t size, size_t alignment = 0) override;
    void deallocate(MemoryBlock& block) override;

    const char* kind() const noexcept override { return "malloc"; }

    MemoryProviderCapabilities getCapabilities() const override;

    std::unique_ptr<IMemoryProvider> clone() const override;

private:
    size_t default_alignment_;
};

#endif // MALLOC_MEMORY_PROVIDER_HPP
