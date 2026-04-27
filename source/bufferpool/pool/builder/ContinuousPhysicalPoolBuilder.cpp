#include "bufferpool/pool/builder/ContinuousPhysicalPoolBuilder.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include "common/Logger.hpp"
extern "C" {
#include <libavcodec/packet.h>
}
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <mutex>

static std::unordered_map<Buffer*, IBufferPoolBuilder*> cphys_buffer_ownership_;
static std::mutex cphys_ownership_mutex_;

ContinuousPhysicalPoolBuilder::ContinuousPhysicalPoolBuilder(
    std::unique_ptr<IMemoryProvider> provider)
    : memory_provider_(std::move(provider))
    , alignment_(memory_provider_ ? memory_provider_->getCapabilities().default_alignment : 64)
    , logger_(log4cplus::Logger::getInstance(
          LOG4CPLUS_TEXT("components.PoolBuilder.ContinuousPhysical")))
{
    LOG4CPLUS_DEBUG_FMT(logger_, "创建: provider=%s, alignment=%zu",
                        memory_provider_ ? memory_provider_->kind() : "null",
                        alignment_);
}

ContinuousPhysicalPoolBuilder::~ContinuousPhysicalPoolBuilder() {
    destroyPool();
    LOG4CPLUS_DEBUG(logger_, "ContinuousPhysicalPoolBuilder destroyed");
}

Buffer* ContinuousPhysicalPoolBuilder::createBuffer(uint32_t id, size_t size) {
    if (!memory_provider_) {
        LOG4CPLUS_ERROR(logger_, "createBuffer: no IMemoryProvider available");
        return nullptr;
    }

    MemoryBlock block = memory_provider_->allocate(size, alignment_);
    if (!block.virt_addr) {
        LOG4CPLUS_ERROR_FMT(logger_, "IMemoryProvider(%s) 分配失败: id=%u size=%zu",
                            memory_provider_->kind(), id, size);
        return nullptr;
    }

    memset(block.virt_addr, 0, size);

    bool is_physical = memory_provider_->getCapabilities().supports_physical_address;
    Buffer::Ownership ownership = is_physical ? Buffer::Ownership::EXTERNAL : Buffer::Ownership::OWNED;

    Buffer* buffer = new Buffer(
        is_physical ? block.handle : id,
        block.virt_addr,
        block.phys_addr,
        size,
        ownership
    );

    return buffer;
}

void ContinuousPhysicalPoolBuilder::deallocateBuffer(Buffer* buffer) {
    if (!buffer) return;

    if (buffer->getAVPacket()) {
        AVPacket* pkt = buffer->getAVPacket();
        av_packet_free(&pkt);
        buffer->setAVPacket(nullptr);
    }

    void* virt_addr = buffer->getVirtualAddress();
    if (virt_addr && memory_provider_) {
        MemoryBlock block;
        block.virt_addr = virt_addr;
        block.phys_addr = buffer->getPhysicalAddress();
        block.size      = buffer->size();
        block.handle    = buffer->id();
        memory_provider_->deallocate(block);
    }

    delete buffer;
}

uint64_t ContinuousPhysicalPoolBuilder::allocatePoolWithBuffers(
    int count, size_t size,
    const std::string& name, const std::string& category)
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

    auto pool = std::make_shared<BufferPool>(token(), name, category);

    for (int i = 0; i < count; i++) {
        Buffer* buffer = createBuffer(i, size);
        if (!buffer) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to create buffer #%d", i);
            cleanupPoolTemp(pool.get());
            return 0;
        }

        if (!IBufferPoolBuilder::addBufferToPoolQueue(pool.get(), buffer, QueueType::FREE)) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to add buffer #%d to pool", i);
            deallocateBuffer(buffer);
            cleanupPoolTemp(pool.get());
            return 0;
        }

        {
            std::lock_guard<std::mutex> lock(cphys_ownership_mutex_);
            cphys_buffer_ownership_[buffer] = this;
        }

        LOG4CPLUS_INFO_FMT(logger_,
            "  Buffer #%d: virt=%p, phys=0x%llx, size=%zu",
            i, buffer->getVirtualAddress(),
            (unsigned long long)buffer->getPhysicalAddress(), size);
    }

    uint64_t pool_id = ComponentTopology::getInstance().registerPool(
        pool, getAllocatorId());
    pool->setRegistryId(pool_id);

    LOG4CPLUS_INFO_FMT(logger_, "BufferPool '%s' ready (pool_id=%lu)",
        pool->getName().c_str(), pool_id);
    return pool_id;
}

Buffer* ContinuousPhysicalPoolBuilder::injectBufferToPool(
    uint64_t pool_id, size_t size, QueueType queue)
{
    auto pool_weak = ComponentTopology::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        LOG4CPLUS_ERROR_FMT(logger_, "injectBufferToPool: pool_id %lu not found", pool_id);
        return nullptr;
    }

    uint32_t id = pool->getTotalCount();
    Buffer* buffer = createBuffer(id, size);
    if (!buffer) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to create buffer #%u", id);
        return nullptr;
    }

    if (!IBufferPoolBuilder::addBufferToPoolQueue(pool.get(), buffer, queue)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to add buffer #%u to pool '%s'",
               id, pool->getName().c_str());
        deallocateBuffer(buffer);
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(cphys_ownership_mutex_);
        cphys_buffer_ownership_[buffer] = this;
    }

    LOG4CPLUS_DEBUG_FMT(logger_, "Buffer #%u injected to pool '%s' (queue: %s)",
           id, pool->getName().c_str(),
           queue == QueueType::FREE ? "FREE" : "FILLED");
    return buffer;
}

Buffer* ContinuousPhysicalPoolBuilder::injectExternalBufferToPool(
    uint64_t pool_id, void* virt_addr, uint64_t phys_addr,
    size_t size, QueueType queue, uint32_t custom_id)
{
    if (!virt_addr || size == 0) {
        LOG4CPLUS_ERROR(logger_, "injectExternalBufferToPool: invalid parameters");
        return nullptr;
    }

    auto pool_weak = ComponentTopology::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        LOG4CPLUS_ERROR_FMT(logger_, "injectExternalBufferToPool: pool_id %lu not found", pool_id);
        return nullptr;
    }

    uint32_t id = pool->getTotalCount();
    Buffer* buffer = new Buffer(id, virt_addr, phys_addr, size, Buffer::Ownership::EXTERNAL);

    if (!IBufferPoolBuilder::addBufferToPoolQueue(pool.get(), buffer, queue)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to add external buffer #%u to pool '%s'",
               id, pool->getName().c_str());
        delete buffer;
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(cphys_ownership_mutex_);
        cphys_buffer_ownership_[buffer] = this;
    }

    return buffer;
}

bool ContinuousPhysicalPoolBuilder::removeBufferFromPool(
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

    if (!IBufferPoolBuilder::removeBufferFromPoolInternal(pool.get(), buffer)) {
        LOG4CPLUS_WARN_FMT(logger_,
            "Failed to remove buffer #%u from pool '%s'",
            buffer->id(), pool->getName().c_str());
        return false;
    }

    deallocateBuffer(buffer);

    {
        std::lock_guard<std::mutex> lock(cphys_ownership_mutex_);
        cphys_buffer_ownership_.erase(buffer);
    }
    return true;
}

bool ContinuousPhysicalPoolBuilder::destroyPool() {
    auto pool_ids = getPoolsByAllocator();

    if (pool_ids.empty()) {
        LOG4CPLUS_DEBUG(logger_, "No pools to destroy");
        return true;
    }

    LOG4CPLUS_INFO_FMT(logger_, "Destroying %zu pool(s)...", pool_ids.size());

    std::lock_guard<std::mutex> lock(cphys_ownership_mutex_);

    for (uint64_t pool_id : pool_ids) {
        auto pool = getPoolSpecialForAllocator(pool_id);
        if (!pool) {
            LOG4CPLUS_WARN_FMT(logger_,
                "pool_id %lu not found (already destroyed?)", pool_id);
            continue;
        }

        std::vector<Buffer*> to_remove;
        for (Buffer* buf : pool->getAllManagedBuffers()) {
            auto it = cphys_buffer_ownership_.find(buf);
            if (it != cphys_buffer_ownership_.end() && it->second == this) {
                to_remove.push_back(buf);
            }
        }

        for (Buffer* buf : to_remove) {
            IBufferPoolBuilder::removeBufferFromPoolInternal(pool.get(), buf);
            deallocateBuffer(buf);
            cphys_buffer_ownership_.erase(buf);
        }

        LOG4CPLUS_DEBUG_FMT(logger_,
            "Pool '%s' destroyed: %zu buffers cleaned",
            pool->getName().c_str(), to_remove.size());

        unregisterPool(pool_id);
    }

    return true;
}

void ContinuousPhysicalPoolBuilder::cleanupPoolTemp(BufferPool* pool) {
    if (!pool) return;

    LOG4CPLUS_DEBUG_FMT(logger_, "清理临时pool '%s'", pool->getName().c_str());

    std::lock_guard<std::mutex> lock(cphys_ownership_mutex_);

    std::vector<Buffer*> to_remove;
    for (auto& [buf, alloc] : cphys_buffer_ownership_) {
        if (alloc == this) {
            to_remove.push_back(buf);
        }
    }

    for (Buffer* buf : to_remove) {
        IBufferPoolBuilder::removeBufferFromPoolInternal(pool, buf);
        deallocateBuffer(buf);
        cphys_buffer_ownership_.erase(buf);
    }
}
