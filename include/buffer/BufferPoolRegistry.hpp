#pragma once

#include "BufferPool.hpp"
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>

// 前向声明
class BufferPool;

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
     * @brief 注册 BufferPool（由 BufferPool 构造函数自动调用）
     * @param pool BufferPool 指针
     * @param name 可读名称（如 "FramebufferPool_FB0", "VideoDecodePool"）
     * @param category 分类（如 "Display", "Video", "Network"）
     * @return 唯一 ID
     */
    uint64_t registerPool(BufferPool* pool, 
                          const std::string& name,
                          const std::string& category = "");
    
    /**
     * @brief 注销 BufferPool（由 BufferPool 析构函数自动调用）
     * @param id 注册时返回的唯一 ID
     */
    void unregisterPool(uint64_t id);
    
    // ========== 查询接口 ==========
    
    /**
     * @brief 获取所有 BufferPool
     * @return 所有 Pool 的指针列表
     */
    std::vector<BufferPool*> getAllPools() const;
    
    /**
     * @brief 按名称查找 BufferPool
     * @param name Pool 名称
     * @return BufferPool* 找到返回指针，否则返回 nullptr
     */
    BufferPool* findByName(const std::string& name) const;
    
    /**
     * @brief 按分类获取所有 BufferPool
     * @param category 分类名称（如 "Display", "Video"）
     * @return 该分类下所有 Pool 的指针列表
     */
    std::vector<BufferPool*> getPoolsByCategory(const std::string& category) const;
    
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
    
    /**
     * @brief Pool 信息结构
     */
    struct PoolInfo {
        BufferPool* pool;                                    // Pool 指针
        uint64_t id;                                         // 唯一 ID
        std::string name;                                    // 可读名称
        std::string category;                                // 分类
        std::chrono::system_clock::time_point created_time;  // 创建时间
    };
    
    // ========== 成员变量 ==========
    mutable std::mutex mutex_;                              // 保护所有成员变量
    std::unordered_map<uint64_t, PoolInfo> pools_;          // ID -> PoolInfo
    std::unordered_map<std::string, uint64_t> name_to_id_;  // Name -> ID（快速查找）
    uint64_t next_id_ = 1;                                  // 下一个可用 ID
};





