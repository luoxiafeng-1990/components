#pragma once

#include "bufferpool/pool/base/IBufferPoolBuilder.hpp"
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

#include <opencv2/opencv.hpp>
#include "opencv2/core/tacv.hpp"

/**
 * @brief MatPoolBuilder - OpenCV cv::Mat 包装构建器
 *
 * 将 OpenCV 处理后的 cv::Mat 包装为 Buffer 对象并注入到 BufferPool。
 */
class MatPoolBuilder : public IBufferPoolBuilder {
public:
    MatPoolBuilder();
    ~MatPoolBuilder() override;

    Buffer* injectMatToPool(cv::Mat* frame, BufferPool* pool);
    bool releaseMat(Buffer* buffer, BufferPool* pool);

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
