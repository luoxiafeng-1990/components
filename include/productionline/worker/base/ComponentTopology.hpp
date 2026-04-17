#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

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
 * @brief ComponentTopology - 组件拓扑注册表（单例）
 *
 * 纯 ID 映射层，不持有任何组件对象指针，仅追踪组件间的层级关系。
 * 与 WorkerRegistry / BufferPoolRegistry 平行存在，不侵入其访问控制。
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
 * ID 空间：Line 和 Group 各自独立编号，均从 1 开始。
 */
class ComponentTopology {
public:
    static ComponentTopology& getInstance();

    ComponentTopology(const ComponentTopology&) = delete;
    ComponentTopology& operator=(const ComponentTopology&) = delete;

    // ==================== 注册 ====================

    /**
     * @brief 注册一条生产线
     * @param name 可选的显示名称
     * @return 拓扑 Line ID（从 1 开始），失败返回 0
     */
    uint64_t registerLine(const std::string& name = "");

    /**
     * @brief 注册一个 WorkerGroup
     * @param name 可选的显示名称（通常为 WorkerGroupConfig::group_id）
     * @return 拓扑 Group ID（从 1 开始），失败返回 0
     */
    uint64_t registerGroup(const std::string& name = "");

    // ==================== 关联（建立关系）====================

    /// Line 直属 Worker（简单 VideoProductionLine，无 Group 中间层）
    void linkWorkerToLine(uint64_t line_id, uint64_t worker_registry_id);

    /// Line → Group（MultiWorkerProductionLine 内的 WorkerGroup）
    void linkGroupToLine(uint64_t line_id, uint64_t group_id);

    /// Group 直属 Consumer Worker
    void linkWorkerToGroup(uint64_t group_id, uint64_t worker_registry_id);

    /// Group → Producer 子 Line（每个 Producer 内部的 VideoProductionLine）
    void linkProducerLineToGroup(uint64_t group_id, uint64_t producer_line_id);

    /// Worker → Pool（任何 Worker 的输出 BufferPool）
    void linkPoolToWorker(uint64_t worker_registry_id, uint64_t pool_id);

    // ==================== 正向查询 ====================

    /// 获取 Line 直属的 Worker ID 列表（简单模式）
    std::vector<uint64_t> getWorkersOfLine(uint64_t line_id) const;

    /// 获取 Line 包含的 Group ID 列表（MultiWorker 模式）
    std::vector<uint64_t> getGroupsOfLine(uint64_t line_id) const;

    /// 获取 Group 包含的 Consumer Worker ID 列表
    std::vector<uint64_t> getWorkersOfGroup(uint64_t group_id) const;

    /// 获取 Group 关联的 Producer 子 Line ID 列表
    std::vector<uint64_t> getProducerLinesOfGroup(uint64_t group_id) const;

    /// 获取 Worker 拥有的 Pool ID 列表
    std::vector<uint64_t> getPoolsOfWorker(uint64_t worker_registry_id) const;

    // ==================== 反向查询 ====================

    /// Worker 所属的 Line ID（简单模式下直属；MultiWorker 下返回 0，需通过 Group 查）
    uint64_t getLineOfWorker(uint64_t worker_registry_id) const;

    /// Worker 所属的 Group ID（仅 MultiWorker 模式有效）
    uint64_t getGroupOfWorker(uint64_t worker_registry_id) const;

    /// Group 所属的 Line ID
    uint64_t getLineOfGroup(uint64_t group_id) const;

    /// 子 Line 所属的 Group ID
    uint64_t getGroupOfProducerLine(uint64_t producer_line_id) const;

    // ==================== 注销 ====================

    /// 注销 Line 及其所有 Group、Worker、Pool 关联
    void unregisterLine(uint64_t line_id);

    /// 注销 Group 及其关联的 Worker、Producer Line
    void unregisterGroup(uint64_t group_id);

    /// 解除 Worker 的所有关联（Line/Group → Worker 和 Worker → Pool）
    void unlinkWorker(uint64_t worker_registry_id);

    /// 解除单个 Pool 的关联
    void unlinkPool(uint64_t pool_id);

    // ==================== 诊断 ====================

    /// 打印完整拓扑树
    void printTopology() const;

    /// 获取已注册的 Line 数量
    size_t getLineCount() const;

    /// 获取已注册的 Group 数量
    size_t getGroupCount() const;

private:
    ComponentTopology();
    ~ComponentTopology() = default;

    // --- Line 数据 ---
    struct LineInfo {
        uint64_t id;
        std::string name;
        std::unordered_set<uint64_t> worker_ids;   // 直属 Worker（简单模式）
        std::vector<uint64_t> group_ids;            // 所属 Group（MultiWorker 模式）
    };

    // --- Group 数据 ---
    struct GroupInfo {
        uint64_t id;
        std::string name;
        uint64_t parent_line_id{0};
        std::unordered_set<uint64_t> worker_ids;          // Consumer Worker
        std::vector<uint64_t> producer_line_ids;           // Producer 子 Line
    };

    // --- 反向索引 ---
    std::unordered_map<uint64_t, uint64_t> worker_to_line_;    // worker_id → line_id
    std::unordered_map<uint64_t, uint64_t> worker_to_group_;   // worker_id → group_id
    std::unordered_map<uint64_t, uint64_t> pool_to_worker_;    // pool_id → worker_id
    std::unordered_map<uint64_t, uint64_t> producer_line_to_group_;  // line_id → group_id

    // --- Worker → Pool 正向 ---
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> worker_pools_;  // worker_id → {pool_id...}

    // --- 主数据 ---
    std::unordered_map<uint64_t, LineInfo> lines_;
    std::unordered_map<uint64_t, GroupInfo> groups_;

    uint64_t next_line_id_;
    uint64_t next_group_id_;

    mutable std::mutex mutex_;
    log4cplus::Logger logger_;
};
