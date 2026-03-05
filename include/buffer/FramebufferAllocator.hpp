#pragma once

#include "buffer/BufferAllocatorBase.hpp"
#include <vector>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

/**
 * @brief FramebufferAllocator - 物理连续 Framebuffer 内存分配器
 * 
 * 继承自 BufferAllocatorBase（抽象基类）
 * 
 * 两种工作模式：
 *   1. TACO 分配模式（推荐）：调用者指定 count + size，内部通过
 *      taco_sys_get_block / handle2_phys / mmap_noncache 分配物理连续内存
 *   2. 外部包装模式：调用者预先分配好内存，通过 BufferInfo 列表传入
 * 
 * 使用示例（TACO 分配模式）：
 * @code
 * BufferAllocatorFacade facade(BufferAllocatorFactory::AllocatorType::FRAMEBUFFER);
 * uint64_t pool_id = facade.allocatePoolWithBuffers(4, frame_size, "FBPool", "Display");
 * @endcode
 * 
 * 使用示例（外部包装模式）：
 * @code
 * auto allocator = std::make_unique<FramebufferAllocator>(infos);
 * uint64_t pool_id = allocator->allocatePoolWithBuffers(0, 0, "FBPool", "Display");
 * @endcode
 */
class FramebufferAllocator : public BufferAllocatorBase {
public:
    /**
     * @brief 外部 Buffer 信息结构（外部包装模式使用）
     */
    struct BufferInfo {
        void* virt_addr;
        uint64_t phys_addr;
        size_t size;
    };
    
    FramebufferAllocator();
    
    explicit FramebufferAllocator(const std::vector<BufferInfo>& external_buffers);
    
    ~FramebufferAllocator() override;
    
    // ==================== 实现基类纯虚函数 ====================
    
    /**
     * @brief 分配 BufferPool 及其 Buffer
     * 
     * - count > 0 且 size > 0：TACO 分配模式，内部调用 taco_sys_* 分配物理连续内存
     * - count == 0：外部包装模式，使用构造时传入的 external_buffers_
     * 
     * TACO 模式下 Buffer::id() == taco blk_id，destroyPool 时自动 munmap + release
     *
     * @param count   buffer 数量（0 = 使用 external_buffers_）
     * @param size    每个 buffer 大小（TACO 模式下必须 > 0）
     * @param name    BufferPool 名称
     * @param category BufferPool 分类
     * @return pool_id，失败返回 0
     */
    uint64_t allocatePoolWithBuffers(
        int count,
        size_t size,
        const std::string& name,
        const std::string& category = ""
    ) override;
    
    Buffer* injectBufferToPool(
        uint64_t pool_id,
        size_t size,
        QueueType queue = QueueType::FREE
    ) override;
    
    Buffer* injectExternalBufferToPool(
        uint64_t pool_id,
        void* virt_addr,
        uint64_t phys_addr,
        size_t size,
        QueueType queue = QueueType::FREE,
        uint32_t custom_id = 0
    ) override;
    
    bool removeBufferFromPool(uint64_t pool_id, Buffer* buffer) override;
    
    /**
     * @brief 销毁所有 BufferPool 及其 Buffer
     * 
     * TACO 分配模式下自动执行 taco_sys_munmap + taco_sys_release_block
     */
    bool destroyPool() override;
    
protected:
    Buffer* createBuffer(uint32_t id, size_t size) override;
    void deallocateBuffer(Buffer* buffer) override;
    
private:
    bool taco_allocated_ = false;
    
    std::vector<BufferInfo> external_buffers_;
    size_t next_buffer_index_;
    
    log4cplus::Logger logger_;
};

