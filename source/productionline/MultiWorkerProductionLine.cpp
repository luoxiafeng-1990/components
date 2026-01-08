#include "productionline/MultiWorkerProductionLine.hpp"
#include "productionline/worker/FfmpegPacketRecorderWorker.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "common/Logger.hpp"
#include <algorithm>
#include <chrono>

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
    , thread_pool_(nullptr)
    , stats_()
    , log_prefix_("[MultiWorkerProductionLine]")
{
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " ⭐ 创建 WorkerGroup 架构: groups=" << config_.groups.size()
                   << ", thread_pool_size=" << config_.thread_pool_size);
    
    // 验证配置
    if (config_.groups.empty()) {
        LOG4CPLUS_WARN(logger, log_prefix_ << " 警告: 没有配置任何 WorkerGroup");
    }
    
    // 统计总的生产者和消费者数量
    size_t total_producers = config_.groups.size();  // 每个 Group 1 个生产者
    size_t total_consumers = 0;
    for (const auto& group : config_.groups) {
        total_consumers += group.consumer_configs.size();
    }
    LOG4CPLUS_INFO(logger, log_prefix_ << " 总计: producers=" << total_producers 
                   << ", consumers=" << total_consumers);
    
    if (config_.thread_pool_size < 1) {
        LOG4CPLUS_WARN(logger, log_prefix_ << " 警告: thread_pool_size < 1, 使用默认值 4");
        config_.thread_pool_size = 4;
    }
}

MultiWorkerProductionLine::~MultiWorkerProductionLine() {
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " 析构开始...");
    
    // 停止所有线程
    stop();
    
    // 清理资源
    groups_.clear();
    thread_pool_.reset();
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " 析构完成");
}

// ============================================================
// 核心接口实现
// ============================================================

bool MultiWorkerProductionLine::start() {
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
    
    // 检查是否已经在运行
    if (running_.load()) {
        LOG4CPLUS_WARN(logger, log_prefix_ << " 已经在运行");
        return false;
    }
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " ⭐ 启动 WorkerGroup 架构...");
    
    // 1. 创建共享线程池
    LOG4CPLUS_INFO(logger, log_prefix_ << " 创建共享线程池 (size=" << config_.thread_pool_size << ")...");
    thread_pool_ = std::make_unique<BS::thread_pool<>>(config_.thread_pool_size);
    
    // 2. 为每个 WorkerGroup 创建运行时环境
    LOG4CPLUS_INFO(logger, log_prefix_ << " 创建 " << config_.groups.size() << " 个 WorkerGroup...");
    groups_.reserve(config_.groups.size());
    
    for (size_t group_idx = 0; group_idx < config_.groups.size(); group_idx++) {
        const auto& group_config = config_.groups[group_idx];
        
        LOG4CPLUS_INFO(logger, log_prefix_ << "  [Group " << group_idx << "] 创建 WorkerGroup '" 
                       << group_config.group_id << "'...");
        
        auto group_runtime = std::make_unique<WorkerGroupRuntime>();
        group_runtime->group_id = group_config.group_id.empty() 
                                 ? ("Group_" + std::to_string(group_idx)) 
                                 : group_config.group_id;
        
        // 设置 Group 配置（使用 Group 配置或全局默认配置）
        group_runtime->sync_timeout_ms = (group_config.sync_timeout_ms > 0) 
                                        ? group_config.sync_timeout_ms 
                                        : config_.default_sync_timeout_ms;
        group_runtime->max_consecutive_errors = (group_config.max_consecutive_errors > 0) 
                                               ? group_config.max_consecutive_errors 
                                               : config_.default_max_consecutive_errors;
        group_runtime->continue_on_error = group_config.continue_on_error;
        
        // 2.1 创建生产者
        LOG4CPLUS_INFO(logger, log_prefix_ << "    创建生产者 Worker...");
        if (!createProducer(group_runtime.get(), group_config.producer_config)) {
            setError("Failed to create producer for group: " + group_runtime->group_id);
            groups_.clear();
            thread_pool_.reset();
            return false;
        }
        
        // 2.2 从生产者获取编解码器参数（用于消费者 Buffer 模式）
        const AVCodecParameters* producer_codec_params = nullptr;
        auto worker_facade_sptr = group_runtime->producer_line->getWorkerFacade();
        if (worker_facade_sptr) {
            WorkerBase* worker_base = worker_facade_sptr->getWorkerBase();
            if (worker_base) {
                // 尝试转换为 FfmpegPacketRecorderWorker
                FfmpegPacketRecorderWorker* rtsp_worker = dynamic_cast<FfmpegPacketRecorderWorker*>(worker_base);
                if (rtsp_worker) {
                    producer_codec_params = rtsp_worker->getCodecParameters();
                    if (producer_codec_params) {
                        LOG4CPLUS_INFO(logger, log_prefix_ << "    获取到生产者编解码器参数 (codec_id=" 
                                       << producer_codec_params->codec_id << ")");
                    } else {
                        LOG4CPLUS_WARN(logger, log_prefix_ << "    生产者编解码器参数为 nullptr");
                    }
                }
            }
        }
        
        // 2.3 创建消费者列表
        LOG4CPLUS_INFO(logger, log_prefix_ << "    创建 " << group_config.consumer_configs.size() << " 个消费者 Worker...");
        if (!createConsumers(group_runtime.get(), group_config.consumer_configs, producer_codec_params)) {
            setError("Failed to create consumers for group: " + group_runtime->group_id);
            groups_.clear();
            thread_pool_.reset();
            return false;
        }
        
        LOG4CPLUS_INFO(logger, log_prefix_ << "  [Group " << group_idx << "] '" << group_runtime->group_id 
                       << "' 创建完成 (producer_pool_id=" << group_runtime->producer_buffer_pool_id 
                       << ", consumers=" << group_runtime->consumers.size() << ")");
        
        groups_.push_back(std::move(group_runtime));
    }
    
    // 3. 设置全局运行状态
    running_.store(true);
    start_time_ = std::chrono::steady_clock::now();
    
    // 4. 启动每个 Group 的独立线程
    LOG4CPLUS_INFO(logger, log_prefix_ << " 启动所有 WorkerGroup 线程...");
    for (auto& group : groups_) {
        group->is_running.store(true);
        group->group_thread = std::thread(&MultiWorkerProductionLine::groupThreadFunc, this, group.get());
        LOG4CPLUS_INFO(logger, log_prefix_ << "  [Group] '" << group->group_id << "' 线程已启动");
    }
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " ⭐ WorkerGroup 架构启动成功！");
    return true;
}



void MultiWorkerProductionLine::stop() {
    if (!running_.load()) {
        return;
    }
    
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
    LOG4CPLUS_INFO(logger, log_prefix_ << " ⭐ 停止 WorkerGroup 架构...");
    
    // 设置停止标志
    running_.store(false);
    
    // 停止所有 WorkerGroup 线程
    for (auto& group : groups_) {
        if (group) {
            group->is_running.store(false);
            if (group->group_thread.joinable()) {
                LOG4CPLUS_INFO(logger, log_prefix_ << "  等待 Group '" << group->group_id << "' 线程退出...");
                group->group_thread.join();
            }
        }
    }
    
    // 停止线程池（等待所有任务完成）
    if (thread_pool_) {
        LOG4CPLUS_INFO(logger, log_prefix_ << " 等待线程池任务完成...");
        thread_pool_->wait();
        thread_pool_.reset();
    }
    
    // 停止所有生产者和消费者
    for (auto& group : groups_) {
        if (!group) continue;
        
        // 停止生产者
        if (group->producer_line) {
            group->producer_line->stop();
        }
        
        // 关闭所有消费者
        for (auto& consumer : group->consumers) {
            if (consumer && consumer->worker) {
                consumer->worker->close();
            }
        }
        
        LOG4CPLUS_INFO(logger, log_prefix_ << "  Group '" << group->group_id << "' 已停止 "
                       << "(processed=" << group->processed_count.load() 
                       << ", success=" << group->success_count.load()
                       << ", error=" << group->error_count.load() << ")");
    }
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " ⭐ WorkerGroup 架构已停止");
    LOG4CPLUS_INFO(logger, log_prefix_ << " 总统计: processed=" << stats_.total_packets_processed.load() 
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
    return groups_[group_index]->producer_buffer_pool_id;
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
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " ========== WorkerGroup 详细统计信息 ==========");
    LOG4CPLUS_INFO(logger, log_prefix_ << " 全局统计:");
    LOG4CPLUS_INFO(logger, log_prefix_ << "   总处理packet数: " << stats_.total_packets_processed.load());
    LOG4CPLUS_INFO(logger, log_prefix_ << "   成功packet数: " << stats_.total_packets_succeeded.load());
    LOG4CPLUS_INFO(logger, log_prefix_ << "   失败packet数: " << stats_.total_packets_failed.load());
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " WorkerGroup 统计 (共 " << groups_.size() << " 个):");
    for (size_t i = 0; i < groups_.size(); i++) {
        if (!groups_[i]) continue;
        
        const auto& group = groups_[i];
        LOG4CPLUS_INFO(logger, log_prefix_ << "   [Group " << i << "] '" << group->group_id << "':");
        LOG4CPLUS_INFO(logger, log_prefix_ << "     已处理: " << group->processed_count.load());
        LOG4CPLUS_INFO(logger, log_prefix_ << "     成功: " << group->success_count.load());
        LOG4CPLUS_INFO(logger, log_prefix_ << "     错误: " << group->error_count.load());
        LOG4CPLUS_INFO(logger, log_prefix_ << "     消费者数量: " << group->consumers.size());
        
        // 详细消费者统计
        for (size_t j = 0; j < group->consumers.size(); j++) {
            if (!group->consumers[j]) continue;
            const auto& consumer = group->consumers[j];
            LOG4CPLUS_INFO(logger, log_prefix_ << "       消费者 #" << j << ": 成功=" << consumer->success_count.load()
                           << ", 错误=" << consumer->error_count.load()
                           << ", 活跃=" << (consumer->is_active.load() ? "是" : "否"));
        }
    }
    LOG4CPLUS_INFO(logger, log_prefix_ << " ================================================");
}

// ============================================================
// 内部方法实现
// ============================================================


// ============================================================
// 辅助方法实现
// ============================================================

bool MultiWorkerProductionLine::createProducer(WorkerGroupRuntime* group, const WorkerConfig& producer_config) {
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
    
    // 创建生产者 VideoProductionLine
    group->producer_line = std::make_unique<VideoProductionLine>(loop_, 1, enable_monitor_);
    
    // 启动生产者
    if (!group->producer_line->start(producer_config)) {
        LOG4CPLUS_ERROR(logger, log_prefix_ << "    生产者启动失败");
        return false;
    }
    
    // 获取生产者的 BufferPool
    group->producer_buffer_pool_id = group->producer_line->getWorkingBufferPoolId();
    if (group->producer_buffer_pool_id == 0) {
        LOG4CPLUS_ERROR(logger, log_prefix_ << "    生产者未创建 BufferPool");
        return false;
    }
    
    group->producer_buffer_pool_weak = BufferPoolRegistry::getInstance().getPool(group->producer_buffer_pool_id);
    auto producer_pool = group->producer_buffer_pool_weak.lock();
    if (!producer_pool) {
        LOG4CPLUS_ERROR(logger, log_prefix_ << "    无法从 Registry 获取生产者 BufferPool");
        return false;
    }
    
    LOG4CPLUS_INFO(logger, log_prefix_ << "    生产者 Worker 已启动 (BufferPool ID: " 
                   << group->producer_buffer_pool_id << ")");
    
    return true;
}

bool MultiWorkerProductionLine::createConsumers(
    WorkerGroupRuntime* group,
    const std::vector<WorkerConfig>& consumer_configs,
    const AVCodecParameters* producer_codec_params)
{
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
    
    group->consumers.reserve(consumer_configs.size());
    
    for (size_t i = 0; i < consumer_configs.size(); i++) {
        // ⭐ 关键：复制配置并自动设置 Buffer 数据源模式
        WorkerConfig consumer_config = consumer_configs[i];
        
        // 启用 Buffer 数据源模式
        consumer_config.decoder.datasource_buffer_mode = true;
        consumer_config.decoder.codec_params = producer_codec_params;
        
        LOG4CPLUS_INFO(logger, log_prefix_ << "      创建消费者 #" << i 
                       << " (类型: " << static_cast<int>(consumer_config.worker_type) 
                       << ", Buffer模式)");
        
        // 创建消费者 Worker
        auto consumer_worker = std::make_shared<BufferFillingWorkerFacade>(consumer_config);
        
        // ⭐ 关键：设置消费者的源 BufferPool（关联到该 Group 的生产者 Pool）
        if (!consumer_worker->setSourceBufferPool(group->producer_buffer_pool_weak)) {
            LOG4CPLUS_ERROR(logger, log_prefix_ << "      消费者 #" << i << " 设置源 BufferPool 失败");
            return false;
        }
        LOG4CPLUS_DEBUG(logger, log_prefix_ << "      消费者 #" << i << " 已关联生产者 BufferPool");
        
        // 打开消费者（会自动创建 BufferPool）
        if (!consumer_worker->open()) {
            LOG4CPLUS_ERROR(logger, log_prefix_ << "      消费者 #" << i << " 打开失败");
            return false;
        }
        
        // 获取消费者的 BufferPool
        BufferPoolType primary_type = consumer_worker->getPrimaryBufferPoolType();
        uint64_t consumer_pool_id = consumer_worker->getOutputBufferPoolId(primary_type);
        if (consumer_pool_id == 0) {
            LOG4CPLUS_ERROR(logger, log_prefix_ << "      消费者 #" << i << " 未创建 BufferPool");
            return false;
        }
        
        auto consumer_pool_weak = BufferPoolRegistry::getInstance().getPool(consumer_pool_id);
        auto consumer_pool = consumer_pool_weak.lock();
        if (!consumer_pool) {
            LOG4CPLUS_ERROR(logger, log_prefix_ << "      消费者 #" << i << " BufferPool 获取失败");
            return false;
        }
        
        // 创建消费者信息
        auto consumer_info = std::make_unique<WorkerGroupRuntime::ConsumerInfo>();
        consumer_info->worker = consumer_worker;
        consumer_info->buffer_pool_id = consumer_pool_id;
        consumer_info->buffer_pool_weak = consumer_pool_weak;
        consumer_info->is_active = true;
        
        group->consumers.push_back(std::move(consumer_info));
        
        LOG4CPLUS_INFO(logger, log_prefix_ << "      消费者 #" << i << " 已创建 (BufferPool ID: " 
                       << consumer_pool_id << ")");
    }
    
    return true;
}

void MultiWorkerProductionLine::groupThreadFunc(WorkerGroupRuntime* group) {
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
    LOG4CPLUS_INFO(logger, log_prefix_ << " [Group '" << group->group_id << "'] 线程启动");
    
    while (group->is_running.load() && running_.load()) {
        // ⭐ 关键：消费者会自动从生产者 BufferPool 获取数据
        // 这里只需要触发消费者处理即可
        
        // 1️⃣ 统计活跃的消费者
        int active_consumers = 0;
        for (const auto& consumer : group->consumers) {
            if (consumer && consumer->is_active.load()) {
                active_consumers++;
            }
        }
        
        if (active_consumers == 0) {
            LOG4CPLUS_WARN(logger, log_prefix_ << " [Group '" << group->group_id << "'] 没有活跃的消费者，等待...");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // 2️⃣ 创建同步门闩
        auto latch = std::make_shared<CountDownLatch>(active_consumers);
        std::atomic<int> success_count{0};
        std::atomic<int> error_count{0};
        
        // 3️⃣ 提交任务给所有活跃消费者
        for (size_t i = 0; i < group->consumers.size(); i++) {
            auto& consumer = group->consumers[i];
            if (!consumer || !consumer->is_active.load()) {
                continue;
            }
            
            // 提交到共享线程池
            thread_pool_->detach_task([this, group, consumer_ptr = consumer.get(), 
                                      latch, &success_count, &error_count, i, logger]() {
                // 消费者内部会自动从生产者 BufferPool 获取 buffer
                // fillBuffer 的 frame_index 参数在 Buffer 模式下被忽略
                Buffer* dummy_buffer = nullptr;  // Buffer 模式下不需要外部 buffer
                bool process_success = consumer_ptr->worker->fillBuffer(0, dummy_buffer);
                
                if (process_success) {
                    success_count.fetch_add(1);
                    consumer_ptr->success_count.fetch_add(1);
                } else {
                    error_count.fetch_add(1);
                    consumer_ptr->error_count.fetch_add(1);
                    
                    // 错误过多，禁用该消费者
                    if (consumer_ptr->error_count.load() > group->max_consecutive_errors) {
                        consumer_ptr->is_active.store(false);
                        LOG4CPLUS_WARN(logger, log_prefix_ << " [Group '" << group->group_id 
                                      << "'] 消费者 #" << i << " 因错误过多被禁用");
                    }
                }
                
                latch->countDown();
            });
        }
        
        // 4️⃣ 同步等待所有消费者完成
        bool all_done = latch->wait(group->sync_timeout_ms);
        
        if (!all_done) {
            LOG4CPLUS_ERROR(logger, log_prefix_ << " [Group '" << group->group_id 
                           << "'] 等待消费者完成超时 (timeout=" << group->sync_timeout_ms << "ms)");
            group->consecutive_errors.fetch_add(1);
        } else {
            group->consecutive_errors.store(0);
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
        
        // 5️⃣ 检查是否需要停止该 Group
        if (group->consecutive_errors.load() > group->max_consecutive_errors) {
            LOG4CPLUS_ERROR(logger, log_prefix_ << " [Group '" << group->group_id 
                           << "'] 连续错误过多，停止运行");
            break;
        }
        
        // ⚠️ 注意：这里需要一个短暂的延迟，否则会死循环
        // 因为消费者从 BufferPool 获取数据可能需要时间
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " [Group '" << group->group_id << "'] 线程结束 "
                   << "(processed=" << group->processed_count.load() 
                   << ", success=" << group->success_count.load()
                   << ", error=" << group->error_count.load() << ")");
}

void MultiWorkerProductionLine::setError(const std::string& error_msg) {
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
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
    LOG4CPLUS_ERROR(logger, log_prefix_ << " 错误: " << error_msg);
}

