#pragma once

#include "bufferpool/buffer/Buffer.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"
#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>

enum class BufferMemoryAllocatorType {
    NORMAL_MALLOC = 0,
    CMA = 1,
    DMA_HEAP = 2,
    TACO_SYS = 3
};

/**
 * @brief IBufferPoolBuilder - BufferPool 构建器抽象基类
 *
 * 职责：
 * - 使用 IMemoryProvider 分配内存
 * - 将内存包装为 Buffer 对象
 * - 组装 Buffer 到 BufferPool
 * - 注册到 ComponentTopology（Registry）
 * - 析构时自动清理
 *
 * 子类：
 * - BufferPoolBuilder:  统一构建器，通过 Buffer::Type 分派（AVFRAME / MAT / RAW）
 */
class IBufferPoolBuilder {
public:
    IBufferPoolBuilder() : allocator_id_(next_allocator_id_++) {}
    virtual ~IBufferPoolBuilder();

    virtual uint64_t allocatePoolWithBuffers(
        int count, size_t size,
        const std::string& name, const std::string& category = "") = 0;

    virtual Buffer* injectBufferToPool(
        uint64_t pool_id, size_t size,
        QueueType queue = QueueType::FREE) = 0;

    virtual Buffer* injectExternalBufferToPool(
        uint64_t pool_id, void* virt_addr, uint64_t phys_addr,
        size_t size, QueueType queue = QueueType::FREE,
        uint32_t custom_id = 0) = 0;

    virtual bool removeBufferFromPool(uint64_t pool_id, Buffer* buffer) = 0;
    virtual bool destroyPool() = 0;

protected:
    uint64_t allocator_id_;
    static std::atomic<uint64_t> next_allocator_id_;

    uint64_t getAllocatorId() const { return allocator_id_; }
    std::vector<uint64_t> getPoolsByAllocator() const;
    std::shared_ptr<BufferPool> getPoolSpecialForAllocator(uint64_t pool_id);
    void unregisterPool(uint64_t pool_id);

    virtual Buffer* createBuffer(uint32_t id, size_t size) = 0;
    virtual void deallocateBuffer(Buffer* buffer) = 0;

    static bool addBufferToPoolQueue(BufferPool* pool, Buffer* buffer, QueueType queue) {
        if (!pool || !buffer) return false;
        return pool->addBufferToQueue(buffer, queue);
    }

    static bool removeBufferFromPoolInternal(BufferPool* pool, Buffer* buffer) {
        if (!pool || !buffer) return false;
        return pool->removeBufferFromPool(buffer);
    }

    static BufferPool::PrivateToken token() {
        return BufferPool::PrivateToken();
    }
};
