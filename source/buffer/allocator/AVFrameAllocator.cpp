#include "buffer/allocator/AVFrameAllocator.hpp"
#include <stdio.h>

// ============================================================
// 构造/析构函数
// ============================================================

AVFrameAllocator::AVFrameAllocator()
    : next_buffer_id_(0)
{
    printf("🔧 AVFrameAllocator created\n");
}

AVFrameAllocator::~AVFrameAllocator() {
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    
    // 释放所有未释放的 AVFrame
    for (auto& [buffer, frame] : buffer_to_frame_) {
        if (frame) {
            av_frame_free(&frame);
            printf("   🗑️ Released AVFrame for Buffer #%u\n", buffer->id());
        }
    }
    
    buffer_to_frame_.clear();
    printf("🧹 AVFrameAllocator destroyed\n");
}

// ============================================================
// 公开接口实现
// ============================================================

Buffer* AVFrameAllocator::injectAVFrameToPool(AVFrame* frame, BufferPool* pool) {
    if (!frame || !pool) {
        printf("❌ AVFrameAllocator::injectAVFrameToPool: invalid parameters\n");
        return nullptr;
    }
    
    // 1. 生成唯一 Buffer ID
    uint32_t id = next_buffer_id_.fetch_add(1);
    
    // 2. 从 AVFrame 提取信息
    void* virt_addr = frame->data[0];
    size_t size = frame->linesize[0] * frame->height;  // 简化计算（实际应根据格式）
    
    if (!virt_addr || size == 0) {
        printf("❌ Invalid AVFrame: data=%p, size=%zu\n", virt_addr, size);
        return nullptr;
    }
    
    // 3. 创建 Buffer 对象（Ownership::EXTERNAL）
    Buffer* buffer = new Buffer(
        id,
        virt_addr,
        0,  // AVFrame 不提供物理地址
        size,
        Buffer::Ownership::EXTERNAL
    );
    
    if (!buffer) {
        printf("❌ Failed to create Buffer object #%u\n", id);
        return nullptr;
    }
    
    // 4. 将 Buffer 添加到 pool 的 filled 队列
    if (!addBufferToPoolQueue(pool, buffer, QueueType::FILLED)) {
        printf("❌ Failed to add buffer #%u to pool '%s'\n", 
               id, pool->getName().c_str());
        delete buffer;
        return nullptr;
    }
    
    // 5. 记录 AVFrame 和 Buffer 的映射
    {
        std::lock_guard<std::mutex> lock(mapping_mutex_);
        buffer_to_frame_[buffer] = frame;
    }
    
    // 6. 记录所有权
    registerBufferOwnership(buffer, this);
    
    printf("✅ AVFrame injected to pool '%s' as Buffer #%u (size=%zu)\n",
           pool->getName().c_str(), id, size);
    
    return buffer;
}

bool AVFrameAllocator::releaseAVFrame(Buffer* buffer, BufferPool* pool) {
    if (!buffer || !pool) {
        printf("❌ AVFrameAllocator::releaseAVFrame: invalid parameters\n");
        return false;
    }
    
    AVFrame* frame = nullptr;
    
    // 1. 查找 Buffer 对应的 AVFrame
    {
        std::lock_guard<std::mutex> lock(mapping_mutex_);
        auto it = buffer_to_frame_.find(buffer);
        if (it != buffer_to_frame_.end()) {
            frame = it->second;
            buffer_to_frame_.erase(it);
        }
    }
    
    // 2. 释放 AVFrame
    if (frame) {
        av_frame_free(&frame);
        printf("   🗑️ Released AVFrame for Buffer #%u\n", buffer->id());
    } else {
        printf("⚠️  No AVFrame found for Buffer #%u\n", buffer->id());
    }
    
    // 3. 从 pool 移除 Buffer
    if (!removeBufferFromPoolInternal(pool, buffer)) {
        printf("⚠️  Failed to remove buffer #%u from pool '%s'\n",
               buffer->id(), pool->getName().c_str());
        // 继续删除 buffer 对象
    }
    
    // 4. 删除 Buffer 对象
    delete buffer;
    
    // 5. 清除所有权记录
    unregisterBufferOwnership(buffer);
    
    printf("✅ Buffer #%u and AVFrame released\n", buffer->id());
    
    return true;
}

// ============================================================
// 核心实现（不应该被直接调用）
// ============================================================

Buffer* AVFrameAllocator::createBuffer(uint32_t id, size_t size) {
    printf("⚠️  AVFrameAllocator::createBuffer should not be called directly\n");
    printf("   Use injectAVFrameToPool() instead\n");
    return nullptr;
}

void AVFrameAllocator::deallocateBuffer(Buffer* buffer) {
    if (!buffer) {
        return;
    }
    
    AVFrame* frame = nullptr;
    
    // 1. 查找 Buffer 对应的 AVFrame
    {
        std::lock_guard<std::mutex> lock(mapping_mutex_);
        auto it = buffer_to_frame_.find(buffer);
        if (it != buffer_to_frame_.end()) {
            frame = it->second;
            buffer_to_frame_.erase(it);
        }
    }
    
    // 2. 释放 AVFrame
    if (frame) {
        av_frame_free(&frame);
        printf("   🗑️ Released AVFrame for Buffer #%u\n", buffer->id());
    }
    
    // 3. 删除 Buffer 对象
    delete buffer;
}

