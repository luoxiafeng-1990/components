#pragma once

#include "../../Buffer.hpp"
#include "../../BufferPool.hpp"
#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <cstddef>
#include <string>

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
     * 4. Allocator 持有 shared_ptr（管理生命周期）
     * 5. 自动注册到 BufferPoolRegistry
     * 
     * @param count Buffer 数量
     * @param size 每个 Buffer 大小
     * @param name BufferPool 名称
     * @param category BufferPool 分类
     * @return shared_ptr<BufferPool> 成功返回 pool，失败返回 nullptr
     * 
     * @note 线程安全：是（BufferPool 内部加锁）
     * @note 失败时自动清理已分配的 buffer
     * @note Allocator 持有 shared_ptr，管理 BufferPool 的生命周期
     */
    virtual std::shared_ptr<BufferPool> allocatePoolWithBuffers(
        int count,
        size_t size,
        const std::string& name,
        const std::string& category = ""
    ) = 0;
    
    /**
     * @brief 创建单个 Buffer 并注入到指定 BufferPool（内部分配）
     * 
     * 适用场景：
     * - 动态扩容：向已有 pool 添加新 buffer
     * - 内部分配：Allocator 自己分配内存
     * 
     * 工作流程：
     * 1. 创建 Buffer（调用 createBuffer，Allocator 内部分配内存）
     * 2. 将 Buffer 添加到 pool 的指定队列（调用 addBufferToQueue）
     * 3. 将 Buffer 添加到 pool 的 managed_buffers_ 集合
     * 4. 记录所有权
     * 
     * @param size Buffer 大小
     * @param pool 目标 BufferPool
     * @param queue 注入到哪个队列（FREE 或 FILLED）
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     * 
     * @note 线程安全：是（BufferPool 内部加锁）
     * @note addBufferToQueue 会自动将 Buffer 添加到 managed_buffers_ 集合
     */
    virtual Buffer* injectBufferToPool(
        size_t size,
        BufferPool* pool,
        QueueType queue = QueueType::FREE
    ) = 0;
    
    /**
     * @brief 注入外部已分配的内存到 BufferPool（外部注入）
     * 
     * 适用场景：
     * - 外部内存包装：将外部已分配的内存包装为 Buffer 对象
     * - Framebuffer 内存：将 Framebuffer 设备内存注入到 Pool
     * - 零拷贝场景：直接使用外部内存，避免拷贝
     * 
     * 工作流程：
     * 1. 创建 Buffer 对象（包装外部内存，Ownership::EXTERNAL）
     * 2. 将 Buffer 添加到 pool 的指定队列（调用 addBufferToQueue）
     * 3. 将 Buffer 添加到 pool 的 managed_buffers_ 集合
     * 4. 记录所有权（如果需要）
     * 
     * @param virt_addr 外部内存的虚拟地址（已分配）
     * @param phys_addr 外部内存的物理地址（如果支持，否则为 0）
     * @param size 外部内存的大小（字节）
     * @param pool 目标 BufferPool
     * @param queue 注入到哪个队列（FREE 或 FILLED）
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     * 
     * @note 线程安全：是（BufferPool 内部加锁）
     * @note 外部内存的生命周期由外部管理，Allocator 只负责创建 Buffer 对象
     * @note Buffer 对象的 ownership 为 EXTERNAL
     * @note addBufferToQueue 会自动将 Buffer 添加到 managed_buffers_ 集合
     */
    virtual Buffer* injectExternalBufferToPool(
        void* virt_addr,
        uint64_t phys_addr,
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
    // ==================== Allocator 管理的 BufferPool ====================
    
    /**
     * @brief Allocator 持有的 BufferPool
     * 
     * 设计原则：
     * - 一个 Allocator 只管理一个 BufferPool（YAGNI 原则）
     * - 通过 allocatePoolWithBuffers() 创建并存储
     * 
     * 线程安全：
     * - 使用 managed_pool_mutex_ 保护
     */
    std::shared_ptr<BufferPool> managed_pool_;
    
    /**
     * @brief 保护 managed_pool_ 的互斥锁
     */
    mutable std::mutex managed_pool_mutex_;
    
    /**
     * @brief 创建 BufferPool（供子类使用）
     * 
     * 设计原则：
     * - 通过 friend 访问 BufferPool 的 private 构造函数
     * - 自动注册到 BufferPoolRegistry
     * - 子类在 allocatePoolWithBuffers() 中调用此方法
     * 
     * @param name Pool 名称
     * @param category Pool 分类
     * @return shared_ptr<BufferPool> 创建的 BufferPool
     * 
     * @note 此方法是 protected static，只能被子类调用
     */
    static std::shared_ptr<BufferPool> createBufferPool(
        const std::string& name,
        const std::string& category
    );
    
public:
    /**
     * @brief 获取当前管理的 BufferPool
     * 
     * @return shared_ptr<BufferPool> 返回 managed_pool_，如果未创建则返回 nullptr
     * 
     * @note 线程安全：是（内部加锁）
     */
    std::shared_ptr<BufferPool> getManagedBufferPool() const {
        std::lock_guard<std::mutex> lock(managed_pool_mutex_);
        return managed_pool_;
    }
    
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
    
    // ==================== Passkey Token（供子类创建 BufferPool）====================
    
    /**
     * @brief 创建 BufferPool 的通行证 Token
     * 
     * 设计模式：Passkey Idiom
     * 
     * 原理：
     * - BufferAllocatorBase 是 BufferPool::PrivateToken 的 friend
     * - 子类可以通过这个 protected static 方法获取 Token
     * - 外部无法获取 Token
     * 
     * 使用示例：
     * @code
     * // 在子类的 allocatePoolWithBuffers() 中：
     * auto pool = std::make_shared<BufferPool>(
     *     token(),              // 从基类获取通行证
     *     name,                 // Pool 名称
     *     category              // Pool 分类
     * );
     * @endcode
     * 
     * @return BufferPool::PrivateToken 通行证
     */
    static BufferPool::PrivateToken token() {
        return BufferPool::PrivateToken();
    }
};
