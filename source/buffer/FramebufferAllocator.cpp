#include "buffer/FramebufferAllocator.hpp"
#include "common/Logger.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include <stdio.h>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <cstring>

extern "C" {
#include "taco_sys_api.h"
}

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
    destroyPool();
    LOG4CPLUS_DEBUG(logger_, "FramebufferAllocator destroyed");
}

// ============================================================
// 批量分配
// ============================================================

uint64_t FramebufferAllocator::allocatePoolWithBuffers(
    int count,
    size_t size,
    const std::string& name,
    const std::string& category)
{
    bool use_taco = (count > 0 && size > 0);

    auto pool = std::make_shared<BufferPool>(
        token(),
        name,
        category
    );

    // 用于 TACO 模式失败时的回滚清理
    auto cleanup_pool = [&]() {
        std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
        for (Buffer* buf : pool->getAllManagedBuffers()) {
            if (use_taco) {
                if (buf->getVirtualAddress()) {
                    taco_sys_munmap(buf->getVirtualAddress(),
                                   static_cast<uint32_t>(buf->size()));
                }
                if (buf->id() != 0) {
                    taco_sys_release_block(buf->id());
                }
            }
            deallocateBuffer(buf);
            framebuffer_buffer_ownership_.erase(buf);
        }
        pool->clearAllManagedBuffers();
    };

    if (use_taco) {
        // ===== TACO 分配模式 =====
        LOG4CPLUS_INFO_FMT(logger_,
            "Creating BufferPool '%s': %d buffers x %zu bytes (TACO alloc)",
            name.c_str(), count, size);

        for (int i = 0; i < count; i++) {
            uint32_t blk_id = taco_sys_get_block(
                TACO_INVALID_POOLID, size, name.c_str());
            if (blk_id == 0) {
                LOG4CPLUS_ERROR_FMT(logger_,
                    "taco_sys_get_block failed for buffer #%d (size=%zu)", i, size);
                cleanup_pool();
                return 0;
            }

            uint64_t phys_addr = taco_sys_handle2_phys_addr(blk_id);
            void* virt_addr = taco_sys_mmap_noncache(
                phys_addr, static_cast<uint32_t>(size));
            if (!virt_addr) {
                LOG4CPLUS_ERROR_FMT(logger_,
                    "taco_sys_mmap_noncache failed for buffer #%d", i);
                taco_sys_release_block(blk_id);
                cleanup_pool();
                return 0;
            }

            memset(virt_addr, 0, size);

            Buffer* buffer = new Buffer(
                blk_id, virt_addr, phys_addr, size,
                Buffer::Ownership::EXTERNAL);

            if (!BufferAllocatorBase::addBufferToPoolQueue(
                    pool.get(), buffer, QueueType::FREE)) {
                LOG4CPLUS_ERROR_FMT(logger_,
                    "Failed to add buffer #%d to pool", i);
                taco_sys_munmap(virt_addr, static_cast<uint32_t>(size));
                taco_sys_release_block(blk_id);
                delete buffer;
                cleanup_pool();
                return 0;
            }

            {
                std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
                framebuffer_buffer_ownership_[buffer] = this;
            }

            LOG4CPLUS_INFO_FMT(logger_,
                "  Buffer #%d: blk_id=%u, phys=0x%llx, virt=%p, size=%zu",
                i, blk_id, (unsigned long long)phys_addr, virt_addr, size);
        }

        taco_allocated_ = true;

    } else {
        // ===== 外部包装模式 =====
        int actual_count = static_cast<int>(external_buffers_.size());
        LOG4CPLUS_INFO_FMT(logger_,
            "Creating BufferPool '%s': wrapping %d external buffers",
            name.c_str(), actual_count);

        for (int i = 0; i < actual_count; i++) {
            Buffer* buffer = createBuffer(i, 0);
            if (!buffer) {
                LOG4CPLUS_ERROR_FMT(logger_,
                    "Failed to wrap external buffer #%d", i);
                cleanup_pool();
                return 0;
            }

            if (!BufferAllocatorBase::addBufferToPoolQueue(
                    pool.get(), buffer, QueueType::FREE)) {
                LOG4CPLUS_ERROR_FMT(logger_,
                    "Failed to add buffer #%d to pool", i);
                deallocateBuffer(buffer);
                cleanup_pool();
                return 0;
            }

            {
                std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
                framebuffer_buffer_ownership_[buffer] = this;
            }
        }
    }

    uint64_t pool_id = BufferPoolRegistry::getInstance().registerPool(
        pool, getAllocatorId());
    pool->setRegistryId(pool_id);

    LOG4CPLUS_INFO_FMT(logger_, "BufferPool '%s' ready (pool_id=%lu)",
        pool->getName().c_str(), pool_id);
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
    QueueType queue,
    uint32_t custom_id
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
    
    uint32_t id = (custom_id != 0) ? custom_id : pool->getTotalCount();
    
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
    auto pool_ids = getPoolsByAllocator();

    if (pool_ids.empty()) {
        LOG4CPLUS_DEBUG(logger_, "No pools to destroy");
        return true;
    }

    LOG4CPLUS_DEBUG_FMT(logger_, "Destroying %zu pool(s)...", pool_ids.size());

    std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);

    for (uint64_t pool_id : pool_ids) {
        auto pool = getPoolSpecialForAllocator(pool_id);
        if (!pool) {
            LOG4CPLUS_WARN_FMT(logger_,
                "pool_id %lu not found (already destroyed?)", pool_id);
            continue;
        }

        std::vector<Buffer*> to_remove;
        for (Buffer* buf : pool->getAllManagedBuffers()) {
            auto it = framebuffer_buffer_ownership_.find(buf);
            if (it != framebuffer_buffer_ownership_.end() && it->second == this) {
                to_remove.push_back(buf);
            }
        }

        for (Buffer* buf : to_remove) {
            if (taco_allocated_) {
                if (buf->getVirtualAddress()) {
                    taco_sys_munmap(buf->getVirtualAddress(),
                                   static_cast<uint32_t>(buf->size()));
                }
                if (buf->id() != 0) {
                    taco_sys_release_block(buf->id());
                }
                LOG4CPLUS_DEBUG_FMT(logger_,
                    "TACO block released: blk_id=%u", buf->id());
            }
            BufferAllocatorBase::removeBufferFromPoolInternal(pool.get(), buf);
            deallocateBuffer(buf);
            framebuffer_buffer_ownership_.erase(buf);
        }

        LOG4CPLUS_DEBUG_FMT(logger_,
            "Pool '%s' destroyed: %zu buffers (taco_cleanup=%s)",
            pool->getName().c_str(), to_remove.size(),
            taco_allocated_ ? "yes" : "no");

        unregisterPool(pool_id);
    }

    taco_allocated_ = false;
    return true;
}

