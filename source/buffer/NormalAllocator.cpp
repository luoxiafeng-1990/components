#include "buffer/NormalAllocator.hpp"
#include "buffer/pool/MallocMemoryProvider.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include "common/Logger.hpp"
extern "C" {
#include <libavcodec/packet.h>
}
#include <cstdlib>
#include <cstring>
#include <stdio.h>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>

// ============================================================
// 构造/析构函数
// ============================================================

NormalAllocator::NormalAllocator(std::unique_ptr<IMemoryProvider> provider)
    : memory_provider_(std::move(provider))
    , type_(BufferMemoryAllocatorType::NORMAL_MALLOC)
    , alignment_(memory_provider_ ? memory_provider_->getCapabilities().default_alignment : 64)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Normal")))
{
    LOG4CPLUS_DEBUG_FMT(logger_, "创建: provider=%s",
                        memory_provider_ ? memory_provider_->kind() : "null");
}

NormalAllocator::NormalAllocator(BufferMemoryAllocatorType type, size_t alignment)
    : memory_provider_(std::make_unique<MallocMemoryProvider>(alignment))
    , type_(type)
    , alignment_(alignment)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Normal")))
{
    LOG4CPLUS_DEBUG_FMT(logger_, "创建: alignment=%zu (兼容模式, provider=malloc)", alignment_);
}

NormalAllocator::~NormalAllocator() {
    // v2.0: 子类析构函数中显式清理所有 Pool
    // 只有 NormalAllocator 自己知道如何释放 Buffer 内存
    // destroyPool() 会自动查询 Registry 获取所有 Pool 并清理
    destroyPool();
    
    LOG4CPLUS_DEBUG(logger_, "析构");
}

// ============================================================
// 核心实现
// ============================================================

Buffer* NormalAllocator::createBuffer(uint32_t id, size_t size) {
    MemoryBlock block = memory_provider_->allocate(size, alignment_);
    if (!block.virt_addr) {
        LOG4CPLUS_ERROR_FMT(logger_, "IMemoryProvider(%s) 分配失败: id=%u size=%zu",
                            memory_provider_->kind(), id, size);
        return nullptr;
    }

    memset(block.virt_addr, 0, size);

    Buffer* buffer = new Buffer(
        id,
        block.virt_addr,
        block.phys_addr,
        size,
        Buffer::Ownership::OWNED
    );

    if (!buffer) {
        LOG4CPLUS_ERROR_FMT(logger_, "Buffer 对象创建失败: id=%u", id);
        memory_provider_->deallocate(block);
        return nullptr;
    }

    return buffer;
}

void NormalAllocator::deallocateBuffer(Buffer* buffer) {
    if (!buffer) {
        return;
    }

    if (buffer->getAVPacket()) {
        AVPacket* pkt = buffer->getAVPacket();
        av_packet_free(&pkt);
        buffer->setAVPacket(nullptr);
    }

    void* virt_addr = buffer->getVirtualAddress();
    if (virt_addr) {
        MemoryBlock block;
        block.virt_addr = virt_addr;
        block.phys_addr = buffer->getPhysicalAddress();
        block.size      = buffer->size();
        memory_provider_->deallocate(block);
    }

    delete buffer;
}

// ============================================================
// 实现基类纯虚函数
// ============================================================

// 所有权跟踪（静态成员，所有Allocator共享）
static std::unordered_map<Buffer*, BufferAllocatorBase*> buffer_ownership_;
static std::mutex ownership_mutex_;

uint64_t NormalAllocator::allocatePoolWithBuffers(
    int count,
    size_t size,
    const std::string& name,
    const std::string& category
) {
    LOG4CPLUS_DEBUG_FMT(logger_, "创建BufferPool: %d buffers", count);
    
    // v2.0 步骤 1: 使用 Passkey Token 创建 BufferPool（shared_ptr）
    auto pool = std::make_shared<BufferPool>(
        token(),    // 从基类获取通行证
        name,
        category
    );
    
    // v2.0 步骤 2: 批量创建 Buffer 并注入到 pool
    for (int i = 0; i < count; i++) {
        Buffer* buffer = createBuffer(i, size);
        if (!buffer) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to create buffer #%d", i);
            cleanupPoolTemp(pool.get());
            return 0;
        }
        
        if (!BufferAllocatorBase::addBufferToPoolQueue(pool.get(), buffer, QueueType::FREE)) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to add buffer #%d to pool", i);
            deallocateBuffer(buffer);
            cleanupPoolTemp(pool.get());
            return 0;
        }
        
        {
            std::lock_guard<std::mutex> lock(ownership_mutex_);
            buffer_ownership_[buffer] = this;
        }
        
        LOG4CPLUS_DEBUG_FMT(logger_, "  Buffer #%d created: virt=%p, phys=0x%lx, size=%zu",
               i, buffer->getVirtualAddress(), buffer->getPhysicalAddress(), size);
    }
    
    // v2.0 步骤 3: 注册到 Registry（转移所有权，传入 Allocator ID）
    uint64_t pool_id = ComponentTopology::getInstance().registerPool(pool, getAllocatorId());
    pool->setRegistryId(pool_id);
    
    LOG4CPLUS_INFO_FMT(logger_, "BufferPool '%s' created with %d buffers (ID: %lu)", 
           name.c_str(), count, pool_id);
    
    // v2.0 步骤 4: 返回 pool_id（Registry 独占持有 Pool）
    return pool_id;
}

Buffer* NormalAllocator::injectBufferToPool(
    uint64_t pool_id,
    size_t size,
    QueueType queue
) {
    // v2.0: 从 Registry 获取 Pool（返回 weak_ptr）
    auto pool_weak = ComponentTopology::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        LOG4CPLUS_ERROR_FMT(logger_, "injectBufferToPool: pool_id %lu not found or already destroyed", pool_id);
        return nullptr;
    }
    
    // 1. 生成 Buffer ID（从 pool 的当前 buffer 数量）
    uint32_t id = pool->getTotalCount();
    
    // 2. 创建 Buffer（内部分配内存）
    Buffer* buffer = createBuffer(id, size);
    if (!buffer) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to create buffer #%u", id);
        return nullptr;
    }
    
    // 3. 通过基类静态方法添加到 pool 的指定队列（会自动添加到 managed_buffers_）
    if (!BufferAllocatorBase::addBufferToPoolQueue(pool.get(), buffer, queue)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to add buffer #%u to pool '%s'", 
               id, pool->getName().c_str());
        deallocateBuffer(buffer);
        return nullptr;
    }
    
    // 4. 记录所有权
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_[buffer] = this;
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Buffer #%u injected to pool '%s' (queue: %s)",
           id, pool->getName().c_str(), 
           queue == QueueType::FREE ? "FREE" : "FILLED");
    
    return buffer;
}

Buffer* NormalAllocator::injectExternalBufferToPool(
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
        LOG4CPLUS_ERROR_FMT(logger_, "injectExternalBufferToPool: pool_id %lu not found or already destroyed", pool_id);
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
    
    // 4. 记录所有权
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_[buffer] = this;
    }
    
    // 仅在TRACE级别输出详细信息
    LOG_TRACE_FMT("External buffer #%u injected (virt=%p, phys=0x%lx, size=%zu)",
           id, virt_addr, phys_addr, size);
    
    return buffer;
}

bool NormalAllocator::removeBufferFromPool(uint64_t pool_id, Buffer* buffer) {
    if (!buffer) {
        LOG4CPLUS_ERROR(logger_, "removeBufferFromPool: buffer is nullptr");
        return false;
    }
    
    // v2.0: 从 Registry 获取 Pool（返回 weak_ptr）
    auto pool_weak = ComponentTopology::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        LOG4CPLUS_ERROR_FMT(logger_, "removeBufferFromPool: pool_id %lu not found or already destroyed", pool_id);
        return false;
    }
    
    // 1. 通过基类静态方法从 pool 移除（只能移除 free_queue 中的）
    if (!BufferAllocatorBase::removeBufferFromPoolInternal(pool.get(), buffer)) {
        LOG4CPLUS_WARN_FMT(logger_, "Failed to remove buffer #%u from pool '%s'",
                     buffer->id(), pool->getName().c_str());
        return false;
    }
    
    // 2. 销毁 Buffer
    deallocateBuffer(buffer);
    
    // 3. 清除所有权记录
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        buffer_ownership_.erase(buffer);
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Buffer #%u removed from pool '%s'",
           buffer->id(), pool->getName().c_str());
    
    return true;
}

bool NormalAllocator::destroyPool() {
    // 1. 获取所有属于此 allocator 的 pool
    auto pool_ids = getPoolsByAllocator();
    
    if (pool_ids.empty()) {
        LOG4CPLUS_DEBUG(logger_, "No pools to destroy");
        return true;
    }
    
    LOG4CPLUS_INFO_FMT(logger_, "🧹 [NormalAllocator] Destroying %zu pool(s)...", pool_ids.size());
    
    std::lock_guard<std::mutex> lock(ownership_mutex_);
    
    // 2. 遍历每个 pool
    for (uint64_t pool_id : pool_ids) {
        // 2.1 获取 pool
        auto pool = getPoolSpecialForAllocator(pool_id);
        if (!pool) {
            LOG4CPLUS_WARN_FMT(logger_, "pool_id %lu not found", pool_id);
            continue;
        }
        
        LOG4CPLUS_DEBUG_FMT(logger_, "销毁pool '%s' (ID: %lu)", pool->getName().c_str(), pool_id);
        
        // 2.2 通过 BufferPool 的公共方法获取所有属于此 pool 的 buffer
        std::vector<Buffer*> to_remove;
        for (Buffer* buf : pool->getAllManagedBuffers()) {
            // 检查 buffer 是否属于此 allocator
            auto it = buffer_ownership_.find(buf);
            if (it != buffer_ownership_.end() && it->second == this) {
                to_remove.push_back(buf);
            }
        }
        
        // 2.3 移除并销毁所有 Buffer
        for (Buffer* buf : to_remove) {
            BufferAllocatorBase::removeBufferFromPoolInternal(pool.get(), buf);
            deallocateBuffer(buf);
            buffer_ownership_.erase(buf);
        }
        
        LOG4CPLUS_DEBUG_FMT(logger_, "Pool '%s' destroyed: removed %zu buffers", 
               pool->getName().c_str(), to_remove.size());
        
        // 2.4 从 Registry 注销（触发 Pool 析构）
        unregisterPool(pool_id);
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "All %zu pool(s) destroyed", pool_ids.size());
    return true;
}

// v2.0 辅助方法：清理临时 Pool（创建失败时使用）
void NormalAllocator::cleanupPoolTemp(BufferPool* pool) {
    if (!pool) {
        return;
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "清理临时pool '%s'", pool->getName().c_str());
    
    std::lock_guard<std::mutex> lock(ownership_mutex_);
    
    // 找到所有属于此 allocator 的 buffer
    std::vector<Buffer*> to_remove;
    for (auto& [buf, alloc] : buffer_ownership_) {
        if (alloc == this) {
            to_remove.push_back(buf);
        }
    }
    
    // 移除并销毁
    for (Buffer* buf : to_remove) {
        BufferAllocatorBase::removeBufferFromPoolInternal(pool, buf);
        deallocateBuffer(buf);
        buffer_ownership_.erase(buf);
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Cleanup complete: removed %zu buffers", to_remove.size());
}

