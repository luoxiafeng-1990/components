#pragma once

#include "bufferpool/buffer/Buffer.hpp"
#include <string>
#include <vector>
#include <queue>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <atomic>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

class IBufferPoolBuilder;

/**
 * @brief BufferPool 类型枚举
 */
enum class BufferPoolType {
    DECODE_VIDEO_PRIMARY,
    DECODE_VIDEO_SECONDARY,
    DECODE_VIDEO_THUMBNAIL,
    DECODE_VIDEO_PREVIEW,
    DECODE_AUDIO_PRIMARY,
    DECODE_AUDIO_SECONDARY,
    PACKET_VIDEO,
    PACKET_AUDIO,
    PACKET_SUBTITLE,
    ENCODE_VIDEO_INPUT,
    ENCODE_VIDEO_OUTPUT,
    ENCODE_AUDIO_INPUT,
    ENCODE_AUDIO_OUTPUT,
    RAW_FILE_READ,
    FRAMEBUFFER_OUTPUT,
    NETWORK_STREAM,
    CUSTOM_1,
    CUSTOM_2,
    CUSTOM_3
};

inline const char* bufferPoolTypeToString(BufferPoolType type) {
    switch (type) {
        case BufferPoolType::DECODE_VIDEO_PRIMARY:    return "DECODE_VIDEO_PRIMARY";
        case BufferPoolType::DECODE_VIDEO_SECONDARY:  return "DECODE_VIDEO_SECONDARY";
        case BufferPoolType::DECODE_VIDEO_THUMBNAIL:  return "DECODE_VIDEO_THUMBNAIL";
        case BufferPoolType::DECODE_VIDEO_PREVIEW:    return "DECODE_VIDEO_PREVIEW";
        case BufferPoolType::DECODE_AUDIO_PRIMARY:    return "DECODE_AUDIO_PRIMARY";
        case BufferPoolType::DECODE_AUDIO_SECONDARY:  return "DECODE_AUDIO_SECONDARY";
        case BufferPoolType::PACKET_VIDEO:            return "PACKET_VIDEO";
        case BufferPoolType::PACKET_AUDIO:            return "PACKET_AUDIO";
        case BufferPoolType::PACKET_SUBTITLE:         return "PACKET_SUBTITLE";
        case BufferPoolType::ENCODE_VIDEO_INPUT:      return "ENCODE_VIDEO_INPUT";
        case BufferPoolType::ENCODE_VIDEO_OUTPUT:     return "ENCODE_VIDEO_OUTPUT";
        case BufferPoolType::ENCODE_AUDIO_INPUT:      return "ENCODE_AUDIO_INPUT";
        case BufferPoolType::ENCODE_AUDIO_OUTPUT:     return "ENCODE_AUDIO_OUTPUT";
        case BufferPoolType::RAW_FILE_READ:           return "RAW_FILE_READ";
        case BufferPoolType::FRAMEBUFFER_OUTPUT:      return "FRAMEBUFFER_OUTPUT";
        case BufferPoolType::NETWORK_STREAM:          return "NETWORK_STREAM";
        case BufferPoolType::CUSTOM_1:                return "CUSTOM_1";
        case BufferPoolType::CUSTOM_2:                return "CUSTOM_2";
        case BufferPoolType::CUSTOM_3:                return "CUSTOM_3";
        default:                                      return "UNKNOWN";
    }
}

enum class QueueType {
    FREE,
    FILLED
};

/**
 * @brief BufferPool - 纯调度器
 * 
 * 职责：
 * - 管理 Buffer 队列（free_queue, filled_queue）
 * - 提供线程安全的调度接口
 * - 不关心 Buffer 来源和生命周期
 */
class BufferPool {
public:
    class PrivateToken {
    private:
        PrivateToken() = default;
        friend class IBufferPoolBuilder;
    };
    
    BufferPool(
        PrivateToken token,
        const std::string& name,
        const std::string& category
    );
    
    ~BufferPool();
    
public:
    Buffer* acquireFree(bool blocking = true, int timeout_ms = -1);
    void submitFilled(Buffer* buffer_ptr);
    void releaseFree(Buffer* buffer_ptr);
    
    Buffer* acquireFilled(bool blocking = true, int timeout_ms = -1);
    void releaseFilled(Buffer* buffer_ptr);
    
    int getFreeCount() const;
    int getFilledCount() const;
    int getTotalCount() const;
    
    const std::string& getName() const { return name_; }
    const std::string& getCategory() const { return category_; }
    uint64_t getRegistryId() const { return registry_id_; }
    void setRegistryId(uint64_t id) { registry_id_ = id; }
    size_t getBufferSize() const;
    
    const std::unordered_set<Buffer*>& getAllManagedBuffers() const {
        return managed_buffers_;
    }
    
    void clearAllManagedBuffers();
    void shutdown();
    bool isRunning() const { return running_.load(); }
    void printStats() const;
    
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    
private:
    friend class IBufferPoolBuilder;
    
    bool addBufferToQueue(Buffer* buffer, QueueType queue);
    bool removeBufferFromPool(Buffer* buffer);
    bool removeFromQueue(std::queue<Buffer*>& queue, Buffer* target);
    
    std::string name_;
    std::string category_;
    uint64_t registry_id_;
    
    std::unordered_set<Buffer*> managed_buffers_;
    std::queue<Buffer*> free_queue_;
    std::queue<Buffer*> filled_queue_;
    
    mutable std::mutex mutex_;
    std::condition_variable free_cv_;
    std::condition_variable filled_cv_;
    std::atomic<bool> running_;
    
    std::string log_prefix_;
    log4cplus::Logger logger_;
};
