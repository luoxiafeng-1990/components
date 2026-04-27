#include "bufferpool/pool/base/BufferPool.hpp"
#include "common/Logger.hpp"
#include <stdexcept>
#include <chrono>

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
    (void)token;
    LOG4CPLUS_INFO(logger_, log_prefix_ << " 创建: category=" << category_);
}

BufferPool::~BufferPool() {
    // log4cplus 可能已被全局析构销毁，此时写日志会导致
    // "log4cplus:ERROR No appenders could be found" 输出到 stderr
    auto root = log4cplus::Logger::getRoot();
    if (!root.getAllAppenders().empty()) {
        LOG4CPLUS_INFO(logger_, "");
        LOG4CPLUS_INFO(logger_, log_prefix_ << " " << std::string(67, '='));
        LOG4CPLUS_INFO(logger_, log_prefix_ << " 析构: total=" << getTotalCount() 
                       << ", free=" << getFreeCount() << ", filled=" << getFilledCount());
        LOG4CPLUS_INFO(logger_, log_prefix_ << " " << std::string(67, '='));
    }
    shutdown();
}

void BufferPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    free_cv_.notify_all();
    filled_cv_.notify_all();
}

Buffer* BufferPool::acquireFree(bool blocking, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (blocking) {
        if (timeout_ms < 0) {
            while (free_queue_.empty() && running_) {
                free_cv_.wait(lock);
            }
        } else {
            auto deadline = std::chrono::steady_clock::now() + 
                           std::chrono::milliseconds(timeout_ms);
            while (free_queue_.empty() && running_) {
                if (free_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                    return nullptr;
                }
            }
        }
    }
    
    if (!running_) {
        return nullptr;
    }
    
    if (free_queue_.empty()) {
        return nullptr;
    }
    
    Buffer* buffer = free_queue_.front();
    free_queue_.pop();
    buffer->setState(Buffer::State::LOCKED_BY_PRODUCER);
    return buffer;
}

void BufferPool::submitFilled(Buffer* buffer_ptr) {
    if (!buffer_ptr) return;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (managed_buffers_.find(buffer_ptr) == managed_buffers_.end()) {
            LOG4CPLUS_WARN_FMT(logger_, "⚠️  Buffer #%u does not belong to pool '%s'",
                   buffer_ptr->id(), name_.c_str());
            return;
        }
        if (buffer_ptr->state() != Buffer::State::LOCKED_BY_PRODUCER) {
            LOG4CPLUS_ERROR_FMT(logger_, "❌ ERROR: submitFilled() called with wrong state: %s (expected LOCKED_BY_PRODUCER)",
                   Buffer::stateToString(buffer_ptr->state()));
            LOG4CPLUS_ERROR_FMT(logger_, "   Buffer #%u in pool '%s'", buffer_ptr->id(), name_.c_str());
            return;
        }
        filled_queue_.push(buffer_ptr);
        buffer_ptr->setState(Buffer::State::READY_FOR_CONSUME);
    }
    filled_cv_.notify_one();
}

void BufferPool::releaseFree(Buffer* buffer_ptr) {
    if (!buffer_ptr) return;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (managed_buffers_.find(buffer_ptr) == managed_buffers_.end()) {
            LOG4CPLUS_WARN_FMT(logger_, "⚠️  Buffer #%u does not belong to pool '%s'",
                   buffer_ptr->id(), name_.c_str());
            return;
        }
        if (buffer_ptr->state() != Buffer::State::LOCKED_BY_PRODUCER) {
            LOG4CPLUS_ERROR_FMT(logger_, "❌ ERROR: releaseFree() called with wrong state: %s (expected LOCKED_BY_PRODUCER)",
                   Buffer::stateToString(buffer_ptr->state()));
            LOG4CPLUS_ERROR_FMT(logger_, "   Buffer #%u in pool '%s'", buffer_ptr->id(), name_.c_str());
            return;
        }
        free_queue_.push(buffer_ptr);
        buffer_ptr->setState(Buffer::State::IDLE);
    }
    free_cv_.notify_one();
}

Buffer* BufferPool::acquireFilled(bool blocking, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (blocking) {
        if (timeout_ms < 0) {
            while (filled_queue_.empty() && running_) {
                filled_cv_.wait(lock);
            }
        } else {
            auto deadline = std::chrono::steady_clock::now() + 
                           std::chrono::milliseconds(timeout_ms);
            while (filled_queue_.empty() && running_) {
                if (filled_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                    return nullptr;
                }
            }
        }
        if (!running_ && filled_queue_.empty()) {
            return nullptr;
        }
    }
    
    if (filled_queue_.empty()) {
        return nullptr;
    }
    
    Buffer* buffer = filled_queue_.front();
    filled_queue_.pop();
    buffer->setState(Buffer::State::LOCKED_BY_CONSUMER);
    return buffer;
}

void BufferPool::releaseFilled(Buffer* buffer) {
    if (!buffer) return;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (managed_buffers_.find(buffer) == managed_buffers_.end()) {
            LOG4CPLUS_WARN_FMT(logger_, "⚠️  Buffer #%u does not belong to pool '%s'",
                   buffer->id(), name_.c_str());
            return;
        }
        if (buffer->state() != Buffer::State::LOCKED_BY_CONSUMER) {
            LOG4CPLUS_ERROR_FMT(logger_, "❌ ERROR: releaseFilled() called with wrong state: %s (expected LOCKED_BY_CONSUMER)",
                   Buffer::stateToString(buffer->state()));
            LOG4CPLUS_ERROR_FMT(logger_, "   Buffer #%u in pool '%s'", buffer->id(), name_.c_str());
            return;
        }
        free_queue_.push(buffer);
        buffer->setState(Buffer::State::IDLE);
        buffer->freeBuffer();
    }
    free_cv_.notify_one();
}

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
    if (!managed_buffers_.empty()) {
        Buffer* first = *managed_buffers_.begin();
        if (first) {
            return first->size();
        }
    }
    return 0;
}

bool BufferPool::addBufferToQueue(Buffer* buffer, QueueType queue) {
    if (!buffer) return false;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (managed_buffers_.find(buffer) != managed_buffers_.end()) {
            LOG4CPLUS_WARN_FMT(logger_, "⚠️  Buffer #%u already in pool '%s'", 
                   buffer->id(), name_.c_str());
            return false;
        }
        managed_buffers_.insert(buffer);
        if (queue == QueueType::FREE) {
            free_queue_.push(buffer);
            buffer->setState(Buffer::State::IDLE);
        } else {
            filled_queue_.push(buffer);
            buffer->setState(Buffer::State::READY_FOR_CONSUME);
        }
    }
    
    if (queue == QueueType::FREE) {
        free_cv_.notify_one();
    } else {
        filled_cv_.notify_one();
    }
    return true;
}

bool BufferPool::removeBufferFromPool(Buffer* buffer) {
    if (!buffer) return false;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (managed_buffers_.find(buffer) == managed_buffers_.end()) {
            return false;
        }
        if (buffer->state() != Buffer::State::IDLE) {
            LOG4CPLUS_WARN_FMT(logger_, "⚠️  Cannot remove buffer #%u: state=%s (must be IDLE)",
                   buffer->id(), Buffer::stateToString(buffer->state()));
            return false;
        }
        bool removed = removeFromQueue(free_queue_, buffer);
        if (!removed) {
            LOG4CPLUS_WARN_FMT(logger_, "⚠️  Buffer #%u not in free_queue", buffer->id());
            return false;
        }
        managed_buffers_.erase(buffer);
    }
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
        } else {
            temp.push(front);
        }
    }
    while (!temp.empty()) {
        queue.push(temp.front());
        temp.pop();
    }
    return found;
}

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
