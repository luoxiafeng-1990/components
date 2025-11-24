#include "buffer/allocator/implementation/AVFrameAllocator.hpp"
#include "buffer/BufferPool.hpp"
#include "buffer/BufferPoolRegistry.hpp"
#include <stdio.h>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>

// ============================================================
// 构造/析构函数
// ============================================================

AVFrameAllocator::AVFrameAllocator()
    : next_buffer_id_(0)
{
    printf("🔧 AVFrameAllocator created\n");
    // 不在构造函数中创建 BufferPool
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
    
    // 4. 将 Buffer 添加到 pool 的 filled 队列（使用基类静态方法）
    if (!BufferAllocatorBase::addBufferToPoolQueue(pool, buffer, QueueType::FILLED)) {
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
    
    // 6. 记录所有权（使用静态所有权跟踪）
    {
        static std::unordered_map<Buffer*, BufferAllocatorBase*> buffer_ownership_;
        static std::mutex ownership_mutex_;
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_[buffer] = this;
    }
    
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
    
    // 3. 从 pool 移除 Buffer（使用基类静态方法）
    if (!BufferAllocatorBase::removeBufferFromPoolInternal(pool, buffer)) {
        printf("⚠️  Failed to remove buffer #%u from pool '%s'\n",
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

// ============================================================
// 实现基类纯虚函数
// ============================================================

// 所有权跟踪（静态成员，所有Allocator共享）
static std::unordered_map<Buffer*, BufferAllocatorBase*> avframe_buffer_ownership_;
static std::mutex avframe_ownership_mutex_;

std::shared_ptr<BufferPool> AVFrameAllocator::allocatePoolWithBuffers(
    int count,
    size_t size,
    const std::string& name,
    const std::string& category
) {
    printf("\n🏭 AVFrameAllocator: Creating BufferPool (empty, for AVFrame injection)...\n");
    
    // 1. 检查是否已经创建过 pool
    {
        std::lock_guard<std::mutex> lock(managed_pool_mutex_);
        if (managed_pool_) {
            printf("⚠️  Warning: BufferPool already exists, returning existing pool\n");
            return managed_pool_;
        }
    }
    
    // 2. 使用 Passkey Token 创建 BufferPool
    auto pool = std::make_shared<BufferPool>(
        token(),    // 从基类获取通行证
        name,
        category
    );
    
    // 3. 注册到 BufferPoolRegistry（name 和 category 从 pool 对象自动获取）
    uint64_t id = BufferPoolRegistry::getInstance().registerPool(pool);
    pool->setRegistryId(id);
    
    printf("   ℹ️  Created empty pool '%s' (ID: %lu)\n", pool->getName().c_str(), id);
    
    // 4. 存储到 managed_pool_
    {
        std::lock_guard<std::mutex> lock(managed_pool_mutex_);
        managed_pool_ = pool;
    }
    
    printf("✅ BufferPool '%s' ready for AVFrame injection\n", pool->getName().c_str());
    
    return pool;
}

Buffer* AVFrameAllocator::injectBufferToPool(
    size_t size,
    BufferPool* pool,
    QueueType queue
) {
    printf("⚠️  AVFrameAllocator::injectBufferToPool: This method is not supported\n");
    printf("   Use injectAVFrameToPool() or injectExternalBufferToPool() instead\n");
    return nullptr;
}

Buffer* AVFrameAllocator::injectExternalBufferToPool(
    void* virt_addr,
    uint64_t phys_addr,
    size_t size,
    BufferPool* pool,
    QueueType queue
) {
    if (!pool || !virt_addr || size == 0) {
        printf("❌ AVFrameAllocator::injectExternalBufferToPool: invalid parameters\n");
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
        printf("❌ Failed to create Buffer object #%u for external memory\n", id);
        return nullptr;
    }
    
    // 3. 通过基类静态方法添加到 pool 的指定队列（会自动添加到 managed_buffers_）
    if (!BufferAllocatorBase::addBufferToPoolQueue(pool, buffer, queue)) {
        printf("❌ Failed to add external buffer #%u to pool '%s'\n", 
               id, pool->getName().c_str());
        delete buffer;  // 只删除 Buffer 对象，不释放外部内存
        return nullptr;
    }
    
    // 4. 记录所有权（外部内存由外部管理，但 Buffer 对象由 Allocator 管理）
    {
        static std::unordered_map<Buffer*, BufferAllocatorBase*> buffer_ownership_;
        static std::mutex ownership_mutex_;
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_[buffer] = this;
    }
    
    printf("✅ External buffer #%u injected to pool '%s' (virt=%p, phys=0x%lx, size=%zu, queue: %s)\n",
           id, pool->getName().c_str(), virt_addr, phys_addr, size,
           queue == QueueType::FREE ? "FREE" : "FILLED");
    
    return buffer;
}

bool AVFrameAllocator::removeBufferFromPool(Buffer* buffer, BufferPool* pool) {
    if (!buffer || !pool) {
        printf("❌ AVFrameAllocator::removeBufferFromPool: invalid parameters\n");
        return false;
    }
    
    // 1. 通过基类静态方法从 pool 移除
    if (!BufferAllocatorBase::removeBufferFromPoolInternal(pool, buffer)) {
        printf("⚠️  Failed to remove buffer #%u from pool '%s' (in use or not in pool)\n",
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
    
    printf("✅ Buffer #%u removed from pool '%s'\n",
           buffer->id(), pool->getName().c_str());
    
    return true;
}

bool AVFrameAllocator::destroyPool(BufferPool* pool) {
    if (!pool) {
        printf("❌ AVFrameAllocator::destroyPool: pool is nullptr\n");
        return false;
    }
    
    printf("🧹 AVFrameAllocator: Destroying pool '%s'...\n", pool->getName().c_str());
    
    // 1. 检查是否是管理的 pool
    {
        std::lock_guard<std::mutex> lock(managed_pool_mutex_);
        if (managed_pool_ && managed_pool_.get() == pool) {
            printf("   ✅ Pool matches managed_pool_\n");
        } else {
            printf("   ⚠️  Warning: Pool does not match managed_pool_\n");
        }
    }
    
    std::lock_guard<std::mutex> lock(avframe_ownership_mutex_);
    
    // 2. 找到所有属于此 allocator 的 buffer
    std::vector<Buffer*> to_remove;
    for (auto& [buf, alloc] : avframe_buffer_ownership_) {
        if (alloc == this) {
            to_remove.push_back(buf);
        }
    }
    
    // 3. 移除并销毁
    for (Buffer* buf : to_remove) {
        BufferAllocatorBase::removeBufferFromPoolInternal(pool, buf);
        deallocateBuffer(buf);
        avframe_buffer_ownership_.erase(buf);
    }
    
    printf("✅ Pool '%s' destroyed: removed %zu buffers\n", pool->getName().c_str(), to_remove.size());
    
    return true;
}

