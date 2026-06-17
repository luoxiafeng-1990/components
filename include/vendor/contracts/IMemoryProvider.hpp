#ifndef IMEMORY_PROVIDER_HPP
#define IMEMORY_PROVIDER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>

/**
 * @brief 内存块描述
 *
 * 通用容器，既能表示普通堆内存，也能表示物理连续 DMA 内存。
 */
struct MemoryBlock {
    void*    virt_addr  = nullptr;
    uint64_t phys_addr  = 0;
    size_t   size       = 0;
    uint32_t handle     = 0;  ///< 厂商句柄（如 taco blk_id），0 表示无
};

/**
 * @brief 内存提供者能力描述
 */
struct MemoryProviderCapabilities {
    bool supports_physical_address = false;
    bool supports_dma              = false;
    bool is_cache_coherent         = true;
    size_t default_alignment       = 64;
};

/**
 * @brief 内存提供者抽象接口
 *
 * 将"如何分配/释放原始内存"与"如何构建 BufferPool"解耦。
 * 具体厂商（TACO、标准 malloc、CUDA 等）各自实现本接口，
 * 并通过 MemoryProviderRegistry 注册工厂。
 *
 * 放置位置：vendor/contracts/（与 IDecoderVendorExtension 同级）
 */
class IMemoryProvider {
public:
    virtual ~IMemoryProvider() = default;

    virtual MemoryBlock allocate(size_t size, size_t alignment = 0) = 0;

    virtual void deallocate(MemoryBlock& block) = 0;

    /// 稳定标识（如 "malloc"、"taco"、"dma_heap"），用于日志和注册表查找
    virtual const char* kind() const noexcept = 0;

    virtual MemoryProviderCapabilities getCapabilities() const = 0;

    /// 深拷贝
    virtual std::unique_ptr<IMemoryProvider> clone() const = 0;
};

#endif // IMEMORY_PROVIDER_HPP
