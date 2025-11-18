#pragma once

#include "../Buffer.hpp"
#include "../BufferPool.hpp"
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
 * @brief BufferAllocatorBase - Buffer 生命周期管理器（抽象基类）
 * 
 * 命名规范：Google C++ Style Guide
 * - Base 后缀：表示这是抽象基类，一眼就能看出
 * - 子类命名：NormalAllocator, AVFrameAllocator, FramebufferAllocator（描述性名称）
 * 
 * 职责：
 * - 创建 Buffer（批量或单个）
 * - 销毁 Buffer
 * - 将 Buffer 注入到 BufferPool（调用 BufferPool 私有方法）
 * - 从 BufferPool 移除 Buffer（调用 BufferPool 私有方法）
 * 
 * 设计原则：
 * - 作为 BufferPool 的友元，可以调用其私有方法
 * - 不持有长期锁（避免死锁）
 * - 所有队列操作委托给 BufferPool（BufferPool 内部加锁）
 * 
 * 使用示例：
 * @code
 * // 批量分配
 * auto allocator = std::make_unique<NormalAllocator>(BufferMemoryAllocatorType::NORMAL_MALLOC);
 * auto pool = allocator->allocatePoolWithBuffers(10, 1024*1024, "MyPool", "Video");
 * 
 * // 单个注入（扩容）
 * Buffer* buf = allocator->injectBufferToPool(1024*1024, pool.get(), QueueType::FREE);
 * 
 * // 移除（缩容）
 * allocator->removeBufferFromPool(buf, pool.get());
 * @endcode
 */
class BufferAllocatorBase {
public:
    virtual ~BufferAllocatorBase() = default;
    
    // ==================== 公开接口 ====================
    
    // ====== 批量分配 ======
    
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
    std::unique_ptr<BufferPool> allocatePoolWithBuffers(
        int count,
        size_t size,
        const std::string& name,
        const std::string& category = ""
    );
    
    // ====== 单个注入（扩容/动态注入）======
    
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
    Buffer* injectBufferToPool(
        size_t size,
        BufferPool* pool,
        QueueType queue = QueueType::FREE
    );
    
    // ====== Buffer 移除（缩容）======
    
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
    bool removeBufferFromPool(Buffer* buffer, BufferPool* pool);
    
protected:
    // ==================== 子类必须实现 ====================
    
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
    
    // ==================== 辅助方法 ====================
    
    /**
     * @brief 注册 Buffer 所有权
     * 
     * 用途：
     * - 跟踪哪些 buffer 属于哪个 allocator
     * - 用于清理时查找所有相关 buffer
     * 
     * @param buffer Buffer 指针
     * @param allocator 所属的 Allocator
     */
    void registerBufferOwnership(Buffer* buffer, BufferAllocatorBase* allocator);
    
    /**
     * @brief 注销 Buffer 所有权
     * 
     * @param buffer Buffer 指针
     */
    void unregisterBufferOwnership(Buffer* buffer);
    
    /**
     * @brief 清理 Pool 中所有属于此 Allocator 的 buffer
     * 
     * 用途：
     * - allocatePoolWithBuffers 失败时清理
     * - 避免内存泄漏
     * 
     * @param pool 要清理的 BufferPool
     */
    void cleanupPool(BufferPool* pool);
    
    // ==================== 友元访问辅助方法 ====================
    // 这些方法通过友元关系访问 BufferPool 的私有方法，供子类使用
    
    /**
     * @brief 将 Buffer 添加到 BufferPool 的指定队列
     * 
     * @param pool BufferPool 指针
     * @param buffer Buffer 指针
     * @param queue 队列类型
     * @return bool 成功返回 true
     */
    bool addBufferToPoolQueue(BufferPool* pool, Buffer* buffer, QueueType queue);
    
    /**
     * @brief 从 BufferPool 移除 Buffer
     * 
     * @param pool BufferPool 指针
     * @param buffer Buffer 指针
     * @return bool 成功返回 true
     */
    bool removeBufferFromPoolInternal(BufferPool* pool, Buffer* buffer);
    
private:
    // 所有权跟踪（buffer → allocator 映射）
    // 使用静态成员以支持跨 Allocator 实例查询
    static std::unordered_map<Buffer*, BufferAllocatorBase*> buffer_ownership_;
    static std::mutex ownership_mutex_;
};
