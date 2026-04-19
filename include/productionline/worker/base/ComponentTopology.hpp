#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <chrono>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

class WorkerBase;
class BufferPool;
class BufferAllocatorBase;

/**
 * @brief 组件拓扑所有者类型
 *
 * 用于 Factory::create() 的 owner_id 参数，
 * 区分 Worker 归属于 Line（简单模式）还是 Group（MultiWorker 模式）。
 */
enum class TopologyOwnerType : uint8_t {
    NONE  = 0,   ///< 不注册拓扑（向后兼容）
    LINE  = 1,   ///< 归属于 Line（VideoProductionLine 简单模式）
    GROUP = 2    ///< 归属于 Group（MultiWorkerProductionLine 的 WorkerGroup）
};

/**
 * @brief ComponentTopology - 统一组件注册表与拓扑管理（单例）
 *
 * 吸收 WorkerRegistry 和 BufferPoolRegistry 的全部职责，
 * 同时管理组件间的层级拓扑关系。
 *
 * 职责：
 * - 持有所有已注册 Worker 的 shared_ptr（与 ProductionLine 共享所有权）
 * - 独占持有所有 BufferPool 的 shared_ptr（引用计数=1）
 * - 管理 Line → Group → Worker → Pool 的四层拓扑关系
 *
 * 四层拓扑结构：
 *
 *   VideoProductionLine（简单模式）：
 *     Line → Worker → Pool
 *
 *   MultiWorkerProductionLine（多 Worker 模式）：
 *     Line → Group → { ProducerLine（子 Line）, ConsumerWorker }
 *                            ↓                         ↓
 *                      Worker → Pool              Worker → Pool
 *
 * 线程安全：所有接口内部使用 mutex 保护。
 */
class ComponentTopology {
public:
    static ComponentTopology& getInstance();

    ComponentTopology(const ComponentTopology&) = delete;
    ComponentTopology& operator=(const ComponentTopology&) = delete;

    // ==================== Worker 注册 ====================

    /**
     * @brief 注册 Worker（由 WorkerFactory::create() 调用）
     * @param worker Worker 的 shared_ptr
     * @return 唯一 ID（从 1 开始递增），失败返回 0
     */
    uint64_t registerWorker(std::shared_ptr<WorkerBase> worker);

    // ==================== Pool 注册 ====================

    /**
     * @brief 注册 BufferPool（由 Allocator 创建后调用）
     * @param pool BufferPool 的 shared_ptr（所有权转移给 Topology）
     * @param allocator_id 创建者 Allocator 的唯一 ID
     * @return 唯一 ID，失败返回 0
     */
    uint64_t registerPool(std::shared_ptr<BufferPool> pool, uint64_t allocator_id);

    /**
     * @brief 获取 BufferPool（返回 weak_ptr，观察者模式）
     * @param id Pool ID
     * @return weak_ptr<BufferPool>，不存在时返回空 weak_ptr
     */
    std::weak_ptr<BufferPool> getPool(uint64_t id) const;

    // ==================== Line / Group 注册 ====================

    /**
     * @brief 注册一条生产线
     * @param name 可选的显示名称
     * @return 拓扑 Line ID（从 1 开始），失败返回 0
     */
    uint64_t registerLine(const std::string& name = "");

    /**
     * @brief 注册一个 WorkerGroup
     * @param name 可选的显示名称
     * @return 拓扑 Group ID（从 1 开始），失败返回 0
     */
    uint64_t registerGroup(const std::string& name = "");

    // ==================== 关联（建立关系）====================

    void linkWorkerToLine(uint64_t line_id, uint64_t worker_id);
    void linkGroupToLine(uint64_t line_id, uint64_t group_id);
    void linkWorkerToGroup(uint64_t group_id, uint64_t worker_id);
    void linkProducerLineToGroup(uint64_t group_id, uint64_t producer_line_id);
    void linkPoolToWorker(uint64_t worker_id, uint64_t pool_id);

    /**
     * @brief 解除 Pool 与 Worker 的拓扑关联
     *
     * 供 WorkerBase::unregisterBufferPool() 调用，
     * 确保 Worker 注销某个 Pool 时拓扑同步更新。
     */
    void unlinkPool(uint64_t pool_id);

    // ==================== 注销 ====================

    void unregisterLine(uint64_t line_id);
    void unregisterGroup(uint64_t group_id);

    // ==================== 诊断 ====================

    void printTopology() const;

private:
    ComponentTopology();
    ~ComponentTopology() = default;

    // ========== Pool 管理（仅 BufferAllocatorBase 友元可调用）==========

    std::shared_ptr<BufferPool> getPoolSpecialForAllocator(uint64_t pool_id);
    std::vector<uint64_t> getPoolsByAllocator(uint64_t allocator_id) const;
    void unregisterPool(uint64_t pool_id);

    friend class BufferAllocatorBase;

    // --- Line 数据 ---
    struct LineInfo {
        uint64_t id;
        std::string name;
        std::unordered_set<uint64_t> worker_ids;
        std::vector<uint64_t> group_ids;
    };

    // --- Group 数据 ---
    struct GroupInfo {
        uint64_t id;
        std::string name;
        uint64_t parent_line_id{0};
        std::unordered_set<uint64_t> worker_ids;
        std::vector<uint64_t> producer_line_ids;
    };

    // --- Worker 数据 ---
    struct WorkerInfo {
        uint64_t id;
        std::shared_ptr<WorkerBase> worker;
        std::chrono::system_clock::time_point created_time;
    };

    // --- Pool 数据 ---
    struct PoolInfo {
        uint64_t id;
        std::shared_ptr<BufferPool> pool;
        std::string name;
        std::string category;
        uint64_t allocator_id;
        std::chrono::system_clock::time_point created_time;
    };

    // --- 反向索引 ---
    std::unordered_map<uint64_t, uint64_t> worker_to_line_;
    std::unordered_map<uint64_t, uint64_t> worker_to_group_;
    std::unordered_map<uint64_t, uint64_t> pool_to_worker_;
    std::unordered_map<uint64_t, uint64_t> producer_line_to_group_;

    // --- Worker → Pool 正向 ---
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> worker_pools_;

    // --- 主数据 ---
    std::unordered_map<uint64_t, LineInfo> lines_;
    std::unordered_map<uint64_t, GroupInfo> groups_;
    std::unordered_map<uint64_t, WorkerInfo> workers_;
    std::unordered_map<uint64_t, PoolInfo> pools_;
    std::unordered_map<std::string, uint64_t> pool_name_to_id_;

    uint64_t next_line_id_;
    uint64_t next_group_id_;
    uint64_t next_worker_id_;
    uint64_t next_pool_id_;

    mutable std::mutex mutex_;
    log4cplus::Logger logger_;
};
