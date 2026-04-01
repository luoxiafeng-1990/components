#include "buffer/FramebufferAllocator.hpp"
#include "common/Logger.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include <unordered_map>
#include <mutex>
#include <cstring>

extern "C" {
#include "taco_sys_api.h"
}

static std::unordered_map<Buffer*, BufferAllocatorBase*> framebuffer_buffer_ownership_;
static std::mutex framebuffer_ownership_mutex_;

// ============================================================
// 构造 / 析构
// ============================================================

FramebufferAllocator::FramebufferAllocator()
    : logger_(log4cplus::Logger::getInstance(
          LOG4CPLUS_TEXT("components.Allocator.Framebuffer")))
{
    LOG4CPLUS_DEBUG(logger_, "创建完成");
}

FramebufferAllocator::~FramebufferAllocator() {
    destroyPool();
    LOG4CPLUS_DEBUG(logger_, "FramebufferAllocator destroyed");
}

// ============================================================
// 分配 BufferPool（内部通过 TACO API 分配物理连续内存）
// ============================================================

uint64_t FramebufferAllocator::allocatePoolWithBuffers(
    int count,
    size_t size,
    const std::string& name,
    const std::string& category)
{
    if (count <= 0 || size == 0) {
        LOG4CPLUS_ERROR_FMT(logger_,
            "allocatePoolWithBuffers: invalid params (count=%d, size=%zu)",
            count, size);
        return 0;
    }

    LOG4CPLUS_INFO_FMT(logger_,
        "Creating BufferPool '%s': %d buffers x %zu bytes",
        name.c_str(), count, size);

    auto pool = std::make_shared<BufferPool>(
        token(), name, category);

    auto cleanup_pool = [&]() {
        std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
        for (Buffer* buf : pool->getAllManagedBuffers()) {
            if (buf->getVirtualAddress()) {
                taco_sys_munmap(buf->getVirtualAddress(),
                               static_cast<uint32_t>(buf->size()));
            }
            if (buf->id() != 0) {
                taco_sys_release_block(buf->id());
            }
            deallocateBuffer(buf);
            framebuffer_buffer_ownership_.erase(buf);
        }
        pool->clearAllManagedBuffers();
    };

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

    uint64_t pool_id = BufferPoolRegistry::getInstance().registerPool(
        pool, getAllocatorId());
    pool->setRegistryId(pool_id);

    LOG4CPLUS_INFO_FMT(logger_, "BufferPool '%s' ready (pool_id=%lu)",
        pool->getName().c_str(), pool_id);
    return pool_id;
}

// ============================================================
// createBuffer / deallocateBuffer
// ============================================================

Buffer* FramebufferAllocator::createBuffer(uint32_t id, size_t size) {
    LOG4CPLUS_WARN(logger_,
        "createBuffer: use allocatePoolWithBuffers instead");
    return nullptr;
}

void FramebufferAllocator::deallocateBuffer(Buffer* buffer) {
    if (!buffer) return;
    LOG4CPLUS_DEBUG_FMT(logger_,
        "Deleting Buffer #%u", buffer->id());
    delete buffer;
}

// ============================================================
// inject / remove（保留接口兼容性）
// ============================================================

Buffer* FramebufferAllocator::injectBufferToPool(
    uint64_t pool_id, size_t size, QueueType queue)
{
    LOG4CPLUS_WARN(logger_,
        "injectBufferToPool: use allocatePoolWithBuffers instead");
    return nullptr;
}

Buffer* FramebufferAllocator::injectExternalBufferToPool(
    uint64_t pool_id,
    void* virt_addr,
    uint64_t phys_addr,
    size_t size,
    QueueType queue,
    uint32_t custom_id)
{
    LOG4CPLUS_WARN(logger_,
        "injectExternalBufferToPool: use allocatePoolWithBuffers instead");
    return nullptr;
}

bool FramebufferAllocator::removeBufferFromPool(
    uint64_t pool_id, Buffer* buffer)
{
    if (!buffer) {
        LOG4CPLUS_ERROR(logger_, "removeBufferFromPool: buffer is nullptr");
        return false;
    }

    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        LOG4CPLUS_ERROR_FMT(logger_,
            "pool_id %lu not found or already destroyed", pool_id);
        return false;
    }

    if (!BufferAllocatorBase::removeBufferFromPoolInternal(pool.get(), buffer)) {
        LOG4CPLUS_WARN_FMT(logger_,
            "Failed to remove buffer #%u from pool '%s'",
            buffer->id(), pool->getName().c_str());
        return false;
    }

    deallocateBuffer(buffer);

    {
        std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
        framebuffer_buffer_ownership_.erase(buffer);
    }
    return true;
}

// ============================================================
// 销毁所有 Pool（自动 TACO 清理）
// ============================================================

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
            if (buf->getVirtualAddress()) {
                taco_sys_munmap(buf->getVirtualAddress(),
                               static_cast<uint32_t>(buf->size()));
            }
            if (buf->id() != 0) {
                taco_sys_release_block(buf->id());
            }
            LOG4CPLUS_DEBUG_FMT(logger_,
                "TACO block released: blk_id=%u", buf->id());

            BufferAllocatorBase::removeBufferFromPoolInternal(pool.get(), buf);
            deallocateBuffer(buf);
            framebuffer_buffer_ownership_.erase(buf);
        }

        LOG4CPLUS_DEBUG_FMT(logger_,
            "Pool '%s' destroyed: %zu buffers cleaned",
            pool->getName().c_str(), to_remove.size());

        unregisterPool(pool_id);
    }

    return true;
}
