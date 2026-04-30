#pragma once

#include "bufferpool/pool/base/IBufferPoolBuilder.hpp"
#include "bufferpool/buffer/Buffer.hpp"
#include "vendor/contracts/IMemoryProvider.hpp"
#include <memory>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavcodec/packet.h>
}

#include <opencv2/opencv.hpp>

/**
 * @brief BufferPoolBuilder - 统一的 BufferPool 构建器
 *
 * 替代原来的 AVFramePoolBuilder / MatPoolBuilder / ContinuousPhysicalPoolBuilder，
 * 通过 Buffer::Type 分派创建逻辑，消除三者 80% 的重复代码。
 *
 * 使用方式：
 *   auto builder = BufferPoolBuilder::forAVFrame();
 *   auto builder = BufferPoolBuilder::forMat();
 *   auto builder = BufferPoolBuilder::forPhysicalMemory(std::move(provider));
 */
class BufferPoolBuilder : public IBufferPoolBuilder {
public:
    // === 静态工厂方法 ===
    static std::unique_ptr<BufferPoolBuilder> forAVFrame();
    static std::unique_ptr<BufferPoolBuilder> forMat();
    static std::unique_ptr<BufferPoolBuilder> forPhysicalMemory(
        std::unique_ptr<IMemoryProvider> provider);

    ~BufferPoolBuilder() override;

    // === IBufferPoolBuilder 接口 ===
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

    // === 类型特化便捷方法 ===
    Buffer* injectAVFrameToPool(AVFrame* frame, BufferPool* pool);
    bool releaseAVFrame(Buffer* buffer, BufferPool* pool);
    Buffer* injectMatToPool(cv::Mat* mat, BufferPool* pool);
    bool releaseMat(Buffer* buffer, BufferPool* pool);

    Buffer::Type getBufferType() const { return buffer_type_; }

protected:
    Buffer* createBuffer(uint32_t id, size_t size) override;
    void deallocateBuffer(Buffer* buffer) override;

private:
    BufferPoolBuilder(Buffer::Type type,
                      std::unique_ptr<IMemoryProvider> provider = nullptr);

    Buffer::Type buffer_type_;
    std::unique_ptr<IMemoryProvider> memory_provider_;
    size_t alignment_ = 64;
    std::atomic<uint32_t> next_buffer_id_{0};
    log4cplus::Logger logger_;

    // 统一所有权追踪（替代原来 3 个独立 static map）
    static std::unordered_map<Buffer*, IBufferPoolBuilder*> buffer_ownership_;
    static std::mutex ownership_mutex_;

    void cleanupPoolTemp(BufferPool* pool);
};
