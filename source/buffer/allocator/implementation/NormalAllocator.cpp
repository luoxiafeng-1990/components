#include "buffer/allocator/implementation/NormalAllocator.hpp"
#include "buffer/BufferPool.hpp"
#include <cstdlib>
#include <cstring>
#include <stdio.h>
#include <vector>
#include <unordered_map>
#include <mutex>

// ============================================================
// 构造/析构函数
// ============================================================

NormalAllocator::NormalAllocator(BufferMemoryAllocatorType type, size_t alignment)
    : type_(type)
    , alignment_(alignment)
{
    printf("🔧 NormalAllocator created (alignment=%zu)\n", alignment_);
}

NormalAllocator::~NormalAllocator() {
    printf("🧹 NormalAllocator destroyed\n");
}

// ============================================================
// 核心实现
// ============================================================

Buffer* NormalAllocator::createBuffer(uint32_t id, size_t size) {
    // 1. 分配对齐内存
    void* virt_addr = nullptr;
    
    if (alignment_ > 0) {
        // 使用对齐分配
        if (posix_memalign(&virt_addr, alignment_, size) != 0) {
            printf("❌ posix_memalign failed for buffer #%u (size=%zu)\n", id, size);
            return nullptr;
        }
    } else {
        // 普通分配
        virt_addr = malloc(size);
        if (!virt_addr) {
            printf("❌ malloc failed for buffer #%u (size=%zu)\n", id, size);
            return nullptr;
        }
    }
    
    // 2. 清零内存（可选，用于调试）
    memset(virt_addr, 0, size);
    
    // 3. 创建 Buffer 对象
    // 普通内存没有物理地址，phys_addr = 0
    Buffer* buffer = new Buffer(
        id,
        virt_addr,
        0,  // phys_addr = 0（普通内存不提供物理地址）
        size,
        Buffer::Ownership::OWNED
    );
    
    if (!buffer) {
        printf("❌ Failed to create Buffer object #%u\n", id);
        free(virt_addr);
        return nullptr;
    }
    
    return buffer;
}

void NormalAllocator::deallocateBuffer(Buffer* buffer) {
    if (!buffer) {
        return;
    }
    
    // 1. 释放内存
    void* virt_addr = buffer->getVirtualAddress();
    if (virt_addr) {
        free(virt_addr);
    }
    
    // 2. 删除 Buffer 对象
    delete buffer;
}

// ============================================================
// 实现基类纯虚函数
// ============================================================

// 所有权跟踪（静态成员，所有Allocator共享）
static std::unordered_map<Buffer*, BufferAllocatorBase*> buffer_ownership_;
static std::mutex ownership_mutex_;

std::unique_ptr<BufferPool> NormalAllocator::allocatePoolWithBuffers(
    int count,
    size_t size,
    const std::string& name,
    const std::string& category
) {
    printf("\n🏭 NormalAllocator: Creating pool '%s' with %d buffers...\n",
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
        
        // 3. 通过基类静态方法添加到 pool 的 free 队列
        if (!BufferAllocatorBase::addBufferToPoolQueue(pool.get(), buffer, QueueType::FREE)) {
            printf("❌ Failed to add buffer #%d to pool\n", i);
            deallocateBuffer(buffer);
            cleanupPool(pool.get());
            return nullptr;
        }
        
        // 4. 记录所有权
        {
            std::lock_guard<std::mutex> lock(ownership_mutex_);
            buffer_ownership_[buffer] = this;
        }
        
        printf("   ✅ Buffer #%d created: virt=%p, phys=0x%lx, size=%zu\n",
               i, buffer->getVirtualAddress(), buffer->getPhysicalAddress(), size);
    }
    
    printf("✅ BufferPool '%s' created with %d buffers by NormalAllocator\n", 
           name.c_str(), count);
    
    return pool;
}

Buffer* NormalAllocator::injectBufferToPool(
    size_t size,
    BufferPool* pool,
    QueueType queue
) {
    if (!pool) {
        printf("❌ NormalAllocator::injectBufferToPool: pool is nullptr\n");
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
    
    // 3. 通过基类静态方法添加到 pool 的指定队列
    if (!BufferAllocatorBase::addBufferToPoolQueue(pool, buffer, queue)) {
        printf("❌ Failed to add buffer #%u to pool '%s'\n", 
               id, pool->getName().c_str());
        deallocateBuffer(buffer);
        return nullptr;
    }
    
    // 4. 记录所有权
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_[buffer] = this;
    }
    
    printf("✅ Buffer #%u injected to pool '%s' (queue: %s)\n",
           id, pool->getName().c_str(), 
           queue == QueueType::FREE ? "FREE" : "FILLED");
    
    return buffer;
}

bool NormalAllocator::removeBufferFromPool(Buffer* buffer, BufferPool* pool) {
    if (!buffer || !pool) {
        printf("❌ NormalAllocator::removeBufferFromPool: invalid parameters\n");
        return false;
    }
    
    // 1. 通过基类静态方法从 pool 移除（只能移除 free_queue 中的）
    if (!BufferAllocatorBase::removeBufferFromPoolInternal(pool, buffer)) {
        printf("⚠️  Failed to remove buffer #%u from pool '%s' (in use or not in pool)\n",
               buffer->id(), pool->getName().c_str());
        return false;
    }
    
    // 2. 销毁 Buffer
    deallocateBuffer(buffer);
    
    // 3. 清除所有权记录
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_.erase(buffer);
    }
    
    printf("✅ Buffer #%u removed from pool '%s'\n",
           buffer->id(), pool->getName().c_str());
    
    return true;
}

bool NormalAllocator::destroyPool(BufferPool* pool) {
    if (!pool) {
        printf("❌ NormalAllocator::destroyPool: pool is nullptr\n");
        return false;
    }
    
    printf("🧹 NormalAllocator: Destroying pool '%s'...\n", pool->getName().c_str());
    
    std::lock_guard<std::mutex> lock(ownership_mutex_);
    
    // 找到所有属于此 allocator 的 buffer
    std::vector<Buffer*> to_remove;
    for (auto& [buf, alloc] : buffer_ownership_) {
        if (alloc == this) {
            // 检查 buffer 是否属于这个 pool
            // 这里简化处理，移除所有属于此 allocator 的 buffer
            to_remove.push_back(buf);
        }
    }
    
    // 移除并销毁
    for (Buffer* buf : to_remove) {
        BufferAllocatorBase::removeBufferFromPoolInternal(pool, buf);
        deallocateBuffer(buf);
        buffer_ownership_.erase(buf);
    }
    
    printf("✅ Pool '%s' destroyed: removed %zu buffers\n", pool->getName().c_str(), to_remove.size());
    
    return true;
}

// 辅助方法：清理 Pool
void NormalAllocator::cleanupPool(BufferPool* pool) {
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
        BufferAllocatorBase::removeBufferFromPoolInternal(pool, buf);
        deallocateBuffer(buf);
        buffer_ownership_.erase(buf);
    }
    
    printf("✅ Cleanup complete: removed %zu buffers\n", to_remove.size());
}

