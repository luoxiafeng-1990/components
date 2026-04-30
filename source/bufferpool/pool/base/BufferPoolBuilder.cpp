#include "bufferpool/pool/base/BufferPoolBuilder.hpp"
#include "bufferpool/buffer/AVFrameBuffer.hpp"
#include "bufferpool/buffer/MatBuffer.hpp"
#include "bufferpool/buffer/RawBuffer.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include "common/Logger.hpp"
#include <cstdlib>
#include <cstring>
#include <vector>

static constexpr int MAX_HARDWARE_BUFFERS = 32;

std::unordered_map<Buffer*, IBufferPoolBuilder*> BufferPoolBuilder::buffer_ownership_;
std::mutex BufferPoolBuilder::ownership_mutex_;

// ========== 构造 / 析构 ==========

BufferPoolBuilder::BufferPoolBuilder(Buffer::Type type,
                                     std::unique_ptr<IMemoryProvider> provider)
    : buffer_type_(type)
    , memory_provider_(std::move(provider))
    , alignment_(memory_provider_ ? memory_provider_->getCapabilities().default_alignment : 64)
    , logger_(log4cplus::Logger::getInstance(
          LOG4CPLUS_TEXT("components.PoolBuilder")))
{
    LOG4CPLUS_DEBUG_FMT(logger_, "创建: type=%s, provider=%s",
                        Buffer::typeToString(type),
                        memory_provider_ ? memory_provider_->kind() : "none");
}

BufferPoolBuilder::~BufferPoolBuilder() {
    destroyPool();
    LOG4CPLUS_DEBUG(logger_, "BufferPoolBuilder destroyed");
}

// ========== 静态工厂方法 ==========

std::unique_ptr<BufferPoolBuilder> BufferPoolBuilder::forAVFrame() {
    return std::unique_ptr<BufferPoolBuilder>(
        new BufferPoolBuilder(Buffer::Type::AVFRAME));
}

std::unique_ptr<BufferPoolBuilder> BufferPoolBuilder::forMat() {
    return std::unique_ptr<BufferPoolBuilder>(
        new BufferPoolBuilder(Buffer::Type::MAT));
}

std::unique_ptr<BufferPoolBuilder> BufferPoolBuilder::forPhysicalMemory(
    std::unique_ptr<IMemoryProvider> provider)
{
    return std::unique_ptr<BufferPoolBuilder>(
        new BufferPoolBuilder(Buffer::Type::RAW, std::move(provider)));
}

// ========== createBuffer ==========

Buffer* BufferPoolBuilder::createBuffer(uint32_t id, size_t size) {
    switch (buffer_type_) {
        case Buffer::Type::AVFRAME: {
            auto* buf = new AVFrameBuffer(id, nullptr, 0, size, Buffer::Ownership::EXTERNAL);
            AVFrame* frame_ptr = av_frame_alloc();
            if (!frame_ptr) {
                LOG4CPLUS_ERROR_FMT(logger_, "Failed to alloc AVFrame for buffer #%u", id);
                delete buf;
                return nullptr;
            }
            buf->setAVFrame(frame_ptr);

            AVPacket* packet_ptr = av_packet_alloc();
            if (!packet_ptr) {
                LOG4CPLUS_ERROR_FMT(logger_, "Failed to alloc AVPacket for buffer #%u", id);
                av_frame_free(&frame_ptr);
                delete buf;
                return nullptr;
            }
            buf->setAVPacket(packet_ptr);
            return buf;
        }

        case Buffer::Type::MAT: {
            auto* buf = new MatBuffer(id, nullptr, 0, size, Buffer::Ownership::EXTERNAL);
            cv::Mat* mat_ptr = new cv::Mat();
            buf->setMat(mat_ptr);

            AVPacket* packet_ptr = av_packet_alloc();
            if (!packet_ptr) {
                LOG4CPLUS_ERROR_FMT(logger_, "Failed to alloc AVPacket for buffer #%u", id);
                delete mat_ptr;
                delete buf;
                return nullptr;
            }
            buf->setAVPacket(packet_ptr);
            return buf;
        }

        case Buffer::Type::RAW: {
            if (!memory_provider_) {
                LOG4CPLUS_ERROR(logger_, "createBuffer(RAW): no IMemoryProvider");
                return nullptr;
            }
            MemoryBlock block = memory_provider_->allocate(size, alignment_);
            if (!block.virt_addr) {
                LOG4CPLUS_ERROR_FMT(logger_, "IMemoryProvider(%s) alloc failed: id=%u size=%zu",
                                    memory_provider_->kind(), id, size);
                return nullptr;
            }
            memset(block.virt_addr, 0, size);
            bool is_physical = memory_provider_->getCapabilities().supports_physical_address;
            auto ownership = is_physical ? Buffer::Ownership::EXTERNAL : Buffer::Ownership::OWNED;
            auto* buf = new RawBuffer(
                is_physical ? block.handle : id,
                block.virt_addr, block.phys_addr, size, ownership,
                memory_provider_.get());
            return buf;
        }
    }
    return nullptr;
}

// ========== deallocateBuffer ==========

void BufferPoolBuilder::deallocateBuffer(Buffer* buffer) {
    // 虚析构函数处理类型特定的资源释放
    delete buffer;
}

// ========== allocatePoolWithBuffers ==========

uint64_t BufferPoolBuilder::allocatePoolWithBuffers(
    int count, size_t size,
    const std::string& name, const std::string& category)
{
    if (count <= 0 || (buffer_type_ == Buffer::Type::RAW && size == 0)) {
        LOG4CPLUS_ERROR_FMT(logger_, "allocatePoolWithBuffers: invalid params (count=%d, size=%zu)", count, size);
        return 0;
    }

    if (count > MAX_HARDWARE_BUFFERS) {
        LOG4CPLUS_WARN_FMT(logger_, "count %d clamped to %d", count, MAX_HARDWARE_BUFFERS);
        count = MAX_HARDWARE_BUFFERS;
    }

    LOG4CPLUS_INFO_FMT(logger_, "Creating BufferPool '%s': %d x %zu bytes (type=%s)",
                        name.c_str(), count, size, Buffer::typeToString(buffer_type_));

    auto pool = std::make_shared<BufferPool>(token(), name, category);

    for (int i = 0; i < count; i++) {
        uint32_t buffer_id = next_buffer_id_.fetch_add(1);
        Buffer* buffer = createBuffer(buffer_id, size);
        if (!buffer) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to create buffer #%d", i);
            cleanupPoolTemp(pool.get());
            return 0;
        }

        if (!IBufferPoolBuilder::addBufferToPoolQueue(pool.get(), buffer, QueueType::FREE)) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to add buffer #%u to FREE queue", buffer_id);
            deallocateBuffer(buffer);
            cleanupPoolTemp(pool.get());
            return 0;
        }

        {
            std::lock_guard<std::mutex> lock(ownership_mutex_);
            buffer_ownership_[buffer] = this;
        }
    }

    uint64_t pool_id = ComponentTopology::getInstance().registerPool(pool, getAllocatorId());
    pool->setRegistryId(pool_id);

    LOG4CPLUS_INFO_FMT(logger_, "BufferPool '%s' ready (pool_id=%lu, type=%s)",
                        name.c_str(), pool_id, Buffer::typeToString(buffer_type_));
    return pool_id;
}

// ========== injectBufferToPool ==========

Buffer* BufferPoolBuilder::injectBufferToPool(
    uint64_t pool_id, size_t size, QueueType queue)
{
    auto pool_weak = ComponentTopology::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        LOG4CPLUS_ERROR_FMT(logger_, "injectBufferToPool: pool_id %lu not found", pool_id);
        return nullptr;
    }

    uint32_t id = next_buffer_id_.fetch_add(1);
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
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_[buffer] = this;
    }
    return buffer;
}

// ========== injectExternalBufferToPool ==========

Buffer* BufferPoolBuilder::injectExternalBufferToPool(
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
        LOG4CPLUS_ERROR_FMT(logger_, "pool_id %lu not found", pool_id);
        return nullptr;
    }

    uint32_t id = next_buffer_id_.fetch_add(1);
    Buffer* buffer = nullptr;

    switch (buffer_type_) {
        case Buffer::Type::AVFRAME:
            buffer = new AVFrameBuffer(id, virt_addr, phys_addr, size, Buffer::Ownership::EXTERNAL);
            break;
        case Buffer::Type::MAT:
            buffer = new MatBuffer(id, virt_addr, phys_addr, size, Buffer::Ownership::EXTERNAL);
            break;
        case Buffer::Type::RAW:
            buffer = new RawBuffer(id, virt_addr, phys_addr, size, Buffer::Ownership::EXTERNAL, nullptr);
            break;
    }

    if (!IBufferPoolBuilder::addBufferToPoolQueue(pool.get(), buffer, queue)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to add external buffer #%u to pool '%s'",
               id, pool->getName().c_str());
        delete buffer;
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_[buffer] = this;
    }
    return buffer;
}

// ========== removeBufferFromPool ==========

bool BufferPoolBuilder::removeBufferFromPool(uint64_t pool_id, Buffer* buffer) {
    if (!buffer) {
        LOG4CPLUS_ERROR(logger_, "removeBufferFromPool: buffer is nullptr");
        return false;
    }

    auto pool_weak = ComponentTopology::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        LOG4CPLUS_ERROR_FMT(logger_, "pool_id %lu not found", pool_id);
        return false;
    }

    if (!IBufferPoolBuilder::removeBufferFromPoolInternal(pool.get(), buffer)) {
        LOG4CPLUS_WARN_FMT(logger_, "Failed to remove buffer #%u from pool '%s'",
               buffer->id(), pool->getName().c_str());
        return false;
    }

    deallocateBuffer(buffer);

    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_.erase(buffer);
    }
    return true;
}

// ========== destroyPool ==========

bool BufferPoolBuilder::destroyPool() {
    auto pool_ids = getPoolsByAllocator();
    if (pool_ids.empty()) {
        LOG4CPLUS_DEBUG(logger_, "No pools to destroy");
        return true;
    }

    LOG4CPLUS_INFO_FMT(logger_, "Destroying %zu pool(s)...", pool_ids.size());

    std::lock_guard<std::mutex> lock(ownership_mutex_);

    for (uint64_t pool_id : pool_ids) {
        auto pool = getPoolSpecialForAllocator(pool_id);
        if (!pool) {
            LOG4CPLUS_WARN_FMT(logger_, "pool_id %lu not found", pool_id);
            continue;
        }

        std::vector<Buffer*> to_remove;
        for (Buffer* buf : pool->getAllManagedBuffers()) {
            auto it = buffer_ownership_.find(buf);
            if (it != buffer_ownership_.end() && it->second == this) {
                to_remove.push_back(buf);
            }
        }

        for (Buffer* buf : to_remove) {
            IBufferPoolBuilder::removeBufferFromPoolInternal(pool.get(), buf);
            deallocateBuffer(buf);
            buffer_ownership_.erase(buf);
        }

        LOG4CPLUS_DEBUG_FMT(logger_, "Pool '%s' destroyed: %zu buffers cleaned",
               pool->getName().c_str(), to_remove.size());

        unregisterPool(pool_id);
    }
    return true;
}

// ========== cleanupPoolTemp ==========

void BufferPoolBuilder::cleanupPoolTemp(BufferPool* pool) {
    if (!pool) return;

    LOG4CPLUS_DEBUG_FMT(logger_, "清理临时pool '%s'", pool->getName().c_str());

    std::lock_guard<std::mutex> lock(ownership_mutex_);

    std::vector<Buffer*> to_remove;
    for (auto& [buf, alloc] : buffer_ownership_) {
        if (alloc == this) {
            to_remove.push_back(buf);
        }
    }

    for (Buffer* buf : to_remove) {
        IBufferPoolBuilder::removeBufferFromPoolInternal(pool, buf);
        deallocateBuffer(buf);
        buffer_ownership_.erase(buf);
    }
}

// ========== 类型特化便捷方法 ==========

Buffer* BufferPoolBuilder::injectAVFrameToPool(AVFrame* frame, BufferPool* pool) {
    if (!frame || !pool) {
        LOG4CPLUS_ERROR(logger_, "injectAVFrameToPool: invalid parameters");
        return nullptr;
    }

    uint32_t id = next_buffer_id_.fetch_add(1);
    void* virt_addr = frame->data[0];
    size_t size = frame->linesize[0] * frame->height;

    if (!virt_addr || size == 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "Invalid AVFrame: data=%p, size=%zu", virt_addr, size);
        return nullptr;
    }

    auto* buffer = new AVFrameBuffer(id, virt_addr, 0, size, Buffer::Ownership::EXTERNAL);
    buffer->setAVFrame(frame);

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to alloc AVPacket for Buffer #%u", id);
        delete buffer;
        return nullptr;
    }
    buffer->setAVPacket(packet);

    if (!IBufferPoolBuilder::addBufferToPoolQueue(pool, buffer, QueueType::FILLED)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to add buffer #%u to pool '%s'",
               id, pool->getName().c_str());
        delete buffer;
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_[buffer] = this;
    }

    LOG4CPLUS_DEBUG_FMT(logger_, "AVFrame injected to pool '%s' as Buffer #%u",
           pool->getName().c_str(), id);
    return buffer;
}

bool BufferPoolBuilder::releaseAVFrame(Buffer* buffer, BufferPool* pool) {
    if (!buffer || !pool) {
        LOG4CPLUS_ERROR(logger_, "releaseAVFrame: invalid parameters");
        return false;
    }

    if (!IBufferPoolBuilder::removeBufferFromPoolInternal(pool, buffer)) {
        LOG4CPLUS_WARN_FMT(logger_, "Failed to remove buffer #%u from pool '%s'",
               buffer->id(), pool->getName().c_str());
    }

    deallocateBuffer(buffer);

    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_.erase(buffer);
    }
    return true;
}

Buffer* BufferPoolBuilder::injectMatToPool(cv::Mat* mat, BufferPool* pool) {
    if (!mat || !pool) {
        LOG4CPLUS_ERROR(logger_, "injectMatToPool: invalid parameters");
        return nullptr;
    }

    uint32_t id = next_buffer_id_.fetch_add(1);
    void* virt_addr = mat->data;
    size_t size = mat->total() * mat->elemSize();

    if (!virt_addr || size == 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "Invalid Mat: data=%p, size=%zu", virt_addr, size);
        return nullptr;
    }

    auto* buffer = new MatBuffer(id, virt_addr, 0, size, Buffer::Ownership::EXTERNAL);
    buffer->setMat(mat);

    if (!IBufferPoolBuilder::addBufferToPoolQueue(pool, buffer, QueueType::FILLED)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to add buffer #%u to pool '%s'",
               id, pool->getName().c_str());
        delete buffer;
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_[buffer] = this;
    }

    LOG4CPLUS_DEBUG_FMT(logger_, "Mat injected to pool '%s' as Buffer #%u",
           pool->getName().c_str(), id);
    return buffer;
}

bool BufferPoolBuilder::releaseMat(Buffer* buffer, BufferPool* pool) {
    if (!buffer || !pool) {
        LOG4CPLUS_ERROR(logger_, "releaseMat: invalid parameters");
        return false;
    }

    if (!IBufferPoolBuilder::removeBufferFromPoolInternal(pool, buffer)) {
        LOG4CPLUS_WARN_FMT(logger_, "Failed to remove buffer #%u from pool '%s'",
               buffer->id(), pool->getName().c_str());
    }

    deallocateBuffer(buffer);

    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_.erase(buffer);
    }
    return true;
}
