#pragma once

#include "buffer/BufferAllocatorBase.hpp"
#include <cstdlib>

/**
 * @brief NormalAllocator - 普通内存分配器
 * 
 * 继承自 BufferAllocatorBase（抽象基类）
 * 
 * 使用标准 C++ 内存分配（malloc/posix_memalign）创建 Buffer
 * 
 * 特点：
 * - 虚拟内存：是
 * - 物理地址：否（phys_addr = 0）
 * - 连续性：不保证物理连续
 * 
 * 使用场景：
 * - CPU 处理的普通数据缓冲
 * - 不需要 DMA 访问的场景
 * 
 * 使用示例：
 * @code
 * auto allocator = std::make_unique<NormalAllocator>(BufferMemoryAllocatorType::NORMAL_MALLOC);
 * auto pool = allocator->allocatePoolWithBuffers(10, 1920*1080*3, "VideoPool", "Video");
 * @endcode
 */
class NormalAllocator : public BufferAllocatorBase {
public:
    /**
     * @brief 构造函数
     * 
     * @param type 分配器类型（通常为 NORMAL_MALLOC）
     * @param alignment 内存对齐（默认 64 字节）
     */
    explicit NormalAllocator(
        BufferMemoryAllocatorType type = BufferMemoryAllocatorType::NORMAL_MALLOC,
        size_t alignment = 64
    );
    
    ~NormalAllocator() override;
    
    // ==================== 实现基类纯虚函数 ====================
    
    /**
     * @brief 批量创建 Buffer 并构建 BufferPool
     * 
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
     * v2.0: @param pool_id BufferPool ID（从 Registry 获取 Pool）
     */
    Buffer* injectBufferToPool(
        uint64_t pool_id,
        size_t size,
        QueueType queue = QueueType::FREE
    ) override;
    
    /**
     * @brief 注入外部已分配的内存到 BufferPool（外部注入）
     * 
     * v2.0: @param pool_id BufferPool ID（从 Registry 获取 Pool）
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
     * v2.0: @param pool_id BufferPool ID（从 Registry 获取 Pool）
     */
    bool removeBufferFromPool(uint64_t pool_id, Buffer* buffer) override;
    
    /**
     * @brief 销毁所有 BufferPool 及其所有 Buffer
     * 
     * 自动查询 Registry 获取所有属于此 Allocator 的 Pool，逐个清理。
     */
    bool destroyPool() override;
    
private:
    /**
     * @brief v2.0: 清理临时 Pool 中所有属于此 Allocator 的 buffer（创建失败时使用）
     */
    void cleanupPoolTemp(BufferPool* pool);
    
protected:
    /**
     * @brief 创建单个 Buffer（分配内存）
     * 
     * 实现逻辑：
     * 1. 使用 posix_memalign 分配对齐内存
     * 2. 创建 Buffer 对象（phys_addr = 0）
     * 3. 返回 Buffer 指针
     * 
     * @param id Buffer ID
     * @param size Buffer 大小
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     */
    Buffer* createBuffer(uint32_t id, size_t size) override;
    
    /**
     * @brief 销毁 Buffer（释放内存）
     * 
     * 实现逻辑：
     * 1. 释放 buffer 的内存（free）
     * 2. 删除 Buffer 对象
     * 
     * @param buffer 要销毁的 Buffer
     */
    void deallocateBuffer(Buffer* buffer) override;
    
private:
    BufferMemoryAllocatorType type_;
    size_t alignment_;
};

