#include "productionline/MultiWorkerProductionLine.hpp"
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
    , record_production_line_(nullptr)
    , record_buffer_pool_id_(0)
    , record_buffer_pool_weak_()
    , consumer_workers_()
    , thread_pool_(nullptr)
    , pending_tasks_(0)
    , consecutive_errors_(0)
    , stats_()
    , log_prefix_("[MultiWorkerProductionLine]")
{
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " 创建: consumers=" << config_.consumer_configs.size()
                   << ", thread_pool_size=" << config_.thread_pool_size);
    
    // 验证配置
    if (config_.consumer_configs.empty()) {
        LOG4CPLUS_WARN(logger, log_prefix_ << " 警告: 没有配置消费者 worker");
    }
    
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
    consumer_workers_.clear();
    record_production_line_.reset();
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
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " 启动多Worker生产流水线...");
    
    // 1. 创建并启动内部的 record worker（生产者）
    LOG4CPLUS_INFO(logger, log_prefix_ << " 创建 Record Worker (生产者)...");
    record_production_line_ = std::make_unique<VideoProductionLine>(loop_, 1, enable_monitor_);
    
    // 设置 record worker 的 worker_type 为 FFMPEG_RTSP_RECORD
    WorkerConfig record_config = config_.record_worker_config;
    record_config.worker_type = WorkerType::FFMPEG_RTSP_RECORD;
    
    if (!record_production_line_->start(record_config)) {
        setError("Failed to start record worker");
        record_production_line_.reset();
        return false;
    }
    
    // 获取 record worker 的 bufferpool
    record_buffer_pool_id_ = record_production_line_->getWorkingBufferPoolId();
    if (record_buffer_pool_id_ == 0) {
        setError("Record worker failed to create BufferPool");
        record_production_line_.reset();
        return false;
    }
    
    record_buffer_pool_weak_ = BufferPoolRegistry::getInstance().getPool(record_buffer_pool_id_);
    auto record_pool = record_buffer_pool_weak_.lock();
    if (!record_pool) {
        setError("Failed to get record BufferPool from Registry");
        record_production_line_.reset();
        return false;
    }
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " Record Worker 已启动，BufferPool ID: " << record_buffer_pool_id_);
    
    // 2. 创建所有消费者 worker
    LOG4CPLUS_INFO(logger, log_prefix_ << " 创建 " << config_.consumer_configs.size() << " 个消费者 Worker...");
    consumer_workers_.reserve(config_.consumer_configs.size());
    
    for (size_t i = 0; i < config_.consumer_configs.size(); i++) {
        const auto& consumer_config = config_.consumer_configs[i];
        
        LOG4CPLUS_INFO(logger, log_prefix_ << "  创建消费者 Worker #" << i << " (类型: " 
                       << static_cast<int>(consumer_config.worker_type) << ")");
        
        // 创建消费者 worker facade
        auto consumer_worker = std::make_shared<BufferFillingWorkerFacade>(consumer_config);
        
        // 打开 worker（会自动创建 BufferPool）
        if (!consumer_worker->open()) {
            LOG4CPLUS_ERROR(logger, log_prefix_ << "  消费者 Worker #" << i << " 打开失败");
            setError("Failed to open consumer worker #" + std::to_string(i));
            // 清理已创建的 worker
            consumer_workers_.clear();
            record_production_line_.reset();
            return false;
        }
        
        // 获取消费者 worker 的 bufferpool
        BufferPoolType primary_type = consumer_worker->getPrimaryBufferPoolType();
        uint64_t consumer_pool_id = consumer_worker->getOutputBufferPoolId(primary_type);
        if (consumer_pool_id == 0) {
            LOG4CPLUS_ERROR(logger, log_prefix_ << "  消费者 Worker #" << i << " 未创建 BufferPool");
            setError("Consumer worker #" + std::to_string(i) + " failed to create BufferPool");
            consumer_workers_.clear();
            record_production_line_.reset();
            return false;
        }
        
        auto consumer_pool_weak = BufferPoolRegistry::getInstance().getPool(consumer_pool_id);
        auto consumer_pool = consumer_pool_weak.lock();
        if (!consumer_pool) {
            LOG4CPLUS_ERROR(logger, log_prefix_ << "  消费者 Worker #" << i << " BufferPool 获取失败");
            setError("Failed to get consumer BufferPool from Registry");
            consumer_workers_.clear();
            record_production_line_.reset();
            return false;
        }
        
        // 创建消费者 worker 信息（使用 unique_ptr 存储，避免移动问题）
        auto info = std::make_unique<ConsumerWorkerInfo>();
        info->worker = consumer_worker;
        info->buffer_pool_id = consumer_pool_id;
        info->buffer_pool_weak = consumer_pool_weak;
        info->is_active = true;
        
        consumer_workers_.push_back(std::move(info));
        
        // 初始化统计信息
        stats_.consumer_success_count[i] = 0;
        stats_.consumer_error_count[i] = 0;
        
        LOG4CPLUS_INFO(logger, log_prefix_ << "  消费者 Worker #" << i << " 已创建，BufferPool ID: " << consumer_pool_id);
    }
    
    if (consumer_workers_.empty()) {
        setError("No consumer workers created");
        record_production_line_.reset();
        return false;
    }
    
    // 3. 创建线程池
    LOG4CPLUS_INFO(logger, log_prefix_ << " 创建线程池 (size=" << config_.thread_pool_size << ")...");
    thread_pool_ = std::make_unique<BS::thread_pool<>>(config_.thread_pool_size);
    
    // 4. 重置状态
    running_.store(true);
    produced_frames_.store(0);
    skipped_frames_.store(0);
    next_frame_index_.store(0);
    consecutive_errors_.store(0);
    pending_tasks_.store(0);
    start_time_ = std::chrono::steady_clock::now();
    
    // 5. 启动生产者线程（调用重写的 producerThreadFunc）
    threads_.reserve(thread_count_);
    active_threads_.store(thread_count_);
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " 启动生产者线程 (" << thread_count_ << " threads)...");
    
    for (int i = 0; i < thread_count_; i++) {
        try {
            threads_.emplace_back(&MultiWorkerProductionLine::producerThreadFunc, this, i);
            LOG4CPLUS_INFO(logger, log_prefix_ << "  生产者线程 #" << i << " 已启动");
        } catch (const std::exception& e) {
            LOG4CPLUS_ERROR(logger, log_prefix_ << " 启动生产者线程 #" << i << " 失败: " << e.what());
            running_.store(false);
            active_threads_.store(0);
            for (auto& thread : threads_) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            threads_.clear();
            thread_pool_.reset();
            consumer_workers_.clear();
            record_production_line_.reset();
            setError(std::string("Failed to start producer thread: ") + e.what());
            return false;
        }
    }
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " 多Worker生产流水线启动成功");
    return true;
}

void MultiWorkerProductionLine::stop() {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    
    if (!running_.load()) {
        return;
    }
    
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
    LOG4CPLUS_INFO(logger, log_prefix_ << " 停止多Worker生产流水线...");
    
    // 设置停止标志
    running_.store(false);
    
    // 等待所有生产者线程退出
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
    
    // 停止线程池（等待所有任务完成）
    if (thread_pool_) {
        thread_pool_->wait();  // 等待所有任务完成
        thread_pool_.reset();
    }
    
    // 停止内部的 record worker
    if (record_production_line_) {
        record_production_line_->stop();
    }
    
    // 关闭所有消费者 worker
    for (auto& info : consumer_workers_) {
        if (info && info->worker) {
            info->worker->close();
        }
    }
    
    // 重置活跃线程计数
    active_threads_.store(0);
    
    // 通知背压等待的线程
    backpressure_cv_.notify_all();
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " 多Worker生产流水线已停止");
    LOG4CPLUS_INFO(logger, log_prefix_ << " 统计: 处理 " << stats_.total_packets_processed.load() << " 个packet");
    LOG4CPLUS_INFO(logger, log_prefix_ << " 统计: 成功 " << stats_.total_packets_succeeded.load() << " 个");
    LOG4CPLUS_INFO(logger, log_prefix_ << " 统计: 失败 " << stats_.total_packets_failed.load() << " 个");
}

// ============================================================
// 查询接口实现
// ============================================================

uint64_t MultiWorkerProductionLine::getConsumerBufferPoolId(size_t index) const {
    if (index >= consumer_workers_.size() || !consumer_workers_[index]) {
        return 0;
    }
    return consumer_workers_[index]->buffer_pool_id;
}

void MultiWorkerProductionLine::printDetailedStats() const {
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " ========== 详细统计信息 ==========");
    LOG4CPLUS_INFO(logger, log_prefix_ << " 总处理packet数: " << stats_.total_packets_processed.load());
    LOG4CPLUS_INFO(logger, log_prefix_ << " 成功packet数: " << stats_.total_packets_succeeded.load());
    LOG4CPLUS_INFO(logger, log_prefix_ << " 失败packet数: " << stats_.total_packets_failed.load());
    
    auto total_time_us = stats_.total_decode_time_us.load();
    if (stats_.total_packets_succeeded.load() > 0) {
        double avg_time_ms = total_time_us / 1000.0 / stats_.total_packets_succeeded.load();
        LOG4CPLUS_INFO(logger, log_prefix_ << " 平均解码耗时: " << avg_time_ms << " ms");
    }
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " 消费者统计:");
    for (size_t i = 0; i < consumer_workers_.size(); i++) {
        auto success = stats_.consumer_success_count.find(i);
        auto error = stats_.consumer_error_count.find(i);
        int64_t success_count = (success != stats_.consumer_success_count.end()) ? success->second.load() : 0;
        int64_t error_count = (error != stats_.consumer_error_count.end()) ? error->second.load() : 0;
        
        LOG4CPLUS_INFO(logger, log_prefix_ << "  消费者 #" << i << ": 成功=" << success_count 
                       << ", 失败=" << error_count 
                       << ", 活跃=" << (consumer_workers_[i] && consumer_workers_[i]->is_active.load() ? "是" : "否"));
    }
    LOG4CPLUS_INFO(logger, log_prefix_ << " ==================================");
}

// ============================================================
// 内部方法实现
// ============================================================

void MultiWorkerProductionLine::producerThreadFunc(int thread_id) {
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
    
    LOG4CPLUS_INFO(logger, log_prefix_ << " 生产者线程 #" << thread_id << " 启动");
    
    int thread_produced = 0;
    int thread_skipped = 0;
    
    while (running_.load()) {
        // 1. 从 record bufferpool 获取 filled buffer
        auto record_pool = record_buffer_pool_weak_.lock();
        if (!record_pool) {
            LOG4CPLUS_WARN(logger, log_prefix_ << " Record BufferPool 已销毁，退出线程 #" << thread_id);
            break;
        }
        
        Buffer* packet_buffer = record_pool->acquireFilled(true, 100);
        if (!packet_buffer) {
            if (running_.load()) {
                continue;  // 继续等待
            } else {
                break;  // 停止信号
            }
        }
        
        // 2. 检查背压（防止任务堆积）
        if (pending_tasks_.load() >= config_.max_pending_tasks) {
            std::unique_lock<std::mutex> lock(backpressure_mutex_);
            backpressure_cv_.wait(lock, [this] {
                return pending_tasks_.load() < config_.max_pending_tasks || !running_.load();
            });
            if (!running_.load()) {
                record_pool->releaseFilled(packet_buffer);
                break;
            }
        }
        
        // 3. 统计活跃的消费者数量
        int active_consumers = 0;
        for (const auto& info : consumer_workers_) {
            if (info && info->is_active.load()) {
                active_consumers++;
            }
        }
        
        if (active_consumers == 0) {
            LOG4CPLUS_WARN(logger, log_prefix_ << " 没有活跃的消费者，跳过此packet");
            record_pool->releaseFilled(packet_buffer);
            skipped_frames_.fetch_add(1);
            thread_skipped++;
            continue;
        }
        
        // 4. 创建同步计数器（CountDownLatch）
        auto latch = std::make_shared<CountDownLatch>(active_consumers);
        std::atomic<int> success_count{0};
        std::atomic<int> error_count{0};
        
        // 5. 为每个活跃的消费者提交任务到线程池
        for (size_t i = 0; i < consumer_workers_.size(); i++) {
            if (!consumer_workers_[i] || !consumer_workers_[i]->is_active.load()) {
                continue;
            }
            
            auto* consumer_info = consumer_workers_[i].get();
            pending_tasks_.fetch_add(1);
            
            // 提交任务到线程池（使用 detach_task，不返回 future）
            thread_pool_->detach_task([this, consumer_info, packet_buffer, latch, 
                                    &success_count, &error_count, i, thread_id]() {
                auto start_time = std::chrono::steady_clock::now();
                
                // 5.1 从消费者 bufferpool 获取 free buffer
                auto consumer_pool = consumer_info->buffer_pool_weak.lock();
                if (!consumer_pool) {
                    error_count.fetch_add(1);
                    consumer_info->error_count.fetch_add(1);
                    stats_.consumer_error_count[i].fetch_add(1);
                    latch->countDown();
                    pending_tasks_.fetch_sub(1);
                    return;
                }
                
                Buffer* decode_buffer = consumer_pool->acquireFree(true, 100);
                if (!decode_buffer) {
                    error_count.fetch_add(1);
                    consumer_info->error_count.fetch_add(1);
                    stats_.consumer_error_count[i].fetch_add(1);
                    latch->countDown();
                    pending_tasks_.fetch_sub(1);
                    return;
                }
                
                // 5.2 复制 AVPacket 到消费者 buffer
                AVPacket* src_packet = packet_buffer->getAVPacket();
                AVPacket* dst_packet = decode_buffer->getAVPacket();
                
                if (!src_packet || !dst_packet) {
                    consumer_pool->releaseFree(decode_buffer);
                    error_count.fetch_add(1);
                    consumer_info->error_count.fetch_add(1);
                    stats_.consumer_error_count[i].fetch_add(1);
                    latch->countDown();
                    pending_tasks_.fetch_sub(1);
                    return;
                }
                
                // 复制 packet（使用 av_packet_ref）
                int ret = av_packet_ref(dst_packet, src_packet);
                if (ret < 0) {
                    consumer_pool->releaseFree(decode_buffer);
                    error_count.fetch_add(1);
                    consumer_info->error_count.fetch_add(1);
                    stats_.consumer_error_count[i].fetch_add(1);
                    latch->countDown();
                    pending_tasks_.fetch_sub(1);
                    return;
                }
                
                // 5.3 调用消费者 worker 的 fillBuffer 进行解码
                // 注意：这里 frame_index 参数不重要，因为我们已经有了 packet
                bool decode_success = consumer_info->worker->fillBuffer(0, decode_buffer);
                
                if (decode_success) {
                    // 解码成功：提交到 filled 队列
                    consumer_pool->submitFilled(decode_buffer);
                    success_count.fetch_add(1);
                    consumer_info->success_count.fetch_add(1);
                    stats_.consumer_success_count[i].fetch_add(1);
                } else {
                    // 解码失败：归还 buffer
                    av_packet_unref(dst_packet);
                    consumer_pool->releaseFree(decode_buffer);
                    error_count.fetch_add(1);
                    consumer_info->error_count.fetch_add(1);
                    stats_.consumer_error_count[i].fetch_add(1);
                    
                    // 检查是否需要禁用该消费者
                    if (consumer_info->error_count.load() > config_.max_consecutive_errors) {
                        consumer_info->is_active.store(false);
                        auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
                        LOG4CPLUS_WARN(logger, log_prefix_ << " 消费者 #" << i 
                                      << " 因错误过多被禁用");
                    }
                }
                
                // 5.4 统计耗时
                auto end_time = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                    end_time - start_time).count();
                stats_.total_decode_time_us.fetch_add(duration);
                
                latch->countDown();
                pending_tasks_.fetch_sub(1);
            });
        }
        
        // 6. 等待所有消费者完成（带超时）
        bool all_done = latch->wait(config_.sync_timeout_ms);
        
        if (!all_done) {
            auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
            LOG4CPLUS_ERROR(logger, log_prefix_ << " 等待消费者完成超时 (timeout=" 
                           << config_.sync_timeout_ms << "ms)");
            consecutive_errors_.fetch_add(1);
        } else {
            consecutive_errors_.store(0);
            stats_.total_packets_processed.fetch_add(1);
            if (success_count.load() > 0) {
                stats_.total_packets_succeeded.fetch_add(1);
                produced_frames_.fetch_add(1);
                thread_produced++;
            }
            if (error_count.load() > 0) {
                stats_.total_packets_failed.fetch_add(error_count.load());
                skipped_frames_.fetch_add(error_count.load());
                thread_skipped += error_count.load();
            }
        }
        
        // 7. 归还 record buffer
        record_pool->releaseFilled(packet_buffer);
        
        // 8. 检查是否需要停止（连续错误过多）
        if (consecutive_errors_.load() > config_.max_consecutive_errors) {
            auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
            LOG4CPLUS_ERROR(logger, log_prefix_ << " 连续错误过多，停止生产者线程 #" << thread_id);
            break;
        }
        
        // 9. 通知背压等待的线程
        backpressure_cv_.notify_all();
    }
    
    // 线程结束
    LOG4CPLUS_INFO(logger, log_prefix_ << " 生产者线程 #" << thread_id << " 结束: produced=" 
                   << thread_produced << ", skipped=" << thread_skipped);
    
    // 减少活跃线程计数
    int remaining = active_threads_.fetch_sub(1) - 1;
    if (remaining == 0) {
        running_.store(false);
        LOG4CPLUS_INFO(logger, log_prefix_ << " 所有生产者线程已完成，生产流水线停止");
    }
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

