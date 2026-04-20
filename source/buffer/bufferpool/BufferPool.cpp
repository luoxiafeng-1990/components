#include "buffer/bufferpool/BufferPool.hpp"
#include "common/Logger.hpp"
#include <stdexcept>
#include <chrono>

// ============================================================
// 构造函数实现
// ============================================================

BufferPool::BufferPool(
    PrivateToken token,
    const std::string& name,
    const std::string& category
)
    : name_(name)
    , category_(category)
    , registry_id_(0)
    , running_(true)
    , log_prefix_("[" + name + "]")
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.BufferPool")))
{
    (void)token;  // 标记 token 已使用
    
    // 打印生命周期开始
    LOG4CPLUS_INFO(logger_, log_prefix_ << " 创建: category=" << category_);
}

BufferPool::~BufferPool() {
    // 打印生命周期结束
    LOG4CPLUS_INFO(logger_, "");
    LOG4CPLUS_INFO(logger_, log_prefix_ << " " << std::string(67, '='));
    LOG4CPLUS_INFO(logger_, log_prefix_ << " 析构: total=" << getTotalCount() 
                   << ", free=" << getFreeCount() << ", filled=" << getFilledCount());
    LOG4CPLUS_INFO(logger_, log_prefix_ << " " << std::string(67, '='));
    
    // 停止等待线程
    shutdown();
    
    // ⚠️ 注意：不再在这里调用 unregisterPool()
    // 原因：
    // 1. unregisterPool() 现在是私有方法，只能由 Allocator 的 destroyPool() 调用
    // 2. 正确的销毁流程：Allocator::destroyPool() → 清理 Buffer → unregisterPool() → Pool 析构
    // 3. 如果在这里调用，会导致重复调用（destroyPool 已经调用过了）
    // 4. 如果 Allocator 没有调用 destroyPool，说明是异常情况，不应该在这里处理
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

void BufferPool::submitFilled(Buffer* buffer_ptr) {
    if (!buffer_ptr) {
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 验证 buffer 属于此 pool
        if (managed_buffers_.find(buffer_ptr) == managed_buffers_.end()) {
            LOG4CPLUS_WARN_FMT(logger_, "⚠️  Buffer #%u does not belong to pool '%s'",
                   buffer_ptr->id(), name_.c_str());
            return;
        }
        
        // 🛡️ 状态检查：确保 buffer 由生产者持有
        if (buffer_ptr->state() != Buffer::State::LOCKED_BY_PRODUCER) {
            LOG4CPLUS_ERROR_FMT(logger_, "❌ ERROR: submitFilled() called with wrong state: %s (expected LOCKED_BY_PRODUCER)",
                   Buffer::stateToString(buffer_ptr->state()));
            LOG4CPLUS_ERROR_FMT(logger_, "   Buffer #%u in pool '%s'", buffer_ptr->id(), name_.c_str());
            return;
        }
        
        // 添加到 filled 队列
        filled_queue_.push(buffer_ptr);
        buffer_ptr->setState(Buffer::State::READY_FOR_CONSUME);
    }
    
    // 通知消费者（锁外通知）
    filled_cv_.notify_one();
}

void BufferPool::releaseFree(Buffer* buffer_ptr) {
    if (!buffer_ptr) {
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 验证 buffer 属于此 pool
        if (managed_buffers_.find(buffer_ptr) == managed_buffers_.end()) {
            LOG4CPLUS_WARN_FMT(logger_, "⚠️  Buffer #%u does not belong to pool '%s'",
                   buffer_ptr->id(), name_.c_str());
            return;
        }
        
        // 🛡️ 状态检查：确保 buffer 由生产者持有（填充失败的场景）
        if (buffer_ptr->state() != Buffer::State::LOCKED_BY_PRODUCER) {
            LOG4CPLUS_ERROR_FMT(logger_, "❌ ERROR: releaseFree() called with wrong state: %s (expected LOCKED_BY_PRODUCER)",
                   Buffer::stateToString(buffer_ptr->state()));
            LOG4CPLUS_ERROR_FMT(logger_, "   Buffer #%u in pool '%s'", buffer_ptr->id(), name_.c_str());
            return;
        }
        
        // 归还到 free 队列
        free_queue_.push(buffer_ptr);
        buffer_ptr->setState(Buffer::State::IDLE);
    }
    
    // 通知生产者（锁外通知）
    free_cv_.notify_one();
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
        
        // 阻塞模式：如果因 shutdown 退出且队列为空，返回 nullptr
        if (!running_ && filled_queue_.empty()) {
            return nullptr;
        }
    }
    
    // 非阻塞模式或 shutdown 后仍有 buffer：
    // 即使已 shutdown，也返回队列中残留的 buffer（用于 drain 阶段）
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
            LOG4CPLUS_WARN_FMT(logger_, "⚠️  Buffer #%u does not belong to pool '%s'",
                   buffer->id(), name_.c_str());
            return;
        }
        
        // 🛡️ 状态检查：确保 buffer 由消费者持有
        if (buffer->state() != Buffer::State::LOCKED_BY_CONSUMER) {
            LOG4CPLUS_ERROR_FMT(logger_, "❌ ERROR: releaseFilled() called with wrong state: %s (expected LOCKED_BY_CONSUMER)",
                   Buffer::stateToString(buffer->state()));
            LOG4CPLUS_ERROR_FMT(logger_, "   Buffer #%u in pool '%s'", buffer->id(), name_.c_str());
            return;
        }
        
        // 归还到 free 队列
        free_queue_.push(buffer);
        buffer->setState(Buffer::State::IDLE);
        
        // ⭐ v2.19新增：清理 buffer 的引用计数和元数据，确保回到 free 队列时是"干净"的
        // 这解决了 AVFrame 数据在 fillBuffer() 后被清空的问题
        buffer->freeBuffer();
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
            LOG4CPLUS_WARN_FMT(logger_, "⚠️  Buffer #%u already in pool '%s'", 
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
            LOG4CPLUS_WARN_FMT(logger_, "⚠️  Cannot remove buffer #%u: state=%s (must be IDLE)",
                   buffer->id(), Buffer::stateToString(buffer->state()));
            return false;
        }
        
        // 从 free_queue 中移除
        bool removed = removeFromQueue(free_queue_, buffer);
        
        if (!removed) {
            LOG4CPLUS_WARN_FMT(logger_, "⚠️  Buffer #%u not in free_queue", buffer->id());
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
    
    
    LOG4CPLUS_INFO(logger_, "========================================");
    LOG4CPLUS_INFO(logger_, "📊 BufferPool '" << name_ << "' Statistics");
    LOG4CPLUS_INFO(logger_, "========================================");
    LOG4CPLUS_INFO(logger_, "  Category: " << (category_.empty() ? "(none)" : category_));
    LOG4CPLUS_INFO(logger_, "  Registry ID: " << registry_id_);
    LOG4CPLUS_INFO(logger_, "  Total buffers: " << managed_buffers_.size());
    LOG4CPLUS_INFO(logger_, "  Free buffers: " << free_queue_.size());
    LOG4CPLUS_INFO(logger_, "  Filled buffers: " << filled_queue_.size());
    LOG4CPLUS_INFO(logger_, "  Running: " << (running_ ? "Yes" : "No"));
    LOG4CPLUS_INFO(logger_, "========================================");
}


void BufferPool::clearAllManagedBuffers() {
    std::lock_guard<std::mutex> lock(mutex_);
    managed_buffers_.clear();
}
