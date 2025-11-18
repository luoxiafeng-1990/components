#include "../../include/buffer/BufferPool.hpp"
#include "../../include/buffer/BufferPoolRegistry.hpp"
#include <stdio.h>
#include <stdexcept>
#include <chrono>

// ============================================================
// 静态工厂方法实现
// ============================================================

std::unique_ptr<BufferPool> BufferPool::CreateEmpty(
    const std::string& name,
    const std::string& category)
{
    return std::unique_ptr<BufferPool>(new BufferPool(name, category));
}

// ============================================================
// 构造函数和析构函数
// ============================================================

BufferPool::BufferPool(const std::string& name, const std::string& category)
    : name_(name)
    , category_(category)
    , registry_id_(0)
    , running_(true)
{
    printf("\n📦 Initializing BufferPool '%s' (empty, managed by Allocator)...\n", 
           name_.c_str());
    
    // 自动注册到全局注册表
    registry_id_ = BufferPoolRegistry::getInstance().registerPool(this, name_, category_);
    
    printf("✅ BufferPool '%s' initialized (ID: %lu)\n", name_.c_str(), registry_id_);
}

BufferPool::~BufferPool() {
    printf("🧹 Destroying BufferPool '%s'...\n", name_.c_str());
    
    // 停止等待线程
    shutdown();
    
    // 注销
    BufferPoolRegistry::getInstance().unregisterPool(registry_id_);
    
    printf("✅ BufferPool '%s' destroyed\n", name_.c_str());
}

void BufferPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    
    // 唤醒所有等待的线程
    free_cv_.notify_all();
    filled_cv_.notify_all();
}

// ============================================================
// 生产者接口实现
// ============================================================

Buffer* BufferPool::acquireFree(bool blocking, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (blocking) {
        if (timeout_ms < 0) {
            // 无限等待
            while (free_queue_.empty() && running_) {
                free_cv_.wait(lock);
            }
        } else {
            // 超时等待
            auto deadline = std::chrono::steady_clock::now() + 
                           std::chrono::milliseconds(timeout_ms);
            
            while (free_queue_.empty() && running_) {
                if (free_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                    return nullptr;  // 超时
                }
            }
        }
    }
    
    // 检查是否因为 shutdown 而退出
    if (!running_) {
        return nullptr;
    }
    
    // 检查队列是否为空
    if (free_queue_.empty()) {
        return nullptr;
    }
    
    // 获取 buffer
    Buffer* buffer = free_queue_.front();
    free_queue_.pop();
    
    // 更新状态
    buffer->setState(Buffer::State::LOCKED_BY_PRODUCER);
    
    return buffer;
}

void BufferPool::submitFilled(Buffer* buffer) {
    if (!buffer) {
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 验证 buffer 属于此 pool
        if (managed_buffers_.find(buffer) == managed_buffers_.end()) {
            printf("⚠️  Buffer #%u does not belong to pool '%s'\n",
                   buffer->id(), name_.c_str());
            return;
        }
        
        // 添加到 filled 队列
        filled_queue_.push(buffer);
        buffer->setState(Buffer::State::READY_FOR_CONSUME);
    }
    
    // 通知消费者（锁外通知）
    filled_cv_.notify_one();
}

// ============================================================
// 消费者接口实现
// ============================================================

Buffer* BufferPool::acquireFilled(bool blocking, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (blocking) {
        if (timeout_ms < 0) {
            // 无限等待
            while (filled_queue_.empty() && running_) {
                filled_cv_.wait(lock);
            }
        } else {
            // 超时等待
            auto deadline = std::chrono::steady_clock::now() + 
                           std::chrono::milliseconds(timeout_ms);
            
            while (filled_queue_.empty() && running_) {
                if (filled_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                    return nullptr;  // 超时
                }
            }
        }
    }
    
    // 检查是否因为 shutdown 而退出
    if (!running_) {
        return nullptr;
    }
    
    // 检查队列是否为空
    if (filled_queue_.empty()) {
        return nullptr;
    }
    
    // 获取 buffer
    Buffer* buffer = filled_queue_.front();
    filled_queue_.pop();
    
    // 更新状态
    buffer->setState(Buffer::State::LOCKED_BY_CONSUMER);
    
    return buffer;
}

void BufferPool::releaseFilled(Buffer* buffer) {
    if (!buffer) {
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 验证 buffer 属于此 pool
        if (managed_buffers_.find(buffer) == managed_buffers_.end()) {
            printf("⚠️  Buffer #%u does not belong to pool '%s'\n",
                   buffer->id(), name_.c_str());
            return;
        }
        
        // 归还到 free 队列
        free_queue_.push(buffer);
        buffer->setState(Buffer::State::IDLE);
    }
    
    // 通知生产者（锁外通知）
    free_cv_.notify_one();
}

// ============================================================
// 查询接口实现
// ============================================================

int BufferPool::getFreeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return free_queue_.size();
}

int BufferPool::getFilledCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return filled_queue_.size();
}

int BufferPool::getTotalCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return managed_buffers_.size();
}

Buffer* BufferPool::getBufferById(uint32_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 遍历所有托管的 buffer，查找匹配的 ID
    for (Buffer* buf : managed_buffers_) {
        if (buf && buf->id() == id) {
            return buf;
        }
    }
    
    return nullptr;
}

size_t BufferPool::getBufferSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 返回第一个 buffer 的大小（假设所有 buffer 大小相同）
    if (!managed_buffers_.empty()) {
        Buffer* first = *managed_buffers_.begin();
        if (first) {
            return first->size();
        }
    }
    
    return 0;
}

// ============================================================
// 私有接口实现（仅供 BufferAllocatorBase 使用）
// ============================================================

bool BufferPool::addBufferToQueue(Buffer* buffer, QueueType queue) {
    if (!buffer) {
        return false;
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 检查是否已托管
        if (managed_buffers_.find(buffer) != managed_buffers_.end()) {
            printf("⚠️  Buffer #%u already in pool '%s'\n", 
                   buffer->id(), name_.c_str());
            return false;
        }
        
        // 添加到托管集合
        managed_buffers_.insert(buffer);
        
        // 添加到指定队列
        if (queue == QueueType::FREE) {
            free_queue_.push(buffer);
            buffer->setState(Buffer::State::IDLE);
        } else {
            filled_queue_.push(buffer);
            buffer->setState(Buffer::State::READY_FOR_CONSUME);
        }
    }  // 释放锁
    
    // 在锁外通知（避免惊群效应）
    if (queue == QueueType::FREE) {
        free_cv_.notify_one();
    } else {
        filled_cv_.notify_one();
    }
    
    return true;
}

bool BufferPool::removeBufferFromPool(Buffer* buffer) {
    if (!buffer) {
        return false;
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 检查是否被托管
        if (managed_buffers_.find(buffer) == managed_buffers_.end()) {
            return false;
        }
        
        // 检查状态（只能移除空闲的）
        if (buffer->state() != Buffer::State::IDLE) {
            printf("⚠️  Cannot remove buffer #%u: state=%s (must be IDLE)\n",
                   buffer->id(), Buffer::stateToString(buffer->state()));
            return false;
        }
        
        // 从 free_queue 中移除
        bool removed = removeFromQueue(free_queue_, buffer);
        
        if (!removed) {
            printf("⚠️  Buffer #%u not in free_queue\n", buffer->id());
            return false;
        }
        
        // 从托管集合移除
        managed_buffers_.erase(buffer);
    }  // 释放锁
    
    // 通知等待的线程（队列已变化）
    free_cv_.notify_all();
    
    return true;
}

bool BufferPool::removeFromQueue(std::queue<Buffer*>& queue, Buffer* target) {
    std::queue<Buffer*> temp;
    bool found = false;
    
    while (!queue.empty()) {
        Buffer* front = queue.front();
        queue.pop();
        
        if (front == target) {
            found = true;
            // 不加回队列
        } else {
            temp.push(front);
        }
    }
    
    // 恢复队列
    while (!temp.empty()) {
        queue.push(temp.front());
        temp.pop();
    }
    
    return found;
}

// ============================================================
// 调试接口实现
// ============================================================

void BufferPool::printStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    printf("\n========================================\n");
    printf("📊 BufferPool '%s' Statistics\n", name_.c_str());
    printf("========================================\n");
    printf("  Category: %s\n", category_.empty() ? "(none)" : category_.c_str());
    printf("  Registry ID: %lu\n", registry_id_);
    printf("  Total buffers: %zu\n", managed_buffers_.size());
    printf("  Free buffers: %zu\n", free_queue_.size());
    printf("  Filled buffers: %zu\n", filled_queue_.size());
    printf("  Running: %s\n", running_ ? "Yes" : "No");
    printf("========================================\n\n");
}

void BufferPool::printAllBuffers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    printf("\n========================================\n");
    printf("📋 BufferPool '%s' - All Buffers\n", name_.c_str());
    printf("========================================\n");
    
    int index = 0;
    for (Buffer* buf : managed_buffers_) {
        printf("  [%d] Buffer #%u: virt=%p, phys=0x%lx, size=%zu, state=%s\n",
               index++,
               buf->id(),
               buf->getVirtualAddress(),
               buf->getPhysicalAddress(),
               buf->size(),
               Buffer::stateToString(buf->state()));
    }
    
    printf("========================================\n\n");
}
