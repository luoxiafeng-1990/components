#ifndef WORKER_CONFIG_MULTI_WORKER_CONFIG_HPP
#define WORKER_CONFIG_MULTI_WORKER_CONFIG_HPP

#include <string>
#include <vector>
#include "productionline/worker/config/Connector.hpp"
#include "productionline/worker/config/FrameSyncTypes.hpp"

// WorkerConfig 完整定义（ProducerConfig/ConsumerConfig 持有 WorkerConfig 实例）
#include "productionline/worker/config/WorkerConfigs.hpp"

/**
 * @brief ProducerConfig - 生产者配置
 */
struct ProducerConfig {
    std::string producer_name;      // 组内唯一标识
    WorkerConfig worker_config;
};

/**
 * @brief ConsumerConfig - 消费者配置
 */
struct ConsumerConfig {
    std::string consumer_name;      // 组内唯一标识（可选）
    WorkerConfig worker_config;
};

/**
 * @brief ConnectorConfig - 连接器配置
 */
struct ConnectorConfig {
    Connector::Mode mode;
    std::vector<std::string> producer_names;  // 关联的生产者名称
    std::vector<std::string> consumer_names;   // 关联的消费者名称
    
    // ⭐ v2.23 新增：帧同步配置
    bool enable_frame_sync = false;          // 是否启用帧同步
    CallbackChain callback_chain;            // 回调链（可选）
};

/**
 * @brief WorkerGroupConfig - Worker 工作组配置结构
 * 
 * ⭐ 设计说明：配置与运行时分离模式（Configuration vs Runtime State）
 * 
 * 职责：
 * - 描述"要创建什么"（配置数据）
 * - 在构造函数时传入，整个生命周期只读
 * - 可序列化/反序列化（纯数据，不包含对象实例）
 * 
 * ⭐ 核心概念：一个 Group = 多个生产者 + 多个消费者 + 多个连接器
 * - Group 内强同步：通过连接器建立生产者-消费者关系
 * - Group 间独立：多个 Group 并行运行，互不干扰
 * - 数据源模式：消费者自动配置为 Buffer 模式，关联到生产者的 BufferPool
 * 
 * 注意：运行时数据（实际创建的对象、线程、统计信息等）存储在 WorkerGroupRuntime 中
 * 
 * @see WorkerGroupRuntime - 对应的运行时数据结构
 */
struct WorkerGroupConfig {
    // 组标识
    std::string group_id;
    
    // 多个生产者和消费者配置
    std::vector<ProducerConfig> producer_configs;
    std::vector<ConsumerConfig> consumer_configs;
    
    // 多个连接器配置
    std::vector<ConnectorConfig> connector_configs;
    
    WorkerGroupConfig() = default;
    explicit WorkerGroupConfig(const std::string& id) : group_id(id) {}
};

/**
 * @brief MultiWorkerConfig - 多Worker配置结构
 * 
 * 设计理念：
 * - 包含全局配置（如线程池大小）
 * - 包含多个 WorkerGroup，每个 Group 包含多个生产者和消费者
 * - 支持复杂的多 Worker 协作场景
 */
struct MultiWorkerConfig {
    // ⭐ 核心：Worker Group 配置列表
    std::vector<WorkerGroupConfig> groups;
    
    // 全局线程池配置（用于初始化全局线程池）
    // 默认值：64，范围：1-128
    int thread_pool_size = 64;
    
    MultiWorkerConfig() = default;
};

#endif // WORKER_CONFIG_MULTI_WORKER_CONFIG_HPP
