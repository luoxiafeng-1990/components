#pragma once

#include "bufferpool/pool/base/IBufferPoolBuilder.hpp"
#include "vendor/contracts/IMemoryProvider.hpp"
#include <memory>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

/**
 * @brief ContinuousPhysicalPoolBuilder - 物理连续内存 BufferPool 构建器
 *
 * 通过构造函数注入 IMemoryProvider（依赖注入），支持：
 * - MallocMemoryProvider: 标准堆内存（posix_memalign）
 * - TacoMemoryProvider:   TACO 物理连续内存
 * - 未来其他 DMA/CMA provider
 *
 * createBuffer / deallocateBuffer 委托给 IMemoryProvider。
 */
class ContinuousPhysicalPoolBuilder : public IBufferPoolBuilder {
public:
    explicit ContinuousPhysicalPoolBuilder(std::unique_ptr<IMemoryProvider> provider);
    ~ContinuousPhysicalPoolBuilder() override;

    uint64_t allocatePoolWithBuffers(
        int count, size_t size,
        const std::string& name, const std::string& category = "") override;

    Buffer* injectBufferToPool(
        uint64_t pool_id, size_t size,
        QueueType queue = QueueType::FREE) override;

    Buffer* injectExternalBufferToPool(
        uint64_t pool_id, void* virt_addr, uint64_t phys_addr,
        size_t size, QueueType queue = QueueType::FREE,
        uint32_t custom_id = 0) override;

    bool removeBufferFromPool(uint64_t pool_id, Buffer* buffer) override;
    bool destroyPool() override;

protected:
    Buffer* createBuffer(uint32_t id, size_t size) override;
    void deallocateBuffer(Buffer* buffer) override;

private:
    void cleanupPoolTemp(BufferPool* pool);

    std::unique_ptr<IMemoryProvider> memory_provider_;
    size_t alignment_;
    log4cplus::Logger logger_;
};
