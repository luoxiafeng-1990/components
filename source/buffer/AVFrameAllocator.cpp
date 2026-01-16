#include "buffer/AVFrameAllocator.hpp"
#include "common/Logger.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include <stdio.h>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>

// ============================================================
// 硬件资源限制常量（基于大厂设计经验）
// ============================================================

/**
 * 硬件解码缓冲区最大数量限制
 *
 * 大厂设计经验总结：
 * - GPU显存限制：高分辨率视频(4K/8K)单个buffer可能需要数百MB
 * - 并发处理：现代GPU可并发处理8-32路解码流
 * - 内存压力：避免过度占用系统内存影响整体性能
 * - 缓冲区轮转：需要足够的buffer进行解码流水线操作
 *
 * 取值32的依据：
 * - 满足4K@60fps的解码流水线需求(至少3-4个buffer用于重排序)
 * - 支持多路并发解码(8-32路同时处理)
 * - 控制显存占用在合理范围内(32*100MB=3.2GB，对于16GB显存是安全的)
 * - 参考FFmpeg/libavcodec、NVIDIA Video Codec SDK等主流实现
 * - 提供更大的buffer池容量以应对复杂的视频处理场景
 */
static constexpr int MAX_HARDWARE_BUFFERS = 32;

// ============================================================
// 所有权跟踪（静态全局变量，所有 AVFrameAllocator 实例共享）
// ============================================================
static std::unordered_map<Buffer*, BufferAllocatorBase*> avframe_buffer_ownership_;
static std::mutex avframe_ownership_mutex_;

// ============================================================
// 构造/析构函数
// ============================================================

AVFrameAllocator::AVFrameAllocator()
    : next_buffer_id_(0)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.AVFrame")))
{
    LOG4CPLUS_DEBUG(logger_, "创建完成");
}

AVFrameAllocator::~AVFrameAllocator() {
    // v2.0: 子类析构函数中显式清理所有 Pool
    // 只有 AVFrameAllocator 自己知道如何释放 AVFrame
    // destroyPool() 会自动查询 Registry 获取所有 Pool 并清理
    destroyPool();
    
    // ⭐ v2.7移除：不再需要清理 buffer_to_frame_ 映射表
    // AVFrame* 的释放已在 deallocateBuffer() 中通过 buffer->getAVFrame() 处理
    
    LOG4CPLUS_DEBUG(logger_, "AVFrameAllocator destroyed");
}

// ============================================================
// 公开接口实现
// ============================================================

Buffer* AVFrameAllocator::injectAVFrameToPool(AVFrame* frame, BufferPool* pool) {
    if (!frame || !pool) {
        LOG4CPLUS_ERROR(logger_, "AVFrameAllocator::injectAVFrameToPool: invalid parameters");
        return nullptr;
    }
    
    // 1. 生成唯一 Buffer ID
    uint32_t id = next_buffer_id_.fetch_add(1);
    
    // 2. 从 AVFrame 提取信息
    void* virt_addr = frame->data[0];  // ⭐ v2.7：明确存储实际数据地址
    size_t size = frame->linesize[0] * frame->height;  // 简化计算（实际应根据格式）
    
    if (!virt_addr || size == 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "Invalid AVFrame: data=%p, size=%zu", virt_addr, size);
        return nullptr;
    }
    
    // 3. 创建 Buffer 对象（Ownership::EXTERNAL）
    Buffer* buffer = new Buffer(
        id,
        virt_addr,  // ⭐ v2.7：virt_addr 存储 frame->data[0]
        0,  // AVFrame 不提供物理地址
        size,
        Buffer::Ownership::EXTERNAL
    );
    
    if (!buffer) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to create Buffer object #%u", id);
        return nullptr;
    }
    
    // 3.5 ⭐ v2.7新增：设置 Buffer 关联的 AVFrame 指针
    buffer->setAVFrame(frame);
    
    // 3.5.1 ⭐ v2.8新增：为 Buffer 分配关联的 AVPacket
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to allocate AVPacket for Buffer #%u", id);
        delete buffer;
        return nullptr;
    }
    buffer->setAVPacket(packet);
    LOG_TRACE_FMT("  AVPacket allocated at %p for Buffer #%u", packet, id);
    
    // 4. 将 Buffer 添加到 pool 的 filled 队列（使用基类静态方法）
    if (!BufferAllocatorBase::addBufferToPoolQueue(pool, buffer, QueueType::FILLED)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to add buffer #%u to pool '%s'", 
               id, pool->getName().c_str());
        delete buffer;
        return nullptr;
    }
    
    // 5. ⭐ v2.7移除：不再需要记录 buffer_to_frame_ 映射，Buffer 自己持有 AVFrame*
    
    // 6. ⭐ 关键修复：注册 Buffer 所有权（用于 destroyPool 时识别）
    {
        std::lock_guard<std::mutex> lock(avframe_ownership_mutex_);
        avframe_buffer_ownership_[buffer] = this;
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "AVFrame injected to pool '%s' as Buffer #%u (size=%zu)",
           pool->getName().c_str(), id, size);
    
    return buffer;
}

bool AVFrameAllocator::releaseAVFrame(Buffer* buffer, BufferPool* pool) {
    if (!buffer || !pool) {
        LOG4CPLUS_ERROR(logger_, "AVFrameAllocator::releaseAVFrame: invalid parameters");
        return false;
    }
    
    // 1. ⭐ v2.7改进：直接从 Buffer 获取 AVFrame 指针
    AVFrame* frame = buffer->getAVFrame();
    
    // 2. 释放 AVFrame
    if (frame) {
        av_frame_free(&frame);
        buffer->setAVFrame(nullptr);  // 清空 Buffer 的 AVFrame 引用
    } else {
        LOG4CPLUS_WARN_FMT(logger_, " No AVFrame found for Buffer #%u", buffer->id());
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
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Buffer #%u and AVFrame released", buffer->id());
    
    return true;
}

// ============================================================
// 核心实现（不应该被直接调用）
// ============================================================

Buffer* AVFrameAllocator::createBuffer(uint32_t id, size_t size) {
    LOG4CPLUS_WARN(logger_, " AVFrameAllocator::createBuffer should not be called directly");
    LOG4CPLUS_WARN(logger_, " Use injectAVFrameToPool() instead");
    return nullptr;
}

void AVFrameAllocator::deallocateBuffer(Buffer* buffer) {
    if (!buffer) {
        return;
    }
    
    // 1. ⭐ v2.7改进：直接从 Buffer 获取 AVFrame 指针
    AVFrame* frame = buffer->getAVFrame();
    
    // 2. 释放 AVFrame
    if (frame) {
        av_frame_free(&frame);
        buffer->setAVFrame(nullptr);  // 清空 Buffer 的 AVFrame 引用
    }
    
    // 3. ⭐ v2.8新增：释放 AVPacket
    AVPacket* packet = buffer->getAVPacket();
    if (packet) {
        av_packet_free(&packet);
        buffer->setAVPacket(nullptr);  // 清空 Buffer 的 AVPacket 引用
    }
    
    // 4. 删除 Buffer 对象
    delete buffer;
}

// ============================================================
// 实现基类纯虚函数
// ============================================================

uint64_t AVFrameAllocator::allocatePoolWithBuffers(
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
    if (count > MAX_HARDWARE_BUFFERS || count <= 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "Warning: count %d exceeds MAX_HARDWARE_BUFFERS %d", count, MAX_HARDWARE_BUFFERS);
        count = MAX_HARDWARE_BUFFERS;
        LOG4CPLUS_WARN_FMT(logger_, "Using MAX_HARDWARE_BUFFERS %d instead of %d", MAX_HARDWARE_BUFFERS, count);
    }
    // 4. 🎯 核心逻辑：提前分配 count 个 AVFrame* "壳子"，包装成 Buffer
    
    for (int i = 0; i < count; i++) {
        // 4.1 分配 AVFrame* "壳子"（只是 AVFrame 结构体，内部 data/buf 都是空的）
        AVFrame* frame_ptr = av_frame_alloc();
        if (!frame_ptr) {
            LOG4CPLUS_ERROR_FMT(logger_, "ERROR: Failed to allocate AVFrame[%d]", i);
            // TODO: 清理已分配的 frames 和 buffers
            return 0;
        }
        
        LOG_TRACE_FMT("  AVFrame[%d] allocated at %p", i, frame_ptr);
        
        // 4.2 生成唯一 Buffer ID
        uint32_t buffer_id = next_buffer_id_.fetch_add(1);
        
        // 4.3 🎯 关键：将 AVFrame* 包装成 Buffer 对象
        //     ⭐ v2.7语义修正：
        //     - virt_addr: 初始为 nullptr（解码后更新为 frame->data[0]）
        //     - phys_addr: 初始化为 0（延迟获取）
        //     - size: Worker 期望的 buffer 大小
        //     - ownership: EXTERNAL（物理内存由 h264_taco 管理）
        Buffer* buffer = new Buffer(
            buffer_id,
            nullptr,           // ⭐ v2.7：virt_addr 初始为 nullptr，解码后更新
            0,                 // phys_addr 初始为 0，在 avcodec_receive_frame 后提取
            size,
            Buffer::Ownership::EXTERNAL
        );
        
        if (!buffer) {
            LOG4CPLUS_ERROR_FMT(logger_, "ERROR: Failed to create Buffer #%u for AVFrame[%d]", buffer_id, i);
            av_frame_free(&frame_ptr);
            return 0;
        }
        
        // 4.4 ⭐ v2.7新增：设置 Buffer 关联的 AVFrame 指针
        buffer->setAVFrame(frame_ptr);
        
        // 4.4.1 ⭐ v2.8新增：为 Buffer 分配关联的 AVPacket
        AVPacket* packet_ptr = av_packet_alloc();
        if (!packet_ptr) {
            LOG4CPLUS_ERROR_FMT(logger_, "ERROR: Failed to allocate AVPacket for buffer #%u", buffer_id);
            av_frame_free(&frame_ptr);
            delete buffer;
            // TODO: 清理已分配的 buffers
            return 0;
        }
        buffer->setAVPacket(packet_ptr);
        LOG_TRACE_FMT("  AVPacket allocated at %p for Buffer #%u", packet_ptr, buffer_id);
        
        // 4.4.2 ⭐ 关键修复：注册 Buffer 所有权（用于 destroyPool 时识别）
        {
            std::lock_guard<std::mutex> lock(avframe_ownership_mutex_);
            avframe_buffer_ownership_[buffer] = this;
        }
        
        // 4.5 🎯 关键：将 Buffer 添加到 BufferPool 的 FREE 队列
        if (!BufferAllocatorBase::addBufferToPoolQueue(pool.get(), buffer, QueueType::FREE)) {
            LOG4CPLUS_ERROR_FMT(logger_, "ERROR: Failed to add Buffer #%u to FREE queue", buffer_id);
            delete buffer;
            av_frame_free(&frame_ptr);
            return 0;
        }
        
        LOG_TRACE_FMT("  Buffer #%u wraps AVFrame* %p", buffer_id, frame_ptr);
    }
    LOG4CPLUS_INFO(logger_, "╔══════════════════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO(logger_, "║  ✅ BufferPool Ready                                         ║");
    LOG4CPLUS_INFO(logger_, "╚══════════════════════════════════════════════════════════════════╝");
    LOG4CPLUS_INFO_FMT(logger_, "   Pool name: %s", pool->getName().c_str());
    LOG4CPLUS_INFO_FMT(logger_, "   Buffers in FREE queue: %d", count);
    LOG4CPLUS_INFO(logger_, "   Each Buffer wraps: AVFrame* shell (physical memory not yet allocated)");
    LOG4CPLUS_INFO(logger_, "╚══════════════════════════════════════════════════════════════════╝");
    
    // v2.0 步骤 3: 注册到 Registry（转移所有权，传入 Allocator ID）
    uint64_t pool_id = BufferPoolRegistry::getInstance().registerPool(pool, getAllocatorId());
    pool->setRegistryId(pool_id);
    
    // v2.0 步骤 4: 返回 pool_id
    return pool_id;
}

Buffer* AVFrameAllocator::injectBufferToPool(
    uint64_t pool_id,
    size_t size,
    QueueType queue
) {
    LOG4CPLUS_WARN(logger_, " [AVFrameAllocator] injectBufferToPool: This method is not supported");
    LOG4CPLUS_WARN(logger_, " Use injectAVFrameToPool() or injectExternalBufferToPool() instead");
    return nullptr;
}

Buffer* AVFrameAllocator::injectExternalBufferToPool(
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
        std::lock_guard<std::mutex> lock(avframe_ownership_mutex_);
        avframe_buffer_ownership_[buffer] = this;
    }
    
    // 仅在TRACE级别输出详细信息
    LOG_TRACE_FMT("External buffer #%u injected (virt=%p, phys=0x%lx, size=%zu)",
           id, virt_addr, phys_addr, size);
    
    return buffer;
}

bool AVFrameAllocator::removeBufferFromPool(uint64_t pool_id, Buffer* buffer) {
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
    
    // 2. 销毁 Buffer（会释放关联的 AVFrame）
    deallocateBuffer(buffer);
    
    // 3. 清除所有权记录
    {
        std::lock_guard<std::mutex> lock(avframe_ownership_mutex_);
        avframe_buffer_ownership_.erase(buffer);
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Buffer #%u removed from pool '%s'",
           buffer->id(), pool->getName().c_str());
    
    return true;
}

bool AVFrameAllocator::destroyPool() {
    // 1. 获取所有属于此 allocator 的 pool
    auto pool_ids = getPoolsByAllocator();
    
    if (pool_ids.empty()) {
        LOG4CPLUS_DEBUG(logger_, "No pools to destroy");
        return true;
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Destroying %zu pool(s)...", pool_ids.size());
    
    std::lock_guard<std::mutex> lock(avframe_ownership_mutex_);
    
    // 2. 遍历每个 pool
    for (uint64_t pool_id : pool_ids) {
        // 2.1 获取 pool
        auto pool = getPoolSpecialForAllocator(pool_id);
        if (!pool) {
            LOG4CPLUS_WARN_FMT(logger_, " [AVFrameAllocator] pool_id %lu not found (already destroyed?)", pool_id);
            continue;
        }
        
        LOG4CPLUS_DEBUG_FMT(logger_, "Destroying pool '%s' (ID: %lu)...", pool->getName().c_str(), pool_id);
        
        // 2.2 通过 BufferPool 的公共方法获取所有属于此 pool 的 buffer
        std::vector<Buffer*> to_remove;
        for (Buffer* buf : pool->getAllManagedBuffers()) {
            // 检查 buffer 是否属于此 allocator
            auto it = avframe_buffer_ownership_.find(buf);
            if (it != avframe_buffer_ownership_.end() && it->second == this) {
                to_remove.push_back(buf);
            }
        }
        
        // 2.3 移除并销毁所有 Buffer（同时释放 AVFrame）
        for (Buffer* buf : to_remove) {
            BufferAllocatorBase::removeBufferFromPoolInternal(pool.get(), buf);
            deallocateBuffer(buf);  // ⭐ v2.7：内部会通过 buffer->getAVFrame() 释放 AVFrame
            avframe_buffer_ownership_.erase(buf);
            
            // ⭐ v2.7移除：不再需要从 buffer_to_frame_ 中移除
        }
        
        LOG4CPLUS_DEBUG_FMT(logger_, "Pool '%s' destroyed: removed %zu buffers", 
               pool->getName().c_str(), to_remove.size());
        
        // 2.4 从 Registry 注销（触发 Pool 析构）
        unregisterPool(pool_id);
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "All %zu pool(s) destroyed", pool_ids.size());
    return true;
}

