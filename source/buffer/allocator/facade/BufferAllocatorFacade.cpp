#include "buffer/allocator/facade/BufferAllocatorFacade.hpp"
#include "buffer/allocator/factory/BufferAllocatorFactory.hpp"
#include <stdio.h>

// ============================================================================
// 构造/析构
// ============================================================================

BufferAllocatorFacade::BufferAllocatorFacade(
    BufferAllocatorFactory::AllocatorType type
) : type_(type) {
    // 🎯 使用 Factory 创建底层 Allocator（配置细节由Factory内部决定）
    allocator_base_uptr_ = BufferAllocatorFactory::create(type);
    if (!allocator_base_uptr_) {
        printf("❌ ERROR: Failed to create Allocator (type=%s)\n", 
               BufferAllocatorFactory::typeToString(type));
    } else {
        printf("✅ BufferAllocatorFacade: Created %s\n", 
               BufferAllocatorFactory::typeToString(type));
    }
}

BufferAllocatorFacade::~BufferAllocatorFacade() {
    // allocator_base_uptr_ 通过 unique_ptr 自动释放
}

// ============================================================================
// 统一接口实现（转发到底层 Allocator）
// ============================================================================

std::unique_ptr<BufferPool> BufferAllocatorFacade::allocatePoolWithBuffers(
    int count,
    size_t size,
    const std::string& name,
    const std::string& category
) {
    if (!allocator_base_uptr_) {
        printf("❌ ERROR: Allocator not initialized\n");
        return nullptr;
    }
    
    return allocator_base_uptr_->allocatePoolWithBuffers(count, size, name, category);
}

Buffer* BufferAllocatorFacade::injectBufferToPool(
    size_t size,
    BufferPool* pool,
    QueueType queue
) {
    if (!allocator_base_uptr_) {
        printf("❌ ERROR: Allocator not initialized\n");
        return nullptr;
    }
    
    return allocator_base_uptr_->injectBufferToPool(size, pool, queue);
}

Buffer* BufferAllocatorFacade::injectExternalBufferToPool(
    void* virt_addr,
    uint64_t phys_addr,
    size_t size,
    BufferPool* pool,
    QueueType queue
) {
    if (!allocator_base_uptr_) {
        printf("❌ ERROR: Allocator not initialized\n");
        return nullptr;
    }
    
    return allocator_base_uptr_->injectExternalBufferToPool(virt_addr, phys_addr, size, pool, queue);
}

bool BufferAllocatorFacade::removeBufferFromPool(Buffer* buffer, BufferPool* pool) {
    if (!allocator_base_uptr_) {
        printf("❌ ERROR: Allocator not initialized\n");
        return false;
    }
    
    return allocator_base_uptr_->removeBufferFromPool(buffer, pool);
}

bool BufferAllocatorFacade::destroyPool(BufferPool* pool) {
    if (!allocator_base_uptr_) {
        printf("❌ ERROR: Allocator not initialized\n");
        return false;
    }
    
    return allocator_base_uptr_->destroyPool(pool);
}

// ==================== 已删除 getManagedBufferPool() ====================
// 
// 设计变更：
// - Allocator 不再持有 BufferPool
// - allocatePoolWithBuffers() 返回 unique_ptr，所有权转移给调用者
// - 不再需要 getManagedBufferPool() 方法

