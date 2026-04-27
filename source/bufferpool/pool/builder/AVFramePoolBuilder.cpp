#include "bufferpool/pool/builder/AVFramePoolBuilder.hpp"
#include "common/Logger.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include <stdio.h>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>

static constexpr int MAX_HARDWARE_BUFFERS = 32;

static std::unordered_map<Buffer*, IBufferPoolBuilder*> avframe_buffer_ownership_;
static std::mutex avframe_ownership_mutex_;

AVFramePoolBuilder::AVFramePoolBuilder()
    : next_buffer_id_(0)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.PoolBuilder.AVFrame")))
{
    LOG4CPLUS_DEBUG(logger_, "创建完成");
}

AVFramePoolBuilder::~AVFramePoolBuilder() {
    destroyPool();
    LOG4CPLUS_DEBUG(logger_, "AVFramePoolBuilder destroyed");
}

Buffer* AVFramePoolBuilder::injectAVFrameToPool(AVFrame* frame, BufferPool* pool) {
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

    Buffer* buffer = new Buffer(id, virt_addr, 0, size, Buffer::Ownership::EXTERNAL);

    buffer->setAVFrame(frame);

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to allocate AVPacket for Buffer #%u", id);
        delete buffer;
        return nullptr;
    }
    buffer->setAVPacket(packet);
    LOG_TRACE_FMT("  AVPacket allocated at %p for Buffer #%u", packet, id);

    if (!IBufferPoolBuilder::addBufferToPoolQueue(pool, buffer, QueueType::FILLED)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to add buffer #%u to pool '%s'",
               id, pool->getName().c_str());
        delete buffer;
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(avframe_ownership_mutex_);
        avframe_buffer_ownership_[buffer] = this;
    }

    LOG4CPLUS_DEBUG_FMT(logger_, "AVFrame injected to pool '%s' as Buffer #%u (size=%zu)",
           pool->getName().c_str(), id, size);

    return buffer;
}

bool AVFramePoolBuilder::releaseAVFrame(Buffer* buffer, BufferPool* pool) {
    if (!buffer || !pool) {
        LOG4CPLUS_ERROR(logger_, "releaseAVFrame: invalid parameters");
        return false;
    }

    AVFrame* frame = buffer->getAVFrame();
    if (frame) {
        av_frame_free(&frame);
        buffer->setAVFrame(nullptr);
    } else {
        LOG4CPLUS_WARN_FMT(logger_, "No AVFrame found for Buffer #%u", buffer->id());
    }

    if (!IBufferPoolBuilder::removeBufferFromPoolInternal(pool, buffer)) {
        LOG4CPLUS_WARN_FMT(logger_, "Failed to remove buffer #%u from pool '%s'",
               buffer->id(), pool->getName().c_str());
    }

    delete buffer;

    {
        std::lock_guard<std::mutex> lock(avframe_ownership_mutex_);
        avframe_buffer_ownership_.erase(buffer);
    }

    LOG4CPLUS_DEBUG_FMT(logger_, "Buffer #%u and AVFrame released", buffer->id());
    return true;
}

Buffer* AVFramePoolBuilder::createBuffer(uint32_t id, size_t size) {
    LOG4CPLUS_WARN(logger_, "createBuffer should not be called directly, use injectAVFrameToPool()");
    return nullptr;
}

void AVFramePoolBuilder::deallocateBuffer(Buffer* buffer) {
    if (!buffer) return;

    AVFrame* frame = buffer->getAVFrame();
    if (frame) {
        av_frame_free(&frame);
        buffer->setAVFrame(nullptr);
    }

    AVPacket* packet = buffer->getAVPacket();
    if (packet) {
        av_packet_free(&packet);
        buffer->setAVPacket(nullptr);
    }

    delete buffer;
}

uint64_t AVFramePoolBuilder::allocatePoolWithBuffers(
    int count, size_t size,
    const std::string& name, const std::string& category
) {
    LOG4CPLUS_DEBUG_FMT(logger_, "allocatePoolWithBuffers: name='%s', count=%d, size=%zu",
           name.c_str(), count, size);

    auto pool = std::make_shared<BufferPool>(token(), name, category);
    if (count > MAX_HARDWARE_BUFFERS || count <= 0) {
        LOG4CPLUS_WARN_FMT(logger_, "count %d clamped to MAX_HARDWARE_BUFFERS %d", count, MAX_HARDWARE_BUFFERS);
        count = MAX_HARDWARE_BUFFERS;
    }

    for (int i = 0; i < count; i++) {
        AVFrame* frame_ptr = av_frame_alloc();
        if (!frame_ptr) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to allocate AVFrame[%d]", i);
            return 0;
        }

        LOG_TRACE_FMT("  AVFrame[%d] allocated at %p", i, frame_ptr);

        uint32_t buffer_id = next_buffer_id_.fetch_add(1);
        Buffer* buffer = new Buffer(buffer_id, nullptr, 0, size, Buffer::Ownership::EXTERNAL);

        buffer->setAVFrame(frame_ptr);

        AVPacket* packet_ptr = av_packet_alloc();
        if (!packet_ptr) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to allocate AVPacket for buffer #%u", buffer_id);
            av_frame_free(&frame_ptr);
            delete buffer;
            return 0;
        }
        buffer->setAVPacket(packet_ptr);
        LOG_TRACE_FMT("  AVPacket allocated at %p for Buffer #%u", packet_ptr, buffer_id);

        {
            std::lock_guard<std::mutex> lock(avframe_ownership_mutex_);
            avframe_buffer_ownership_[buffer] = this;
        }

        if (!IBufferPoolBuilder::addBufferToPoolQueue(pool.get(), buffer, QueueType::FREE)) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to add Buffer #%u to FREE queue", buffer_id);
            delete buffer;
            av_frame_free(&frame_ptr);
            return 0;
        }

        LOG_TRACE_FMT("  Buffer #%u wraps AVFrame* %p", buffer_id, frame_ptr);
    }

    LOG4CPLUS_INFO_FMT(logger_, "BufferPool '%s' ready with %d AVFrame buffers", name.c_str(), count);

    uint64_t pool_id = ComponentTopology::getInstance().registerPool(pool, getAllocatorId());
    pool->setRegistryId(pool_id);
    return pool_id;
}

Buffer* AVFramePoolBuilder::injectBufferToPool(
    uint64_t pool_id, size_t size, QueueType queue
) {
    LOG4CPLUS_WARN(logger_, "injectBufferToPool: use injectAVFrameToPool() instead");
    return nullptr;
}

Buffer* AVFramePoolBuilder::injectExternalBufferToPool(
    uint64_t pool_id, void* virt_addr, uint64_t phys_addr,
    size_t size, QueueType queue, uint32_t custom_id
) {
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
    Buffer* buffer = new Buffer(id, virt_addr, phys_addr, size, Buffer::Ownership::EXTERNAL);

    if (!IBufferPoolBuilder::addBufferToPoolQueue(pool.get(), buffer, queue)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to add external buffer #%u to pool '%s'",
               id, pool->getName().c_str());
        delete buffer;
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(avframe_ownership_mutex_);
        avframe_buffer_ownership_[buffer] = this;
    }

    LOG_TRACE_FMT("External buffer #%u injected (virt=%p, phys=0x%lx, size=%zu)",
           id, virt_addr, phys_addr, size);

    return buffer;
}

bool AVFramePoolBuilder::removeBufferFromPool(uint64_t pool_id, Buffer* buffer) {
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
        std::lock_guard<std::mutex> lock(avframe_ownership_mutex_);
        avframe_buffer_ownership_.erase(buffer);
    }

    LOG4CPLUS_DEBUG_FMT(logger_, "Buffer #%u removed from pool '%s'",
           buffer->id(), pool->getName().c_str());
    return true;
}

bool AVFramePoolBuilder::destroyPool() {
    auto pool_ids = getPoolsByAllocator();

    if (pool_ids.empty()) {
        LOG4CPLUS_DEBUG(logger_, "No pools to destroy");
        return true;
    }

    LOG4CPLUS_DEBUG_FMT(logger_, "Destroying %zu pool(s)...", pool_ids.size());

    std::lock_guard<std::mutex> lock(avframe_ownership_mutex_);

    for (uint64_t pool_id : pool_ids) {
        auto pool = getPoolSpecialForAllocator(pool_id);
        if (!pool) {
            LOG4CPLUS_WARN_FMT(logger_, "pool_id %lu not found", pool_id);
            continue;
        }

        LOG4CPLUS_DEBUG_FMT(logger_, "Destroying pool '%s' (ID: %lu)...", pool->getName().c_str(), pool_id);

        std::vector<Buffer*> to_remove;
        for (Buffer* buf : pool->getAllManagedBuffers()) {
            auto it = avframe_buffer_ownership_.find(buf);
            if (it != avframe_buffer_ownership_.end() && it->second == this) {
                to_remove.push_back(buf);
            }
        }

        for (Buffer* buf : to_remove) {
            IBufferPoolBuilder::removeBufferFromPoolInternal(pool.get(), buf);
            deallocateBuffer(buf);
            avframe_buffer_ownership_.erase(buf);
        }

        LOG4CPLUS_DEBUG_FMT(logger_, "Pool '%s' destroyed: removed %zu buffers",
               pool->getName().c_str(), to_remove.size());

        unregisterPool(pool_id);
    }

    LOG4CPLUS_DEBUG_FMT(logger_, "All %zu pool(s) destroyed", pool_ids.size());
    return true;
}
