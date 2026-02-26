#include "buffer/FramebufferAllocator.hpp"
#include "common/Logger.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include <stdio.h>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>

// ============================================================
// 静态变量声明（必须在所有方法之前）
// ============================================================

// 所有权跟踪（静态成员，所有Allocator共享）
static std::unordered_map<Buffer*, BufferAllocatorBase*> framebuffer_buffer_ownership_;
static std::mutex framebuffer_ownership_mutex_;

// ============================================================
// 构造/析构函数
// ============================================================

FramebufferAllocator::FramebufferAllocator()
    : external_buffers_()
    , next_buffer_index_(0)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Framebuffer")))
{
   LOG4CPLUS_DEBUG(logger_, "创建完成");
}

FramebufferAllocator::FramebufferAllocator(const std::vector<BufferInfo>& external_buffers)
    : external_buffers_(external_buffers)
    , next_buffer_index_(0)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Framebuffer")))
{
    LOG4CPLUS_DEBUG_FMT(logger_, "创建: 包装%zu个external buffers", 
           external_buffers_.size());
}


FramebufferAllocator::~FramebufferAllocator() {
    // v2.0: 子类析构函数中显式清理所有 Pool
    // 只有 FramebufferAllocator 自己知道如何管理外部内存
    // destroyPool() 会自动查询 Registry 获取所有 Pool 并清理
    destroyPool();
    
    LOG4CPLUS_DEBUG(logger_, "FramebufferAllocator destroyed (external memory not freed)");
}

// ============================================================
// 重写批量分配（批量包装）
// ============================================================

uint64_t FramebufferAllocator::allocatePoolWithBuffers(
    int count,
    size_t size,
    const std::string& name,
    const std::string& category)
{
    LOG4CPLUS_INFO_FMT(logger_, "🏭 [FramebufferAllocator] Creating BufferPool with %d buffers...", count);
    // v2.0 步骤 1: 使用 Passkey Token 创建 BufferPool（shared_ptr）
    auto pool = std::make_shared<BufferPool>(
        token(),    // 从基类获取通行证
        name,
        category
    );
    
    // v2.0 步骤 2: 批量包装外部 Buffer 并添加到 pool
    int actual_count = (count > 0) ? count : static_cast<int>(external_buffers_.size());
    
    for (int i = 0; i < actual_count; i++) {
        Buffer* buffer = createBuffer(i, 0);  // size 参数被忽略
        if (!buffer) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to wrap external buffer #%d", i);
            // 清理已创建的 buffers（pool还未注册，需要手动清理）
            // 遍历pool的managed_buffers_清理已添加的buffer
            {
                std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
                for (Buffer* buf : pool->getAllManagedBuffers()) {
                    deallocateBuffer(buf);
                    framebuffer_buffer_ownership_.erase(buf);
                }
            }
            // 清空 managed_buffers_ 集合，避免悬空指针
            pool->clearAllManagedBuffers();
            return 0;
        }
        
        if (!BufferAllocatorBase::addBufferToPoolQueue(pool.get(), buffer, QueueType::FREE)) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to add buffer #%d to pool", i);
            deallocateBuffer(buffer);
            // 清理已创建的 buffers（pool还未注册，需要手动清理）
            {
                std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
                for (Buffer* buf : pool->getAllManagedBuffers()) {
                    deallocateBuffer(buf);
                    framebuffer_buffer_ownership_.erase(buf);
                }
            }
            // 清空 managed_buffers_ 集合，避免悬空指针
            pool->clearAllManagedBuffers();
            return 0;
        }
        
        {
            std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
            framebuffer_buffer_ownership_[buffer] = this;
        }
        
        LOG4CPLUS_DEBUG_FMT(logger_, "  Buffer #%d wrapped: virt=%p, phys=0x%lx, size=%zu (EXTERNAL)",
               i, buffer->getVirtualAddress(), buffer->getPhysicalAddress(), buffer->size());
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "BufferPool '%s' created with %d buffers", 
           pool->getName().c_str(), actual_count);
    
    // v2.0 步骤 3: 注册到 Registry（转移所有权，传入 Allocator ID）
    uint64_t pool_id = BufferPoolRegistry::getInstance().registerPool(pool, getAllocatorId());
    pool->setRegistryId(pool_id);
    
    // v2.0 步骤 4: 返回 pool_id
    return pool_id;
}

// ============================================================
// 核心实现
// ============================================================

Buffer* FramebufferAllocator::createBuffer(uint32_t id, size_t size) {
    // 检查 id 是否越界
    if (id >= external_buffers_.size()) {
        LOG4CPLUS_ERROR_FMT(logger_, "Buffer ID %u out of range (max: %zu)", 
               id, external_buffers_.size());
        return nullptr;
    }
    
    // 获取外部 buffer 信息
    const BufferInfo& info = external_buffers_[id];
    
    // 创建 Buffer 对象（Ownership::EXTERNAL）
    Buffer* buffer = new Buffer(
        id,
        info.virt_addr,
        info.phys_addr,
        info.size,
        Buffer::Ownership::EXTERNAL
    );
    
    if (!buffer) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to create Buffer object #%u", id);
        return nullptr;
    }
    
    return buffer;
}

void FramebufferAllocator::deallocateBuffer(Buffer* buffer) {
    if (!buffer) {
        return;
    }
    
    // 1. 不释放内存（外部管理）
    LOG4CPLUS_DEBUG_FMT(logger_, "Deleting Buffer #%u (external memory retained)", buffer->id());
    
    // 2. 仅删除 Buffer 对象
    delete buffer;
}

// ============================================================
// 实现基类纯虚函数
// ============================================================

Buffer* FramebufferAllocator::injectBufferToPool(
    uint64_t pool_id,
    size_t size,
    QueueType queue
) {
    LOG4CPLUS_WARN(logger_, " [FramebufferAllocator] injectBufferToPool: This method is not supported");
    LOG4CPLUS_WARN(logger_, " FramebufferAllocator only supports wrapping pre-allocated external memory");
    LOG4CPLUS_WARN(logger_, " Use allocatePoolWithBuffers() or injectExternalBufferToPool() instead");
    return nullptr;
}

Buffer* FramebufferAllocator::injectExternalBufferToPool(
    uint64_t pool_id,
    void* virt_addr,
    uint64_t phys_addr,
    size_t size,
    QueueType queue
) {
    if (!virt_addr || size == 0) {
        LOG4CPLUS_ERROR(logger_, "injectExternalBufferToPool: invalid parameters");
        return nullptr;
    }
    
    // v2.0: 从 Registry 获取 Pool（返回 weak_ptr）
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        LOG4CPLUS_ERROR_FMT(logger_, "pool_id %lu not found or already destroyed", pool_id);
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
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to create Buffer object #%u for external memory", id);
        return nullptr;
    }
    
    // 3. 通过基类静态方法添加到 pool 的指定队列（会自动添加到 managed_buffers_）
    if (!BufferAllocatorBase::addBufferToPoolQueue(pool.get(), buffer, queue)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to add external buffer #%u to pool '%s'", 
               id, pool->getName().c_str());
        delete buffer;  // 只删除 Buffer 对象，不释放外部内存
        return nullptr;
    }
    
    // 4. 记录所有权（外部内存由外部管理，但 Buffer 对象由 Allocator 管理）
    {
        std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
        framebuffer_buffer_ownership_[buffer] = this;
    }
    
    // 仅在TRACE级别输出详细信息
    LOG_TRACE_FMT("External buffer #%u injected (virt=%p, phys=0x%lx, size=%zu)",
           id, virt_addr, phys_addr, size);
    
    return buffer;
}

bool FramebufferAllocator::removeBufferFromPool(uint64_t pool_id, Buffer* buffer) {
    if (!buffer) {
        LOG4CPLUS_ERROR(logger_, "removeBufferFromPool: buffer is nullptr");
        return false;
    }
    
    // v2.0: 从 Registry 获取 Pool（返回 weak_ptr）
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        LOG4CPLUS_ERROR_FMT(logger_, "pool_id %lu not found or already destroyed", pool_id);
        return false;
    }
    
    // 1. 通过基类静态方法从 pool 移除
    if (!BufferAllocatorBase::removeBufferFromPoolInternal(pool.get(), buffer)) {
        LOG4CPLUS_WARN_FMT(logger_, " Failed to remove buffer #%u from pool '%s' (in use or not in pool)",
               buffer->id(), pool->getName().c_str());
        return false;
    }
    
    // 2. 销毁 Buffer（仅删除对象，不释放外部内存）
    deallocateBuffer(buffer);
    
    // 3. 清除所有权记录
    {
        std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
        framebuffer_buffer_ownership_.erase(buffer);
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Buffer #%u removed from pool '%s'",
           buffer->id(), pool->getName().c_str());
    
    return true;
}

bool FramebufferAllocator::destroyPool() {
    // 1. 获取所有属于此 allocator 的 pool
    auto pool_ids = getPoolsByAllocator();
    
    if (pool_ids.empty()) {
        LOG4CPLUS_DEBUG(logger_, "No pools to destroy");
        return true;
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Destroying %zu pool(s)...", pool_ids.size());
    
    std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
    
    // 2. 遍历每个 pool
    for (uint64_t pool_id : pool_ids) {
        // 2.1 获取 pool
        auto pool = getPoolSpecialForAllocator(pool_id);
        if (!pool) {
            LOG4CPLUS_WARN_FMT(logger_, " [FramebufferAllocator] pool_id %lu not found (already destroyed?)", pool_id);
            continue;
        }
        
        LOG4CPLUS_DEBUG_FMT(logger_, "Destroying pool '%s' (ID: %lu)...", pool->getName().c_str(), pool_id);
        
        // 2.2 通过 BufferPool 的公共方法获取所有属于此 pool 的 buffer
        std::vector<Buffer*> to_remove;
        for (Buffer* buf : pool->getAllManagedBuffers()) {
            // 检查 buffer 是否属于此 allocator
            auto it = framebuffer_buffer_ownership_.find(buf);
            if (it != framebuffer_buffer_ownership_.end() && it->second == this) {
                to_remove.push_back(buf);
            }
        }
        
        // 2.3 移除并销毁所有 Buffer（仅删除对象，不释放外部内存）
        for (Buffer* buf : to_remove) {
            BufferAllocatorBase::removeBufferFromPoolInternal(pool.get(), buf);
            deallocateBuffer(buf);
            framebuffer_buffer_ownership_.erase(buf);
        }
        
        LOG4CPLUS_DEBUG_FMT(logger_, "Pool '%s' destroyed: removed %zu buffers (external memory retained)", 
               pool->getName().c_str(), to_remove.size());
        
        // 2.4 从 Registry 注销（触发 Pool 析构）
        unregisterPool(pool_id);
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "All %zu pool(s) destroyed", pool_ids.size());
    return true;
}

