#pragma once

#include "BufferPool.hpp"
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>
#include <memory>

// 前向声明
class BufferPool;
class VideoProductionLine;  // 用于 friend 声明
class BufferAllocatorBase;  // 用于 friend 声明（v2.0 新增）

/**
 * @brief BufferPool 全局注册表（单例）
 * 
 * v2.0 架构：Registry 中心化资源管理器
 * 
 * 职责：
 * - 独占持有所有 BufferPool 实例（shared_ptr，引用计数=1）
 * - 提供全局查询和监控接口
 * - 支持命名和分类管理
 * - 协调 Allocator 的清理操作（通过友元）
 * 
 * 设计模式：
 * - 单例模式（全局唯一）
 * - 注册表模式（集中管理）
 * - 友元模式（Allocator 访问私有清理方法）
 * 
 * 线程安全：所有接口内部使用 mutex 保护
 * 
 * v2.0 变更：
 * - Registry 独占持有 BufferPool（shared_ptr，引用计数=1）
 * - 公开接口返回 weak_ptr（观察者模式）
 * - 新增 Allocator 友元（访问私有清理方法）
 * - 精简接口：只保留一个 getPool() 方法，统一使用 ID 获取
 */
class BufferPoolRegistry {
public:
    /**
     * @brief 获取单例实例
     * @return BufferPoolRegistry& 全局唯一实例
     */
    static BufferPoolRegistry& getInstance();
    
    // 禁止拷贝和移动
    BufferPoolRegistry(const BufferPoolRegistry&) = delete;
    BufferPoolRegistry& operator=(const BufferPoolRegistry&) = delete;
    BufferPoolRegistry(BufferPoolRegistry&&) = delete;
    BufferPoolRegistry& operator=(BufferPoolRegistry&&) = delete;
    
    // ========== 注册管理接口 ==========
    
    /**
     * @brief 注册 BufferPool（由 Allocator 创建 pool 后自动调用）
     * 
     * v2.0 设计：
     * - Registry 独占持有 BufferPool（shared_ptr，引用计数=1）
     * - Allocator 立即转移所有权给 Registry
     * - Registry 负责 BufferPool 的生命周期管理
     * - 记录创建者 Allocator 的 ID，用于归属关系追踪
     * 
     * @param pool BufferPool 的 shared_ptr（所有权转移给 Registry）
     * @param allocator_id 创建者 Allocator 的唯一 ID
     * @return 唯一 ID
     * 
     * @note 线程安全：是
     * @note 注册后，Registry 成为唯一持有者（引用计数=1）
     */
    uint64_t registerPool(std::shared_ptr<BufferPool> pool, uint64_t allocator_id);
    
    // ========== 公开接口（所有人都可以调用）==========
    
    /**
     * @brief 获取 BufferPool（返回 weak_ptr，观察者模式）
     * 
     * v2.0 设计：
     * - 返回 weak_ptr<BufferPool>，不持有所有权（观察者模式）
     * - 所有人（包括友元类）都用这个方法获取 Pool
     * - 调用者必须使用 weak_ptr::lock() 获取临时 shared_ptr
     * - 如果 Pool 已销毁，lock() 返回 nullptr
     * 
     * @param id Pool ID
     * @return weak_ptr<BufferPool> 如果不存在返回空的 weak_ptr
     * 
     * @note 线程安全：是
     * @note 使用示例：
     * @code
     * auto pool_weak = registry.getPool(pool_id);
     * if (auto pool = pool_weak.lock()) {
     *     // 使用 pool
     * } else {
     *     // Pool 已销毁
     * }
     * @endcode
     */
    std::weak_ptr<BufferPool> getPool(uint64_t id) const;
    
    /**
     * @brief 获取注册的 BufferPool 总数
     * @return size_t Pool 数量
     */
    size_t getPoolCount() const;
    
    
    // ========== 全局监控接口 ==========
    
    /**
     * @brief 打印所有 BufferPool 的统计信息
     * 
     * 输出格式：
     * ========================================
     * 📊 Global BufferPool Statistics
     * ========================================
     * Total Pools: 3
     * 
     * [Display] FramebufferPool_FB0 (ID: 1)
     *   Buffers: 4 total, 2 free, 2 filled
     *   Memory: 32.0 MB
     *   Created: 2025-11-13 10:30:45
     * ...
     */
    void printAllStats() const;
    
    /**
     * @brief 获取所有 BufferPool 的总内存使用量
     * @return size_t 总字节数
     */
    size_t getTotalMemoryUsage() const;
    
    /**
     * @brief 全局统计信息结构
     */
    struct GlobalStats {
        int total_pools;         // 总 Pool 数量
        int total_buffers;       // 总 Buffer 数量
        int total_free;          // 总空闲 Buffer 数量
        int total_filled;        // 总已填充 Buffer 数量
        size_t total_memory;     // 总内存使用量（字节）
    };
    
    /**
     * @brief 获取全局统计信息
     * @return GlobalStats 统计数据
     */
    GlobalStats getGlobalStats() const;
    
private:
    // 私有构造函数（单例模式）
    BufferPoolRegistry() = default;
    ~BufferPoolRegistry() = default;
    
    // ========== v2.0 新增：Allocator 友元访问 ==========
    
    /**
     * @brief 供 Allocator 清理时使用（私有方法，只有友元可调用）
     * 
     * v2.0 设计：
     * - Allocator 通过友元访问此方法
     * - 用于在 Allocator 析构时获取 Pool 并清理 Buffer
     * - 返回 shared_ptr（不是 weak_ptr），保证清理期间 Pool 不被销毁
     * - 公开接口返回 weak_ptr，但 Allocator 清理时需要 shared_ptr
     * 
     * @param id Pool ID
     * @return shared_ptr<BufferPool> 临时持有，用于清理
     * 
     * @note 只有 friend class BufferAllocatorBase 可以调用
     * @note 与公开接口 getPool() 的区别：
     *       - getPool() 返回 weak_ptr（观察者模式）
     *       - getPoolForAllocatorCleanup() 返回 shared_ptr（用于清理操作）
     */
    std::shared_ptr<BufferPool> getPoolForAllocatorCleanup(uint64_t id);
    
    /**
     * @brief 获取指定 Allocator 创建的所有 Pool ID（私有方法，只有友元可调用）
     * 
     * v2.0 设计：
     * - Allocator 析构时调用此方法查询所有属于它的 Pool
     * - 返回所有匹配的 Pool ID 列表
     * - 用于自动清理所有 Pool
     * 
     * @param allocator_id Allocator 的唯一 ID
     * @return std::vector<uint64_t> 所有属于此 Allocator 的 Pool ID 列表
     * 
     * @note 只有 friend class BufferAllocatorBase 可以调用
     * @note 线程安全：是（内部有 mutex 保护）
     */
    std::vector<uint64_t> getPoolsByAllocatorId(uint64_t allocator_id) const;
    
    /**
     * @brief 注销 BufferPool（私有方法，只能由 Allocator 的 destroyPool 调用）
     * 
     * ⚠️ 重要：此方法不应该被外部直接调用！
     * 
     * 正确的销毁流程：
     * 1. Allocator::destroyPool() 清理所有 Buffer（调用 deallocateBuffer）
     * 2. Allocator::destroyPool() 调用 unregisterPool() 注销
     * 3. unregisterPool() 释放 shared_ptr，触发 Pool 析构
     * 
     * 为什么必须是私有？
     * - 只有 Allocator 知道如何正确清理 Buffer（不同 Allocator 有不同的清理方式）
     * - 如果外部直接调用 unregisterPool，会导致 Buffer 内存泄漏
     * - 如果先调用 unregisterPool，再调用 destroyPool，destroyPool 无法获取 Pool（已从 Registry 移除）
     * 
     * @param id 注册时返回的唯一 ID
     * 
     * @note 线程安全：是
     * @note 只有 friend class BufferAllocatorBase 可以调用
     */
    void unregisterPool(uint64_t id);
    
    /**
     * @brief Pool 信息结构
     */
    struct PoolInfo {
        std::shared_ptr<BufferPool> pool;                    // v2.0: Pool 的 shared_ptr（独占持有）
        uint64_t id;                                         // 唯一 ID
        std::string name;                                    // 可读名称
        std::string category;                                // 分类
        std::chrono::system_clock::time_point created_time; // 创建时间
        uint64_t allocator_id;                               // 🆕 创建者 Allocator 的唯一 ID
    };
    
    // ========== 成员变量 ==========
    mutable std::mutex mutex_;                              // 保护所有成员变量
    std::unordered_map<uint64_t, PoolInfo> pools_;          // ID -> PoolInfo
    std::unordered_map<std::string, uint64_t> name_to_id_;  // Name -> ID（快速查找）
    uint64_t next_id_ = 1;                                  // 下一个可用 ID
    
    // ========== 友元声明 ==========
    friend class BufferAllocatorBase;    // v2.0 新增：Allocator 可以调用清理方法
};





