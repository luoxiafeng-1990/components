#include "productionline/MultiWorkerProductionLine.hpp"
#include "productionline/worker/FfmpegPacketRecorderWorker.hpp"
#include "productionline/worker/BufferPacketSource.hpp"
#include "productionline/Connector.hpp"
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
    , stats_()
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
            if (conn_cfg.producer_ids.empty()) {
                setError("Group[" + std::to_string(group_idx) + "] Connector[" + std::to_string(conn_idx) 
                        + "] 没有关联任何生产者");
                return false;
            }
            
            // 校验连接器必须关联至少一个消费者
            if (conn_cfg.consumer_ids.empty()) {
                setError("Group[" + std::to_string(group_idx) + "] Connector[" + std::to_string(conn_idx) 
                        + "] 没有关联任何消费者");
                return false;
            }
            
            // 校验连接器的 producer_ids 必须在 Group 中存在
            for (const auto& pid : conn_cfg.producer_ids) {
                bool found = false;
                for (const auto& pcfg : group_config.producer_configs) {
                    if (pcfg.producer_id == pid) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    setError("Group[" + std::to_string(group_idx) + "] Connector[" + std::to_string(conn_idx) 
                            + "] 关联的生产者 ID '" + pid + "' 不存在");
                    return false;
                }
            }
            
            // 校验连接器的 consumer_ids 必须在 Group 中存在
            for (const auto& cid : conn_cfg.consumer_ids) {
                bool found = false;
                for (const auto& ccfg : group_config.consumer_configs) {
                    if (ccfg.consumer_id == cid) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    setError("Group[" + std::to_string(group_idx) + "] Connector[" + std::to_string(conn_idx) 
                            + "] 关联的消费者 ID '" + cid + "' 不存在");
                    return false;
                }
            }
            
            // 校验：根据连接器模式检查对应关系
            switch (conn_cfg.mode) {
                case Connector::Mode::ONE_TO_ONE:
                    // 1:1 模式：生产者数量必须等于消费者数量
                    if (conn_cfg.producer_ids.size() != conn_cfg.consumer_ids.size()) {
                        setError("Group[" + std::to_string(group_idx) + "] Connector[" + std::to_string(conn_idx) 
                                + "] ONE_TO_ONE 模式：生产者数量(" + std::to_string(conn_cfg.producer_ids.size()) 
                                + ") 必须等于消费者数量(" + std::to_string(conn_cfg.consumer_ids.size()) + ")");
                        return false;
                    }
                    break;
                    
                case Connector::Mode::ONE_TO_MANY:
                    // 1:N 模式：必须只有1个生产者
                    if (conn_cfg.producer_ids.size() != 1) {
                        setError("Group[" + std::to_string(group_idx) + "] Connector[" + std::to_string(conn_idx) 
                                + "] ONE_TO_MANY 模式：必须只有1个生产者，实际有 " 
                                + std::to_string(conn_cfg.producer_ids.size()) + " 个");
                        return false;
                    }
                    break;
                    
                case Connector::Mode::MANY_TO_ONE:
                    // N:1 模式：必须只有1个消费者
                    if (conn_cfg.consumer_ids.size() != 1) {
                        setError("Group[" + std::to_string(group_idx) + "] Connector[" + std::to_string(conn_idx) 
                                + "] MANY_TO_ONE 模式：必须只有1个消费者，实际有 " 
                                + std::to_string(conn_cfg.consumer_ids.size()) + " 个");
                        return false;
                    }
                    break;
                    
                case Connector::Mode::MANY_TO_MANY:
                    // N:M 模式：至少1个生产者，至少1个消费者（已在上面检查）
                    break;
            }
        }
        
        // 3.5 校验：检查是否有未连接的 Producer
        std::set<std::string> connected_producer_ids;
        for (const auto& conn_cfg : group_config.connector_configs) {
            for (const auto& pid : conn_cfg.producer_ids) {
                connected_producer_ids.insert(pid);
            }
        }
        for (const auto& pcfg : group_config.producer_configs) {
            if (connected_producer_ids.find(pcfg.producer_id) == connected_producer_ids.end()) {
                setError("Group[" + std::to_string(group_idx) + "] 生产者 '" + pcfg.producer_id 
                        + "' 没有被任何连接器连接");
                return false;
            }
        }
        
        // 3.6 校验：检查是否有未连接的 Consumer
        std::set<std::string> connected_consumer_ids;
        for (const auto& conn_cfg : group_config.connector_configs) {
            for (const auto& cid : conn_cfg.consumer_ids) {
                connected_consumer_ids.insert(cid);
            }
        }
        for (const auto& ccfg : group_config.consumer_configs) {
            if (connected_consumer_ids.find(ccfg.consumer_id) == connected_consumer_ids.end()) {
                setError("Group[" + std::to_string(group_idx) + "] 消费者 '" + ccfg.consumer_id 
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
    
    // ========== 步骤2：初始化全局线程池 ==========
    GlobalThreadPool::getInstance().setSize(config_.thread_pool_size);
    LOG4CPLUS_INFO(logger_, "全局线程池已初始化 (size=" << config_.thread_pool_size << ")");
    
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
        
        auto group = std::make_unique<GroupData>();
        group->group_id = group_config.group_id.empty() 
                        ? ("Group_" + std::to_string(group_idx)) 
                        : group_config.group_id;
        
        // ========== 步骤4.1：创建所有生产者 ==========
        LOG4CPLUS_INFO(logger_, "   创建 " << group_config.producer_configs.size() 
                       << " 个生产者 Worker...");
        
        for (const auto& pcfg : group_config.producer_configs) {
            LOG4CPLUS_INFO(logger_, "     创建生产者 '" << pcfg.producer_id << "'...");
            
            // 创建父类 VideoProductionLine 作为生产者
            auto producer_line = std::make_unique<VideoProductionLine>(
                loop_, thread_count_, enable_monitor_
            );
            
            // 调用父类的 start() 方法启动生产者
            if (!producer_line->start(pcfg.worker_config)) {
                setError("Failed to start producer: " + pcfg.producer_id);
                groups_.clear();
                return false;
            }
            
            // 获取生产者的 BufferPool 信息
            uint64_t buffer_pool_id = producer_line->getWorkingBufferPoolId();
            if (buffer_pool_id == 0) {
                setError("Producer failed to create BufferPool: " + pcfg.producer_id);
                groups_.clear();
                return false;
            }
            
            auto buffer_pool_weak = BufferPoolRegistry::getInstance().getPool(buffer_pool_id);
            auto pool = buffer_pool_weak.lock();
            if (!pool) {
                setError("Failed to get BufferPool from Registry: " + pcfg.producer_id);
                groups_.clear();
                return false;
            }
            
            // 保存生产者信息（包括 BufferPool）
            auto producer_info = std::make_unique<GroupData::ProducerInfo>();
            producer_info->producer_id = pcfg.producer_id;
            producer_info->producer_line = std::move(producer_line);
            producer_info->buffer_pool_id = buffer_pool_id;
            producer_info->buffer_pool_weak = buffer_pool_weak;
            
            group->producer_by_id[pcfg.producer_id] = producer_info.get();
            group->producers.push_back(std::move(producer_info));
            
            LOG4CPLUS_INFO(logger_, "     生产者 '" << pcfg.producer_id 
                           << "' 已启动 (BufferPool ID: " << buffer_pool_id << ")");
        }
        
        // ========== 步骤4.2：创建所有 Connector 并保存 shared_source（v2.20 优化）==========
        LOG4CPLUS_INFO(logger_, "   创建 " << group_config.connector_configs.size() 
                       << " 个连接器...");
        
        for (size_t conn_idx = 0; conn_idx < group_config.connector_configs.size(); conn_idx++) {
            const auto& conn_cfg = group_config.connector_configs[conn_idx];
            
            LOG4CPLUS_INFO(logger_, "     创建连接器 #" << conn_idx 
                           << " (Mode: " << static_cast<int>(conn_cfg.mode) << ")...");
            
            // 4.2.1 通过 producer_ids 找到索引
            std::vector<size_t> producer_indices;
            for (const auto& pid : conn_cfg.producer_ids) {
                bool found = false;
                for (size_t i = 0; i < group->producers.size(); i++) {
                    if (group->producers[i]->producer_id == pid) {
                        producer_indices.push_back(i);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    setError("Producer not found: " + pid);
                    groups_.clear();
                    return false;
                }
            }
            
            // 4.2.2 通过 consumer_ids 找到索引
            std::vector<size_t> consumer_indices;
            for (const auto& cid : conn_cfg.consumer_ids) {
                bool found = false;
                for (size_t i = 0; i < group_config.consumer_configs.size(); i++) {
                    if (group_config.consumer_configs[i].consumer_id == cid) {
                        consumer_indices.push_back(i);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    setError("Consumer not found: " + cid);
                    groups_.clear();
                    return false;
                }
            }
            
            // 4.2.3 创建连接器
            auto connector = std::make_unique<Connector>(
                conn_cfg.mode,
                producer_indices,
                consumer_indices
            );
            
            // 4.2.4 如果是 ONE_TO_MANY，创建 shared_source 并保存到 Connector
            if (conn_cfg.mode == Connector::Mode::ONE_TO_MANY) {
                if (conn_cfg.producer_ids.empty() || producer_indices.empty()) {
                    setError("ONE_TO_MANY connector has no producers");
                    groups_.clear();
                    return false;
                }
                
                // 获取第一个生产者的 codec_params
                size_t producer_idx = producer_indices[0];
                auto* producer_info = group->producers[producer_idx].get();
                
                const AVCodecParameters* codec_params = nullptr;
                if (producer_info->producer_line) {
                    auto worker_facade = producer_info->producer_line->getWorkerFacade();
                    if (worker_facade) {
                        codec_params = worker_facade->getCodecParameters();
                    }
                }
                
                if (!codec_params) {
                    setError("Cannot get codec_params from producer: " + conn_cfg.producer_ids[0]);
                    groups_.clear();
                    return false;
                }
                
                // 创建共享 BufferPacketSource
                size_t subscriber_count = conn_cfg.consumer_ids.size();
                auto shared_source = std::make_shared<BufferPacketSource>(codec_params, subscriber_count);
                
                // 设置源 BufferPool
                shared_source->setSourceBufferPool(producer_info->buffer_pool_weak);
                
                // ⭐ v2.20：保存到 Connector 内部
                connector->setSharedSource(shared_source);
                
                LOG4CPLUS_INFO(logger_, "     ⭐ ONE_TO_MANY 连接器：创建共享 BufferPacketSource (" 
                               << subscriber_count << " 个订阅者)");
            }
            
            group->connectors.push_back(std::move(connector));
            LOG4CPLUS_INFO(logger_, "     连接器 #" << conn_idx << " 已创建");
        }
        
        // ========== 步骤4.3：创建并打开所有消费者（v2.20 优化：根据 Connector 模式配置）==========
        LOG4CPLUS_INFO(logger_, "   创建 " << group_config.consumer_configs.size() 
                       << " 个消费者 Worker...");
        
        for (size_t consumer_cfg_idx = 0; consumer_cfg_idx < group_config.consumer_configs.size(); consumer_cfg_idx++) {
            const auto& ccfg = group_config.consumer_configs[consumer_cfg_idx];
            LOG4CPLUS_INFO(logger_, "     创建消费者 '" << ccfg.consumer_id << "'...");
            
            // 配置 buffer mode
            WorkerConfig consumer_config = ccfg.worker_config;
            consumer_config.decoder.datasource_buffer_mode = true;
            
            // 4.3.1 查找该消费者所属的 Connector
            Connector* owner_connector = nullptr;
            int consumer_idx_in_connector = -1;
            size_t connector_idx = 0;
            
            for (auto& conn : group->connectors) {
                const auto& consumer_indices = conn->getConsumerIndices();
                for (size_t i = 0; i < consumer_indices.size(); i++) {
                    if (consumer_indices[i] == consumer_cfg_idx) {
                        owner_connector = conn.get();
                        consumer_idx_in_connector = static_cast<int>(i);
                        break;
                    }
                }
                if (owner_connector) break;
                connector_idx++;
            }
            
            if (!owner_connector) {
                setError("Consumer '" + ccfg.consumer_id + "' is not connected to any Connector");
                groups_.clear();
                return false;
            }
            
            // 4.3.2 根据 Connector 模式配置 config
            auto shared_source = owner_connector->getSharedSource();
            GroupData::ProducerInfo* producer_info = nullptr;
            
            if (shared_source) {
                // ⭐ ONE_TO_MANY 模式：使用共享数据源
                consumer_config.decoder.shared_packet_source = shared_source;
                LOG4CPLUS_INFO(logger_, "       ✅ ONE_TO_MANY 模式：使用 Connector #" 
                               << connector_idx << " 的共享 BufferPacketSource");
            } else {
                // ⭐ 普通模式：设置 codec_params（用于创建独立 BufferPacketSource）
                int producer_idx = owner_connector->getProducerIndexForConsumer(consumer_idx_in_connector);
                if (producer_idx < 0 || static_cast<size_t>(producer_idx) >= owner_connector->getProducerIndices().size()) {
                    setError("Cannot get producer index for consumer: " + ccfg.consumer_id);
                    groups_.clear();
                    return false;
                }
                
                size_t actual_producer_idx = owner_connector->getProducerIndices()[producer_idx];
                producer_info = group->producers[actual_producer_idx].get();
                
                // 获取 producer 的 codec_params
                const AVCodecParameters* codec_params = nullptr;
                AVRational time_base = {1, 25};
                if (producer_info->producer_line) {
                    auto worker_facade = producer_info->producer_line->getWorkerFacade();
                    if (worker_facade) {
                        codec_params = worker_facade->getCodecParameters();
                        time_base = worker_facade->getTimeBase();
                    }
                }
                
                if (!codec_params) {
                    setError("Cannot get codec_params from producer for consumer: " + ccfg.consumer_id);
                    groups_.clear();
                    return false;
                }
                
                consumer_config.decoder.codec_params = codec_params;
                consumer_config.decoder.time_base = time_base;
                LOG4CPLUS_INFO(logger_, "       ✅ 普通模式：从生产者 '" 
                               << producer_info->producer_id << "' 获取 codec_params");
            }
            
            // 4.3.3 创建消费者 Worker（构造函数根据 config 创建 packet_source）
            auto consumer_worker = std::make_shared<BufferFillingWorkerFacade>(consumer_config);
            
            // 4.3.4 如果是普通模式，设置 BufferPool
            if (!shared_source && producer_info) {
                if (!consumer_worker->setSourceBufferPool(producer_info->buffer_pool_weak)) {
                    setError("Consumer failed to set source BufferPool: " + ccfg.consumer_id);
                    groups_.clear();
                    return false;
                }
                LOG4CPLUS_INFO(logger_, "       ✅ 已设置源 BufferPool");
            }
            
            // 4.3.5 打开消费者（创建输出 BufferPool）
            if (!consumer_worker->open()) {
                setError("Consumer failed to open: " + ccfg.consumer_id);
                groups_.clear();
                return false;
            }
            
            // 保存消费者信息
            auto consumer_info = std::make_unique<GroupData::ConsumerInfo>();
            consumer_info->consumer_id = ccfg.consumer_id;
            consumer_info->worker = consumer_worker;
            
            // 获取消费者的输出 BufferPool 信息
            BufferPoolType primary_type = consumer_worker->getPrimaryBufferPoolType();
            consumer_info->buffer_pool_id = consumer_worker->getOutputBufferPoolId(primary_type);
            consumer_info->buffer_pool_weak = BufferPoolRegistry::getInstance().getPool(consumer_info->buffer_pool_id);
            
            // 保存 buffer_pool_id 用于日志（避免 use-after-move）
            uint64_t buffer_pool_id_for_log = consumer_info->buffer_pool_id;
            
            group->consumer_by_id[ccfg.consumer_id] = consumer_info.get();
            group->consumers.push_back(std::move(consumer_info));
            
            LOG4CPLUS_INFO(logger_, "     消费者 '" << ccfg.consumer_id 
                           << "' 已创建并打开 (BufferPool ID: " << buffer_pool_id_for_log << ")");
        }
        
        // ========== v2.20：步骤4.4 已删除（Connector 在步骤4.2创建，Consumer 在步骤4.3打开）==========
        
        groups_.push_back(std::move(group));
        
        LOG4CPLUS_INFO(logger_, " [Group " << group_idx << "] '" 
                       << groups_.back()->group_id << "' 创建完成");
    }
    
    // ========== 步骤5：初始化状态 ==========
    running_.store(true);
    start_time_ = std::chrono::steady_clock::now();
    
    // ========== 步骤6：启动所有 Group 线程 ==========
    LOG4CPLUS_INFO(logger_, "启动所有 WorkerGroup 线程...");
    
    for (size_t i = 0; i < groups_.size(); i++) {
        auto& group = groups_[i];
        group->is_running.store(true);
        
        try {
            group->group_thread = std::thread(&MultiWorkerProductionLine::groupThreadFunc, this, group.get());
            LOG4CPLUS_INFO(logger_, " [Group " << i << "] '" << group->group_id 
                           << "' 线程已启动");
        } catch (const std::exception& e) {
            LOG4CPLUS_ERROR(logger_, "Failed to start Group thread: " << e.what());
            running_.store(false);
            for (auto& g : groups_) {
                if (g) {
                    g->is_running.store(false);
                    if (g->group_thread.joinable()) {
                        g->group_thread.join();
                    }
                }
            }
            groups_.clear();
            setError(std::string("Failed to start Group thread: ") + e.what());
            return false;
        }
    }
    
    LOG4CPLUS_INFO(logger_, "⭐ WorkerGroup 架构启动成功！");
    return true;
}



void MultiWorkerProductionLine::stop() {
    if (!running_.load()) {
        return;
    }
    
    LOG4CPLUS_INFO(logger_, "⭐ 停止 WorkerGroup 架构...");
    
    // 设置停止标志
    running_.store(false);
    
    // 停止所有 WorkerGroup 线程
    for (auto& group : groups_) {
        if (group) {
            group->is_running.store(false);
            if (group->group_thread.joinable()) {
                LOG4CPLUS_INFO(logger_, " 等待 Group '" << group->group_id << "' 线程退出...");
                group->group_thread.join();
            }
        }
    }
    
    // 等待全局线程池任务完成
    GlobalThreadPool::getInstance().wait();
    
    // 停止所有生产者和消费者
    for (auto& group : groups_) {
        if (!group) continue;
        
        // 停止所有生产者
        for (auto& producer_info : group->producers) {
            if (producer_info && producer_info->producer_line) {
                producer_info->producer_line->stop();
            }
        }
        
        // 关闭所有消费者
        for (auto& consumer_info : group->consumers) {
            if (consumer_info && consumer_info->worker) {
                consumer_info->worker->close();
            }
        }
        
        LOG4CPLUS_INFO(logger_, " Group '" << group->group_id << "' 已停止 "
                       << "(processed=" << group->processed_count.load() 
                       << ", success=" << group->success_count.load()
                       << ", error=" << group->error_count.load() << ")");
    }
    
    LOG4CPLUS_INFO(logger_, "⭐ WorkerGroup 架构已停止");
    LOG4CPLUS_INFO(logger_, "总统计: processed=" << stats_.total_packets_processed.load() 
                   << ", success=" << stats_.total_packets_succeeded.load()
                   << ", failed=" << stats_.total_packets_failed.load());
}

// ============================================================
// 查询接口实现
// ============================================================

uint64_t MultiWorkerProductionLine::getGroupProducerBufferPoolId(size_t group_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    const auto& group = groups_[group_index];
    // 返回第一个生产者的 BufferPool ID（如果有多个生产者，可能需要扩展接口）
    if (group->producers.empty() || !group->producers[0]) {
        return 0;
    }
    return group->producers[0]->buffer_pool_id;
}

size_t MultiWorkerProductionLine::getGroupConsumerCount(size_t group_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    return groups_[group_index]->consumers.size();
}

uint64_t MultiWorkerProductionLine::getGroupConsumerBufferPoolId(size_t group_index, size_t consumer_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    const auto& group = groups_[group_index];
    if (consumer_index >= group->consumers.size() || !group->consumers[consumer_index]) {
        return 0;
    }
    return group->consumers[consumer_index]->buffer_pool_id;
}

void MultiWorkerProductionLine::printDetailedStats() const {
    
    LOG4CPLUS_INFO(logger_, "========== WorkerGroup 详细统计信息 ==========");
    LOG4CPLUS_INFO(logger_, "全局统计:");
    LOG4CPLUS_INFO(logger_, "  总处理packet数: " << stats_.total_packets_processed.load());
    LOG4CPLUS_INFO(logger_, "  成功packet数: " << stats_.total_packets_succeeded.load());
    LOG4CPLUS_INFO(logger_, "  失败packet数: " << stats_.total_packets_failed.load());
    
    LOG4CPLUS_INFO(logger_, "WorkerGroup 统计 (共 " << groups_.size() << " 个):");
    for (size_t i = 0; i < groups_.size(); i++) {
        if (!groups_[i]) continue;
        
        const auto& group = groups_[i];
        LOG4CPLUS_INFO(logger_, "  [Group " << i << "] '" << group->group_id << "':");
        LOG4CPLUS_INFO(logger_, "    已处理: " << group->processed_count.load());
        LOG4CPLUS_INFO(logger_, "    成功: " << group->success_count.load());
        LOG4CPLUS_INFO(logger_, "    错误: " << group->error_count.load());
        LOG4CPLUS_INFO(logger_, "    生产者数量: " << group->producers.size());
        LOG4CPLUS_INFO(logger_, "    消费者数量: " << group->consumers.size());
        LOG4CPLUS_INFO(logger_, "    连接器数量: " << group->connectors.size());
    }
    LOG4CPLUS_INFO(logger_, "================================================");
}

// ============================================================
// 内部方法实现
// ============================================================

void MultiWorkerProductionLine::groupThreadFunc(GroupData* group) {
    LOG4CPLUS_INFO(logger_, "[Group '" << group->group_id << "'] 线程启动");
    
    // 获取全局线程池
    auto& thread_pool = GlobalThreadPool::getInstance().getThreadPool();
    
    // 默认超时时间（5秒）
    const int DEFAULT_TIMEOUT_MS = 5000;
    
    while (group->is_running.load() && running_.load()) {
        // ========== 步骤1：统计活跃的消费者 ==========
        int active_consumers = 0;
        for (auto& connector : group->connectors) {
            for (size_t consumer_idx : connector->getConsumerIndices()) {
                if (consumer_idx < group->consumers.size() && 
                    group->consumers[consumer_idx] && 
                    group->consumers[consumer_idx]->worker->isOpen()) {
                    active_consumers++;
                }
            }
        }
        
        if (active_consumers == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // ========== 步骤2：创建同步门闩 ==========
        auto latch = std::make_shared<CountDownLatch>(active_consumers);
        std::atomic<int> success_count{0};
        std::atomic<int> error_count{0};
        
        // ========== 步骤3：提交任务给所有活跃消费者 ==========
        for (auto& connector : group->connectors) {
            for (size_t i = 0; i < connector->getConsumerIndices().size(); i++) {
                size_t consumer_idx = connector->getConsumerIndices()[i];
                
                if (consumer_idx >= group->consumers.size() || 
                    !group->consumers[consumer_idx] || 
                    !group->consumers[consumer_idx]->worker->isOpen()) {
                    continue;
                }
                
                auto* consumer_info = group->consumers[consumer_idx].get();
                
                // 提交到全局线程池
                thread_pool.detach_task([this, group, consumer_info, latch, &success_count, &error_count, consumer_idx]() {
                    // ========== 步骤1：检查 BufferPool 是否可用 ==========
                    auto pool_sptr = consumer_info->buffer_pool_weak.lock();
                    if (!pool_sptr) {
                        LOG4CPLUS_ERROR(logger_, "[Consumer '" << consumer_info->consumer_id 
                                       << "'] BufferPool 不存在或已销毁");
                        error_count.fetch_add(1);
                        group->error_count.fetch_add(1);
                        latch->countDown();
                        return;
                    }
                    
                    // ========== 步骤2：循环调用 fillBuffer 直到失败 ==========
                    int frames_processed = 0;
                    
                    while (true) {
                        // 2.1 获取空闲 Buffer（100ms 超时）
                        Buffer* buffer = pool_sptr->acquireFree(true, 100);
                        if (!buffer) {
                            // 无法获取空闲 Buffer（资源限制），退出循环
                            // LOG4CPLUS_DEBUG(logger_, "[Consumer '" << consumer_info->consumer_id 
                            //                << "'] 无法获取空闲 Buffer（超时），已处理 " << frames_processed << " 帧");
                            break;
                        }
                        
                        // 2.2 调用 fillBuffer 填充数据
                        bool process_success = consumer_info->worker->fillBuffer(0, buffer);
                        
                        if (process_success) {
                            // ✅ 填充成功：提交到已填充队列，继续循环
                            pool_sptr->submitFilled(buffer);
                            frames_processed++;
                            success_count.fetch_add(1);
                        } else {
                            // ❌ 填充失败（缓存已空，无法立即读取新 packet）：释放 buffer，退出循环
                            pool_sptr->releaseFree(buffer);
                            break;
                        }
                    }
                    
                    // ========== 步骤3：统计结果 ==========
                    if (frames_processed > 0) {
                        // 成功处理了至少一帧
                        group->success_count.fetch_add(frames_processed);
                        // LOG4CPLUS_DEBUG(logger_, "[Consumer '" << consumer_info->consumer_id 
                        //                << "'] 本轮处理 " << frames_processed << " 帧");
                    } else {
                        // 一帧都没处理成功，可能是真正的错误
                        error_count.fetch_add(1);
                        group->error_count.fetch_add(1);
                    }
                    
                    latch->countDown();
                });
            }
        }
        
        // ========== 步骤4：同步等待所有消费者完成 ==========
        bool all_done = latch->wait(DEFAULT_TIMEOUT_MS);
        
        if (!all_done) {
            LOG4CPLUS_ERROR(logger_, "[Group '" << group->group_id 
                           << "'] 等待消费者完成超时");
        } else {
            group->processed_count.fetch_add(1);
            
            if (success_count.load() > 0) {
                group->success_count.fetch_add(success_count.load());
                stats_.total_packets_succeeded.fetch_add(success_count.load());
            }
            if (error_count.load() > 0) {
                group->error_count.fetch_add(error_count.load());
                stats_.total_packets_failed.fetch_add(error_count.load());
            }
            
            stats_.total_packets_processed.fetch_add(1);
        }
        
        // ⚠️ 注意：这里需要一个短暂的延迟
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    LOG4CPLUS_INFO(logger_, "[Group '" << group->group_id << "'] 线程结束 "
                   << "(processed=" << group->processed_count.load() 
                   << ", success=" << group->success_count.load()
                   << ", error=" << group->error_count.load() << ")");
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

