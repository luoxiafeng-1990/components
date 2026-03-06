#pragma once

#include "buffer/BufferAllocatorBase.hpp"
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

/**
 * @brief FramebufferAllocator - TACO 物理连续内存分配器
 * 
 * 内部通过 taco_sys_get_block / handle2_phys / mmap_noncache
 * 为每个 buffer 独立分配物理连续内存。
 * 
 * Buffer::id() == taco blk_id，destroyPool 时自动 munmap + release_block。
 * 
 * 使用示例：
 * @code
 * BufferAllocatorFacade facade(BufferAllocatorFactory::AllocatorType::FRAMEBUFFER);
 * uint64_t pool_id = facade.allocatePoolWithBuffers(4, frame_size, "FBPool", "Display");
 * // ... 使用 pool ...
 * // facade 析构时自动 destroyPool → taco_sys_munmap + taco_sys_release_block
 * @endcode
 */
class FramebufferAllocator : public BufferAllocatorBase {
public:
    FramebufferAllocator();
    ~FramebufferAllocator() override;
    
    /**
     * @brief 分配 BufferPool，内部调用 taco_sys_* 分配物理连续内存
     * 
     * @param count  buffer 数量（必须 > 0）
     * @param size   每个 buffer 大小（必须 > 0）
     * @param name   BufferPool 名称（同时用作 taco_sys_get_block 的 zone_name）
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
     * @brief 销毁所有 BufferPool，自动 taco_sys_munmap + taco_sys_release_block
     */
    bool destroyPool() override;
    
protected:
    Buffer* createBuffer(uint32_t id, size_t size) override;
    void deallocateBuffer(Buffer* buffer) override;
    
private:
    log4cplus::Logger logger_;
};
