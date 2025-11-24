#include "buffer/allocator/facade/BufferAllocatorFacade.hpp"
#include "buffer/allocator/factory/BufferAllocatorFactory.hpp"
#include <stdio.h>

// ============================================================================
// 构造/析构
// ============================================================================

BufferAllocatorFacade::BufferAllocatorFacade(
    BufferAllocatorFactory::AllocatorType type,
    BufferMemoryAllocatorType mem_type,
    size_t alignment
) : type_(type) {
    // 使用 Factory 创建底层 Allocator
    allocator_ = BufferAllocatorFactory::create(type, mem_type, alignment);
    if (!allocator_) {
        printf("❌ ERROR: Failed to create Allocator (type=%s)\n", 
               BufferAllocatorFactory::typeToString(type));
    } else {
        printf("✅ BufferAllocatorFacade: Created %s\n", 
               BufferAllocatorFactory::typeToString(type));
    }
}

BufferAllocatorFacade::~BufferAllocatorFacade() {
    // allocator_ 通过 unique_ptr 自动释放
}

// ============================================================================
// 统一接口实现（转发到底层 Allocator）
// ============================================================================

std::shared_ptr<BufferPool> BufferAllocatorFacade::allocatePoolWithBuffers(
    int count,
    size_t size,
    const std::string& name,
    const std::string& category
) {
    if (!allocator_) {
        printf("❌ ERROR: Allocator not initialized\n");
        return nullptr;
    }
    
    return allocator_->allocatePoolWithBuffers(count, size, name, category);
}

Buffer* BufferAllocatorFacade::injectBufferToPool(
    size_t size,
    BufferPool* pool,
    QueueType queue
) {
    if (!allocator_) {
        printf("❌ ERROR: Allocator not initialized\n");
        return nullptr;
    }
    
    return allocator_->injectBufferToPool(size, pool, queue);
}

Buffer* BufferAllocatorFacade::injectExternalBufferToPool(
    void* virt_addr,
    uint64_t phys_addr,
    size_t size,
    BufferPool* pool,
    QueueType queue
) {
    if (!allocator_) {
        printf("❌ ERROR: Allocator not initialized\n");
        return nullptr;
    }
    
    return allocator_->injectExternalBufferToPool(virt_addr, phys_addr, size, pool, queue);
}

bool BufferAllocatorFacade::removeBufferFromPool(Buffer* buffer, BufferPool* pool) {
    if (!allocator_) {
        printf("❌ ERROR: Allocator not initialized\n");
        return false;
    }
    
    return allocator_->removeBufferFromPool(buffer, pool);
}

bool BufferAllocatorFacade::destroyPool(BufferPool* pool) {
    if (!allocator_) {
        printf("❌ ERROR: Allocator not initialized\n");
        return false;
    }
    
    return allocator_->destroyPool(pool);
}

std::shared_ptr<BufferPool> BufferAllocatorFacade::getManagedBufferPool() const {
    if (!allocator_) {
        printf("❌ ERROR: Allocator not initialized\n");
        return nullptr;
    }
    
    return allocator_->getManagedBufferPool();
}

