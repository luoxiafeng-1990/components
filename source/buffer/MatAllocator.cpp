#include "buffer/MatAllocator.hpp"
#include "common/Logger.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include <stdio.h>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>

static std::unordered_map<Buffer*, BufferAllocatorBase*> mat_buffer_ownership_;
static std::mutex mat_ownership_mutex_;

MatAllocator::MatAllocator()
    : next_buffer_id_(0)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Mat")))
{
    LOG4CPLUS_DEBUG(logger_, "创建完成");
}

MatAllocator::~MatAllocator() {
    destroyPool();
    
    LOG4CPLUS_DEBUG(logger_, "MatAllocator destroyed");
}

Buffer* MatAllocator::injectMatToPool(cv::Mat* mat, BufferPool* pool) {
    if (!mat || !pool) {
        LOG4CPLUS_ERROR(logger_, "MatAllocator::injectMatToPool: invalid parameters");
        return nullptr;
    }
    
    // 1. 生成唯一 Buffer ID
    uint32_t id = next_buffer_id_.fetch_add(1);
    
    // 2. 从 Mat 提取信息
    void* virt_addr = mat->data;
    size_t size = mat->total() * mat->elemSize();
    
    if (!virt_addr || size == 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "Invalid Mat: data=%p, size=%zu", virt_addr, size);
        return nullptr;
    }
    
    // 3. 创建 Buffer 对象（Ownership::EXTERNAL）
    Buffer* buffer = new Buffer(
        id,
        virt_addr,
        0,  // Mat 不提供物理地址
        size,
        Buffer::Ownership::EXTERNAL
    );
    
    if (!buffer) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to create Buffer object #%u", id);
        return nullptr;
    }
    
    // 这里是浅拷贝，mat和mat_两个指针指向同一内存地址
    buffer->setMat(mat);
    
    // 4. 将 Buffer 添加到 pool 的 filled 队列（使用基类静态方法）
    if (!BufferAllocatorBase::addBufferToPoolQueue(pool, buffer, QueueType::FILLED)) {
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

bool MatAllocator::releaseMat(Buffer* buffer, BufferPool* pool) {
    if (!buffer || !pool) {
        LOG4CPLUS_ERROR(logger_, "MatAllocator::releaseMat: invalid parameters");
        return false;
    }
    
    // 1. ⭐ v2.7改进：直接从 Buffer 获取 Mat 指针
    cv::Mat* mat = buffer->getMat();
    
    // 2. 释放 Mat
    if (mat) {
        delete mat;
        buffer->setMat(nullptr);  // 清空 Buffer 的 Mat 引用
    } else {
        LOG4CPLUS_WARN_FMT(logger_, " No Mat found for Buffer #%u", buffer->id());
    }
    
    // 3. 从 pool 移除 Buffer（使用基类静态方法）
    if (!BufferAllocatorBase::removeBufferFromPoolInternal(pool, buffer)) {
        LOG4CPLUS_WARN_FMT(logger_, " Failed to remove buffer #%u from pool '%s'",
               buffer->id(), pool->getName().c_str());
        // 继续删除 buffer 对象
    }
    
    // 4. 删除 Buffer 对象
    delete buffer;
    
    // 5. 清除所有权记录（使用静态所有权跟踪）
    {
        static std::unordered_map<Buffer*, BufferAllocatorBase*> buffer_ownership_;
        static std::mutex ownership_mutex_;
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_.erase(buffer);
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Buffer #%u and Mat released", buffer->id());
    
    return true;
}

Buffer* MatAllocator::createBuffer(uint32_t id, size_t size) {
    LOG4CPLUS_WARN(logger_, " MatAllocator::createBuffer should not be called directly");
    LOG4CPLUS_WARN(logger_, " Use injectMatToPool() instead");
    return nullptr;
}

void MatAllocator::deallocateBuffer(Buffer* buffer) {
    if (!buffer) {
        return;
    }

    // 1. ⭐ 先释放 Mat（必须在 AVFrame free 之前）
    // cv::Mat(AVFrame*) 零拷贝：Mat 和 AVFrame 共享同一块 GPU 内存。
    // 若先 av_frame_free，GPU 内存立即无效，再 delete mat 时 Mat 析构器崩溃。
    cv::Mat* mat = buffer->getMat();
    if (mat) {
        delete mat;
        buffer->setMat(nullptr);
    }

    // 2. 再释放 AVFrame
    AVFrame* avframe = buffer->getAVFrame();
    if (avframe) {
        av_frame_free(&avframe);
        buffer->setAVFrame(nullptr);
    }

    AVPacket* packet_ptr = buffer->getAVPacket();
    if (packet_ptr){
        delete packet_ptr;
        buffer->setAVPacket(nullptr);
    }
    
    // 4. 删除 Buffer 对象
    delete buffer;
}

uint64_t MatAllocator::allocatePoolWithBuffers(
    int count,
    size_t size,
    const std::string& name,
    const std::string& category
) {
    LOG4CPLUS_DEBUG_FMT(logger_, "allocatePoolWithBuffers: name='%s', category='%s', count=%d, size=%zu", 
           name.c_str(), category.c_str(), count, size);
    
    // v2.0 步骤 1: 使用 Passkey Token 创建 BufferPool（shared_ptr）
    auto pool = std::make_shared<BufferPool>(
        token(),
        name,
        category
    );
    
    for (int i = 0; i < count; i++) {
        // 4.1 分配 Mat* "壳子"（只是 Mat 结构体，内部 data/buf 都是空的）
        cv::Mat* mat_ptr = new cv::Mat();
        if (!mat_ptr) {
            LOG4CPLUS_ERROR_FMT(logger_, "ERROR: Failed to allocate Mat[%d]", i);
            // TODO: 清理已分配的 mats 和 buffers
            return 0;
        }
        
        LOG_TRACE_FMT("  Mat[%d] allocated at %p", i, mat_ptr);
        
        // 4.2 生成唯一 Buffer ID
        uint32_t buffer_id = next_buffer_id_.fetch_add(1);
        
        Buffer* buffer = new Buffer(
            buffer_id,
            nullptr,           // ⭐ v2.7：virt_addr 初始为 nullptr，解码后更新
            0,                 // phys_addr 初始为 0，在 avcodec_receive_mat 后提取
            size,
            Buffer::Ownership::EXTERNAL
        );
        
        if (!buffer) {
            LOG4CPLUS_ERROR_FMT(logger_, "ERROR: Failed to create Buffer #%u for Mat[%d]", buffer_id, i);
            delete mat_ptr;
            return 0;
        }
        
        // 4.4 ⭐ v2.7新增：设置 Buffer 关联的 Mat 指针
        buffer->setMat(mat_ptr);

        AVPacket* packet_ptr = av_packet_alloc();
        if (!packet_ptr) {
            LOG4CPLUS_ERROR_FMT(logger_, "ERROR: Failed to allocate AVPacket for buffer #%u", buffer_id);
            delete mat_ptr;
            delete buffer;
            return 0;
        }
        buffer->setAVPacket(packet_ptr);
        
        // 4.4.2 ⭐ 关键修复：注册 Buffer 所有权（用于 destroyPool 时识别）
        {
            std::lock_guard<std::mutex> lock(mat_ownership_mutex_);
            mat_buffer_ownership_[buffer] = this;
        }
        
        // 4.5 🎯 关键：将 Buffer 添加到 BufferPool 的 FREE 队列
        if (!BufferAllocatorBase::addBufferToPoolQueue(pool.get(), buffer, QueueType::FREE)) {
            LOG4CPLUS_ERROR_FMT(logger_, "ERROR: Failed to add Buffer #%u to FREE queue", buffer_id);
            delete buffer;
            delete mat_ptr;
            return 0;
        }
        
        LOG_TRACE_FMT("  Buffer #%u wraps Mat* %p", buffer_id, mat_ptr);
    }
    LOG4CPLUS_INFO(logger_, "╔══════════════════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO(logger_, "║  ✅ BufferPool Ready                                         ║");
    LOG4CPLUS_INFO(logger_, "╚══════════════════════════════════════════════════════════════════╝");
    LOG4CPLUS_INFO_FMT(logger_, "   Pool name: %s", pool->getName().c_str());
    LOG4CPLUS_INFO_FMT(logger_, "   Buffers in FREE queue: %d", count);
    LOG4CPLUS_INFO(logger_, "   Each Buffer wraps: Mat* shell (physical memory not yet allocated)");
    LOG4CPLUS_INFO(logger_, "╚══════════════════════════════════════════════════════════════════╝");
    
    // v2.0 步骤 3: 注册到 Registry（转移所有权，传入 Allocator ID）
    uint64_t pool_id = ComponentTopology::getInstance().registerPool(pool, getAllocatorId());
    pool->setRegistryId(pool_id);
    
    // v2.0 步骤 4: 返回 pool_id
    return pool_id;
}

Buffer* MatAllocator::injectBufferToPool(
    uint64_t pool_id,
    size_t size,
    QueueType queue
) {
    LOG4CPLUS_WARN(logger_, " [MatAllocator] injectBufferToPool: This method is not supported");
    LOG4CPLUS_WARN(logger_, " Use injectMatToPool() or injectExternalBufferToPool() instead");
    return nullptr;
}

Buffer* MatAllocator::injectExternalBufferToPool(
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
    auto pool_weak = ComponentTopology::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        LOG4CPLUS_ERROR_FMT(logger_, "pool_id %lu not found or already destroyed", pool_id);
        return nullptr;
    }
    
    // 1. 生成唯一 Buffer ID
    uint32_t id = next_buffer_id_.fetch_add(1);
    
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
    
    // 4. ⭐ 关键修复：注册 Buffer 所有权（用于 destroyPool 时识别）
    {
        std::lock_guard<std::mutex> lock(mat_ownership_mutex_);
        mat_buffer_ownership_[buffer] = this;
    }
    
    // 仅在TRACE级别输出详细信息
    LOG_TRACE_FMT("External buffer #%u injected (virt=%p, phys=0x%lx, size=%zu)",
           id, virt_addr, phys_addr, size);
    
    return buffer;
}

bool MatAllocator::removeBufferFromPool(uint64_t pool_id, Buffer* buffer) {
    if (!buffer) {
        LOG4CPLUS_ERROR(logger_, "removeBufferFromPool: buffer is nullptr");
        return false;
    }
    
    // v2.0: 从 Registry 获取 Pool（返回 weak_ptr）
    auto pool_weak = ComponentTopology::getInstance().getPool(pool_id);
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
    
    // 2. 销毁 Buffer（会释放关联的 Mat）
    deallocateBuffer(buffer);
    
    // 3. 清除所有权记录
    {
        std::lock_guard<std::mutex> lock(mat_ownership_mutex_);
        mat_buffer_ownership_.erase(buffer);
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Buffer #%u removed from pool '%s'",
           buffer->id(), pool->getName().c_str());
    
    return true;
}

bool MatAllocator::destroyPool() {
    // 1. 获取所有属于此 allocator 的 pool
    auto pool_ids = getPoolsByAllocator();
    
    if (pool_ids.empty()) {
        LOG4CPLUS_DEBUG(logger_, "No pools to destroy");
        return true;
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Destroying %zu pool(s)...", pool_ids.size());
    
    std::lock_guard<std::mutex> lock(mat_ownership_mutex_);
    
    // 2. 遍历每个 pool
    for (uint64_t pool_id : pool_ids) {
        // 2.1 获取 pool
        auto pool = getPoolSpecialForAllocator(pool_id);
        if (!pool) {
            LOG4CPLUS_WARN_FMT(logger_, " [MatAllocator] pool_id %lu not found (already destroyed?)", pool_id);
            continue;
        }
        
        LOG4CPLUS_DEBUG_FMT(logger_, "Destroying pool '%s' (ID: %lu)...", pool->getName().c_str(), pool_id);
        
        // 2.2 通过 BufferPool 的公共方法获取所有属于此 pool 的 buffer
        std::vector<Buffer*> to_remove;
        for (Buffer* buf : pool->getAllManagedBuffers()) {
            // 检查 buffer 是否属于此 allocator
            auto it = mat_buffer_ownership_.find(buf);
            if (it != mat_buffer_ownership_.end() && it->second == this) {
                to_remove.push_back(buf);
            }
        }
        
        // 2.3 移除并销毁所有 Buffer（同时释放 Mat）
        for (Buffer* buf : to_remove) {
            BufferAllocatorBase::removeBufferFromPoolInternal(pool.get(), buf);
            deallocateBuffer(buf);  // ⭐ v2.7：内部会通过 buffer->getMat() 释放 Mat
            mat_buffer_ownership_.erase(buf);
            
            // ⭐ v2.7移除：不再需要从 buffer_to_mat_ 中移除
        }
        
        LOG4CPLUS_DEBUG_FMT(logger_, "Pool '%s' destroyed: removed %zu buffers", 
               pool->getName().c_str(), to_remove.size());
        
        // 2.4 从 Registry 注销（触发 Pool 析构）
        unregisterPool(pool_id);
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "All %zu pool(s) destroyed", pool_ids.size());
    return true;
}

