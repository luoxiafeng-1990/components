#pragma once

#include "buffer/bufferpool/Buffer.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include <cstddef>  // for size_t

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
 * v2.0 架构：Allocator 负责 BufferPool 的完整生命周期
 * 
 * 架构角色：抽象基类（Abstract Base Class / Interface）
 * 
 * 设计模式：
 * - 接口隔离原则（ISP）
 * - 友元模式（Friend Pattern）
 * - Template Method Pattern（模板方法）
 * 
 * 职责：
 * - 创建 BufferPool（通过 Passkey Token）
 * - 立即注册到 Registry（转移所有权）
 * - 记录 pool_id（不持有指针）
 * - 析构时清理：通过友元获取 Pool → 销毁 Buffer → unregister
 * - 管理 Buffer 对象的生命周期
 * 
 * v2.0 核心变更：
 * - Allocator 不持有 BufferPool 指针（只记录 pool_id）
 * - Registry 独占持有 BufferPool（shared_ptr，引用计数=1）
 * - Allocator 通过友元访问 Registry 的私有清理方法
 * - allocatePoolWithBuffers() 返回 pool_id（而不是 unique_ptr）
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
 * 
 * // 创建并注册 BufferPool（返回 ID）
 * uint64_t pool_id = allocator->allocatePoolWithBuffers(10, 1024*1024, "MyPool", "Video");
 * 
 * // 使用者从 Registry 获取
 * auto pool = BufferPoolRegistry::getInstance().getPool(pool_id);
 * 
 * // Allocator 析构时自动清理
 * @endcode
 */
class BufferAllocatorBase {
public:
    /**
     * @brief 构造函数
     * 
     * v2.0 设计：
     * - 自动分配唯一的 allocator_id_
     * - 使用静态原子计数器保证全局唯一
     */
    BufferAllocatorBase() : allocator_id_(next_allocator_id_++) {}
    
    /**
     * @brief 析构函数
     * 
     * v2.0 设计：
     * - 基类析构函数不调用 destroyPool()（因为 destroyPool() 是纯虚函数）
     * - 子类必须在自己的析构函数中调用 destroyPool() 清理所有 Pool
     * - destroyPool() 会自动查询 Registry 获取所有 Pool 并清理
     */
    virtual ~BufferAllocatorBase();
    
    // ==================== 纯虚函数接口（子类必须实现）====================
    
    /**
     * @brief 批量创建 Buffer 并构建 BufferPool
     * 
     * v2.0 工作流程：
     * 1. 创建 BufferPool（shared_ptr）
     * 2. 循环创建 Buffer（调用 createBuffer）
     * 3. 将 Buffer 添加到 pool 的 free 队列
     * 4. 注册到 Registry（转移所有权）
     * 5. 记录 pool_id（不持有指针）
     * 6. 返回 pool_id
     * 
     * @param count Buffer 数量
     * @param size 每个 Buffer 大小
     * @param name BufferPool 名称
     * @param category BufferPool 分类
     * @return uint64_t 成功返回 pool_id，失败返回 0
     * 
     * @note 线程安全：是（BufferPool 内部加锁）
     * @note 失败时自动清理已分配的 buffer
     * @note Registry 独占持有 BufferPool（引用计数=1）
     * @note Allocator 只记录 pool_id，不持有指针
     */
    virtual uint64_t allocatePoolWithBuffers(
        int count,
        size_t size,
        const std::string& name,
        const std::string& category = ""
    ) = 0;
    
    /**
     * @brief 创建单个 Buffer 并注入到指定 BufferPool（内部分配）
     * 
     * v2.0 注意：需要通过 Registry 获取 Pool
     * 
     * 适用场景：
     * - 动态扩容：向已有 pool 添加新 buffer
     * - 内部分配：Allocator 自己分配内存
     * 
     * 工作流程：
     * 1. 从 Registry 获取 BufferPool（临时访问）
     * 2. 创建 Buffer（调用 createBuffer，Allocator 内部分配内存）
     * 3. 将 Buffer 添加到 pool 的指定队列
     * 4. 释放临时 shared_ptr
     * 
     * @param pool_id BufferPool ID
     * @param size Buffer 大小
     * @param queue 注入到哪个队列（FREE 或 FILLED）
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     * 
     * @note 线程安全：是（BufferPool 内部加锁）
     * @note addBufferToQueue 会自动将 Buffer 添加到 managed_buffers_ 集合
     */
    virtual Buffer* injectBufferToPool(
        uint64_t pool_id,
        size_t size,
        QueueType queue = QueueType::FREE
    ) = 0;
    
    /**
     * @brief 注入外部已分配的内存到 BufferPool（外部注入）
     * 
     * v2.0 注意：需要通过 Registry 获取 Pool
     * 
     * 适用场景：
     * - 外部内存包装：将外部已分配的内存包装为 Buffer 对象
     * - Framebuffer 内存：将 Framebuffer 设备内存注入到 Pool
     * - 零拷贝场景：直接使用外部内存，避免拷贝
     * 
     * @param pool_id BufferPool ID
     * @param virt_addr 外部内存的虚拟地址（已分配）
     * @param phys_addr 外部内存的物理地址（如果支持，否则为 0）
     * @param size 外部内存的大小（字节）
     * @param queue 注入到哪个队列（FREE 或 FILLED）
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     * 
     * @note 线程安全：是（BufferPool 内部加锁）
     * @note 外部内存的生命周期由外部管理，Allocator 只负责创建 Buffer 对象
     * @note Buffer 对象的 ownership 为 EXTERNAL
     */
    virtual Buffer* injectExternalBufferToPool(
        uint64_t pool_id,
        void* virt_addr,
        uint64_t phys_addr,
        size_t size,
        QueueType queue = QueueType::FREE
    ) = 0;
    
    /**
     * @brief 从 BufferPool 移除并销毁 Buffer
     * 
     * v2.0 注意：需要通过 Registry 获取 Pool
     * 
     * 适用场景：
     * - 动态缩容：减少 pool 中的 buffer 数量
     * 
     * @param pool_id BufferPool ID
     * @param buffer 要移除的 Buffer
     * @return true 成功，false 失败（buffer 正在使用或不在 pool 中）
     * 
     * @note 线程安全：是（BufferPool 内部加锁）
     * @note 只能移除 IDLE 状态的 buffer
     */
    virtual bool removeBufferFromPool(uint64_t pool_id, Buffer* buffer) = 0;
    
    /**
     * @brief 销毁所有 BufferPool 及其所有 Buffer
     * 
     * v2.0 注意：自动处理所有属于此 Allocator 的 Pool
     * 
     * 适用场景：
     * - 清理资源：销毁所有不再使用的 BufferPool
     * - 析构时清理：Allocator 析构时自动清理所有创建的 Pool
     * 
     * 工作流程：
     * 1. 查询 Registry 获取所有属于此 Allocator 的 Pool ID
     * 2. 遍历每个 Pool：
     *    - 通过友元从 Registry 获取 Pool（临时访问）
     *    - 销毁所有 Buffer（调用 deallocateBuffer）
     *    - 从 Registry 注销（触发 Pool 析构）
     * 
     * @return true 成功，false 失败
     * 
     * @note 线程安全：是（BufferPool 内部加锁）
     * @note 只能销毁所有 Buffer 都处于 IDLE 状态的 pool
     * @note 子类析构函数可以直接调用此方法清理所有 Pool
     */
    virtual bool destroyPool() = 0;
    
protected:
    // ==================== v2.0 新架构：Allocator ID 机制 ====================
    
    /**
     * @brief Allocator 的唯一标识符
     * 
     * v2.0 设计：
     * - 每个 Allocator 实例在构造时分配唯一 ID
     * - 用于 Registry 中记录 Pool 的归属关系
     * - 析构时通过此 ID 查询并清理所有创建的 Pool
     * 
     * @note 此 ID 是只读的，不允许通过此 ID 访问 Allocator 对象
     * @note Registry 使用此 ID 记录 Pool 的创建者
     */
    uint64_t allocator_id_;
    
    /**
     * @brief 全局 Allocator ID 计数器（静态原子变量）
     * 
     * 用于生成全局唯一的 Allocator ID
     */
    static std::atomic<uint64_t> next_allocator_id_;
    
    /**
     * @brief 获取 Allocator 的唯一 ID（只读）
     * 
     * @return uint64_t Allocator 的唯一标识符
     * 
     * @note 此 ID 仅用于 Registry 中记录 Pool 的归属关系
     * @note 不允许通过此 ID 访问 Allocator 对象
     */
    uint64_t getAllocatorId() const { return allocator_id_; }
    
    /**
     * @brief 获取所有属于此 Allocator 的 Pool ID（辅助方法）
     * 
     * 子类在 destroyPool() 实现中使用此方法获取所有 Pool ID。
     * 
     * @return std::vector<uint64_t> 所有 Pool ID 的列表
     * 
     * @note 此方法是非虚的，可以在任何地方安全调用
     * @note destroyPool() 内部会自动调用此方法获取所有 Pool
     */
    std::vector<uint64_t> getPoolsByAllocator() const;
    
    /**
     * @brief 供子类清理时使用（通过友元访问 Registry）
     * 
     * v2.0 设计：
     * - BufferAllocatorBase 是 BufferPoolRegistry 的友元
     * - 子类可以通过此方法访问 Registry 的私有清理方法
     * - 返回 shared_ptr（不是 weak_ptr），保证清理期间 Pool 不被销毁
     * 
     * @param pool_id BufferPool ID
     * @return shared_ptr<BufferPool> 临时持有，用于清理
     * 
     * @note 只有子类在 destroyPool() 实现中使用
     * @note 实现位于 BufferAllocatorBase.cpp 中（避免头文件依赖）
     */
    std::shared_ptr<BufferPool> getPoolSpecialForAllocator(uint64_t pool_id);
    
    /**
     * @brief 注销 Pool（供子类使用）
     * 
     * v2.0 设计：
     * - 子类在 destroyPool() 中清理完 Buffer 后，需要注销 Pool
     * - 通过友元访问 Registry 的私有方法 unregisterPool()
     * - 只能由 Allocator 调用，确保销毁流程正确
     * 
     * 正确的销毁流程（针对单个 Pool）：
     * 1. Allocator::destroyPool() 遍历所有 Pool
     * 2. 对每个 Pool：清理所有 Buffer (deallocateBuffer)
     * 3. 对每个 Pool：调用 unregisterPool() 注销
     * 4. unregisterPool() 释放 shared_ptr，触发 Pool 析构
     * 
     * @param pool_id Pool ID
     * 
     * @note 只有子类可以调用（protected）
     * @note 必须在清理完所有 Buffer 后调用
     */
    void unregisterPool(uint64_t pool_id);
    
    
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
     * auto pool = std::make_unique<BufferPool>(
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
