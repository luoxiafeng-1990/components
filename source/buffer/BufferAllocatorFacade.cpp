#include "buffer/BufferAllocatorFacade.hpp"
#include "buffer/BufferAllocatorFactory.hpp"
#include "common/Logger.hpp"
#include <stdio.h>

// ============================================================================
// 构造/析构
// ============================================================================

BufferAllocatorFacade::BufferAllocatorFacade(
    BufferAllocatorFactory::AllocatorType type
) : type_(type) {
    LOG_DEBUG_FMT("[BufferAllocatorFacade] 创建: 类型=%s", 
                  BufferAllocatorFactory::typeToString(type));
    // 使用 Factory 创建底层 Allocator
    allocator_base_uptr_ = BufferAllocatorFactory::create(type);
    if (!allocator_base_uptr_) {
        LOG_ERROR_FMT("[BufferAllocatorFacade] 创建底层Allocator失败");
    }
}

BufferAllocatorFacade::~BufferAllocatorFacade() {
    // ⭐ 关键修改：析构时自动清理所有 Pool（包括 AVFrame）
    //
    // 设计原则：
    // 1. Allocator 负责创建的资源，由 Allocator 负责清理（RAII 原则）
    // 2. Worker 析构时，allocator_facade_ 最先析构（成员变量声明顺序的逆序）
    // 3. allocator_facade_ 析构时调用 destroyPool() 清理所有 Pool
    // 4. 每个 Pool 中的 Buffer 和 AVFrame 被正确释放
    //
    // 清理流程：
    // ~Worker() → ~allocator_facade_() → destroyPool() → 遍历所有 Pool → 
    // 遍历 Pool 中所有 Buffer → deallocateBuffer() → av_frame_free()
    
    LOG_DEBUG("[BufferAllocatorFacade] 析构: 自动清理所有 Pool...");
    
    if (allocator_base_uptr_) {
        // 调用底层 Allocator 的 destroyPool()
        // 会自动查询 Registry 获取所有归属的 Pool 并清理
        // 对于 AVFrameAllocator，会释放所有 Buffer 中的 AVFrame
        allocator_base_uptr_->destroyPool();
    }
    
    // allocator_base_uptr_ 通过 unique_ptr 自动释放
    LOG_DEBUG("[BufferAllocatorFacade] 析构完成");
}

// ============================================================================
// v2.0 统一接口实现（转发到底层 Allocator）
// ============================================================================

uint64_t BufferAllocatorFacade::allocatePoolWithBuffers(
    int count,
    size_t size,
    const std::string& name,
    const std::string& category
) {
    if (!allocator_base_uptr_) {
        LOG_ERROR("[BufferAllocatorFacade] Allocator not initialized");
        return 0;
    }
    
    return allocator_base_uptr_->allocatePoolWithBuffers(count, size, name, category);
}

Buffer* BufferAllocatorFacade::injectBufferToPool(
    uint64_t pool_id,
    size_t size,
    QueueType queue
) {
    if (!allocator_base_uptr_) {
        LOG_ERROR("[BufferAllocatorFacade] Allocator not initialized");
        return nullptr;
    }
    
    return allocator_base_uptr_->injectBufferToPool(pool_id, size, queue);
}

Buffer* BufferAllocatorFacade::injectExternalBufferToPool(
    uint64_t pool_id,
    void* virt_addr,
    uint64_t phys_addr,
    size_t size,
    QueueType queue
) {
    if (!allocator_base_uptr_) {
        LOG_ERROR("[BufferAllocatorFacade] Allocator not initialized");
        return nullptr;
    }
    
    return allocator_base_uptr_->injectExternalBufferToPool(pool_id, virt_addr, phys_addr, size, queue);
}

bool BufferAllocatorFacade::removeBufferFromPool(uint64_t pool_id, Buffer* buffer) {
    if (!allocator_base_uptr_) {
        LOG_ERROR("[BufferAllocatorFacade] Allocator not initialized");
        return false;
    }
    
    return allocator_base_uptr_->removeBufferFromPool(pool_id, buffer);
}

bool BufferAllocatorFacade::destroyPool() {
    if (!allocator_base_uptr_) {
        LOG_ERROR("[BufferAllocatorFacade] Allocator not initialized");
        return false;
    }
    
    return allocator_base_uptr_->destroyPool();
}

// ==================== v2.0 已删除 getManagedBufferPool() ====================
// 
// 设计变更：
// - Allocator 不再持有 BufferPool
// - allocatePoolWithBuffers() 返回 pool_id，Registry 持有 Pool
// - 使用者从 Registry 获取临时访问（getPool(pool_id)）
// - 不再需要 getManagedBufferPool() 方法
