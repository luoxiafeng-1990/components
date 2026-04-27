#include "bufferpool/pool/builder/MatPoolBuilder.hpp"
#include "common/Logger.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include <stdio.h>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>

extern "C" {
#include <libavutil/frame.h>
#include <libavcodec/packet.h>
}

static std::unordered_map<Buffer*, IBufferPoolBuilder*> mat_buffer_ownership_;
static std::mutex mat_ownership_mutex_;

MatPoolBuilder::MatPoolBuilder()
    : next_buffer_id_(0)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.PoolBuilder.Mat")))
{
    LOG4CPLUS_DEBUG(logger_, "创建完成");
}

MatPoolBuilder::~MatPoolBuilder() {
    destroyPool();
    LOG4CPLUS_DEBUG(logger_, "MatPoolBuilder destroyed");
}

Buffer* MatPoolBuilder::injectMatToPool(cv::Mat* mat, BufferPool* pool) {
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

    Buffer* buffer = new Buffer(id, virt_addr, 0, size, Buffer::Ownership::EXTERNAL);

    buffer->setMat(mat);

    if (!IBufferPoolBuilder::addBufferToPoolQueue(pool, buffer, QueueType::FILLED)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to add buffer #%u to pool '%s'",
               id, pool->getName().c_str());
        delete buffer;
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(mat_ownership_mutex_);
        mat_buffer_ownership_[buffer] = this;
    }

    LOG4CPLUS_DEBUG_FMT(logger_, "Mat injected to pool '%s' as Buffer #%u (size=%zu)",
           pool->getName().c_str(), id, size);
    return buffer;
}

bool MatPoolBuilder::releaseMat(Buffer* buffer, BufferPool* pool) {
    if (!buffer || !pool) {
        LOG4CPLUS_ERROR(logger_, "releaseMat: invalid parameters");
        return false;
    }

    cv::Mat* mat = buffer->getMat();
    if (mat) {
        delete mat;
        buffer->setMat(nullptr);
    } else {
        LOG4CPLUS_WARN_FMT(logger_, "No Mat found for Buffer #%u", buffer->id());
    }

    if (!IBufferPoolBuilder::removeBufferFromPoolInternal(pool, buffer)) {
        LOG4CPLUS_WARN_FMT(logger_, "Failed to remove buffer #%u from pool '%s'",
               buffer->id(), pool->getName().c_str());
    }

    delete buffer;

    {
        std::lock_guard<std::mutex> lock(mat_ownership_mutex_);
        mat_buffer_ownership_.erase(buffer);
    }

    LOG4CPLUS_DEBUG_FMT(logger_, "Buffer #%u and Mat released", buffer->id());
    return true;
}

Buffer* MatPoolBuilder::createBuffer(uint32_t id, size_t size) {
    LOG4CPLUS_WARN(logger_, "createBuffer should not be called directly, use injectMatToPool()");
    return nullptr;
}

void MatPoolBuilder::deallocateBuffer(Buffer* buffer) {
    if (!buffer) return;

    cv::Mat* mat = buffer->getMat();
    if (mat) {
        delete mat;
        buffer->setMat(nullptr);
    }

    AVFrame* avframe = buffer->getAVFrame();
    if (avframe) {
        av_frame_free(&avframe);
        buffer->setAVFrame(nullptr);
    }

    AVPacket* packet_ptr = buffer->getAVPacket();
    if (packet_ptr) {
        delete packet_ptr;
        buffer->setAVPacket(nullptr);
    }

    delete buffer;
}

uint64_t MatPoolBuilder::allocatePoolWithBuffers(
    int count, size_t size,
    const std::string& name, const std::string& category
) {
    LOG4CPLUS_DEBUG_FMT(logger_, "allocatePoolWithBuffers: name='%s', count=%d, size=%zu",
           name.c_str(), count, size);

    auto pool = std::make_shared<BufferPool>(token(), name, category);

    for (int i = 0; i < count; i++) {
        cv::Mat* mat_ptr = new cv::Mat();

        LOG_TRACE_FMT("  Mat[%d] allocated at %p", i, mat_ptr);

        uint32_t buffer_id = next_buffer_id_.fetch_add(1);
        Buffer* buffer = new Buffer(buffer_id, nullptr, 0, size, Buffer::Ownership::EXTERNAL);

        buffer->setMat(mat_ptr);

        AVPacket* packet_ptr = av_packet_alloc();
        if (!packet_ptr) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to allocate AVPacket for buffer #%u", buffer_id);
            delete mat_ptr;
            delete buffer;
            return 0;
        }
        buffer->setAVPacket(packet_ptr);

        {
            std::lock_guard<std::mutex> lock(mat_ownership_mutex_);
            mat_buffer_ownership_[buffer] = this;
        }

        if (!IBufferPoolBuilder::addBufferToPoolQueue(pool.get(), buffer, QueueType::FREE)) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to add Buffer #%u to FREE queue", buffer_id);
            delete buffer;
            delete mat_ptr;
            return 0;
        }

        LOG_TRACE_FMT("  Buffer #%u wraps Mat* %p", buffer_id, mat_ptr);
    }

    LOG4CPLUS_INFO_FMT(logger_, "BufferPool '%s' ready with %d Mat buffers", name.c_str(), count);

    uint64_t pool_id = ComponentTopology::getInstance().registerPool(pool, getAllocatorId());
    pool->setRegistryId(pool_id);
    return pool_id;
}

Buffer* MatPoolBuilder::injectBufferToPool(
    uint64_t pool_id, size_t size, QueueType queue
) {
    LOG4CPLUS_WARN(logger_, "injectBufferToPool: use injectMatToPool() instead");
    return nullptr;
}

Buffer* MatPoolBuilder::injectExternalBufferToPool(
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
        std::lock_guard<std::mutex> lock(mat_ownership_mutex_);
        mat_buffer_ownership_[buffer] = this;
    }

    LOG_TRACE_FMT("External buffer #%u injected (virt=%p, phys=0x%lx, size=%zu)",
           id, virt_addr, phys_addr, size);
    return buffer;
}

bool MatPoolBuilder::removeBufferFromPool(uint64_t pool_id, Buffer* buffer) {
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
        std::lock_guard<std::mutex> lock(mat_ownership_mutex_);
        mat_buffer_ownership_.erase(buffer);
    }

    LOG4CPLUS_DEBUG_FMT(logger_, "Buffer #%u removed from pool '%s'",
           buffer->id(), pool->getName().c_str());
    return true;
}

bool MatPoolBuilder::destroyPool() {
    auto pool_ids = getPoolsByAllocator();

    if (pool_ids.empty()) {
        LOG4CPLUS_DEBUG(logger_, "No pools to destroy");
        return true;
    }

    LOG4CPLUS_DEBUG_FMT(logger_, "Destroying %zu pool(s)...", pool_ids.size());

    std::lock_guard<std::mutex> lock(mat_ownership_mutex_);

    for (uint64_t pool_id : pool_ids) {
        auto pool = getPoolSpecialForAllocator(pool_id);
        if (!pool) {
            LOG4CPLUS_WARN_FMT(logger_, "pool_id %lu not found", pool_id);
            continue;
        }

        LOG4CPLUS_DEBUG_FMT(logger_, "Destroying pool '%s' (ID: %lu)...", pool->getName().c_str(), pool_id);

        std::vector<Buffer*> to_remove;
        for (Buffer* buf : pool->getAllManagedBuffers()) {
            auto it = mat_buffer_ownership_.find(buf);
            if (it != mat_buffer_ownership_.end() && it->second == this) {
                to_remove.push_back(buf);
            }
        }

        for (Buffer* buf : to_remove) {
            IBufferPoolBuilder::removeBufferFromPoolInternal(pool.get(), buf);
            deallocateBuffer(buf);
            mat_buffer_ownership_.erase(buf);
        }

        LOG4CPLUS_DEBUG_FMT(logger_, "Pool '%s' destroyed: removed %zu buffers",
               pool->getName().c_str(), to_remove.size());

        unregisterPool(pool_id);
    }

    LOG4CPLUS_DEBUG_FMT(logger_, "All %zu pool(s) destroyed", pool_ids.size());
    return true;
}
