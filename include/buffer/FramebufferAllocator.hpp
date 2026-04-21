#pragma once

#include "buffer/BufferAllocatorBase.hpp"
#include "vendor/contracts/IMemoryProvider.hpp"
#include <memory>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

/**
 * @brief FramebufferAllocator - TACO 物理连续内存 BufferPool 构建器
 * 
 * v3.0 架构变更：
 * - 通过构造函数注入 IMemoryProvider（依赖注入）
 * - createBuffer / deallocateBuffer 委托给 IMemoryProvider
 * - 默认构造函数从 MemoryProviderRegistry 查找 "taco" provider
 */
class FramebufferAllocator : public BufferAllocatorBase {
public:
    /**
     * @brief v3.0 推荐构造函数：注入内存提供者
     */
    explicit FramebufferAllocator(std::unique_ptr<IMemoryProvider> provider);

    /**
     * @brief 旧构造函数（向后兼容，从 Registry 查找 "taco" provider）
     * @deprecated 请使用 FramebufferAllocator(unique_ptr<IMemoryProvider>) 代替
     */
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
    std::unique_ptr<IMemoryProvider> memory_provider_;
    log4cplus::Logger logger_;
};
