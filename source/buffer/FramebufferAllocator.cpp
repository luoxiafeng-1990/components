#include "buffer/FramebufferAllocator.hpp"
#include "vendor/contracts/MemoryProviderRegistry.hpp"
#include "common/Logger.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include <unordered_map>
#include <mutex>
#include <cstring>

static std::unordered_map<Buffer*, BufferAllocatorBase*> framebuffer_buffer_ownership_;
static std::mutex framebuffer_ownership_mutex_;

// ============================================================
// 构造 / 析构
// ============================================================

FramebufferAllocator::FramebufferAllocator(std::unique_ptr<IMemoryProvider> provider)
    : memory_provider_(std::move(provider))
    , logger_(log4cplus::Logger::getInstance(
          LOG4CPLUS_TEXT("components.Allocator.Framebuffer")))
{
    LOG4CPLUS_DEBUG_FMT(logger_, "创建完成: provider=%s",
                        memory_provider_ ? memory_provider_->kind() : "null");
}

FramebufferAllocator::FramebufferAllocator()
    : memory_provider_(MemoryProviderRegistry::instance().hasProvider("taco")
                        ? MemoryProviderRegistry::instance().create("taco")
                        : nullptr)
    , logger_(log4cplus::Logger::getInstance(
          LOG4CPLUS_TEXT("components.Allocator.Framebuffer")))
{
    if (!memory_provider_) {
        LOG4CPLUS_WARN(logger_,
            "MemoryProviderRegistry 中未找到 'taco' provider，"
            "allocatePoolWithBuffers 将无法工作");
    }
    LOG4CPLUS_DEBUG(logger_, "创建完成 (兼容模式)");
}

FramebufferAllocator::~FramebufferAllocator() {
    destroyPool();
    LOG4CPLUS_DEBUG(logger_, "FramebufferAllocator destroyed");
}

// ============================================================
// 分配 BufferPool（通过 IMemoryProvider 分配物理连续内存）
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

    if (!memory_provider_) {
        LOG4CPLUS_ERROR(logger_,
            "allocatePoolWithBuffers: no IMemoryProvider available");
        return 0;
    }

    LOG4CPLUS_INFO_FMT(logger_,
        "Creating BufferPool '%s': %d buffers x %zu bytes (provider=%s)",
        name.c_str(), count, size, memory_provider_->kind());

    auto pool = std::make_shared<BufferPool>(
        token(), name, category);

    std::vector<MemoryBlock> allocated_blocks;

    auto cleanup_pool = [&]() {
        std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
        for (Buffer* buf : pool->getAllManagedBuffers()) {
            framebuffer_buffer_ownership_.erase(buf);
            delete buf;
        }
        pool->clearAllManagedBuffers();
        for (auto& blk : allocated_blocks) {
            if (blk.virt_addr) {
                memory_provider_->deallocate(blk);
            }
        }
    };

    for (int i = 0; i < count; i++) {
        MemoryBlock block = memory_provider_->allocate(size);
        if (!block.virt_addr) {
            LOG4CPLUS_ERROR_FMT(logger_,
                "IMemoryProvider(%s) 分配失败: buffer #%d (size=%zu)",
                memory_provider_->kind(), i, size);
            cleanup_pool();
            return 0;
        }
        allocated_blocks.push_back(block);

        memset(block.virt_addr, 0, size);

        Buffer* buffer = new Buffer(
            block.handle, block.virt_addr, block.phys_addr, size,
            Buffer::Ownership::EXTERNAL);

        if (!BufferAllocatorBase::addBufferToPoolQueue(
                pool.get(), buffer, QueueType::FREE)) {
            LOG4CPLUS_ERROR_FMT(logger_,
                "Failed to add buffer #%d to pool", i);
            memory_provider_->deallocate(block);
            allocated_blocks.pop_back();
            delete buffer;
            cleanup_pool();
            return 0;
        }

        {
            std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
            framebuffer_buffer_ownership_[buffer] = this;
        }

        LOG4CPLUS_INFO_FMT(logger_,
            "  Buffer #%d: handle=%u, phys=0x%llx, virt=%p, size=%zu",
            i, block.handle, (unsigned long long)block.phys_addr,
            block.virt_addr, size);
    }

    uint64_t pool_id = ComponentTopology::getInstance().registerPool(
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

    auto pool_weak = ComponentTopology::getInstance().getPool(pool_id);
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
// 销毁所有 Pool（通过 IMemoryProvider 清理）
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
            if (buf->getVirtualAddress() && memory_provider_) {
                MemoryBlock block;
                block.virt_addr = buf->getVirtualAddress();
                block.phys_addr = buf->getPhysicalAddress();
                block.size      = buf->size();
                block.handle    = buf->id();
                memory_provider_->deallocate(block);
            }
            LOG4CPLUS_DEBUG_FMT(logger_,
                "Memory block released: handle=%u", buf->id());

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
