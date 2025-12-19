#include "buffer/NormalAllocator.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "common/Logger.hpp"
#include <cstdlib>
#include <cstring>
#include <stdio.h>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>

// ============================================================
// 构造/析构函数
// ============================================================

NormalAllocator::NormalAllocator(BufferMemoryAllocatorType type, size_t alignment)
    : type_(type)
    , alignment_(alignment)
{
    LOG_DEBUG_FMT("[NormalAllocator] 创建: alignment=%zu", alignment_);
}

NormalAllocator::~NormalAllocator() {
    // v2.0: 子类析构函数中显式清理所有 Pool
    // 只有 NormalAllocator 自己知道如何释放 Buffer 内存
    // destroyPool() 会自动查询 Registry 获取所有 Pool 并清理
    destroyPool();
    
    LOG_DEBUG("[NormalAllocator] 析构");
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
            LOG_ERROR_FMT("[NormalAllocator] posix_memalign failed for buffer #%u (size=%zu)", id, size);
            return nullptr;
        }
    } else {
        // 普通分配
        virt_addr = malloc(size);
        if (!virt_addr) {
            LOG_ERROR_FMT("[NormalAllocator] malloc failed for buffer #%u (size=%zu)", id, size);
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
        LOG_ERROR_FMT("[NormalAllocator] Failed to create Buffer object #%u", id);
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

uint64_t NormalAllocator::allocatePoolWithBuffers(
    int count,
    size_t size,
    const std::string& name,
    const std::string& category
) {
    LOG_DEBUG_FMT("[NormalAllocator] 创建BufferPool: %d buffers", count);
    
    // v2.0 步骤 1: 使用 Passkey Token 创建 BufferPool（shared_ptr）
    auto pool = std::make_shared<BufferPool>(
        token(),    // 从基类获取通行证
        name,
        category
    );
    
    // v2.0 步骤 2: 批量创建 Buffer 并注入到 pool
    for (int i = 0; i < count; i++) {
        Buffer* buffer = createBuffer(i, size);
        if (!buffer) {
            LOG_ERROR_FMT("[NormalAllocator] Failed to create buffer #%d", i);
            cleanupPoolTemp(pool.get());
            return 0;
        }
        
        if (!BufferAllocatorBase::addBufferToPoolQueue(pool.get(), buffer, QueueType::FREE)) {
            LOG_ERROR_FMT("[NormalAllocator] Failed to add buffer #%d to pool", i);
            deallocateBuffer(buffer);
            cleanupPoolTemp(pool.get());
            return 0;
        }
        
        {
            std::lock_guard<std::mutex> lock(ownership_mutex_);
            buffer_ownership_[buffer] = this;
        }
        
        LOG_DEBUG_FMT("[NormalAllocator]   Buffer #%d created: virt=%p, phys=0x%lx, size=%zu",
               i, buffer->getVirtualAddress(), buffer->getPhysicalAddress(), size);
    }
    
    // v2.0 步骤 3: 注册到 Registry（转移所有权，传入 Allocator ID）
    uint64_t pool_id = BufferPoolRegistry::getInstance().registerPool(pool, getAllocatorId());
    pool->setRegistryId(pool_id);
    
    LOG_INFO_FMT("[NormalAllocator] BufferPool '%s' created with %d buffers (ID: %lu)", 
           name.c_str(), count, pool_id);
    
    // v2.0 步骤 4: 返回 pool_id（Registry 独占持有 Pool）
    return pool_id;
}

Buffer* NormalAllocator::injectBufferToPool(
    uint64_t pool_id,
    size_t size,
    QueueType queue
) {
    // v2.0: 从 Registry 获取 Pool（返回 weak_ptr）
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        LOG_ERROR_FMT("[NormalAllocator] injectBufferToPool: pool_id %lu not found or already destroyed", pool_id);
        return nullptr;
    }
    
    // 1. 生成 Buffer ID（从 pool 的当前 buffer 数量）
    uint32_t id = pool->getTotalCount();
    
    // 2. 创建 Buffer（内部分配内存）
    Buffer* buffer = createBuffer(id, size);
    if (!buffer) {
        LOG_ERROR_FMT("[NormalAllocator] Failed to create buffer #%u", id);
        return nullptr;
    }
    
    // 3. 通过基类静态方法添加到 pool 的指定队列（会自动添加到 managed_buffers_）
    if (!BufferAllocatorBase::addBufferToPoolQueue(pool.get(), buffer, queue)) {
        LOG_ERROR_FMT("[NormalAllocator] Failed to add buffer #%u to pool '%s'", 
               id, pool->getName().c_str());
        deallocateBuffer(buffer);
        return nullptr;
    }
    
    // 4. 记录所有权
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_[buffer] = this;
    }
    
    LOG_DEBUG_FMT("[NormalAllocator] Buffer #%u injected to pool '%s' (queue: %s)",
           id, pool->getName().c_str(), 
           queue == QueueType::FREE ? "FREE" : "FILLED");
    
    return buffer;
}

Buffer* NormalAllocator::injectExternalBufferToPool(
    uint64_t pool_id,
    void* virt_addr,
    uint64_t phys_addr,
    size_t size,
    QueueType queue
) {
    if (!virt_addr || size == 0) {
        LOG_ERROR("[NormalAllocator] injectExternalBufferToPool: invalid parameters");
        return nullptr;
    }
    
    // v2.0: 从 Registry 获取 Pool（返回 weak_ptr）
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        LOG_ERROR_FMT("[NormalAllocator] injectExternalBufferToPool: pool_id %lu not found or already destroyed", pool_id);
        return nullptr;
    }
    
    // 1. 生成 Buffer ID（从 pool 的当前 buffer 数量）
    uint32_t id = pool->getTotalCount();
    
    // 2. 创建 Buffer 对象（包装外部内存，Ownership::EXTERNAL）
    Buffer* buffer = new Buffer(
        id,
        virt_addr,
        phys_addr,
        size,
        Buffer::Ownership::EXTERNAL
    );
    
    if (!buffer) {
        LOG_ERROR_FMT("[NormalAllocator] Failed to create Buffer object #%u for external memory", id);
        return nullptr;
    }
    
    // 3. 通过基类静态方法添加到 pool 的指定队列（会自动添加到 managed_buffers_）
    if (!BufferAllocatorBase::addBufferToPoolQueue(pool.get(), buffer, queue)) {
        LOG_ERROR_FMT("[NormalAllocator] Failed to add external buffer #%u to pool '%s'", 
               id, pool->getName().c_str());
        delete buffer;  // 只删除 Buffer 对象，不释放外部内存
        return nullptr;
    }
    
    // 4. 记录所有权
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_[buffer] = this;
    }
    
    // 仅在TRACE级别输出详细信息
    LOG_TRACE_FMT("[NormalAllocator] External buffer #%u injected (virt=%p, phys=0x%lx, size=%zu)",
           id, virt_addr, phys_addr, size);
    
    return buffer;
}

bool NormalAllocator::removeBufferFromPool(uint64_t pool_id, Buffer* buffer) {
    if (!buffer) {
        LOG_ERROR("[NormalAllocator] removeBufferFromPool: buffer is nullptr");
        return false;
    }
    
    // v2.0: 从 Registry 获取 Pool（返回 weak_ptr）
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        LOG_ERROR_FMT("[NormalAllocator] removeBufferFromPool: pool_id %lu not found or already destroyed", pool_id);
        return false;
    }
    
    // 1. 通过基类静态方法从 pool 移除（只能移除 free_queue 中的）
    if (!BufferAllocatorBase::removeBufferFromPoolInternal(pool.get(), buffer)) {
        LOG_WARN_FMT("[NormalAllocator] Failed to remove buffer #%u from pool '%s'",
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
    
    LOG_DEBUG_FMT("[NormalAllocator] Buffer #%u removed from pool '%s'",
           buffer->id(), pool->getName().c_str());
    
    return true;
}

bool NormalAllocator::destroyPool() {
    // 1. 获取所有属于此 allocator 的 pool
    auto pool_ids = getPoolsByAllocator();
    
    if (pool_ids.empty()) {
        LOG_DEBUG("[NormalAllocator] No pools to destroy");
        return true;
    }
    
    LOG_INFO_FMT("🧹 [NormalAllocator] Destroying %zu pool(s)...", pool_ids.size());
    
    std::lock_guard<std::mutex> lock(ownership_mutex_);
    
    // 2. 遍历每个 pool
    for (uint64_t pool_id : pool_ids) {
        // 2.1 获取 pool
        auto pool = getPoolSpecialForAllocator(pool_id);
        if (!pool) {
            LOG_WARN_FMT("[NormalAllocator] pool_id %lu not found", pool_id);
            continue;
        }
        
        LOG_DEBUG_FMT("[NormalAllocator] 销毁pool '%s' (ID: %lu)", pool->getName().c_str(), pool_id);
        
        // 2.2 通过 BufferPool 的公共方法获取所有属于此 pool 的 buffer
        std::vector<Buffer*> to_remove;
        for (Buffer* buf : pool->getAllManagedBuffers()) {
            // 检查 buffer 是否属于此 allocator
            auto it = buffer_ownership_.find(buf);
            if (it != buffer_ownership_.end() && it->second == this) {
                to_remove.push_back(buf);
            }
        }
        
        // 2.3 移除并销毁所有 Buffer
        for (Buffer* buf : to_remove) {
            BufferAllocatorBase::removeBufferFromPoolInternal(pool.get(), buf);
            deallocateBuffer(buf);
            buffer_ownership_.erase(buf);
        }
        
        LOG_DEBUG_FMT("[NormalAllocator] Pool '%s' destroyed: removed %zu buffers", 
               pool->getName().c_str(), to_remove.size());
        
        // 2.4 从 Registry 注销（触发 Pool 析构）
        unregisterPool(pool_id);
    }
    
    LOG_DEBUG_FMT("[NormalAllocator] All %zu pool(s) destroyed", pool_ids.size());
    return true;
}

// v2.0 辅助方法：清理临时 Pool（创建失败时使用）
void NormalAllocator::cleanupPoolTemp(BufferPool* pool) {
    if (!pool) {
        return;
    }
    
    LOG_DEBUG_FMT("[NormalAllocator] 清理临时pool '%s'", pool->getName().c_str());
    
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
    
    LOG_DEBUG_FMT("[NormalAllocator] Cleanup complete: removed %zu buffers", to_remove.size());
}

