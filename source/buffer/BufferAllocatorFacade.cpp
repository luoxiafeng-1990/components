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
    // 使用 Factory 创建底层 Allocator
    allocator_base_uptr_ = BufferAllocatorFactory::create(type);
    if (!allocator_base_uptr_) {
        LOG_ERROR_FMT("[BufferAllocatorFacade] Failed to create Allocator (type=%s)", 
                      BufferAllocatorFactory::typeToString(type));
    } else {
        LOG_DEBUG_FMT("[BufferAllocatorFacade] Created %s", 
                      BufferAllocatorFactory::typeToString(type));
    }
}

BufferAllocatorFacade::~BufferAllocatorFacade() {
    // allocator_base_uptr_ 通过 unique_ptr 自动释放
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
