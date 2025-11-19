#pragma once

#include "../../Buffer.hpp"
#include "../../BufferPool.hpp"
#include <memory>
#include <unordered_map>
#include <mutex>

/**
 * @brief Buffer 内存分配器类型枚举
 * 
 * 定义支持的内存分配策略：
 * - NORMAL_MALLOC: 普通堆内存（malloc/posix_memalign）
 * - CMA: CMA 连续物理内存
 * - DMA_HEAP: DMA-HEAP 分配器
 * - TACO_SYS: TACO 系统专用分配器
 */
enum class BufferMemoryAllocatorType {
    NORMAL_MALLOC = 0,
    CMA = 1,
    DMA_HEAP = 2,
    TACO_SYS = 3
};

/**
 * @brief BufferAllocatorBase - Buffer分配器基类（纯抽象接口类）
 * 
 * 架构角色：抽象基类（Abstract Base Class / Interface）
 * 
 * 设计模式：接口隔离原则（ISP）
 * 
 * 职责：
 * - 定义所有Allocator必须实现的接口
 * - 保证接口的完整性和一致性
 * - 作为所有Allocator实现类的统一基类
 * 
 * 设计原则：
 * - 纯抽象基类：所有方法都是纯虚函数（= 0）
 * - 子类必须实现所有纯虚函数
 * - 作为 BufferPool 的友元，可以调用其私有方法
 * - 不持有长期锁（避免死锁）
 * - 所有队列操作委托给 BufferPool（BufferPool 内部加锁）
 * 
 * 子类实现：
 * - NormalAllocator: 普通内存分配器
 * - AVFrameAllocator: AVFrame包装分配器
 * - FramebufferAllocator: Framebuffer内存包装分配器
 * 
 * 使用示例：
 * @code
 * // 通过Factory创建
 * auto allocator = BufferAllocatorFactory::create(BufferAllocatorFactory::AllocatorType::NORMAL);
 * auto pool = allocator->allocatePoolWithBuffers(10, 1024*1024, "MyPool", "Video");
 * 
 * // 单个注入（扩容）
 * Buffer* buf = allocator->injectBufferToPool(1024*1024, pool.get(), QueueType::FREE);
 * 
 * // 移除（缩容）
 * allocator->removeBufferFromPool(buf, pool.get());
 * 
 * // 销毁Pool
 * allocator->destroyPool(pool.get());
 * @endcode
 */
class BufferAllocatorBase {
public:
    virtual ~BufferAllocatorBase() = default;
    
    // ==================== 纯虚函数接口（子类必须实现）====================
    
    /**
     * @brief 批量创建 Buffer 并构建 BufferPool
     * 
     * 工作流程：
     * 1. 创建空的 BufferPool（CreateEmpty）
     * 2. 循环创建 Buffer（调用 createBuffer）
     * 3. 将 Buffer 添加到 pool 的 free 队列（调用 addBufferToQueue）
     * 4. 记录所有权
     * 
     * @param count Buffer 数量
     * @param size 每个 Buffer 大小
     * @param name BufferPool 名称
     * @param category BufferPool 分类
     * @return unique_ptr<BufferPool> 成功返回 pool，失败返回 nullptr
     * 
     * @note 线程安全：是（BufferPool 内部加锁）
     * @note 失败时自动清理已分配的 buffer
     */
    virtual std::unique_ptr<BufferPool> allocatePoolWithBuffers(
        int count,
        size_t size,
        const std::string& name,
        const std::string& category = ""
    ) = 0;
    
    /**
     * @brief 创建单个 Buffer 并注入到指定 BufferPool
     * 
     * 适用场景：
     * - 动态扩容：向已有 pool 添加新 buffer
     * - 动态注入：RTSP/FFmpeg 解码后注入
     * 
     * 工作流程：
     * 1. 创建 Buffer（调用 createBuffer）
     * 2. 将 Buffer 添加到 pool 的指定队列（调用 addBufferToQueue）
     * 3. 记录所有权
     * 
     * @param size Buffer 大小
     * @param pool 目标 BufferPool
     * @param queue 注入到哪个队列（FREE 或 FILLED）
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     * 
     * @note 线程安全：是（BufferPool 内部加锁）
     */
    virtual Buffer* injectBufferToPool(
        size_t size,
        BufferPool* pool,
        QueueType queue = QueueType::FREE
    ) = 0;
    
    /**
     * @brief 从 BufferPool 移除并销毁 Buffer
     * 
     * 适用场景：
     * - 动态缩容：减少 pool 中的 buffer 数量
     * 
     * 工作流程：
     * 1. 从 pool 移除 Buffer（调用 removeBufferFromPool）
     * 2. 销毁 Buffer（调用 deallocateBuffer）
     * 3. 清除所有权记录
     * 
     * @param buffer 要移除的 Buffer
     * @param pool 所属的 BufferPool
     * @return true 成功，false 失败（buffer 正在使用或不在 pool 中）
     * 
     * @note 线程安全：是（BufferPool 内部加锁）
     * @note 只能移除 IDLE 状态的 buffer
     */
    virtual bool removeBufferFromPool(Buffer* buffer, BufferPool* pool) = 0;
    
    /**
     * @brief 销毁整个 BufferPool 及其所有 Buffer
     * 
     * 适用场景：
     * - 清理资源：销毁不再使用的 BufferPool
     * - 析构时清理：Allocator 析构时清理所有创建的 Pool
     * 
     * 工作流程：
     * 1. 从 pool 移除所有 Buffer
     * 2. 销毁所有 Buffer（调用 deallocateBuffer）
     * 3. 清除所有权记录
     * 4. 销毁 BufferPool 对象（如果由 Allocator 创建）
     * 
     * @param pool 要销毁的 BufferPool
     * @return true 成功，false 失败
     * 
     * @note 线程安全：是（BufferPool 内部加锁）
     * @note 只能销毁所有 Buffer 都处于 IDLE 状态的 pool
     */
    virtual bool destroyPool(BufferPool* pool) = 0;
    
protected:
    // ==================== 子类必须实现的核心方法 ====================
    
    /**
     * @brief 创建单个 Buffer（核心分配逻辑）
     * 
     * 子类实现要求：
     * - 分配指定大小的内存
     * - 获取物理地址（如果支持）
     * - 创建并返回 Buffer 对象
     * 
     * @param id Buffer ID
     * @param size Buffer 大小
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     */
    virtual Buffer* createBuffer(uint32_t id, size_t size) = 0;
    
    /**
     * @brief 销毁 Buffer（核心释放逻辑）
     * 
     * 子类实现要求：
     * - 释放 buffer 的内存
     * - 删除 Buffer 对象
     * 
     * @param buffer 要销毁的 Buffer
     */
    virtual void deallocateBuffer(Buffer* buffer) = 0;
    
    // ==================== 友元访问辅助方法（供子类使用）====================
    // 这些方法通过友元关系访问 BufferPool 的私有方法
    
    /**
     * @brief 将 Buffer 添加到 BufferPool 的指定队列
     * 
     * @param pool BufferPool 指针
     * @param buffer Buffer 指针
     * @param queue 队列类型
     * @return bool 成功返回 true
     */
    static bool addBufferToPoolQueue(BufferPool* pool, Buffer* buffer, QueueType queue) {
        if (!pool || !buffer) {
            return false;
        }
        // 通过友元关系访问 BufferPool 的私有方法
        return pool->addBufferToQueue(buffer, queue);
    }
    
    /**
     * @brief 从 BufferPool 移除 Buffer
     * 
     * @param pool BufferPool 指针
     * @param buffer Buffer 指针
     * @return bool 成功返回 true
     */
    static bool removeBufferFromPoolInternal(BufferPool* pool, Buffer* buffer) {
        if (!pool || !buffer) {
            return false;
        }
        // 通过友元关系访问 BufferPool 的私有方法
        return pool->removeBufferFromPool(buffer);
    }
};
