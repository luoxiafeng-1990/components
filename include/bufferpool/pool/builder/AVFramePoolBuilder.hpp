#pragma once

#include "bufferpool/pool/base/IBufferPoolBuilder.hpp"
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

/**
 * @brief AVFramePoolBuilder - AVFrame 包装构建器
 *
 * 将 FFmpeg 解码后的 AVFrame 包装为 Buffer 对象并注入到 BufferPool。
 */
class AVFramePoolBuilder : public IBufferPoolBuilder {
public:
    AVFramePoolBuilder();
    ~AVFramePoolBuilder() override;

    Buffer* injectAVFrameToPool(AVFrame* frame, BufferPool* pool);
    bool releaseAVFrame(Buffer* buffer, BufferPool* pool);

    uint64_t allocatePoolWithBuffers(
        int count, size_t size,
        const std::string& name, const std::string& category = "") override;

    Buffer* injectBufferToPool(
        uint64_t pool_id, size_t size,
        QueueType queue = QueueType::FREE) override;

    Buffer* injectExternalBufferToPool(
        uint64_t pool_id, void* virt_addr, uint64_t phys_addr,
        size_t size, QueueType queue = QueueType::FREE,
        uint32_t custom_id = 0) override;

    bool removeBufferFromPool(uint64_t pool_id, Buffer* buffer) override;
    bool destroyPool() override;

protected:
    Buffer* createBuffer(uint32_t id, size_t size) override;
    void deallocateBuffer(Buffer* buffer) override;

private:
    std::atomic<uint32_t> next_buffer_id_;
    std::mutex mapping_mutex_;
    log4cplus::Logger logger_;
};
