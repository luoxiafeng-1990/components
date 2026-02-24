#include "productionline/MultiWorkerProductionLine.hpp"
#include "productionline/worker/FfmpegPacketRecorderWorker.hpp"
#include "productionline/worker/EncodedPacketSourceFromBuffer.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "common/Logger.hpp"
#include "common/GlobalThreadPool.hpp"
#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <thread>

// ============================================================
// 构造函数和析构函数
// ============================================================

MultiWorkerProductionLine::MultiWorkerProductionLine(
    const MultiWorkerConfig& config,
    bool loop,
    int thread_count,
    bool enable_monitor)
    : VideoProductionLine(loop, thread_count, enable_monitor)
    , config_(config)
    , groups_()
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.MultiWorker")))
    , log_prefix_("")
{
    LOG4CPLUS_INFO(logger_, "⭐ 创建 WorkerGroup 架构: groups=" << config_.groups.size()
                   << ", thread_pool_size=" << config_.thread_pool_size);
    
    // 验证配置
    if (config_.groups.empty()) {
        LOG4CPLUS_WARN(logger_, "警告: 没有配置任何 WorkerGroup");
    }
    
    // 统计总的生产者和消费者数量
    size_t total_producers = config_.groups.size();  // 每个 Group 1 个生产者
    size_t total_consumers = 0;
    for (const auto& group : config_.groups) {
        total_consumers += group.consumer_configs.size();
    }
    LOG4CPLUS_INFO(logger_, "总计: producers=" << total_producers 
                   << ", consumers=" << total_consumers);
    
    if (config_.thread_pool_size < 1) {
        LOG4CPLUS_WARN(logger_, "警告: thread_pool_size < 1, 使用默认值 4");
        config_.thread_pool_size = 4;
    }
}

MultiWorkerProductionLine::~MultiWorkerProductionLine() {
    LOG4CPLUS_INFO(logger_, "析构开始...");
    
    // 停止所有线程
    stop();
    
    // 清理资源
    groups_.clear();
    
    LOG4CPLUS_INFO(logger_, "析构完成");
}

// ============================================================
// 核心接口实现
// ============================================================

bool MultiWorkerProductionLine::validateConfig() const {
    
    for (size_t group_idx = 0; group_idx < config_.groups.size(); group_idx++) {
        const auto& group_config = config_.groups[group_idx];
        
        // 3.1 校验：每个 Group 必须至少有一个生产者
        if (group_config.producer_configs.empty()) {
            setError("Group[" + std::to_string(group_idx) + "] 没有配置任何生产者");
            return false;
        }
        
        // 3.2 校验：每个 Group 必须至少有一个消费者
        if (group_config.consumer_configs.empty()) {
            setError("Group[" + std::to_string(group_idx) + "] 没有配置任何消费者");
            return false;
        }
        
        // 3.3 校验：每个 Group 必须至少有一个连接器
        if (group_config.connector_configs.empty()) {
            setError("Group[" + std::to_string(group_idx) + "] 没有配置任何连接器");
            return false;
        }
        
        // 3.4 校验连接器配置
        for (size_t conn_idx = 0; conn_idx < group_config.connector_configs.size(); conn_idx++) {
            const auto& conn_cfg = group_config.connector_configs[conn_idx];
            
            // 校验连接器必须关联至少一个生产者
            if (conn_cfg.producer_names.empty()) {
                setError("Group[" + std::to_string(group_idx) + "] Connector[" + std::to_string(conn_idx) 
                        + "] 没有关联任何生产者");
                return false;
            }
            
            // 校验连接器必须关联至少一个消费者
            if (conn_cfg.consumer_names.empty()) {
                setError("Group[" + std::to_string(group_idx) + "] Connector[" + std::to_string(conn_idx) 
                        + "] 没有关联任何消费者");
                return false;
            }
            
            // 校验连接器的 producer_names 必须在 Group 中存在
            for (const auto& pid : conn_cfg.producer_names) {
                bool found = false;
                for (const auto& pcfg : group_config.producer_configs) {
                    if (pcfg.producer_name == pid) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    setError("Group[" + std::to_string(group_idx) + "] Connector[" + std::to_string(conn_idx) 
                            + "] 关联的生产者名称 '" + pid + "' 不存在");
                    return false;
                }
            }
            
            // 校验连接器的 consumer_names 必须在 Group 中存在
            for (const auto& cid : conn_cfg.consumer_names) {
                bool found = false;
                for (const auto& ccfg : group_config.consumer_configs) {
                    if (ccfg.consumer_name == cid) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    setError("Group[" + std::to_string(group_idx) + "] Connector[" + std::to_string(conn_idx) 
                            + "] 关联的消费者名称 '" + cid + "' 不存在");
                    return false;
                }
            }
            
            // 校验：根据连接器模式检查对应关系
            switch (conn_cfg.mode) {
                case Connector::Mode::ONE_TO_ONE:
                    // 1:1 模式：生产者数量必须等于消费者数量
                    if (conn_cfg.producer_names.size() != conn_cfg.consumer_names.size()) {
                        setError("Group[" + std::to_string(group_idx) + "] Connector[" + std::to_string(conn_idx) 
                                + "] ONE_TO_ONE 模式：生产者数量(" + std::to_string(conn_cfg.producer_names.size()) 
                                + ") 必须等于消费者数量(" + std::to_string(conn_cfg.consumer_names.size()) + ")");
                        return false;
                    }
                    break;
                    
                case Connector::Mode::ONE_TO_MANY:
                    // 1:N 模式：必须只有1个生产者
                    if (conn_cfg.producer_names.size() != 1) {
                        setError("Group[" + std::to_string(group_idx) + "] Connector[" + std::to_string(conn_idx) 
                                + "] ONE_TO_MANY 模式：必须只有1个生产者，实际有 " 
                                + std::to_string(conn_cfg.producer_names.size()) + " 个");
                        return false;
                    }
                    break;
                    
                case Connector::Mode::MANY_TO_ONE:
                    // N:1 模式：必须只有1个消费者
                    if (conn_cfg.consumer_names.size() != 1) {
                        setError("Group[" + std::to_string(group_idx) + "] Connector[" + std::to_string(conn_idx) 
                                + "] MANY_TO_ONE 模式：必须只有1个消费者，实际有 " 
                                + std::to_string(conn_cfg.consumer_names.size()) + " 个");
                        return false;
                    }
                    break;
                    
                case Connector::Mode::MANY_TO_MANY:
                    // N:M 模式：至少1个生产者，至少1个消费者（已在上面检查）
                    break;
            }
        }
        
        // 3.5 校验：检查是否有未连接的 Producer
        std::set<std::string> connected_producer_names;
        for (const auto& conn_cfg : group_config.connector_configs) {
            for (const auto& pid : conn_cfg.producer_names) {
                connected_producer_names.insert(pid);
            }
        }
        for (const auto& pcfg : group_config.producer_configs) {
            if (connected_producer_names.find(pcfg.producer_name) == connected_producer_names.end()) {
                setError("Group[" + std::to_string(group_idx) + "] 生产者 '" + pcfg.producer_name 
                        + "' 没有被任何连接器连接");
                return false;
            }
        }
        
        // 3.6 校验：检查是否有未连接的 Consumer
        std::set<std::string> connected_consumer_names;
        for (const auto& conn_cfg : group_config.connector_configs) {
            for (const auto& cid : conn_cfg.consumer_names) {
                connected_consumer_names.insert(cid);
            }
        }
        for (const auto& ccfg : group_config.consumer_configs) {
            if (connected_consumer_names.find(ccfg.consumer_name) == connected_consumer_names.end()) {
                setError("Group[" + std::to_string(group_idx) + "] 消费者 '" + ccfg.consumer_name 
                        + "' 没有被任何连接器连接");
                return false;
            }
        }
    }
    
    return true;
}

bool MultiWorkerProductionLine::start() {
    
    // ========== 步骤1：检查是否已经在运行 ==========
    if (running_.load()) {
        LOG4CPLUS_WARN(logger_, "已经在运行");
        return false;
    }
    
    LOG4CPLUS_INFO(logger_, "⭐ 启动 WorkerGroup 架构...");
    
    // ========== 步骤2：初始化全局线程池（调用父类方法，包含验证逻辑）==========
    initializeGlobalThreadPool(config_.thread_pool_size);
    
    // ========== 步骤3：校验配置 ==========
    LOG4CPLUS_INFO(logger_, "校验配置...");
    if (!validateConfig()) {
        LOG4CPLUS_ERROR(logger_, "配置校验失败");
        return false;
    }
    LOG4CPLUS_INFO(logger_, "配置校验通过");
    
    // ========== 步骤4：为每个 Group 创建运行时环境 ==========
    LOG4CPLUS_INFO(logger_, "创建 " << config_.groups.size() << " 个 WorkerGroup...");
    groups_.reserve(config_.groups.size());
    
    for (size_t group_idx = 0; group_idx < config_.groups.size(); group_idx++) {
        const auto& group_config = config_.groups[group_idx];
        
        LOG4CPLUS_INFO(logger_, " [Group " << group_idx << "] 创建 WorkerGroup '" 
                       << group_config.group_id << "'...");
        
        auto group = std::make_unique<WorkerGroupRuntime>();
        group->group_id = group_config.group_id.empty() 
                        ? ("Group_" + std::to_string(group_idx)) 
                        : group_config.group_id;
        
        // 步骤4.1：创建所有生产者
        if (!createProducersForGroup(group.get(), group_config)) {
            return false;
        }
        
        // 步骤4.2：创建所有 Connector 并保存 shared_source
        if (!createConnectorsForGroup(group.get(), group_config)) {
            return false;
        }
        
        // 步骤4.3：创建并打开所有消费者
        if (!createConsumersForGroup(group.get(), group_config)) {
            return false;
        }
        
        groups_.push_back(std::move(group));
        
        LOG4CPLUS_INFO(logger_, " [Group " << group_idx << "] '" 
                       << groups_.back()->group_id << "' 创建完成");
    }
    
    // ========== 步骤5：初始化状态 ==========
    running_.store(true);
    start_time_ = std::chrono::steady_clock::now();
    
    // ========== 步骤6：启动所有 Group 线程 ==========
    if (!startGroupThreads()) {
        return false;
    }
    
    LOG4CPLUS_INFO(logger_, "⭐ WorkerGroup 架构启动成功！");
    return true;
}



void MultiWorkerProductionLine::stop() {
    if (!running_.load()) {
        return;
    }
    
    LOG4CPLUS_INFO(logger_, "⭐ 停止生产车间...");
    
    // 设置停止标志
    running_.store(false);
    
    // 停止所有 Group 调度
    for (auto& group : groups_) {
        if (group) {
            group->is_running.store(false);
        }
    }
    
    // 等待线程池任务完成（所有 Worker 线程会自动退出）
    LOG4CPLUS_INFO(logger_, "等待所有 Worker 线程退出...");
    GlobalThreadPool::getInstance().wait();
    
    // 停止所有生产者和消费者
    for (auto& group : groups_) {
        if (!group) continue;
        
        // 停止所有生产者
        for (auto& producer_info : group->producer_infos) {
            if (producer_info && producer_info->producer_line) {
                producer_info->producer_line->stop();
            }
        }
        
        // 关闭所有消费者
        for (auto& consumer_info : group->consumer_infos) {
            if (consumer_info && consumer_info->worker) {
                consumer_info->worker->close();
            }
        }
        
        LOG4CPLUS_INFO(logger_, " Group '" << group->group_id << "' 已停止 "
                       << "(success=" << group->stats.frames_produced.load()
                       << ", failed=" << group->stats.frames_failed.load() << ")");
    }
    
    LOG4CPLUS_INFO(logger_, "⭐ 生产车间已停止");
    LOG4CPLUS_INFO(logger_, "总统计: success=" << getAllLineFramesProduced()
                   << ", failed=" << getAllLineFramesFailed());
}

// ============================================================
// start() 相关辅助函数实现
// ============================================================

bool MultiWorkerProductionLine::createProducersForGroup(WorkerGroupRuntime* group, const WorkerGroupConfig& group_config) {
    LOG4CPLUS_INFO(logger_, "   创建 " << group_config.producer_configs.size() 
                   << " 个生产者 Worker...");
    
    for (const auto& pcfg : group_config.producer_configs) {
        LOG4CPLUS_INFO(logger_, "     创建生产者 '" << pcfg.producer_name << "'...");
        
        // 创建父类 VideoProductionLine 作为生产者
        auto producer_line = std::make_unique<VideoProductionLine>(
            loop_, thread_count_, enable_monitor_
        );
        
        // 调用父类的 start() 方法启动生产者
        if (!producer_line->start(pcfg.worker_config)) {
            setError("Failed to start producer: " + pcfg.producer_name);
            groups_.clear();
            return false;
        }
        
        // 获取生产者的 BufferPool 信息
        uint64_t buffer_pool_id = producer_line->getWorkingBufferPoolId();
        if (buffer_pool_id == 0) {
            setError("Producer failed to create BufferPool: " + pcfg.producer_name);
            groups_.clear();
            return false;
        }
        
        auto buffer_pool_weak = BufferPoolRegistry::getInstance().getPool(buffer_pool_id);
        auto pool = buffer_pool_weak.lock();
        if (!pool) {
            setError("Failed to get BufferPool from Registry: " + pcfg.producer_name);
            groups_.clear();
            return false;
        }
        
        // 保存生产者信息（包括 BufferPool）
        auto producer_info = std::make_unique<WorkerGroupRuntime::ProducerInfo>();
        producer_info->producer_name = pcfg.producer_name;
        producer_info->producer_line = std::move(producer_line);
        producer_info->buffer_pool_id = buffer_pool_id;
        producer_info->buffer_pool_weak = buffer_pool_weak;
        
        group->producer_info_mapped_by_name[pcfg.producer_name] = producer_info.get();
        group->producer_infos.push_back(std::move(producer_info));
        
        LOG4CPLUS_INFO(logger_, "     生产者 '" << pcfg.producer_name 
                       << "' 已启动 (BufferPool ID: " << buffer_pool_id << ")");
    }
    
    return true;
}

bool MultiWorkerProductionLine::createConnectorsForGroup(WorkerGroupRuntime* group, const WorkerGroupConfig& group_config) {
    LOG4CPLUS_INFO(logger_, "   创建 " << group_config.connector_configs.size() 
                   << " 个连接器...");
    
    for (size_t conn_idx = 0; conn_idx < group_config.connector_configs.size(); conn_idx++) {
        const auto& conn_cfg = group_config.connector_configs[conn_idx];
        
        LOG4CPLUS_INFO(logger_, "     创建连接器 #" << conn_idx 
                       << " (Mode: " << static_cast<int>(conn_cfg.mode) << ")...");
        
        // 4.2.1 创建连接器（直接使用名字，无需转换）
        // 注意：producer_names 和 consumer_names 已在 validateConfig() 中验证过
        auto connector = std::make_unique<Connector>(
            conn_cfg.mode,
            conn_cfg.producer_names,
            conn_cfg.consumer_names
        );
        
        // 4.2.3 为每个生产者创建 shared_source 并保存到 Connector
        // ⭐ 设计变更：每个生产者都有自己独立的共享数据源
        for (const auto& producer_name : conn_cfg.producer_names) {
            auto it = group->producer_info_mapped_by_name.find(producer_name);
            if (it == group->producer_info_mapped_by_name.end()) {
                setError("Producer not found in mapping: " + producer_name);
                groups_.clear();
                return false;
            }
            
            auto* producer_info = it->second;
            
            const AVCodecParameters* codec_params = nullptr;
            if (producer_info->producer_line) {
                auto worker_facade = producer_info->producer_line->getWorkerFacade();
                if (worker_facade) {
                    codec_params = worker_facade->getSourceCodecParameters();
                }
            }
            
            if (!codec_params) {
                setError("Cannot get codec_params from producer: " + producer_name);
                groups_.clear();
                return false;
            }
            
            // 计算该生产者的订阅者数量（根据连接器模式和生产者-消费者映射关系）
            size_t subscriber_count = 0;
            switch (conn_cfg.mode) {
                case Connector::Mode::ONE_TO_ONE:
                    subscriber_count = 1;  // 1:1 模式，每个生产者对应1个消费者
                    break;
                case Connector::Mode::ONE_TO_MANY:
                    // 1:N 模式，所有消费者都订阅这个生产者
                    subscriber_count = conn_cfg.consumer_names.size();
                    break;
                case Connector::Mode::MANY_TO_ONE:
                    subscriber_count = 1;  // N:1 模式，每个生产者对应1个消费者
                    break;
                case Connector::Mode::MANY_TO_MANY:
                    // N:M 模式，计算该生产者对应的消费者数量（轮询策略）
                    // 遍历所有消费者，统计映射到该生产者的数量
                    for (size_t consumer_idx = 0; consumer_idx < conn_cfg.consumer_names.size(); consumer_idx++) {
                        // 手动实现轮询策略：consumer_names[i] -> producer_names[i % producer_names.size()]
                        size_t producer_idx = consumer_idx % conn_cfg.producer_names.size();
                        if (conn_cfg.producer_names[producer_idx] == producer_name) {
                            subscriber_count++;
                        }
                    }
                    break;
            }
            
            // 创建共享 EncodedPacketSourceFromBuffer
            auto shared_source = std::make_shared<EncodedPacketSourceFromBuffer>(codec_params, subscriber_count);
            
            // 设置源 BufferPool
            shared_source->setSourceBufferPool(producer_info->buffer_pool_weak);
            
            // ⭐ 保存到 Connector 内部（按生产者名称索引）
            connector->setSharedSource(producer_name, shared_source);
            
            LOG4CPLUS_INFO(logger_, "     ⭐ 为生产者 '" << producer_name 
                           << "' 创建共享 EncodedPacketSourceFromBuffer (" << subscriber_count << " 个订阅者)");
        }
        
        group->connectors.push_back(std::move(connector));
        
        // ⭐ v2.23 新增：如果启用了帧同步，创建 WorkerSyncCoordinator
        if (conn_cfg.enable_frame_sync && !conn_cfg.callback_chain.empty()) {
            auto coordinator = std::make_unique<WorkerSyncCoordinator>(
                conn_cfg.consumer_names,  // 该 Connector 的所有 Consumer
                conn_cfg.callback_chain   // 回调链
            );
            
            group->connector_coordinators[conn_idx] = std::move(coordinator);
            
            LOG4CPLUS_INFO(logger_, "     ⭐ 为连接器 #" << conn_idx 
                           << " 创建了 WorkerSyncCoordinator"
                           << " (Workers: " << conn_cfg.consumer_names.size()
                           << ", Callbacks: " << conn_cfg.callback_chain.size() << ")");
        }
        
        LOG4CPLUS_INFO(logger_, "     连接器 #" << conn_idx << " 已创建");
    }
    
    return true;
}

bool MultiWorkerProductionLine::createConsumersForGroup(WorkerGroupRuntime* group, const WorkerGroupConfig& group_config) {
    LOG4CPLUS_INFO(logger_, "   创建 " << group_config.consumer_configs.size() 
                   << " 个消费者 Worker...");
    
    for (size_t consumer_cfg_idx = 0; consumer_cfg_idx < group_config.consumer_configs.size(); consumer_cfg_idx++) {
        const auto& ccfg = group_config.consumer_configs[consumer_cfg_idx];
        LOG4CPLUS_INFO(logger_, "     创建消费者 '" << ccfg.consumer_name << "'...");
        
        // ⭐ v2.22 重构：配置 buffer mode（数据源配置移至 datasource）
        WorkerConfig consumer_config = ccfg.worker_config;
        consumer_config.data_source.buffer_mode = true;
        
        // 4.3.1 查找该消费者所属的 Connector（使用查询方法）
        Connector* owner_connector = group->getConnectorForConsumer(ccfg.consumer_name);
        
        // ⭐ v2.24 新增：如果 Connector 启用了帧同步，设置 deferred_commit = true
        if (owner_connector) {
            // 查找对应的 ConnectorConfig
            for (const auto& conn_cfg : group_config.connector_configs) {
                bool is_consumer_in_connector = std::find(
                    conn_cfg.consumer_names.begin(), 
                    conn_cfg.consumer_names.end(), 
                    ccfg.consumer_name) != conn_cfg.consumer_names.end();
                    
                if (is_consumer_in_connector && conn_cfg.enable_frame_sync) {
                    consumer_config.data_source.deferred_commit = true;
                    LOG4CPLUS_DEBUG(logger_, "       ⭐ 启用 deferred_commit（帧同步模式）");
                    break;
                }
            }
        }
        
        if (!owner_connector) {
            setError("Consumer '" + ccfg.consumer_name + "' is not connected to any Connector");
            groups_.clear();
            return false;
        }
        
        // 4.3.2 根据 Connector 模式配置 config
        // ⭐ 首先获取该消费者对应的生产者名称
        std::string producer_name = owner_connector->getProducerNameForConsumer(ccfg.consumer_name);
        if (producer_name.empty()) {
            setError("Cannot get producer name for consumer: " + ccfg.consumer_name);
            groups_.clear();
            return false;
        }
        
        // ⭐ 尝试获取该生产者对应的共享数据源
        auto shared_source = owner_connector->getSharedSource(producer_name);
        WorkerGroupRuntime::ProducerInfo* producer_info = nullptr;
        
        if (shared_source) {
            // ⭐ 使用共享数据源（所有模式都支持）
            consumer_config.data_source.shared_packet_source = shared_source;
            LOG4CPLUS_INFO(logger_, "       ✅ 使用生产者 '" << producer_name 
                           << "' 的共享 EncodedPacketSourceFromBuffer");
        } else {
            // ⭐ 如果没有共享数据源，则使用 codec_params 创建独立 EncodedPacketSourceFromBuffer
            // 通过映射直接查找 ProducerInfo
            auto it = group->producer_info_mapped_by_name.find(producer_name);
            if (it == group->producer_info_mapped_by_name.end()) {
                setError("Producer not found in mapping: " + producer_name);
                groups_.clear();
                return false;
            }
            producer_info = it->second;
            
            // 获取 producer 的 codec_params
            const AVCodecParameters* codec_params = nullptr;
            AVRational time_base = {1, 25};
            if (producer_info->producer_line) {
                auto worker_facade = producer_info->producer_line->getWorkerFacade();
                if (worker_facade) {
                    codec_params = worker_facade->getSourceCodecParameters();
                    time_base = worker_facade->getTimeBase();
                }
            }
            
            if (!codec_params) {
                setError("Cannot get codec_params from producer for consumer: " + ccfg.consumer_name);
                groups_.clear();
                return false;
            }
            
            consumer_config.data_source.codec_params = codec_params;
            consumer_config.data_source.time_base = time_base;
            LOG4CPLUS_INFO(logger_, "       ✅ 普通模式：从生产者 '" 
                           << producer_info->producer_name << "' 获取 codec_params");
        }
        
        // 4.3.3 创建消费者 Worker（构造函数根据 config 创建 packet_source）
        auto consumer_worker = std::make_shared<BufferFillingWorkerFacade>(consumer_config);
        
        // 4.3.4 如果是普通模式，设置 BufferPool
        if (!shared_source && producer_info) {
            if (!consumer_worker->setSourceBufferPool(producer_info->buffer_pool_weak)) {
                setError("Consumer failed to set source BufferPool: " + ccfg.consumer_name);
                groups_.clear();
                return false;
            }
            LOG4CPLUS_INFO(logger_, "       ✅ 已设置源 BufferPool");
        }
        
        // 4.3.5 打开消费者（创建输出 BufferPool）
        if (!consumer_worker->open()) {
            setError("Consumer failed to open: " + ccfg.consumer_name);
            groups_.clear();
            return false;
        }
        
        // 保存消费者信息
        auto consumer_info = std::make_unique<WorkerGroupRuntime::ConsumerInfo>();
        consumer_info->consumer_name = ccfg.consumer_name;
        consumer_info->worker = consumer_worker;
        
        // 获取消费者的输出 BufferPool 信息
        BufferPoolType primary_type = consumer_worker->getPrimaryBufferPoolType();
        consumer_info->buffer_pool_id = consumer_worker->getOutputBufferPoolId(primary_type);
        consumer_info->buffer_pool_weak = BufferPoolRegistry::getInstance().getPool(consumer_info->buffer_pool_id);
        
        // 保存 buffer_pool_id 用于日志（避免 use-after-move）
        uint64_t buffer_pool_id_for_log = consumer_info->buffer_pool_id;
        
        group->consumer_info_mapped_by_name[ccfg.consumer_name] = consumer_info.get();
        group->consumer_infos.push_back(std::move(consumer_info));
        
        LOG4CPLUS_INFO(logger_, "     消费者 '" << ccfg.consumer_name 
                       << "' 已创建并打开 (BufferPool ID: " << buffer_pool_id_for_log << ")");
    }
    
    return true;
}

bool MultiWorkerProductionLine::startGroupThreads() {
    LOG4CPLUS_INFO(logger_, "启动所有 WorkerGroup 调度任务...");
    
    // 获取全局线程池
    auto& thread_pool = GlobalThreadPool::getInstance().getThreadPool();
    
    for (size_t i = 0; i < groups_.size(); i++) {
        auto& group = groups_[i];
        
        // ========== 初始化 Worker 统计 ==========
        for (auto& consumer_info : group->consumer_infos) {
            if (consumer_info) {
                auto stats = std::make_unique<WorkerGroupRuntime::WorkerProductionStats>();
                group->worker_stats[consumer_info->consumer_name] = std::move(stats);
            }
        }
        
        // ========== 提交 Group 调度任务到线程池 ==========
        group->is_running.store(true);
        
        try {
            thread_pool.detach_task([this, group]() {
                groupThreadFunc(group);
            });
            
            LOG4CPLUS_INFO(logger_, " [Group " << i << "] '" << group->group_id 
                           << "' 调度任务已提交到线程池");
        } catch (const std::exception& e) {
            LOG4CPLUS_ERROR(logger_, "Failed to submit Group task: " << e.what());
            running_.store(false);
            for (auto& g : groups_) {
                if (g) {
                    g->is_running.store(false);
                }
            }
            groups_.clear();
            setError(std::string("Failed to submit Group task: ") + e.what());
            return false;
        }
    }
    
    return true;
}

// ============================================================
// 查询接口实现
// ============================================================

uint64_t MultiWorkerProductionLine::getGroupConsumerBufferPoolId(size_t group_index, size_t consumer_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    const auto& group = groups_[group_index];
    if (consumer_index >= group->consumer_infos.size() || !group->consumer_infos[consumer_index]) {
        return 0;
    }
    return group->consumer_infos[consumer_index]->buffer_pool_id;
}

// ============================================================
// WorkerGroupRuntime 查询方法实现
// ============================================================

MultiWorkerProductionLine::WorkerGroupRuntime::ProducerInfo* 
MultiWorkerProductionLine::WorkerGroupRuntime::getProducerInfo(const std::string& producer_name) const {
    auto it = producer_info_mapped_by_name.find(producer_name);
    if (it != producer_info_mapped_by_name.end()) {
        return it->second;
    }
    return nullptr;
}

MultiWorkerProductionLine::WorkerGroupRuntime::ConsumerInfo* 
MultiWorkerProductionLine::WorkerGroupRuntime::getConsumerInfo(const std::string& consumer_name) const {
    auto it = consumer_info_mapped_by_name.find(consumer_name);
    if (it != consumer_info_mapped_by_name.end()) {
        return it->second;
    }
    return nullptr;
}

uint64_t MultiWorkerProductionLine::WorkerGroupRuntime::getProducerBufferPoolId(const std::string& producer_name) const {
    auto* producer_info = getProducerInfo(producer_name);
    if (producer_info) {
        return producer_info->buffer_pool_id;
    }
    return 0;
}

uint64_t MultiWorkerProductionLine::WorkerGroupRuntime::getConsumerBufferPoolId(const std::string& consumer_name) const {
    auto* consumer_info = getConsumerInfo(consumer_name);
    if (consumer_info) {
        return consumer_info->buffer_pool_id;
    }
    return 0;
}

Connector* MultiWorkerProductionLine::WorkerGroupRuntime::getConnectorForConsumer(const std::string& consumer_name) const {
    for (auto& connector : connectors) {
        if (connector && connector->containsConsumer(consumer_name)) {
            return connector.get();
        }
    }
    return nullptr;
}

size_t MultiWorkerProductionLine::WorkerGroupRuntime::getConnectorIndex(const Connector* connector) const {
    for (size_t i = 0; i < connectors.size(); i++) {
        if (connectors[i].get() == connector) {
            return i;
        }
    }
    return SIZE_MAX;
}

void MultiWorkerProductionLine::printDetailedStats() const {
    
    LOG4CPLUS_INFO(logger_, "========== 生产车间详细统计信息 ==========");
    LOG4CPLUS_INFO(logger_, "全局统计:");
    LOG4CPLUS_INFO(logger_, "  成功生产帧数: " << getAllLineFramesProduced());
    LOG4CPLUS_INFO(logger_, "  失败帧数: " << getAllLineFramesFailed());
    
    LOG4CPLUS_INFO(logger_, "生产线统计 (共 " << groups_.size() << " 条):");
    for (size_t i = 0; i < groups_.size(); i++) {
        if (!groups_[i]) continue;
        
        const auto& group = groups_[i];
        LOG4CPLUS_INFO(logger_, "  [生产线 " << i << "] '" << group->group_id << "':");
        LOG4CPLUS_INFO(logger_, "    成功: " << group->stats.frames_produced.load());
        LOG4CPLUS_INFO(logger_, "    失败: " << group->stats.frames_failed.load());
        LOG4CPLUS_INFO(logger_, "    生产者数量: " << group->producer_infos.size());
        LOG4CPLUS_INFO(logger_, "    Worker 数量: " << group->consumer_infos.size());
        LOG4CPLUS_INFO(logger_, "    活跃 Worker: " << getActiveWorkerCount(i));
        LOG4CPLUS_INFO(logger_, "    连接器数量: " << group->connectors.size());
        
        // ⭐ 打印每个 Worker 的详细统计
        for (const auto& consumer_info : group->consumer_infos) {
            if (consumer_info) {
                auto state_it = group->worker_stats.find(consumer_info->consumer_name);
                if (state_it != group->worker_stats.end()) {
                    const auto* worker_stats = state_it->second.get();
                    LOG4CPLUS_INFO(logger_, "      Worker '" << consumer_info->consumer_name << "':");
                    LOG4CPLUS_INFO(logger_, "        成功生产帧数: " << worker_stats->worker_frames_produced.load());
                    LOG4CPLUS_INFO(logger_, "        失败帧数: " << worker_stats->worker_frames_failed.load());
                    LOG4CPLUS_INFO(logger_, "        连续失败: " << worker_stats->consecutive_failures.load());
                    LOG4CPLUS_INFO(logger_, "        是否活跃: " << (worker_stats->is_active.load() ? "是" : "否"));
                }
            }
        }
    }
    LOG4CPLUS_INFO(logger_, "================================================");
}

// ============================================================
// 内部方法实现
// ============================================================

void MultiWorkerProductionLine::groupThreadFunc(const std::shared_ptr<WorkerGroupRuntime>& group) {
    LOG4CPLUS_INFO(logger_, "[Group '" << group->group_id << "'] 启动所有 Worker 任务");
    
    // 获取全局线程池
    auto& thread_pool = GlobalThreadPool::getInstance().getThreadPool();
    
    // ========== 为每个 Worker 提交常驻任务（只提交一次）==========
        for (auto& connector : group->connectors) {
            for (const auto& consumer_name : connector->getConsumerNames()) {
                auto it = group->consumer_info_mapped_by_name.find(consumer_name);
            if (it == group->consumer_info_mapped_by_name.end() || 
                !it->second || 
                !it->second->worker->isOpen()) {
                LOG4CPLUS_WARN(logger_, "   Worker '" << consumer_name << "' 不可用，跳过");
                continue;
            }
            
            auto* consumer_info = it->second;
            
            // 提交 Worker 常驻任务到线程池
            thread_pool.detach_task([this, group, consumer_info, consumer_name]() {
                workerThreadFunc(group, consumer_info, consumer_name);
            });
            
            LOG4CPLUS_INFO(logger_, "   Worker '" << consumer_name << "' 任务已提交");
        }
    }
    
    LOG4CPLUS_INFO(logger_, "[Group '" << group->group_id << "'] 所有 Worker 任务已提交");
}

bool MultiWorkerProductionLine::performFrameSync(
    const std::shared_ptr<WorkerGroupRuntime>& group,
    const std::string& consumer_name,
    WorkerGroupRuntime::ConsumerInfo* consumer_info,
    Buffer* buffer,
    const FillResult& result
) {
    Connector* owner_connector = group->getConnectorForConsumer(consumer_name);
    if (!owner_connector) {
        return true;  // 没有 Connector，默认允许提交
    }
    
    size_t conn_idx = group->getConnectorIndex(owner_connector);
    auto it = group->connector_coordinators.find(conn_idx);
    
    if (it == group->connector_coordinators.end()) {
        return true;  // 未启用帧同步，默认允许提交
    }
    
    // ========== 以下代码只在帧同步模式下执行 ==========
    // 注意：帧同步模式下，deferred_commit 已在 createConsumersForGroup 中被设置为 true
    // 因此 Worker 内部不会调用 commitEncodedPacket，由此处负责调用
    
    std::string producer_name = owner_connector->getProducerNameForConsumer(consumer_name);
    auto shared_source = std::dynamic_pointer_cast<EncodedPacketSourceFromBuffer>(
        owner_connector->getSharedSource(producer_name));
    
    if (!shared_source) {
        LOG4CPLUS_ERROR(logger_, "[Worker '" << consumer_name 
                        << "'] shared_source 不是 EncodedPacketSourceFromBuffer 类型");
        return true;  // 类型错误，默认允许提交
    }
    // v2.34 修复：CodecEagain 不再跳过同步点
    // 原因：如果一个 Worker 返回 CodecEagain 时直接 commit 而不进入同步点，
    // 另一个 Worker 可能已经在同步点等待（如 CodecError），导致死锁。
    // 修复：让 CodecEagain 和其他状态一样走正常的同步流程（arrive + commit），
    // WorkerSyncCoordinator::arrive() 已能正确处理 EAGAIN + ERROR 的混合场景。

    if (result.shouldContinue() && result.isAcquireError() && 
        (result.acquireCause() == AcquireStatus::PacketAlreadyProcessed ||
         result.acquireCause() == AcquireStatus::NonVideoPacket)) { 
        return true;
    }
    // 获取当前帧版本号
    uint64_t frame_version = shared_source->getCurrentBufferVersion();
    
    // ⭐ v2.29 修改：到达同步点，传递 fillBuffer 的结果状态
    // v2.34 变更：使用 FillResult
    bool sync_result = it->second->arrive(consumer_name, frame_version, buffer, result);
    
    // 帧同步完成后，调用 commit 释放 packet
    // 所有 Worker 一起 commit，确保 fetchTaskFunc 不会提前被唤醒
    shared_source->commitEncodedPacket(consumer_info->worker->getWorkerBase());
    
    if (!sync_result && buffer != nullptr) {
        LOG4CPLUS_WARN(logger_, "[Worker '" << consumer_name 
                       << "'] 同步协调器拒绝提交 Frame " << frame_version);
    }
    
    return sync_result;
}

void MultiWorkerProductionLine::workerThreadFunc(
    const std::shared_ptr<WorkerGroupRuntime>& group,
    WorkerGroupRuntime::ConsumerInfo* consumer_info,
    const std::string& consumer_name
) {
    LOG4CPLUS_INFO(logger_, "[Worker '" << consumer_name << "'] 线程启动");
    
    // 获取 Worker 统计
    auto stats_it = group->worker_stats.find(consumer_name);
    if (stats_it == group->worker_stats.end()) {
        LOG4CPLUS_ERROR(logger_, "[Worker '" << consumer_name << "'] 统计信息不存在");
        return;
    }
    auto* worker_stats = stats_it->second.get();
    
    // 获取 BufferPool
    auto pool_sptr = consumer_info->buffer_pool_weak.lock();
    if (!pool_sptr) {
        LOG4CPLUS_ERROR(logger_, "[Worker '" << consumer_name << "'] BufferPool 已销毁");
                        return;
    }
                    
    const int MAX_CONSECUTIVE_FAILURES = 100;
    int buffer_wait_count = 0;
    
    // ========== 主循环：永不退出（除非 stop 或 fatal）==========
    while (group->is_running.load() && running_.load() && worker_stats->is_active.load()) {
        // 步骤1：获取空闲 buffer（类似 producerThreadFunc）
        Buffer* buffer = nullptr;
        while (group->is_running.load() && running_.load() && buffer == nullptr) {
            buffer = pool_sptr->acquireFree(true, 100);  // 100ms 超时
            if (buffer == nullptr) {
                buffer_wait_count++;
                // 每100次等待才打印一次日志
                if (buffer_wait_count % 100 == 0) {
                    LOG4CPLUS_DEBUG(logger_, "[Worker '" << consumer_name 
                                   << "'] 等待空闲 buffer (count=" << buffer_wait_count << ")");
                }
            }
        }
        
        // 检查是否因为停止信号退出循环
        if (!group->is_running.load() || !running_.load()) 
            break;
                        
        buffer_wait_count = 0;
        
        // v2.33 变更：fillBuffer 返回 FillResult
        FillResult fill_result = consumer_info->worker->fillBuffer(0, buffer);
            
        if (fill_result.ok()) {
            // ✅ 解码成功
            
            // ⭐ v2.29 修改：帧同步逻辑，传递 SUCCESS 状态
            // v2.34 变更：使用 FillResult
            bool should_submit = performFrameSync(group, consumer_name, consumer_info, 
                                                  buffer, fill_result);
            
            // 根据同步结果决定是否提交
            if (should_submit) {
                pool_sptr->submitFilled(buffer);
                worker_stats->worker_frames_produced.fetch_add(1);
                group->stats.frames_produced.fetch_add(1);
            } else {
                pool_sptr->releaseFree(buffer);
            }
            
            worker_stats->consecutive_failures.store(0);
            
        } else {
            // ❌ 失败：释放 buffer
            pool_sptr->releaseFree(buffer);
            
            // v2.33 变更：直接使用 FillResult 判断状态
            // v2.34 重构：拆分 shouldRetry 为 shouldContinue + shouldRetry
            if (fill_result.shouldContinue()) {
                // ⏭ 跳过当前 packet，获取下一个（PacketAlreadyProcessed / NonVideoPacket / InvalidData）
                performFrameSync(group, consumer_name, consumer_info, nullptr, 
                                fill_result);
                continue;
            }
            
            if (fill_result.shouldRetry()) {
                // 🔄 重试当前操作（Again / TimedOut / CodecEagain）
                performFrameSync(group, consumer_name, consumer_info, nullptr, 
                                fill_result);
                continue;
            }
            
            if (fill_result.isEof()) {
                // 📍 EOF：数据源已结束，退出线程
                LOG4CPLUS_INFO(logger_, "[Worker '" << consumer_name 
                               << "'] 检测到数据源 EOF，正常退出");
                break;
            }
            
            // ❌ 错误状态
            performFrameSync(group, consumer_name, consumer_info, nullptr, 
                            fill_result);
            
            // 检查原因
            if (consumer_info->worker->isAtEnd()) {
                // EOF：数据源已结束
                // ⭐ 参考 VideoProductionLine 的逻辑：
                // Consumer Worker 在共享模式下，EOF 意味着 Producer 已停止
                // 应该退出线程，而不是继续等待
                LOG4CPLUS_INFO(logger_, "[Worker '" << consumer_name 
                               << "'] 检测到数据源 EOF，正常退出");
                break;  // 退出线程
                
            } else {
                // 非 EOF 失败：可能是解码错误
                int failures = worker_stats->consecutive_failures.fetch_add(1) + 1;
                worker_stats->worker_frames_failed.fetch_add(1);
                group->stats.frames_failed.fetch_add(1);
                
                if (failures >= MAX_CONSECUTIVE_FAILURES) {
                    // FATAL：连续失败过多
                    LOG4CPLUS_ERROR(logger_, "[Worker '" << consumer_name 
                                   << "'] 连续失败 " << failures << " 次 - FATAL");
                    worker_stats->is_active.store(false);
                    break;
                }
                
                // 短暂休眠，避免忙等
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    pool_sptr->shutdown();
    LOG4CPLUS_INFO(logger_, "[Worker '" << consumer_name << "'] 线程结束 "
                   << "(produced=" << worker_stats->worker_frames_produced.load() 
                   << ", failed=" << worker_stats->worker_frames_failed.load() << ")");
}

// ============================================================
// 全局查询接口实现
// ============================================================

int64_t MultiWorkerProductionLine::getAllLineFramesProduced() const {
    int64_t total = 0;
    for (const auto& group : groups_) {
        if (group) {
            total += group->stats.frames_produced.load();
        }
    }
    return total;
}

int64_t MultiWorkerProductionLine::getAllLineFramesFailed() const {
    int64_t total = 0;
    for (const auto& group : groups_) {
        if (group) {
            total += group->stats.frames_failed.load();
        }
    }
    return total;
}

int MultiWorkerProductionLine::getActiveWorkerCount(size_t group_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    
    const auto& group = groups_[group_index];
    int count = 0;
    for (const auto& [name, stats] : group->worker_stats) {
        if (stats && stats->is_active.load()) {
            count++;
        }
    }
    return count;
}

void MultiWorkerProductionLine::setError(const std::string& error_msg) const {
    // 保存错误消息
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        error_queue_.push(error_msg);
        // 保持错误队列大小合理
        if (error_queue_.size() > 100) {
            error_queue_.pop();
        }
    }
    
    // 打印到控制台
    LOG4CPLUS_ERROR(logger_, "错误: " << error_msg);
}

// ============================================================
// 统计报告定时器
// ============================================================

void MultiWorkerProductionLine::startStatsReportTimer() {
    // 先停止旧定时器（如果存在）
    Timer::TimerId old_timer_id = stats_report_timer_id_.exchange(0);
    if (old_timer_id != 0) {
        stats_report_timer_.cancel(old_timer_id);
    }
    
    // 启动定时器服务
    stats_report_timer_.start();
    
    // 创建新定时器（每5秒调用一次 printDetailedStats）
    Timer::TimerId new_timer_id = stats_report_timer_.scheduleRepeated(
        5000,  // 5秒
        [this]() {
            // 在定时器线程中调用，printDetailedStats 是 const 方法，线程安全
            this->printDetailedStats();
        }
    );
    
    // 保存定时器ID
    stats_report_timer_id_.store(new_timer_id);
    
    LOG4CPLUS_INFO(logger_, "📊 统计报告定时器已启动（每5秒）");
}

void MultiWorkerProductionLine::stopStatsReportTimer() {
    // 取消定时器
    Timer::TimerId timer_id = stats_report_timer_id_.exchange(0);
    if (timer_id != 0) {
        stats_report_timer_.cancel(timer_id);
    }
    
    // 停止定时器服务
    stats_report_timer_.stop();
    
    LOG4CPLUS_INFO(logger_, "📊 统计报告定时器已停止");
}

