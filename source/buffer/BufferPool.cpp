#include "../../include/buffer/BufferPool.hpp"
#include <stdio.h>
#include <string.h>
#include <stdexcept>
#include <algorithm>
#include <chrono>

// ============================================================
// 构造函数实现
// ============================================================

BufferPool::BufferPool(int count, size_t size, bool use_cma)
    : buffer_size_(size)
    , next_buffer_id_(0)
{
    printf("\n📦 Initializing BufferPool (owned buffers)...\n");
    printf("   Buffer count: %d\n", count);
    printf("   Buffer size: %zu bytes (%.2f MB)\n", size, size / (1024.0 * 1024.0));
    printf("   Memory type: %s\n", use_cma ? "CMA/DMA (连续物理内存)" : "Normal (普通内存)");
    
    initializeOwnedBuffers(count, size, use_cma);
    
    printf("✅ BufferPool initialized successfully\n");
    printf("   Total buffers: %d\n", getTotalCount());
    printf("   Free buffers: %d\n", getFreeCount());
    printf("   Filled buffers: %d\n", getFilledCount());
}

BufferPool::BufferPool(const std::vector<ExternalBufferInfo>& external_buffers)
    : buffer_size_(0)
    , next_buffer_id_(0)
{
    printf("\n📦 Initializing BufferPool (external buffers - simple mode)...\n");
    printf("   External buffer count: %zu\n", external_buffers.size());
    
    if (external_buffers.empty()) {
        throw std::invalid_argument("External buffer array is empty");
    }
    
    initializeExternalBuffers(external_buffers);
    
    printf("✅ BufferPool initialized successfully (external mode)\n");
    printf("   Total buffers: %d\n", getTotalCount());
    printf("   Free buffers: %d\n", getFreeCount());
}

BufferPool::BufferPool(std::vector<std::unique_ptr<BufferHandle>> handles)
    : buffer_size_(0)
    , next_buffer_id_(0)
{
    printf("\n📦 Initializing BufferPool (external buffers - lifetime tracking)...\n");
    printf("   BufferHandle count: %zu\n", handles.size());
    
    if (handles.empty()) {
        throw std::invalid_argument("BufferHandle array is empty");
    }
    
    initializeFromHandles(std::move(handles));
    
    printf("✅ BufferPool initialized successfully (tracked external mode)\n");
    printf("   Total buffers: %d\n", getTotalCount());
    printf("   Free buffers: %d\n", getFreeCount());
    printf("   Lifetime trackers: %zu\n", lifetime_trackers_.size());
}

BufferPool::~BufferPool() {
    printf("\n🧹 Cleaning up BufferPool...\n");
    printf("   Total buffers: %d\n", getTotalCount());
    printf("   Free buffers: %d\n", getFreeCount());
    printf("   Filled buffers: %d\n", getFilledCount());
    
    // 释放自有内存（通过 allocator）
    if (allocator_ && allocator_->name() != std::string("ExternalAllocator")) {
        for (auto& buffer : buffers_) {
            if (buffer.ownership() == Buffer::Ownership::OWNED) {
                allocator_->deallocate(buffer.getVirtualAddress(), buffer.size());
            }
        }
    }
    
    // 外部 buffer 通过 BufferHandle 自动释放（RAII）
    // external_handles_ 会在析构时自动清理
    
    printf("✅ BufferPool cleaned up\n");
}

// ============================================================
// 内部初始化方法
// ============================================================

void BufferPool::initializeOwnedBuffers(int count, size_t size, bool use_cma) {
    // 选择分配器
    if (use_cma) {
        allocator_ = std::make_unique<CMAAllocator>();
    } else {
        allocator_ = std::make_unique<NormalAllocator>();
    }
    
    printf("   Selected allocator: %s\n", allocator_->name());
    
    // 预分配容器空间
    buffers_.reserve(count);
    buffer_map_.reserve(count);
    
    // 分配每个 buffer
    for (int i = 0; i < count; i++) {
        uint64_t phys_addr = 0;
        void* virt_addr = allocator_->allocate(size, &phys_addr);
        
        if (virt_addr == nullptr) {
            printf("❌ ERROR: Failed to allocate buffer #%d\n", i);
            
            // 如果是 CMA 失败，尝试降级到普通内存
            if (use_cma) {
                printf("⚠️  Falling back to normal memory...\n");
                allocator_ = std::make_unique<NormalAllocator>();
                virt_addr = allocator_->allocate(size, &phys_addr);
            }
            
            if (virt_addr == nullptr) {
                // 清理已分配的资源
                throw std::runtime_error("Buffer allocation failed");
            }
        }
        
        // 创建 Buffer 对象
        uint32_t id = next_buffer_id_++;
        buffers_.emplace_back(id, virt_addr, phys_addr, size, Buffer::Ownership::OWNED);
        
        // 添加到索引
        buffer_map_[id] = &buffers_.back();
        
        // 放入空闲队列
        free_queue_.push(&buffers_.back());
        
        printf("   Buffer #%u: virt=%p, phys=0x%016lx\n", id, virt_addr, phys_addr);
    }
}

void BufferPool::initializeExternalBuffers(const std::vector<ExternalBufferInfo>& infos) {
    // 使用 ExternalAllocator（不实际分配/释放）
    allocator_ = std::make_unique<ExternalAllocator>();
    
    // 确定 buffer 大小（取第一个）
    buffer_size_ = infos[0].size;
    
    // 预分配容器
    buffers_.reserve(infos.size());
    buffer_map_.reserve(infos.size());
    
    // 创建 Buffer 对象
    for (const auto& info : infos) {
        // 验证大小一致性
        if (info.size != buffer_size_) {
            printf("⚠️  Warning: External buffer size mismatch (%zu vs %zu)\n",
                   info.size, buffer_size_);
        }
        
        // 如果物理地址未提供（0），尝试自动获取
        uint64_t phys_addr = info.phys_addr;
        if (phys_addr == 0) {
            phys_addr = getPhysicalAddress(info.virt_addr);
            if (phys_addr == 0) {
                printf("⚠️  Warning: Failed to get physical address for external buffer %p\n",
                       info.virt_addr);
            }
        }
        
        // 创建 Buffer 对象
        uint32_t id = next_buffer_id_++;
        buffers_.emplace_back(id, info.virt_addr, phys_addr, 
                             info.size, Buffer::Ownership::EXTERNAL);
        
        // 添加到索引
        buffer_map_[id] = &buffers_.back();
        
        // 放入空闲队列
        free_queue_.push(&buffers_.back());
        
        printf("   Buffer #%u: virt=%p, phys=0x%016lx (external)\n", 
               id, info.virt_addr, phys_addr);
    }
}

void BufferPool::initializeFromHandles(std::vector<std::unique_ptr<BufferHandle>> handles) {
    // 使用 ExternalAllocator
    allocator_ = std::make_unique<ExternalAllocator>();
    
    // 确定 buffer 大小
    buffer_size_ = handles[0]->size();
    
    // 预分配容器
    size_t count = handles.size();
    buffers_.reserve(count);
    buffer_map_.reserve(count);
    lifetime_trackers_.reserve(count);
    
    // 创建 Buffer 对象并保存生命周期跟踪器
    for (auto& handle : handles) {
        // 获取信息
        void* virt_addr = handle->getVirtualAddress();
        uint64_t phys_addr = handle->getPhysicalAddress();
        size_t size = handle->size();
        
        // 如果物理地址未知，尝试获取
        if (phys_addr == 0) {
            phys_addr = getPhysicalAddress(virt_addr);
        }
        
        // 创建 Buffer 对象
        uint32_t id = next_buffer_id_++;
        buffers_.emplace_back(id, virt_addr, phys_addr, size, Buffer::Ownership::EXTERNAL);
        
        // 添加到索引
        buffer_map_[id] = &buffers_.back();
        
        // 保存生命周期跟踪器
        lifetime_trackers_.push_back(handle->getLifetimeTracker());
        
        // 放入空闲队列
        free_queue_.push(&buffers_.back());
        
        printf("   Buffer #%u: virt=%p, phys=0x%016lx (tracked external)\n",
               id, virt_addr, phys_addr);
    }
    
    // 转移 BufferHandle 所有权
    external_handles_ = std::move(handles);
}

// ============================================================
// 生产者接口实现
// ============================================================

Buffer* BufferPool::acquireFree(bool blocking, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (blocking) {
        if (timeout_ms > 0) {
            // 带超时的等待
            auto timeout = std::chrono::milliseconds(timeout_ms);
            if (!free_cv_.wait_for(lock, timeout, [this] { return !free_queue_.empty(); })) {
                // 超时
                return nullptr;
            }
        } else {
            // 无限等待
            free_cv_.wait(lock, [this] { return !free_queue_.empty(); });
        }
    } else {
        // 非阻塞模式
        if (free_queue_.empty()) {
            return nullptr;
        }
    }
    
    // 从空闲队列取出一个 buffer
    Buffer* buffer = free_queue_.front();
    free_queue_.pop();
    
    // 校验 buffer 有效性（特别是外部 buffer）
    if (!validateBuffer(buffer)) {
        printf("❌ ERROR: Acquired invalid buffer #%u\n", buffer->id());
        // 重新放回队列（避免丢失）
        free_queue_.push(buffer);
        return nullptr;
    }
    
    // 更新状态
    buffer->setState(Buffer::State::LOCKED_BY_PRODUCER);
    buffer->addRef();
    
    return buffer;
}

void BufferPool::submitFilled(Buffer* buffer) {
    if (buffer == nullptr) {
        printf("⚠️  Warning: Trying to submit null buffer\n");
        return;
    }
    
    // 校验
    if (!verifyBufferOwnership(buffer)) {
        printf("❌ ERROR: Buffer #%u does not belong to this pool\n", buffer->id());
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 更新状态
        buffer->setState(Buffer::State::READY_FOR_CONSUME);
        
        // 放入就绪队列
        filled_queue_.push(buffer);
        
        // 通知消费者
        filled_cv_.notify_one();
    }
}

// ============================================================
// 消费者接口实现
// ============================================================

Buffer* BufferPool::acquireFilled(bool blocking, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (blocking) {
        if (timeout_ms > 0) {
            // 带超时的等待
            auto timeout = std::chrono::milliseconds(timeout_ms);
            if (!filled_cv_.wait_for(lock, timeout, [this] { return !filled_queue_.empty(); })) {
                // 超时
                return nullptr;
            }
        } else {
            // 无限等待
            filled_cv_.wait(lock, [this] { return !filled_queue_.empty(); });
        }
    } else {
        // 非阻塞模式
        if (filled_queue_.empty()) {
            return nullptr;
        }
    }
    
    // 从就绪队列取出一个 buffer
    Buffer* buffer = filled_queue_.front();
    filled_queue_.pop();
    
    // 校验
    if (!validateBuffer(buffer)) {
        printf("❌ ERROR: Acquired invalid filled buffer #%u\n", buffer->id());
        return nullptr;
    }
    
    // 更新状态
    buffer->setState(Buffer::State::LOCKED_BY_CONSUMER);
    
    return buffer;
}

void BufferPool::releaseFilled(Buffer* buffer) {
    if (buffer == nullptr) {
        printf("⚠️  Warning: Trying to release null buffer\n");
        return;
    }
    
    // 校验
    if (!verifyBufferOwnership(buffer)) {
        printf("❌ ERROR: Buffer #%u does not belong to this pool\n", buffer->id());
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 更新状态
        buffer->releaseRef();
        buffer->setState(Buffer::State::IDLE);
        
        // 放回空闲队列
        free_queue_.push(buffer);
        
        // 通知生产者
        free_cv_.notify_one();
    }
}

// ============================================================
// 查询接口实现
// ============================================================

int BufferPool::getFreeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(free_queue_.size());
}

int BufferPool::getFilledCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(filled_queue_.size());
}

int BufferPool::getTotalCount() const {
    return static_cast<int>(buffers_.size());
}

size_t BufferPool::getBufferSize() const {
    return buffer_size_;
}

Buffer* BufferPool::getBufferById(uint32_t id) {
    auto it = buffer_map_.find(id);
    if (it != buffer_map_.end()) {
        return it->second;
    }
    return nullptr;
}

const Buffer* BufferPool::getBufferById(uint32_t id) const {
    auto it = buffer_map_.find(id);
    if (it != buffer_map_.end()) {
        return it->second;
    }
    return nullptr;
}

// ============================================================
// 校验接口实现
// ============================================================

bool BufferPool::validateBuffer(const Buffer* buffer) const {
    if (!buffer) {
        return false;
    }
    
    // 基础校验
    if (!buffer->isValid()) {
        return false;
    }
    
    // 所有权检查
    if (!verifyBufferOwnership(buffer)) {
        return false;
    }
    
    // 如果是外部 buffer，检查生命周期
    if (buffer->ownership() == Buffer::Ownership::EXTERNAL) {
        uint32_t id = buffer->id();
        if (id < lifetime_trackers_.size()) {
            auto tracker = lifetime_trackers_[id];
            if (auto alive = tracker.lock()) {
                if (!(*alive)) {
                    printf("⚠️  Warning: External buffer #%u has been destroyed\n", id);
                    return false;
                }
            } else {
                printf("⚠️  Warning: External buffer #%u lifetime tracker expired\n", id);
                return false;
            }
        }
    }
    
    // 用户自定义校验
    return buffer->validate();
}

bool BufferPool::validateAllBuffers() const {
    for (const auto& buffer : buffers_) {
        if (!validateBuffer(&buffer)) {
            return false;
        }
    }
    return true;
}

// ============================================================
// 调试接口实现
// ============================================================

void BufferPool::printStats() const {
    printf("\n📊 BufferPool Statistics:\n");
    printf("   Total buffers: %d\n", getTotalCount());
    printf("   Free buffers: %d\n", getFreeCount());
    printf("   Filled buffers: %d\n", getFilledCount());
    printf("   Buffer size: %zu bytes (%.2f MB)\n", 
           buffer_size_, buffer_size_ / (1024.0 * 1024.0));
    printf("   Allocator: %s\n", allocator_ ? allocator_->name() : "None");
    printf("   External handles: %zu\n", external_handles_.size());
    printf("   Lifetime trackers: %zu\n", lifetime_trackers_.size());
    
    // 引用计数统计
    int total_refs = 0;
    for (const auto& buffer : buffers_) {
        total_refs += buffer.refCount();
    }
    printf("   Total ref count: %d\n", total_refs);
    
    // 有效性检查
    printf("   All buffers valid: %s\n", validateAllBuffers() ? "✅ Yes" : "❌ No");
}

void BufferPool::printAllBuffers() const {
    printf("\n📋 All Buffers:\n");
    for (const auto& buffer : buffers_) {
        buffer.printInfo();
        printf("\n");
    }
}

// ============================================================
// 高级功能：DMA-BUF 导出
// ============================================================

int BufferPool::exportBufferAsDmaBuf(uint32_t buffer_id) {
    Buffer* buffer = getBufferById(buffer_id);
    if (!buffer) {
        printf("❌ ERROR: Buffer #%u not found\n", buffer_id);
        return -1;
    }
    
    // 检查是否已经有 DMA-BUF fd
    int existing_fd = buffer->getDmaBufFd();
    if (existing_fd >= 0) {
        printf("ℹ️  Buffer #%u already exported as DMA-BUF fd=%d\n", buffer_id, existing_fd);
        return existing_fd;
    }
    
    // 只有 CMA buffer 可以导出
    if (allocator_->name() != std::string("CMAAllocator")) {
        printf("❌ ERROR: Only CMA buffers can be exported as DMA-BUF\n");
        return -1;
    }
    
    // 从 CMAAllocator 获取 fd
    CMAAllocator* cma = dynamic_cast<CMAAllocator*>(allocator_.get());
    if (!cma) {
        printf("❌ ERROR: Failed to cast to CMAAllocator\n");
        return -1;
    }
    
    int fd = cma->getDmaBufFd(buffer->getVirtualAddress());
    if (fd >= 0) {
        buffer->setDmaBufFd(fd);
        printf("✅ Buffer #%u exported as DMA-BUF fd=%d\n", buffer_id, fd);
    } else {
        printf("❌ ERROR: Failed to get DMA-BUF fd for buffer #%u\n", buffer_id);
    }
    
    return fd;
}

// ============================================================
// 辅助方法
// ============================================================

bool BufferPool::verifyBufferOwnership(const Buffer* buffer) const {
    // 检查 buffer 地址是否在 buffers_ 范围内
    if (buffers_.empty()) {
        return false;
    }
    
    const Buffer* first = &buffers_.front();
    const Buffer* last = &buffers_.back();
    
    return buffer >= first && buffer <= last;
}

uint64_t BufferPool::getPhysicalAddress(void* virt_addr) {
    if (!allocator_) {
        return 0;
    }
    
    // 临时分配并获取物理地址（然后立即释放）
    // 注意：这只是为了演示，实际应该直接使用底层方法
    uint64_t phys_addr = 0;
    
    // 使用 NormalAllocator 的 getPhysicalAddress 方法
    NormalAllocator normal;
    phys_addr = normal.getPhysicalAddress(virt_addr);
    
    return phys_addr;
}

