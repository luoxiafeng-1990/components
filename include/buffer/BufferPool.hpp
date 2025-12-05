#pragma once

#include "Buffer.hpp"
#include <string>
#include <vector>
#include <queue>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <atomic>

// 前向声明
class BufferAllocatorBase;

/**
 * @brief QueueType - 队列类型枚举
 */
enum class QueueType {
    FREE,      // 空闲队列
    FILLED     // 填充队列
};

/**
 * @brief BufferPool - 纯调度器
 * 
 * 职责：
 * - 管理 Buffer 队列（free_queue, filled_queue）
 * - 提供线程安全的调度接口
 * - 不关心 Buffer 来源和生命周期
 * 
 * 设计原则：
 * - 对外：提供调度接口（acquire/submit/release）
 * - 对内：提供队列操作接口（仅供 BufferAllocatorBase 使用）
 * - 线程安全：所有操作使用互斥锁保护
 * 
 * 使用示例：
 * @code
 * // 1. 通过 Allocator 创建 Pool
 * auto allocator = std::make_unique<NormalAllocator>(BufferMemoryAllocatorType::NORMAL_MALLOC);
 * auto pool = allocator->allocatePoolWithBuffers(10, 1024*1024, "MyPool", "Video");
 * 
 * // 2. 生产者使用
 * Buffer* buf = pool->acquireFree(true, 100);
 * // ... 填充数据 ...
 * pool->submitFilled(buf);
 * 
 * // 3. 消费者使用
 * Buffer* buf = pool->acquireFilled(true, 100);
 * // ... 使用数据 ...
 * pool->releaseFilled(buf);
 * @endcode
 */
class BufferPool {
public:
    // ==================== PrivateToken（通行证）====================
    
    /**
     * @brief 私有通行证（Passkey Idiom）
     * 
     * 设计模式：
     * - 构造函数是 private
     * - 只有 BufferAllocatorBase 是 friend，可以创建
     * - 外部无法创建此 Token
     * - 持有 Token 才能调用 BufferPool 构造函数
     * 
     * 用途：
     * - 控制 BufferPool 的创建权限
     * - 只有 Allocator 子类可以创建 BufferPool
     */
    class PrivateToken {
    private:
        PrivateToken() = default;
        
        // 只有 BufferAllocatorBase 可以创建 PrivateToken
        friend class BufferAllocatorBase;
    };
    
    // ==================== 构造函数（需要 PrivateToken）====================
    
    /**
     * @brief 构造函数（需要通行证）
     * 
     * @param token 通行证（只有持有 Token 才能创建）
     * @param name Pool 名称
     * @param category Pool 分类
     * 
     * @note 虽然是 public，但外部无法创建 PrivateToken，因此无法调用
     */
    BufferPool(
        PrivateToken token,
        const std::string& name,
        const std::string& category
    );
    
    /**
     * @brief 析构函数
     */
    ~BufferPool();
    
public:
    // ====== 生产者接口 ======
    
    /**
     * @brief 获取空闲 Buffer
     * 
     * 线程安全：是
     * 阻塞行为：由 blocking 参数决定
     * 
     * @param blocking 是否阻塞等待
     * @param timeout_ms 超时时间（毫秒），-1 表示无限等待
     * @return Buffer* 成功返回 buffer，失败/超时返回 nullptr
     */
    Buffer* acquireFree(bool blocking = true, int timeout_ms = -1);
    
    /**
     * @brief 提交已填充的 Buffer
     * 
     * 线程安全：是
     * 
     * @param buffer_ptr 填充好的 buffer
     */
    void submitFilled(Buffer* buffer_ptr);
    
    /**
     * @brief 归还未填充的 Buffer（生产者填充失败时使用）
     * 
     * 使用场景：
     * - 生产者通过 acquireFree() 获取 Buffer 后
     * - fillBuffer() 失败，Buffer 未填充数据
     * - 需要将 Buffer 归还到 free_queue_
     * 
     * 工作流程：
     * 1. 验证 buffer 属于此 pool
     * 2. 将 buffer 归还到 free_queue_
     * 3. 设置状态为 IDLE
     * 4. 唤醒等待的生产者
     * 
     * 线程安全：是
     * 
     * @param buffer_ptr 未填充的 buffer
     */
    void releaseFree(Buffer* buffer_ptr);
    
    // ====== 消费者接口 ======
    
    /**
     * @brief 获取已填充的 Buffer
     * 
     * 线程安全：是
     * 阻塞行为：由 blocking 参数决定
     * 
     * @param blocking 是否阻塞等待
     * @param timeout_ms 超时时间（毫秒），-1 表示无限等待
     * @return Buffer* 成功返回 buffer，失败/超时返回 nullptr
     */
    Buffer* acquireFilled(bool blocking = true, int timeout_ms = -1);
    
    /**
     * @brief 归还已使用的 Buffer
     * 
     * 线程安全：是
     * 
     * @param buffer_ptr 已使用的 buffer
     */
    void releaseFilled(Buffer* buffer_ptr);
    
    // ====== 查询接口 ======
    
    /**
     * @brief 获取空闲 buffer 数量
     * 线程安全：是
     */
    int getFreeCount() const;
    
    /**
     * @brief 获取就绪 buffer 数量
     * 线程安全：是
     */
    int getFilledCount() const;
    
    /**
     * @brief 获取总 buffer 数量
     * 线程安全：是
     */
    int getTotalCount() const;
    
    /**
     * @brief 获取 Pool 名称
     */
    const std::string& getName() const { return name_; }
    
    /**
     * @brief 获取 Pool 分类
     */
    const std::string& getCategory() const { return category_; }
    
    /**
     * @brief 获取注册表 ID
     */
    uint64_t getRegistryId() const { return registry_id_; }
    
    /**
     * @brief 设置注册表 ID（由 CreateEmpty() 调用）
     */
    void setRegistryId(uint64_t id) { registry_id_ = id; }
    
    /**
     * @brief 根据 ID 获取 Buffer（用于特定场景，如 framebuffer）
     * 线程安全：是
     * 
     * @param id Buffer ID
     * @return Buffer* 找到返回 buffer，否则返回 nullptr
     */
    Buffer* getBufferById(uint32_t id) const;
    
    /**
     * @brief 获取 Buffer 大小（返回第一个 buffer 的大小）
     * 注意：假设所有 buffer 大小相同
     * 线程安全：是
     * 
     * @return size_t Buffer 大小，如果没有 buffer 返回 0
     */
    size_t getBufferSize() const;
    
    /**
     * @brief 获取所有托管的 Buffer（仅供 Allocator 使用）
     * 
     * 使用场景：
     * - Allocator 在 destroyPool() 时需要遍历所有 buffer
     * - 检查哪些 buffer 属于自己，然后销毁
     * 
     * 线程安全：是（返回引用，调用者需要自行加锁）
     * 
     * @return const std::unordered_set<Buffer*>& 所有 managed buffers 的引用
     * 
     * @note 仅供 BufferAllocatorBase 及其子类使用
     * @note 返回的是引用，调用者不应修改集合本身
     */
    const std::unordered_set<Buffer*>& getAllManagedBuffers() const {
        return managed_buffers_;
    }
    
    /**
     * @brief 清空所有托管的 Buffer 集合（仅供 Allocator 使用）
     * 
     * 使用场景：
     * - Allocator 在创建 Pool 失败时，需要清理已添加的 buffer
     * - 在错误处理流程中使用，清空 managed_buffers_ 避免悬空指针
     * 
     * 线程安全：是（内部使用 mutex_ 保护）
     * 
     * @note 仅供 BufferAllocatorBase 及其子类使用
     * @note 调用此方法前，应该已经通过 deallocateBuffer() 销毁了所有 buffer 对象
     * @note 此方法只清空集合，不销毁 buffer 对象本身
     */
    void clearAllManagedBuffers();
    
    // ====== 生命周期管理 ======
    
    /**
     * @brief 停止 BufferPool（唤醒所有等待线程）
     * 
     * 使用场景：
     * - BufferPool 析构前
     * - 需要清理资源时
     * 
     * 作用：
     * - 设置 running_ = false
     * - 唤醒所有等待的线程
     * - 防止死锁
     */
    void shutdown();
    
    // ====== 调试接口 ======
    
    /**
     * @brief 打印统计信息
     */
    void printStats() const;
    
    /**
     * @brief 打印所有 buffer 详情
     */
    void printAllBuffers() const;
    
    // ====== 禁止拷贝 ======
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    
private:
    // ==================== 友元声明 ====================
    
    // 允许 BufferAllocatorBase 访问私有方法 addBufferToQueue() 和 removeBufferFromPool()
    friend class BufferAllocatorBase;
    
    // ==================== 私有接口（仅供 BufferAllocatorBase 使用）====================
    
    /**
     * @brief 添加 Buffer 到指定队列
     * 
     * 线程安全：是（内部使用 mutex_ 保护）
     * 访问权限：私有（通过友元访问）
     * 
     * 工作流程：
     * 1. 检查 buffer 是否已在 managed_buffers_ 中
     * 2. 添加到 managed_buffers_ 集合
     * 3. 添加到指定队列（free_queue_ 或 filled_queue_）
     * 4. 更新 buffer 状态
     * 5. 通知等待的线程
     * 
     * @param buffer Buffer 指针
     * @param queue 目标队列（FREE 或 FILLED）
     * @return true 成功，false 失败（buffer 已存在）
     */
    bool addBufferToQueue(Buffer* buffer, QueueType queue);
    
    /**
     * @brief 从 Pool 中移除 Buffer
     * 
     * 线程安全：是（内部使用 mutex_ 保护）
     * 访问权限：私有（通过友元访问）
     * 
     * 限制条件：
     * - 只能移除 IDLE 状态的 buffer（在 free_queue 中）
     * - 不能移除正在使用的 buffer
     * 
     * 工作流程：
     * 1. 检查 buffer 是否在 managed_buffers_ 中
     * 2. 检查 buffer 状态是否为 IDLE
     * 3. 从 free_queue_ 中移除
     * 4. 从 managed_buffers_ 中移除
     * 5. 通知等待的线程（队列已变化）
     * 
     * @param buffer Buffer 指针
     * @return true 成功，false 失败（buffer 不在 pool 中或正在使用）
     */
    bool removeBufferFromPool(Buffer* buffer);
    
    // ====== 辅助方法 ======
    
    /**
     * @brief 从队列中移除指定 buffer
     * 
     * @note 调用者必须已持有 mutex_
     * @param queue 目标队列
     * @param target 要移除的 buffer
     * @return true 成功，false buffer 不在队列中
     */
    bool removeFromQueue(std::queue<Buffer*>& queue, Buffer* target);
    
    // ==================== 成员变量 ====================
    
    // 基本信息
    std::string name_;
    std::string category_;
    uint64_t registry_id_;
    
    // Buffer 管理（不拥有 Buffer 对象，只管理指针）
    std::unordered_set<Buffer*> managed_buffers_;  // 所有托管的 Buffer
    std::queue<Buffer*> free_queue_;                // 空闲队列
    std::queue<Buffer*> filled_queue_;              // 填充队列
    
    // 线程安全
    mutable std::mutex mutex_;                      // 保护所有队列和状态
    std::condition_variable free_cv_;               // 空闲队列条件变量
    std::condition_variable filled_cv_;             // 填充队列条件变量
    std::atomic<bool> running_;                     // 运行状态（用于停止等待）
};
