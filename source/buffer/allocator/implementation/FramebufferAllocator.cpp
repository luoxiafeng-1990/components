#include "buffer/allocator/implementation/FramebufferAllocator.hpp"
#include "buffer/BufferPool.hpp"
#include "buffer/BufferPoolRegistry.hpp"
#include "display/LinuxFramebufferDevice.hpp"
#include <stdio.h>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>

// ============================================================
// 构造/析构函数
// ============================================================

FramebufferAllocator::FramebufferAllocator()
    : external_buffers_()
    , next_buffer_index_(0)
    , managed_pool_sptr_(nullptr)  // 显式初始化为空（延迟初始化模式）
{
   printf("🔧 FramebufferAllocator created (BufferPool will be lazy-initialized)\n");
}

FramebufferAllocator::FramebufferAllocator(const std::vector<BufferInfo>& external_buffers)
    : external_buffers_(external_buffers)
    , next_buffer_index_(0)
    , managed_pool_sptr_(nullptr)  // 显式初始化为空（延迟初始化模式）
{
    printf("🔧 FramebufferAllocator created with %zu external buffers (BufferPool will be lazy-initialized)\n", 
           external_buffers_.size());
}

FramebufferAllocator::FramebufferAllocator(LinuxFramebufferDevice* device)
    : next_buffer_index_(0)
    , managed_pool_sptr_(nullptr)  // 显式初始化为空（延迟初始化模式）
{
    if (!device) {
        printf("❌ ERROR: Device pointer is null\n");
        return;
    }
    
    // 调用私有方法构建 BufferInfo 列表
    external_buffers_ = buildBufferInfosFromDevice(device);
    
    printf("🔧 FramebufferAllocator created from device with %zu buffers (BufferPool will be lazy-initialized)\n", 
           external_buffers_.size());
}

FramebufferAllocator::~FramebufferAllocator() {
    printf("🧹 FramebufferAllocator destroyed (external memory not freed)\n");
}

// ============================================================
// 重写批量分配（批量包装）
// ============================================================

std::shared_ptr<BufferPool> FramebufferAllocator::allocatePoolWithBuffers(
    int count,
    size_t size,
    const std::string& name,
    const std::string& category)
{
    // 1. 检查是否已经创建过 pool
    {
        std::lock_guard<std::mutex> lock(managed_pool_mutex_);
        if (managed_pool_sptr_) {
            printf("⚠️  Warning: BufferPool already exists, returning existing pool\n");
            return managed_pool_sptr_;
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
    
    // 4. 批量包装外部 Buffer 并添加到 pool
    for (int i = 0; i < count ; i++) {
        Buffer* buffer = createBuffer(i, 0);  // size 参数被忽略
        if (!buffer) {
            printf("❌ Failed to wrap external buffer #%d\n", i);
            cleanupPool(pool.get());
            return nullptr;
        }
        
        if (!BufferAllocatorBase::addBufferToPoolQueue(pool.get(), buffer, QueueType::FREE)) {
            printf("❌ Failed to add buffer #%d to pool\n", i);
            deallocateBuffer(buffer);
            cleanupPool(pool.get());
            return nullptr;
        }
        
        {
            static std::unordered_map<Buffer*, BufferAllocatorBase*> buffer_ownership_;
            static std::mutex ownership_mutex_;
            std::lock_guard<std::mutex> lock(ownership_mutex_);
            buffer_ownership_[buffer] = this;
        }
        
        printf("   ✅ Buffer #%d wrapped: virt=%p, phys=0x%lx, size=%zu (EXTERNAL)\n",
               i, buffer->getVirtualAddress(), buffer->getPhysicalAddress(), buffer->size());
    }
    
    // 5. 存储到 managed_pool_sptr_（基类成员）
    {
        std::lock_guard<std::mutex> lock(managed_pool_mutex_);
        managed_pool_sptr_ = pool;
    }
    
    printf("✅ BufferPool '%s' created with %d external buffers\n", 
           pool->getName().c_str(), count);
    
    return pool;
}

// ============================================================
// 核心实现
// ============================================================

Buffer* FramebufferAllocator::createBuffer(uint32_t id, size_t size) {
    // 检查 id 是否越界
    if (id >= external_buffers_.size()) {
        printf("❌ Buffer ID %u out of range (max: %zu)\n", 
               id, external_buffers_.size());
        return nullptr;
    }
    
    // 获取外部 buffer 信息
    const BufferInfo& info = external_buffers_[id];
    
    // 创建 Buffer 对象（Ownership::EXTERNAL）
    Buffer* buffer = new Buffer(
        id,
        info.virt_addr,
        info.phys_addr,
        info.size,
        Buffer::Ownership::EXTERNAL
    );
    
    if (!buffer) {
        printf("❌ Failed to create Buffer object #%u\n", id);
        return nullptr;
    }
    
    return buffer;
}

void FramebufferAllocator::deallocateBuffer(Buffer* buffer) {
    if (!buffer) {
        return;
    }
    
    // 1. 不释放内存（外部管理）
    printf("   🗑️ Deleting Buffer #%u (external memory retained)\n", buffer->id());
    
    // 2. 仅删除 Buffer 对象
    delete buffer;
}

// ============================================================
// 私有辅助方法：从设备构建 BufferInfo 列表
// ============================================================

std::vector<FramebufferAllocator::BufferInfo> 
FramebufferAllocator::buildBufferInfosFromDevice(LinuxFramebufferDevice* device)
{
    std::vector<BufferInfo> infos;
    
    if (!device) {
        printf("❌ ERROR: Device pointer is null in buildBufferInfosFromDevice\n");
        return infos;
    }
    
    // 1. 从设备获取 mmap 信息
    auto mapped_info = device->getMappedInfo();
    
    printf("📋 Building BufferInfo list from device:\n");
    printf("   base_addr=%p, buffer_size=%zu, buffer_count=%d\n",
           mapped_info.base_addr, mapped_info.buffer_size, mapped_info.buffer_count);
    
    // 2. 计算每个 buffer 的地址并构建 BufferInfo
    unsigned char* base = (unsigned char*)mapped_info.base_addr;
    infos.reserve(mapped_info.buffer_count);
    
    for (int i = 0; i < mapped_info.buffer_count; i++) {
        infos.push_back({
            .virt_addr = (void*)(base + i * mapped_info.buffer_size),
            .phys_addr = 0,  // 物理地址由系统自动获取
            .size = mapped_info.buffer_size
        });
        
        printf("   Buffer[%d]: virt=%p, size=%zu\n", 
               i, infos.back().virt_addr, infos.back().size);
    }
    
    return infos;
}

// ============================================================
// 实现基类纯虚函数
// ============================================================

// 所有权跟踪（静态成员，所有Allocator共享）
static std::unordered_map<Buffer*, BufferAllocatorBase*> framebuffer_buffer_ownership_;
static std::mutex framebuffer_ownership_mutex_;

Buffer* FramebufferAllocator::injectBufferToPool(
    size_t size,
    BufferPool* pool,
    QueueType queue
) {
    printf("⚠️  FramebufferAllocator::injectBufferToPool: This method is not supported\n");
    printf("   FramebufferAllocator only supports wrapping pre-allocated external memory\n");
    printf("   Use allocatePoolWithBuffers() or injectExternalBufferToPool() instead\n");
    return nullptr;
}

Buffer* FramebufferAllocator::injectExternalBufferToPool(
    void* virt_addr,
    uint64_t phys_addr,
    size_t size,
    BufferPool* pool,
    QueueType queue
) {
    if (!pool || !virt_addr || size == 0) {
        printf("❌ FramebufferAllocator::injectExternalBufferToPool: invalid parameters\n");
        return nullptr;
    }
    
    // 1. 生成 Buffer ID（从 pool 的当前 buffer 数量）
    uint32_t id = pool->getTotalCount();
    
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
        std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
        framebuffer_buffer_ownership_[buffer] = this;
    }
    
    printf("✅ External buffer #%u injected to pool '%s' (virt=%p, phys=0x%lx, size=%zu, queue: %s)\n",
           id, pool->getName().c_str(), virt_addr, phys_addr, size,
           queue == QueueType::FREE ? "FREE" : "FILLED");
    
    return buffer;
}

bool FramebufferAllocator::removeBufferFromPool(Buffer* buffer, BufferPool* pool) {
    if (!buffer || !pool) {
        printf("❌ FramebufferAllocator::removeBufferFromPool: invalid parameters\n");
        return false;
    }
    
    // 1. 通过基类静态方法从 pool 移除
    if (!BufferAllocatorBase::removeBufferFromPoolInternal(pool, buffer)) {
        printf("⚠️  Failed to remove buffer #%u from pool '%s' (in use or not in pool)\n",
               buffer->id(), pool->getName().c_str());
        return false;
    }
    
    // 2. 销毁 Buffer（仅删除对象，不释放外部内存）
    deallocateBuffer(buffer);
    
    // 3. 清除所有权记录
    {
        std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
        framebuffer_buffer_ownership_.erase(buffer);
    }
    
    printf("✅ Buffer #%u removed from pool '%s'\n",
           buffer->id(), pool->getName().c_str());
    
    return true;
}

bool FramebufferAllocator::destroyPool(BufferPool* pool) {
    if (!pool) {
        printf("❌ FramebufferAllocator::destroyPool: pool is nullptr\n");
        return false;
    }
    
    printf("🧹 FramebufferAllocator: Destroying pool '%s'...\n", pool->getName().c_str());
    
    // 1. 检查是否是管理的 pool
    {
        std::lock_guard<std::mutex> lock(managed_pool_mutex_);
        if (managed_pool_sptr_ && managed_pool_sptr_.get() == pool) {
            printf("   ✅ Pool matches managed_pool_sptr_\n");
        } else {
            printf("   ⚠️  Warning: Pool does not match managed_pool_sptr_\n");
        }
    }
    
    std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
    
    // 2. 找到所有属于此 allocator 的 buffer
    std::vector<Buffer*> to_remove;
    for (auto& [buf, alloc] : framebuffer_buffer_ownership_) {
        if (alloc == this) {
            to_remove.push_back(buf);
        }
    }
    
    // 3. 移除并销毁
    for (Buffer* buf : to_remove) {
        BufferAllocatorBase::removeBufferFromPoolInternal(pool, buf);
        deallocateBuffer(buf);
        framebuffer_buffer_ownership_.erase(buf);
    }
    
    printf("✅ Pool '%s' destroyed: removed %zu buffers (external memory retained)\n", 
           pool->getName().c_str(), to_remove.size());
    
    return true;
}

// 辅助方法：清理 Pool
void FramebufferAllocator::cleanupPool(BufferPool* pool) {
    if (!pool) {
        return;
    }
    
    printf("🧹 Cleaning up pool '%s'...\n", pool->getName().c_str());
    
    std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
    
    // 找到所有属于此 allocator 的 buffer
    std::vector<Buffer*> to_remove;
    for (auto& [buf, alloc] : framebuffer_buffer_ownership_) {
        if (alloc == this) {
            to_remove.push_back(buf);
        }
    }
    
    // 移除并销毁
    for (Buffer* buf : to_remove) {
        BufferAllocatorBase::removeBufferFromPoolInternal(pool, buf);
        deallocateBuffer(buf);
        framebuffer_buffer_ownership_.erase(buf);
    }
    
    printf("✅ Cleanup complete: removed %zu buffers\n", to_remove.size());
}

