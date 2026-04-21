#include "productionline/line/MultiWorkerProductionLine.hpp"
#include "productionline/worker/base/WorkerFactory.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include "productionline/worker/core/FfmpegPacketRecorderWorker.hpp"
#include "productionline/worker/datasource/encodeddata/EncodedPacketSourceFromBuffer.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include "common/Logger.hpp"
#include "common/GlobalThreadPool.hpp"
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

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
    
    if (config_.groups.empty()) {
        LOG4CPLUS_WARN(logger_, "警告: 没有配置任何 WorkerGroup");
    }
    
    size_t total_producers = 0;
    size_t total_consumers = 0;
    for (const auto& group : config_.groups) {
        total_producers += group.producers.size();
        total_consumers += group.consumers.size();
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
    stop();
    groups_.clear();
    LOG4CPLUS_INFO(logger_, "析构完成");
}

std::string MultiWorkerProductionLine::getProducerNameForConsumer(
    const GroupConfig& config, const std::string& consumer_name)
{
    std::vector<std::string> producer_names;
    for (const auto& [name, _] : config.producers)
        producer_names.push_back(name);

    std::vector<std::string> consumer_names;
    for (const auto& [name, _] : config.consumers)
        consumer_names.push_back(name);

    size_t consumer_idx = SIZE_MAX;
    for (size_t i = 0; i < consumer_names.size(); i++) {
        if (consumer_names[i] == consumer_name) {
            consumer_idx = i;
            break;
        }
    }

    if (consumer_idx == SIZE_MAX || producer_names.empty())
        return "";

    switch (config.mode) {
        case GroupConfig::Mode::ONE_TO_ONE:
            return (consumer_idx < producer_names.size()) ? producer_names[consumer_idx] : "";
        case GroupConfig::Mode::ONE_TO_MANY:
        case GroupConfig::Mode::MANY_TO_ONE:
            return producer_names[0];
        case GroupConfig::Mode::MANY_TO_MANY:
            return producer_names[consumer_idx % producer_names.size()];
    }
    return "";
}

bool MultiWorkerProductionLine::validateConfig() const {
    
    for (size_t group_idx = 0; group_idx < config_.groups.size(); group_idx++) {
        const auto& group_config = config_.groups[group_idx];
        
        if (group_config.producers.empty()) {
            setError("Group[" + std::to_string(group_idx) + "] 没有配置任何生产者");
            return false;
        }
        
        if (group_config.consumers.empty()) {
            setError("Group[" + std::to_string(group_idx) + "] 没有配置任何消费者");
            return false;
        }
        
        size_t num_producers = group_config.producers.size();
        size_t num_consumers = group_config.consumers.size();
        
        switch (group_config.mode) {
            case GroupConfig::Mode::ONE_TO_ONE:
                if (num_producers != num_consumers) {
                    setError("Group[" + std::to_string(group_idx) 
                            + "] ONE_TO_ONE 模式：生产者数量(" + std::to_string(num_producers)
                            + ") 必须等于消费者数量(" + std::to_string(num_consumers) + ")");
                    return false;
                }
                break;
            case GroupConfig::Mode::ONE_TO_MANY:
                if (num_producers != 1) {
                    setError("Group[" + std::to_string(group_idx) 
                            + "] ONE_TO_MANY 模式：必须只有1个生产者，实际有 " 
                            + std::to_string(num_producers) + " 个");
                    return false;
                }
                break;
            case GroupConfig::Mode::MANY_TO_ONE:
                if (num_consumers != 1) {
                    setError("Group[" + std::to_string(group_idx) 
                            + "] MANY_TO_ONE 模式：必须只有1个消费者，实际有 " 
                            + std::to_string(num_consumers) + " 个");
                    return false;
                }
                break;
            case GroupConfig::Mode::MANY_TO_MANY:
                break;
        }
    }
    
    return true;
}

bool MultiWorkerProductionLine::start() {
    
    if (running_.load()) {
        LOG4CPLUS_WARN(logger_, "已经在运行");
        return false;
    }
    
    LOG4CPLUS_INFO(logger_, "⭐ 启动 WorkerGroup 架构...");
    
    initializeGlobalThreadPool(config_.thread_pool_size);
    
    LOG4CPLUS_INFO(logger_, "校验配置...");
    if (!validateConfig()) {
        LOG4CPLUS_ERROR(logger_, "配置校验失败");
        return false;
    }
    LOG4CPLUS_INFO(logger_, "配置校验通过");
    
    multi_line_registry_id_ = ComponentTopology::getInstance().registerLine("MultiWorkerProductionLine");
    
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
        
        group->topology_group_id = ComponentTopology::getInstance().registerGroup(group->group_id);
        ComponentTopology::getInstance().linkGroupToLine(multi_line_registry_id_, group->topology_group_id);
        
        group->config_ref = &group_config;
        
        if (!createProducersForGroup(group.get(), group_config)) {
            return false;
        }
        
        if (!setupSharedSources(group.get(), group_config)) {
            return false;
        }
        
        if (!setupCoordinator(group.get(), group_config)) {
            return false;
        }
        
        if (!createConsumersForGroup(group.get(), group_config)) {
            return false;
        }
        
        groups_.push_back(std::move(group));
        
        LOG4CPLUS_INFO(logger_, " [Group " << group_idx << "] '" 
                       << groups_.back()->group_id << "' 创建完成");
    }
    
    running_.store(true);
    start_time_ = std::chrono::steady_clock::now();
    
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
    
    running_.store(false);
    
    for (auto& group : groups_) {
        if (group) {
            group->is_running.store(false);
        }
    }
    
    LOG4CPLUS_INFO(logger_, "等待所有 Worker 线程退出...");
    GlobalThreadPool::getInstance().wait();
    
    for (auto& group : groups_) {
        if (!group) continue;
        
        for (auto& [name, info] : group->producers) {
            if (info.producer_line) {
                info.producer_line->stop();
            }
        }
        
        for (auto& [name, info] : group->consumers) {
            if (info.worker) {
                info.worker->close();
            }
        }
        
        LOG4CPLUS_INFO(logger_, " Group '" << group->group_id << "' 已停止 "
                       << "(success=" << group->stats.frames_produced.load()
                       << ", failed=" << group->stats.frames_failed.load() << ")");
    }
    
    for (auto& group : groups_) {
        if (group && group->topology_group_id != 0) {
            ComponentTopology::getInstance().unregisterGroup(group->topology_group_id);
            group->topology_group_id = 0;
        }
    }
    if (multi_line_registry_id_ != 0) {
        ComponentTopology::getInstance().unregisterLine(multi_line_registry_id_);
        multi_line_registry_id_ = 0;
    }
    
    LOG4CPLUS_INFO(logger_, "⭐ 生产车间已停止");
    LOG4CPLUS_INFO(logger_, "总统计: success=" << getAllLineFramesProduced()
                   << ", failed=" << getAllLineFramesFailed());
}

bool MultiWorkerProductionLine::createProducersForGroup(WorkerGroupRuntime* group, const GroupConfig& group_config) {
    LOG4CPLUS_INFO(logger_, "   创建 " << group_config.producers.size() 
                   << " 个生产者 Worker...");
    
    for (const auto& [producer_name, worker_config] : group_config.producers) {
        LOG4CPLUS_INFO(logger_, "     创建生产者 '" << producer_name << "'...");
        
        auto producer_line = std::make_unique<VideoProductionLine>(
            loop_, thread_count_, enable_monitor_
        );
        
        if (!producer_line->start(worker_config)) {
            setError("Failed to start producer: " + producer_name);
            groups_.clear();
            return false;
        }
        
        uint64_t buffer_pool_id = producer_line->getWorkingBufferPoolId();
        if (buffer_pool_id == 0) {
            setError("Producer failed to create BufferPool: " + producer_name);
            groups_.clear();
            return false;
        }
        
        auto buffer_pool_weak = ComponentTopology::getInstance().getPool(buffer_pool_id);
        auto pool = buffer_pool_weak.lock();
        if (!pool) {
            setError("Failed to get BufferPool from Registry: " + producer_name);
            groups_.clear();
            return false;
        }
        
        ComponentTopology::getInstance().linkProducerLineToGroup(
            group->topology_group_id, producer_line->getLineRegistryId());
        
        WorkerGroupRuntime::ProducerInfo info;
        info.producer_line = std::move(producer_line);
        info.buffer_pool_id = buffer_pool_id;
        info.buffer_pool_weak = buffer_pool_weak;
        
        group->producers[producer_name] = std::move(info);
        
        LOG4CPLUS_INFO(logger_, "     生产者 '" << producer_name 
                       << "' 已启动 (BufferPool ID: " << buffer_pool_id << ")");
    }
    
    return true;
}

bool MultiWorkerProductionLine::setupSharedSources(WorkerGroupRuntime* group, const GroupConfig& group_config) {
    LOG4CPLUS_INFO(logger_, "   设置共享数据源...");
    
    std::vector<std::string> producer_names;
    for (const auto& [name, _] : group_config.producers)
        producer_names.push_back(name);
    
    std::vector<std::string> consumer_names;
    for (const auto& [name, _] : group_config.consumers)
        consumer_names.push_back(name);
    
    for (const auto& producer_name : producer_names) {
        auto it = group->producers.find(producer_name);
        if (it == group->producers.end()) {
            setError("Producer not found in runtime: " + producer_name);
            groups_.clear();
            return false;
        }
        
        auto& producer_info = it->second;
        
        const AVCodecParameters* codec_params = nullptr;
        if (producer_info.producer_line) {
            auto worker = producer_info.producer_line->getWorker();
            if (worker) {
                codec_params = worker->getSourceCodecParameters();
            }
        }
        
        if (!codec_params) {
            setError("Cannot get codec_params from producer: " + producer_name);
            groups_.clear();
            return false;
        }
        
        size_t subscriber_count = 0;
        switch (group_config.mode) {
            case GroupConfig::Mode::ONE_TO_ONE:
                subscriber_count = 1;
                break;
            case GroupConfig::Mode::ONE_TO_MANY:
                subscriber_count = consumer_names.size();
                break;
            case GroupConfig::Mode::MANY_TO_ONE:
                subscriber_count = 1;
                break;
            case GroupConfig::Mode::MANY_TO_MANY:
                for (size_t ci = 0; ci < consumer_names.size(); ci++) {
                    size_t pi = ci % producer_names.size();
                    if (producer_names[pi] == producer_name) {
                        subscriber_count++;
                    }
                }
                break;
        }
        
        subscriber_count = std::max<size_t>(1, subscriber_count);
        auto shared_source = std::make_shared<EncodedPacketSourceFromBuffer>(codec_params, subscriber_count);
        shared_source->setSourceBufferPool(producer_info.buffer_pool_weak);
        
        group->shared_sources[producer_name] = shared_source;
        
        LOG4CPLUS_INFO(logger_, "     ⭐ 为生产者 '" << producer_name 
                       << "' 创建共享 EncodedPacketSourceFromBuffer (" << subscriber_count << " 个订阅者)");
    }
    
    return true;
}

bool MultiWorkerProductionLine::setupCoordinator(WorkerGroupRuntime* group, const GroupConfig& group_config) {
    if (group_config.enable_frame_sync && !group_config.callback_chain.empty()) {
        std::vector<std::string> consumer_names;
        for (const auto& [name, _] : group_config.consumers)
            consumer_names.push_back(name);
        
        auto coordinator = std::make_unique<WorkerSyncCoordinator>(
            consumer_names,
            group_config.callback_chain
        );
        
        group->coordinator = std::move(coordinator);
        
        LOG4CPLUS_INFO(logger_, "     ⭐ 创建了 WorkerSyncCoordinator"
                       << " (Workers: " << consumer_names.size()
                       << ", Callbacks: " << group_config.callback_chain.size() << ")");
    }
    
    return true;
}

bool MultiWorkerProductionLine::createConsumersForGroup(WorkerGroupRuntime* group, const GroupConfig& group_config) {
    LOG4CPLUS_INFO(logger_, "   创建 " << group_config.consumers.size() 
                   << " 个消费者 Worker...");
    
    for (const auto& [consumer_name, worker_cfg] : group_config.consumers) {
        LOG4CPLUS_INFO(logger_, "     创建消费者 '" << consumer_name << "'...");
        
        WorkerConfig consumer_config = worker_cfg;
        consumer_config.data_source.buffer_mode = true;
        
        if (group_config.enable_frame_sync) {
            consumer_config.data_source.deferred_commit = true;
            LOG4CPLUS_DEBUG(logger_, "       ⭐ 启用 deferred_commit（帧同步模式）");
        }
        
        std::string producer_name = getProducerNameForConsumer(group_config, consumer_name);
        if (producer_name.empty()) {
            setError("Cannot get producer name for consumer: " + consumer_name);
            groups_.clear();
            return false;
        }
        
        auto source_it = group->shared_sources.find(producer_name);
        WorkerGroupRuntime::ProducerInfo* producer_info_ptr = nullptr;
        
        if (source_it != group->shared_sources.end()) {
            consumer_config.data_source.shared_packet_source = source_it->second;
            LOG4CPLUS_INFO(logger_, "       ✅ 使用生产者 '" << producer_name 
                           << "' 的共享 EncodedPacketSourceFromBuffer");
        } else {
            auto pit = group->producers.find(producer_name);
            if (pit == group->producers.end()) {
                setError("Producer not found in runtime: " + producer_name);
                groups_.clear();
                return false;
            }
            producer_info_ptr = &pit->second;
            
            const AVCodecParameters* codec_params = nullptr;
            AVRational time_base = {1, 25};
            if (producer_info_ptr->producer_line) {
                auto worker = producer_info_ptr->producer_line->getWorker();
                if (worker) {
                    codec_params = worker->getSourceCodecParameters();
                    time_base = worker->getTimeBase();
                }
            }
            
            if (!codec_params) {
                setError("Cannot get codec_params from producer for consumer: " + consumer_name);
                groups_.clear();
                return false;
            }
            
            consumer_config.data_source.codec_params = codec_params;
            consumer_config.data_source.time_base = time_base;
            LOG4CPLUS_INFO(logger_, "       ✅ 普通模式：从生产者 '" 
                           << producer_name << "' 获取 codec_params");
        }
        
        auto consumer_worker = WorkerFactory::create(
            consumer_config, TopologyOwnerType::GROUP, group->topology_group_id);
        
        if (source_it == group->shared_sources.end() && producer_info_ptr) {
            if (!consumer_worker->setSourceBufferPool(producer_info_ptr->buffer_pool_weak)) {
                setError("Consumer failed to set source BufferPool: " + consumer_name);
                groups_.clear();
                return false;
            }
            LOG4CPLUS_INFO(logger_, "       ✅ 已设置源 BufferPool");
        }
        
        if (!consumer_worker->open()) {
            setError("Consumer failed to open: " + consumer_name);
            groups_.clear();
            return false;
        }
        
        WorkerGroupRuntime::ConsumerInfo cinfo;
        cinfo.worker = consumer_worker;
        
        BufferPoolType primary_type = consumer_worker->getPrimaryBufferPoolType();
        cinfo.buffer_pool_id = consumer_worker->getOutputBufferPoolId(primary_type);
        cinfo.buffer_pool_weak = ComponentTopology::getInstance().getPool(cinfo.buffer_pool_id);
        
        uint64_t buffer_pool_id_for_log = cinfo.buffer_pool_id;
        
        group->consumers[consumer_name] = std::move(cinfo);
        
        LOG4CPLUS_INFO(logger_, "     消费者 '" << consumer_name 
                       << "' 已创建并打开 (BufferPool ID: " << buffer_pool_id_for_log << ")");
    }
    
    return true;
}

bool MultiWorkerProductionLine::startGroupThreads() {
    LOG4CPLUS_INFO(logger_, "启动所有 WorkerGroup 调度任务...");
    
    auto& thread_pool = GlobalThreadPool::getInstance().getThreadPool();
    
    for (size_t i = 0; i < groups_.size(); i++) {
        auto& group = groups_[i];
        
        for (const auto& [name, _] : group->consumers) {
            group->worker_stats[name] = std::make_unique<WorkerGroupRuntime::WorkerProductionStats>();
        }
        
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

uint64_t MultiWorkerProductionLine::getGroupConsumerBufferPoolId(size_t group_index, size_t consumer_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    const auto& group = groups_[group_index];
    if (consumer_index >= group->consumers.size()) {
        return 0;
    }
    auto it = group->consumers.begin();
    std::advance(it, consumer_index);
    return it->second.buffer_pool_id;
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
        LOG4CPLUS_INFO(logger_, "    生产者数量: " << group->producers.size());
        LOG4CPLUS_INFO(logger_, "    Worker 数量: " << group->consumers.size());
        LOG4CPLUS_INFO(logger_, "    活跃 Worker: " << getActiveWorkerCount(i));
        
        for (const auto& [name, info] : group->consumers) {
            auto state_it = group->worker_stats.find(name);
            if (state_it != group->worker_stats.end()) {
                const auto* worker_stats = state_it->second.get();
                LOG4CPLUS_INFO(logger_, "      Worker '" << name << "':");
                LOG4CPLUS_INFO(logger_, "        成功生产帧数: " << worker_stats->worker_frames_produced.load());
                LOG4CPLUS_INFO(logger_, "        失败帧数: " << worker_stats->worker_frames_failed.load());
                LOG4CPLUS_INFO(logger_, "        连续失败: " << worker_stats->consecutive_failures.load());
                LOG4CPLUS_INFO(logger_, "        是否活跃: " << (worker_stats->is_active.load() ? "是" : "否"));
            }
        }
    }
    LOG4CPLUS_INFO(logger_, "================================================");
}

void MultiWorkerProductionLine::groupThreadFunc(const std::shared_ptr<WorkerGroupRuntime>& group) {
    LOG4CPLUS_INFO(logger_, "[Group '" << group->group_id << "'] 启动所有 Worker 任务");
    
    auto& thread_pool = GlobalThreadPool::getInstance().getThreadPool();
    
    for (auto& [consumer_name, consumer_info] : group->consumers) {
        if (!consumer_info.worker || !consumer_info.worker->isOpen()) {
            LOG4CPLUS_WARN(logger_, "   Worker '" << consumer_name << "' 不可用，跳过");
            continue;
        }
        
        thread_pool.detach_task([this, group, consumer_name]() {
            workerThreadFunc(group, consumer_name);
        });
        
        LOG4CPLUS_INFO(logger_, "   Worker '" << consumer_name << "' 任务已提交");
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
    if (!group->coordinator) {
        return true;
    }
    
    if (result.shouldBypassFrameSync()) {
        return true;
    }
    
    std::string producer_name = getProducerNameForConsumer(*group->config_ref, consumer_name);
    auto source_it = group->shared_sources.find(producer_name);
    if (source_it == group->shared_sources.end()) {
        LOG4CPLUS_ERROR(logger_, "[Worker '" << consumer_name 
                        << "'] shared_source not found for producer: " << producer_name);
        return true;
    }
    
    auto shared_source = std::dynamic_pointer_cast<EncodedPacketSourceFromBuffer>(source_it->second);
    if (!shared_source) {
        LOG4CPLUS_ERROR(logger_, "[Worker '" << consumer_name 
                        << "'] shared_source 不是 EncodedPacketSourceFromBuffer 类型");
        return true;
    }
    
    uint64_t frame_version = shared_source->getCurrentBufferVersion();
    
    bool sync_result = group->coordinator->arrive(consumer_name, frame_version, buffer, result);
    
    shared_source->commitEncodedPacket(consumer_info->worker.get());
    
    if (!sync_result && buffer != nullptr) {
        LOG4CPLUS_WARN(logger_, "[Worker '" << consumer_name 
                       << "'] 同步协调器拒绝提交 Frame " << frame_version);
    }
    
    return sync_result;
}

void MultiWorkerProductionLine::workerThreadFunc(
    const std::shared_ptr<WorkerGroupRuntime>& group,
    const std::string& consumer_name
) {
    LOG4CPLUS_INFO(logger_, "[Worker '" << consumer_name << "'] 线程启动");
    
    auto consumer_it = group->consumers.find(consumer_name);
    if (consumer_it == group->consumers.end()) {
        LOG4CPLUS_ERROR(logger_, "[Worker '" << consumer_name << "'] 未在 runtime 中找到");
        return;
    }
    auto* consumer_info = &consumer_it->second;
    
    auto stats_it = group->worker_stats.find(consumer_name);
    if (stats_it == group->worker_stats.end()) {
        LOG4CPLUS_ERROR(logger_, "[Worker '" << consumer_name << "'] 统计信息不存在");
        return;
    }
    auto* worker_stats = stats_it->second.get();
    
    auto pool_sptr = consumer_info->buffer_pool_weak.lock();
    if (!pool_sptr) {
        LOG4CPLUS_ERROR(logger_, "[Worker '" << consumer_name << "'] BufferPool 已销毁");
        return;
    }
                    
    const int MAX_CONSECUTIVE_FAILURES = 100;
    int buffer_wait_count = 0;
    
    while (group->is_running.load() && running_.load() && worker_stats->is_active.load()) {
        Buffer* buffer = nullptr;
        while (group->is_running.load() && running_.load() && buffer == nullptr) {
            buffer = pool_sptr->acquireFree(true, 100);
            if (buffer == nullptr) {
                buffer_wait_count++;
                if (buffer_wait_count % 100 == 0) {
                    LOG4CPLUS_DEBUG(logger_, "[Worker '" << consumer_name 
                                   << "'] 等待空闲 buffer (count=" << buffer_wait_count << ")");
                }
            }
        }
        
        if (!group->is_running.load() || !running_.load()) 
            break;
                        
        buffer_wait_count = 0;

        FillResult fill_result = consumer_info->worker->fillBuffer(0, buffer);

        bool stop_worker = false;
        switch (fill_result.toAction()) {
            case FillResult::ConsumerAction::kSubmit: {
                bool should_submit = performFrameSync(group, consumer_name, consumer_info,
                                                     buffer, fill_result);
                if (should_submit) {
                    pool_sptr->submitFilled(buffer);
                    worker_stats->worker_frames_produced.fetch_add(1);
                    group->stats.frames_produced.fetch_add(1);
                } else {
                    pool_sptr->releaseFree(buffer);
                }
                worker_stats->consecutive_failures.store(0);
                break;
            }

            case FillResult::ConsumerAction::kSkip:
                pool_sptr->releaseFree(buffer);
                performFrameSync(group, consumer_name, consumer_info, nullptr, fill_result);
                break;

            case FillResult::ConsumerAction::kRetry:
                pool_sptr->releaseFree(buffer);
                performFrameSync(group, consumer_name, consumer_info, nullptr, fill_result);
                break;

            case FillResult::ConsumerAction::kTerminate:
                pool_sptr->releaseFree(buffer);
                performFrameSync(group, consumer_name, consumer_info, nullptr, fill_result);

                if (fill_result.isEoFlush() || consumer_info->worker->isAtEnd()) {
                    LOG4CPLUS_INFO(logger_, "[Worker '" << consumer_name
                                   << "'] data source exhausted, exiting normally");
                    stop_worker = true;
                } else {
                    int failures = worker_stats->consecutive_failures.fetch_add(1) + 1;
                    worker_stats->worker_frames_failed.fetch_add(1);
                    group->stats.frames_failed.fetch_add(1);
                    LOG4CPLUS_ERROR_FMT(logger_,
                        "[Worker '%s'] fillBuffer terminal error: [%s] %s (consecutive=%d)",
                        consumer_name.c_str(),
                        errorSourceToString(fill_result.source()),
                        fill_result.statusString(),
                        failures);
                    if (failures >= MAX_CONSECUTIVE_FAILURES) {
                        LOG4CPLUS_ERROR(logger_, "[Worker '" << consumer_name
                                       << "'] " << MAX_CONSECUTIVE_FAILURES
                                       << " consecutive failures - FATAL, stopping");
                        worker_stats->is_active.store(false);
                        stop_worker = true;
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                break;
        }

        if (stop_worker) break;
    }
    pool_sptr->shutdown();

    {
        if (group->config_ref) {
            std::string producer_name = getProducerNameForConsumer(*group->config_ref, consumer_name);
            auto source_it = group->shared_sources.find(producer_name);
            if (source_it != group->shared_sources.end()) {
                auto shared_source = std::dynamic_pointer_cast<EncodedPacketSourceFromBuffer>(source_it->second);
                if (shared_source) {
                    shared_source->unsubscribe(consumer_info->worker.get());
                }
            }

            if (group->coordinator) {
                group->coordinator->removeWorker(consumer_name);
            }
        }
    }

    LOG4CPLUS_INFO(logger_, "[Worker '" << consumer_name << "'] 线程结束 "
                   << "(produced=" << worker_stats->worker_frames_produced.load() 
                   << ", failed=" << worker_stats->worker_frames_failed.load() << ")");
}

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
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        error_queue_.push(error_msg);
        if (error_queue_.size() > 100) {
            error_queue_.pop();
        }
    }
    
    LOG4CPLUS_ERROR(logger_, "错误: " << error_msg);
}
