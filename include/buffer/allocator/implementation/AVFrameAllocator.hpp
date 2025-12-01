#pragma once

#include "../base/BufferAllocatorBase.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

/**
 * @brief AVFrameAllocator - AVFrame 包装分配器
 * 
 * 继承自 BufferAllocatorBase（抽象基类）
 * 
 * 用于将 FFmpeg 解码后的 AVFrame 包装为 Buffer 对象并注入到 BufferPool
 * 
 * 特点：
 * - 虚拟内存：AVFrame->data[0]（FFmpeg 分配）
 * - 物理地址：0（AVFrame 不提供物理地址）
 * - 连续性：不保证
 * 
 * 职责：
 * - 包装 AVFrame 为 Buffer 对象
 * - 管理 AVFrame 的生命周期（av_frame_free）
 * - 支持动态注入到 BufferPool
 * 
 * 使用场景：
 * - RTSP 实时流解码
 * - FFmpeg 视频解码
 * - 需要动态创建 Buffer 的场景
 * 
 * 使用示例：
 * @code
 * // 1. 创建 Allocator
 * auto allocator = std::make_unique<AVFrameAllocator>();
 * 
 * // 2. 创建空的 Pool
 * auto pool = BufferPool::CreateEmpty("RTSP_Pool", "RTSP");
 * 
 * // 3. 解码后动态注入
 * AVFrame* frame = av_frame_alloc();
 * // ... 解码 ...
 * 
 * // 4. 注入到 pool（自动包装为 Buffer）
 * Buffer* buf = allocator->injectAVFrameToPool(frame, pool.get());
 * 
 * // 5. 消费者使用
 * Buffer* filled = pool->acquireFilled(true);
 * // ... 使用 filled->getVirtualAddress() ...
 * pool->releaseFilled(filled);
 * @endcode
 */
class AVFrameAllocator : public BufferAllocatorBase {
public:
    /**
     * @brief 构造函数
     */
    AVFrameAllocator();
    
    ~AVFrameAllocator() override;
    
    /**
     * @brief 将 AVFrame 包装为 Buffer 并注入到 Pool
     * 
     * 工作流程：
     * 1. 生成唯一 Buffer ID
     * 2. 从 AVFrame 提取虚拟地址和大小
     * 3. 创建 Buffer 对象（Ownership::EXTERNAL）
     * 4. 将 Buffer 添加到 pool 的 filled 队列
     * 5. 记录 AVFrame 和 Buffer 的映射（用于释放）
     * 
     * @param frame AVFrame 指针（所有权转移给 Allocator）
     * @param pool 目标 BufferPool
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     * 
     * @note AVFrame 的生命周期由 Allocator 管理
     * @note 当 Buffer 被 releaseFilled 后，AVFrame 不会自动释放
     * @note 需要调用 releaseAVFrame() 或 removeBufferFromPool() 来释放 AVFrame
     */
    Buffer* injectAVFrameToPool(AVFrame* frame, BufferPool* pool);
    
    /**
     * @brief 释放与 Buffer 关联的 AVFrame
     * 
     * 工作流程：
     * 1. 查找 Buffer 对应的 AVFrame
     * 2. 调用 av_frame_free() 释放 AVFrame
     * 3. 从 pool 移除 Buffer
     * 4. 删除 Buffer 对象
     * 
     * @param buffer Buffer 指针
     * @param pool 所属的 BufferPool
     * @return bool 成功返回 true
     * 
     * @note 此方法会同时释放 AVFrame 和 Buffer
     */
    bool releaseAVFrame(Buffer* buffer, BufferPool* pool);
    
    // ==================== 实现基类纯虚函数 ====================
    
    /**
     * @brief 批量创建 Buffer 并构建 BufferPool
     * 
     * @note AVFrameAllocator 主要用于动态注入，此方法创建空的 BufferPool
     * v2.0: @return uint64_t 返回 pool_id，Registry 持有 Pool
     */
    uint64_t allocatePoolWithBuffers(
        int count,
        size_t size,
        const std::string& name,
        const std::string& category = ""
    ) override;
    
    /**
     * @brief 创建单个 Buffer 并注入到指定 BufferPool（内部分配）
     * 
     * v2.0: @param pool_id BufferPool ID（从 Registry 获取）
     * @note AVFrameAllocator 不支持此方法，应使用 injectAVFrameToPool
     */
    Buffer* injectBufferToPool(
        uint64_t pool_id,
        size_t size,
        QueueType queue = QueueType::FREE
    ) override;
    
    /**
     * @brief 注入外部已分配的内存到 BufferPool（外部注入）
     * 
     * v2.0: @param pool_id BufferPool ID（从 Registry 获取）
     * @note AVFrameAllocator 支持此方法，可以包装外部内存为 Buffer
     */
    Buffer* injectExternalBufferToPool(
        uint64_t pool_id,
        void* virt_addr,
        uint64_t phys_addr,
        size_t size,
        QueueType queue = QueueType::FREE
    ) override;
    
    /**
     * @brief 从 BufferPool 移除并销毁 Buffer
     * 
     * v2.0: @param pool_id BufferPool ID
     */
    bool removeBufferFromPool(uint64_t pool_id, Buffer* buffer) override;
    
    /**
     * @brief 销毁整个 BufferPool 及其所有 Buffer
     * 
     * v2.0: @param pool_id BufferPool ID
     */
    bool destroyPool(uint64_t pool_id) override;
    
protected:
    /**
     * @brief 创建单个 Buffer（从 AVFrame）
     * 
     * 注意：此方法不应该被直接调用
     * 应该使用 injectAVFrameToPool() 代替
     * 
     * @param id Buffer ID
     * @param size Buffer 大小
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     */
    Buffer* createBuffer(uint32_t id, size_t size) override;
    
    /**
     * @brief 销毁 Buffer（释放 AVFrame）
     * 
     * 实现逻辑：
     * 1. 查找 Buffer 对应的 AVFrame
     * 2. 调用 av_frame_free() 释放 AVFrame
     * 3. 删除 Buffer 对象
     * 
     * @param buffer 要销毁的 Buffer
     */
    void deallocateBuffer(Buffer* buffer) override;
    
private:
    // Buffer ID 计数器（用于生成唯一 ID）
    std::atomic<uint32_t> next_buffer_id_;
    
    // Buffer → AVFrame 映射（用于释放时查找）
    std::unordered_map<Buffer*, AVFrame*> buffer_to_frame_;
    std::mutex mapping_mutex_;
};

