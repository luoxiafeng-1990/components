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

/**
 * @brief BufferPool 全局注册表（单例）
 * 
 * 职责：
 * - 跟踪系统中所有 BufferPool 实例
 * - 提供全局查询和监控接口
 * - 支持命名和分类管理
 * - 自动化生命周期管理
 * 
 * 设计模式：
 * - 单例模式（全局唯一）
 * - 注册表模式（集中管理）
 * 
 * 线程安全：所有接口内部使用 mutex 保护
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
     * 设计变更：
     * - 使用 weak_ptr 存储，不持有所有权（观察者模式）
     * - 调用者需要提供临时 shared_ptr 用于创建 weak_ptr
     * - Registry 不影响 BufferPool 的生命周期
     * 
     * @param temp_shared 临时 shared_ptr（用于创建 weak_ptr，注册后可以释放）
     * @return 唯一 ID
     */
    uint64_t registerPoolWeak(std::shared_ptr<BufferPool> temp_shared);
    
    /**
     * @brief 注销 BufferPool（由 BufferPool 析构函数自动调用）
     * @param id 注册时返回的唯一 ID
     */
    void unregisterPool(uint64_t id);
    
    // ========== 只读接口（公开，任何人都可以调用）==========
    
    /**
     * @brief 获取 BufferPool（只读版本）
     * 
     * 注意：如果 BufferPool 已被销毁，返回 nullptr
     * 
     * @param id Pool ID
     * @return shared_ptr<const BufferPool> 只读版本，如果 Pool 已销毁则返回 nullptr
     */
    std::shared_ptr<const BufferPool> getPoolReadOnly(uint64_t id) const;
    
    /**
     * @brief 通过名称获取 BufferPool（只读版本）
     * @param name Pool 名称
     * @return shared_ptr<const BufferPool> 只读版本
     */
    std::shared_ptr<const BufferPool> getPoolReadOnlyByName(const std::string& name) const;
    
    /**
     * @brief 获取所有 BufferPool（只读版本）
     * @return 所有 Pool 的只读版本列表
     */
    std::vector<std::shared_ptr<const BufferPool>> getAllPoolsReadOnly() const;
    
    /**
     * @brief 按分类获取所有 BufferPool（只读版本）
     * @param category 分类名称（如 "Display", "Video"）
     * @return 该分类下所有 Pool 的只读版本列表
     */
    std::vector<std::shared_ptr<const BufferPool>> getPoolsByCategoryReadOnly(const std::string& category) const;
    
    /**
     * @brief 查询所有 Worker 创建的 BufferPool（只读版本）
     * @return Worker 创建的 Pool 列表（只读）
     */
    std::vector<std::shared_ptr<const BufferPool>> getWorkerPoolsReadOnly() const;
    
    /**
     * @brief 查询指定 Worker 的 BufferPool（只读版本）
     * @param worker_name Worker 名称
     * @return BufferPool 的只读版本
     */
    std::shared_ptr<const BufferPool> getWorkerPoolReadOnly(const std::string& worker_name) const;
    
    /**
     * @brief 获取注册的 BufferPool 总数
     * @return size_t Pool 数量
     */
    size_t getPoolCount() const;
    
    // ========== 读写接口（仅 ProductionLine 可以调用）==========
    
    /**
     * @brief 获取 BufferPool（读写版本，仅 ProductionLine 使用）
     * 
     * 权限控制：通过 friend 类限制，只有 VideoProductionLine 可以调用
     * 
     * 注意：如果 BufferPool 已被销毁，返回 nullptr
     * 
     * @param id Pool ID
     * @return shared_ptr<BufferPool> 读写版本，如果 Pool 已销毁则返回 nullptr
     */
    std::shared_ptr<BufferPool> getPoolForProductionLine(uint64_t id);
    
    /**
     * @brief 通过名称获取 BufferPool（读写版本，仅 ProductionLine 使用）
     * 
     * 注意：如果 BufferPool 已被销毁，返回 nullptr
     * 
     * @param name Pool 名称
     * @return shared_ptr<BufferPool> 读写版本，如果 Pool 已销毁则返回 nullptr
     */
    std::shared_ptr<BufferPool> getPoolByNameForProductionLine(const std::string& name);
    
    /**
     * @brief 清理已失效的 weak_ptr（定期调用，可选）
     * 
     * 当 BufferPool 被销毁后，Registry 中的 weak_ptr 会失效
     * 此方法用于清理这些失效的条目
     */
    void cleanupExpiredPools();
    
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
    
    /**
     * @brief Pool 信息结构
     */
    struct PoolInfo {
        std::weak_ptr<BufferPool> pool;                      // Pool 的 weak_ptr（观察者，不持有所有权）
        uint64_t id;                                         // 唯一 ID
        std::string name;                                    // 可读名称
        std::string category;                                // 分类
        std::chrono::system_clock::time_point created_time; // 创建时间
    };
    
    // ========== 成员变量 ==========
    mutable std::mutex mutex_;                              // 保护所有成员变量
    std::unordered_map<uint64_t, PoolInfo> pools_;          // ID -> PoolInfo
    std::unordered_map<std::string, uint64_t> name_to_id_;  // Name -> ID（快速查找）
    uint64_t next_id_ = 1;                                  // 下一个可用 ID
    
    // 声明 friend 类（只有 ProductionLine 可以调用读写接口）
    friend class VideoProductionLine;
};





