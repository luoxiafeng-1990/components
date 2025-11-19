#include "buffer/allocator/BufferAllocatorBase.hpp"
#include <stdio.h>

// 静态成员定义
std::unordered_map<Buffer*, BufferAllocatorBase*> BufferAllocatorBase::buffer_ownership_;
std::mutex BufferAllocatorBase::ownership_mutex_;

// ============================================================
// 批量分配实现
// ============================================================

std::unique_ptr<BufferPool> BufferAllocatorBase::allocatePoolWithBuffers(
    int count,
    size_t size,
    const std::string& name,
    const std::string& category)
{
    printf("\n🏭 BufferAllocator: Creating pool '%s' with %d buffers...\n",
           name.c_str(), count);
    
    // 1. 创建空池
    auto pool = BufferPool::CreateEmpty(name, category);
    if (!pool) {
        printf("❌ Failed to create empty pool\n");
        return nullptr;
    }
    
    // 2. 批量创建 Buffer 并注入
    for (int i = 0; i < count; i++) {
        Buffer* buffer = createBuffer(i, size);
        if (!buffer) {
            printf("❌ Failed to create buffer #%d\n", i);
            // 失败：清理已分配的 buffer
            cleanupPool(pool.get());
            return nullptr;
        }
        
        // 3. 通过辅助方法添加到 pool 的 free 队列
        if (!addBufferToPoolQueue(pool.get(), buffer, QueueType::FREE)) {
            printf("❌ Failed to add buffer #%d to pool\n", i);
            deallocateBuffer(buffer);
            cleanupPool(pool.get());
            return nullptr;
        }
        
        // 4. 记录所有权
        registerBufferOwnership(buffer, this);
        
        printf("   ✅ Buffer #%d created: virt=%p, phys=0x%lx, size=%zu\n",
               i, buffer->getVirtualAddress(), buffer->getPhysicalAddress(), size);
    }
    
    printf("✅ BufferPool '%s' created with %d buffers by allocator\n", 
           name.c_str(), count);
    
    return pool;
}

// ============================================================
// 单个注入实现
// ============================================================

Buffer* BufferAllocatorBase::injectBufferToPool(
    size_t size,
    BufferPool* pool,
    QueueType queue)
{
    if (!pool) {
        printf("❌ BufferAllocatorBase::injectBufferToPool: pool is nullptr\n");
        return nullptr;
    }
    
    // 1. 生成 Buffer ID（从 pool 的当前 buffer 数量）
    uint32_t id = pool->getTotalCount();
    
    // 2. 创建 Buffer
    Buffer* buffer = createBuffer(id, size);
    if (!buffer) {
        printf("❌ Failed to create buffer #%u\n", id);
        return nullptr;
    }
    
    // 3. 通过辅助方法添加到 pool 的指定队列
    if (!addBufferToPoolQueue(pool, buffer, queue)) {
        printf("❌ Failed to add buffer #%u to pool '%s'\n", 
               id, pool->getName().c_str());
        deallocateBuffer(buffer);
        return nullptr;
    }
    
    // 4. 记录所有权
    registerBufferOwnership(buffer, this);
    
    printf("✅ Buffer #%u injected to pool '%s' (queue: %s)\n",
           id, pool->getName().c_str(), 
           queue == QueueType::FREE ? "FREE" : "FILLED");
    
    return buffer;
}

// ============================================================
// Buffer 移除实现
// ============================================================

bool BufferAllocatorBase::removeBufferFromPool(Buffer* buffer, BufferPool* pool) {
    if (!buffer || !pool) {
        printf("❌ BufferAllocatorBase::removeBufferFromPool: invalid parameters\n");
        return false;
    }
    
    // 1. 通过辅助方法从 pool 移除（只能移除 free_queue 中的）
    if (!removeBufferFromPoolInternal(pool, buffer)) {
        printf("⚠️  Failed to remove buffer #%u from pool '%s' (in use or not in pool)\n",
               buffer->id(), pool->getName().c_str());
        return false;
    }
    
    // 2. 销毁 Buffer
    deallocateBuffer(buffer);
    
    // 3. 清除所有权记录
    unregisterBufferOwnership(buffer);
    
    printf("✅ Buffer #%u removed from pool '%s'\n",
           buffer->id(), pool->getName().c_str());
    
    return true;
}

// ============================================================
// 辅助方法实现
// ============================================================

void BufferAllocatorBase::registerBufferOwnership(Buffer* buffer, BufferAllocatorBase* allocator) {
    std::lock_guard<std::mutex> lock(ownership_mutex_);
    buffer_ownership_[buffer] = allocator;
}

void BufferAllocatorBase::unregisterBufferOwnership(Buffer* buffer) {
    std::lock_guard<std::mutex> lock(ownership_mutex_);
    buffer_ownership_.erase(buffer);
}

void BufferAllocatorBase::cleanupPool(BufferPool* pool) {
    if (!pool) {
        return;
    }
    
    printf("🧹 Cleaning up pool '%s'...\n", pool->getName().c_str());
    
    std::lock_guard<std::mutex> lock(ownership_mutex_);
    
    // 找到所有属于此 allocator 的 buffer
    std::vector<Buffer*> to_remove;
    for (auto& [buf, alloc] : buffer_ownership_) {
        if (alloc == this) {
            to_remove.push_back(buf);
        }
    }
    
    // 移除并销毁
    for (Buffer* buf : to_remove) {
        removeBufferFromPoolInternal(pool, buf);  // 通过辅助方法访问
        deallocateBuffer(buf);
        buffer_ownership_.erase(buf);
    }
    
    printf("✅ Cleanup complete: removed %zu buffers\n", to_remove.size());
}

// ============================================================
// 友元访问辅助方法实现
// ============================================================

bool BufferAllocatorBase::addBufferToPoolQueue(BufferPool* pool, Buffer* buffer, QueueType queue) {
    if (!pool || !buffer) {
        return false;
    }
    // 通过友元关系访问 BufferPool 的私有方法
    return pool->addBufferToQueue(buffer, queue);
}

bool BufferAllocatorBase::removeBufferFromPoolInternal(BufferPool* pool, Buffer* buffer) {
    if (!pool || !buffer) {
        return false;
    }
    // 通过友元关系访问 BufferPool 的私有方法
    return pool->removeBufferFromPool(buffer);
}
