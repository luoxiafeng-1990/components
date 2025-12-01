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
// 静态变量声明（必须在所有方法之前）
// ============================================================

// 所有权跟踪（静态成员，所有Allocator共享）
static std::unordered_map<Buffer*, BufferAllocatorBase*> framebuffer_buffer_ownership_;
static std::mutex framebuffer_ownership_mutex_;

// ============================================================
// 构造/析构函数
// ============================================================

FramebufferAllocator::FramebufferAllocator()
    : external_buffers_()
    , next_buffer_index_(0)
{
   printf("🔧 FramebufferAllocator created\n");
}

FramebufferAllocator::FramebufferAllocator(const std::vector<BufferInfo>& external_buffers)
    : external_buffers_(external_buffers)
    , next_buffer_index_(0)
{
    printf("🔧 FramebufferAllocator created with %zu external buffers\n", 
           external_buffers_.size());
}

FramebufferAllocator::FramebufferAllocator(LinuxFramebufferDevice* device)
    : next_buffer_index_(0)
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
    // v2.0: 在子类析构中清理 BufferPool
    // 此时对象还是 FramebufferAllocator 类型，可以正确调用子类的 destroyPool()
    if (pool_id_ != 0) {
        printf("🧹 [FramebufferAllocator] Cleaning up BufferPool (ID: %lu)...\n", pool_id_);
        destroyPool(pool_id_);  // 调用子类的 destroyPool() 实现
    }
    printf("🧹 FramebufferAllocator destroyed (external memory not freed)\n");
}

// ============================================================
// 重写批量分配（批量包装）
// ============================================================

uint64_t FramebufferAllocator::allocatePoolWithBuffers(
    int count,
    size_t size,
    const std::string& name,
    const std::string& category)
{
    printf("\n🏭 [FramebufferAllocator] Creating BufferPool with %d buffers...\n", count);
    
    // v2.0 步骤 1: 使用 Passkey Token 创建 BufferPool（shared_ptr）
    auto pool = std::make_shared<BufferPool>(
        token(),    // 从基类获取通行证
        name,
        category
    );
    
    printf("✅ Created BufferPool '%s'\n", pool->getName().c_str());
    
    // v2.0 步骤 2: 批量包装外部 Buffer 并添加到 pool
    int actual_count = (count > 0) ? count : static_cast<int>(external_buffers_.size());
    printf("🔧 Wrapping %d external buffers...\n", actual_count);
    
    for (int i = 0; i < actual_count; i++) {
        Buffer* buffer = createBuffer(i, 0);  // size 参数被忽略
        if (!buffer) {
            printf("❌ Failed to wrap external buffer #%d\n", i);
            // 清理已创建的 buffers
            destroyPool(0);  // 稍后会设置 pool_id
            return 0;
        }
        
        if (!BufferAllocatorBase::addBufferToPoolQueue(pool.get(), buffer, QueueType::FREE)) {
            printf("❌ Failed to add buffer #%d to pool\n", i);
            deallocateBuffer(buffer);
            destroyPool(0);  // 稍后会设置 pool_id
            return 0;
        }
        
        {
            std::lock_guard<std::mutex> lock(framebuffer_ownership_mutex_);
            framebuffer_buffer_ownership_[buffer] = this;
        }
        
        printf("   ✅ Buffer #%d wrapped: virt=%p, phys=0x%lx, size=%zu (EXTERNAL)\n",
               i, buffer->getVirtualAddress(), buffer->getPhysicalAddress(), buffer->size());
    }
    
    printf("✅ BufferPool '%s' created with %d buffers\n", 
           pool->getName().c_str(), actual_count);
    
    // v2.0 步骤 3: 注册到 Registry（转移所有权）
    uint64_t pool_id = BufferPoolRegistry::getInstance().registerPool(pool);
    pool->setRegistryId(pool_id);
    
    // v2.0 步骤 4: 记录 pool_id
    pool_id_ = pool_id;
    
    printf("✅ [FramebufferAllocator] BufferPool registered (ID: %lu, ref_count=1)\n", pool_id);
    
    // v2.0 步骤 5: 返回 pool_id
    return pool_id;
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

Buffer* FramebufferAllocator::injectBufferToPool(
    uint64_t pool_id,
    size_t size,
    QueueType queue
) {
    printf("⚠️  [FramebufferAllocator] injectBufferToPool: This method is not supported\n");
    printf("   FramebufferAllocator only supports wrapping pre-allocated external memory\n");
    printf("   Use allocatePoolWithBuffers() or injectExternalBufferToPool() instead\n");
    return nullptr;
}

Buffer* FramebufferAllocator::injectExternalBufferToPool(
    uint64_t pool_id,
    void* virt_addr,
    uint64_t phys_addr,
    size_t size,
    QueueType queue
) {
    if (!virt_addr || size == 0) {
        printf("❌ [FramebufferAllocator] injectExternalBufferToPool: invalid parameters\n");
        return nullptr;
    }
    
    // v2.0: 从 Registry 获取 Pool（返回 weak_ptr）
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        printf("❌ [FramebufferAllocator] pool_id %lu not found or already destroyed\n", pool_id);
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
    if (!BufferAllocatorBase::addBufferToPoolQueue(pool.get(), buffer, queue)) {
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

bool FramebufferAllocator::removeBufferFromPool(uint64_t pool_id, Buffer* buffer) {
    if (!buffer) {
        printf("❌ [FramebufferAllocator] removeBufferFromPool: buffer is nullptr\n");
        return false;
    }
    
    // v2.0: 从 Registry 获取 Pool（返回 weak_ptr）
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        printf("❌ [FramebufferAllocator] pool_id %lu not found or already destroyed\n", pool_id);
        return false;
    }
    
    // 1. 通过基类静态方法从 pool 移除
    if (!BufferAllocatorBase::removeBufferFromPoolInternal(pool.get(), buffer)) {
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

bool FramebufferAllocator::destroyPool(uint64_t pool_id) {
    if (pool_id == 0) {
        printf("❌ [FramebufferAllocator] destroyPool: invalid pool_id\n");
        return false;
    }
    
    // v2.0: 通过基类辅助方法从 Registry 获取 Pool
    auto pool = getPoolForCleanup(pool_id);
    if (!pool) {
        printf("⚠️  [FramebufferAllocator] pool_id %lu not found (already destroyed?)\n", pool_id);
        return false;
    }
    
    printf("🧹 [FramebufferAllocator] Destroying pool '%s' (ID: %lu)...\n", pool->getName().c_str(), pool_id);
    
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
        BufferAllocatorBase::removeBufferFromPoolInternal(pool.get(), buf);
        deallocateBuffer(buf);
        framebuffer_buffer_ownership_.erase(buf);
    }
    
    printf("✅ [FramebufferAllocator] Pool destroyed: removed %zu buffers (external memory retained)\n", 
           to_remove.size());
    
    // 4. 从 Registry 注销（触发 Pool 析构）
    BufferPoolRegistry::getInstance().unregisterPool(pool_id);
    
    // 5. 清除 pool_id
    pool_id_ = 0;
    
    return true;
}

