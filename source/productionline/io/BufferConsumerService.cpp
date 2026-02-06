/**
 * @file BufferConsumerService.cpp
 * @brief Buffer 消费服务实现
 */

#include "productionline/io/BufferConsumerService.hpp"
#include "productionline/MultiWorkerProductionLine.hpp"
#include "productionline/WorkerSyncCoordinator.hpp"
#include "common/GlobalThreadPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"

#include <future>
#include <sstream>
#include <iomanip>
#include <thread>

namespace consumer {

// ============================================================
// 构造和析构
// ============================================================

BufferConsumerService::BufferConsumerService()
    : running_(false)
    , stop_requested_(false)
    , thread_pool_(nullptr)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("consumer.BufferConsumerService")))
{
}

BufferConsumerService::~BufferConsumerService() {
    requestStop();
}

void BufferConsumerService::setThreadPool(std::shared_ptr<BS::thread_pool<>> pool) {
    thread_pool_ = pool;
}

// ============================================================
// COMPARE 模式辅助函数（需要在 start() 之前定义）
// ============================================================

/**
 * @brief 将旧的 COMPARE 模式参数转换为 MultiWorkerConfig
 * 
 * @param configs {hw_config, sw_config}
 * @param flags 消费类型标志
 * @return pair<MultiWorkerConfig, shared_ptr<CompareCallbackContext>>
 */
static std::pair<MultiWorkerConfig, std::shared_ptr<CompareCallbackContext>>
buildMultiWorkerConfigForCompare(
    const std::vector<WorkerConfig>& configs,
    uint32_t flags)
{
    MultiWorkerConfig multi_config;
    WorkerGroupConfig group("compare_group");
    
    // ========================================
    // 1. 配置生产者（Packet Source）
    // ========================================
    ProducerConfig producer_cfg;
    producer_cfg.producer_name = "packet_source";
    producer_cfg.worker_config = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(configs[0].data_source.path)
                .setBufferCount(configs[0].data_source.buffer_count > 0 
                    ? configs[0].data_source.buffer_count : 32)
                .setMaxFrames(configs[0].data_source.max_frames)  // v2.23 新增：传递帧数限制
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
        .build();
    group.producer_configs.push_back(producer_cfg);
    
    // ========================================
    // 2. 配置消费者（Decoder Workers）
    // ========================================
    // 消费者1：硬件解码器
    ConsumerConfig consumer_hw;
    consumer_hw.consumer_name = "hw_decoder";
    consumer_hw.worker_config = configs[0];
    // 合并外部 flags 到 consumer_type
    if (flags & CONSUME_DISPLAY) {
        consumer_hw.worker_config.consumer_type.display.enable = true;
    }
    if (flags & CONSUME_SAVE_RAW) {
        consumer_hw.worker_config.consumer_type.save_raw.enable = true;
    }
    if (flags & CONSUME_SAVE_ENCODED) {
        consumer_hw.worker_config.consumer_type.save_encoded.enable = true;
    }
    group.consumer_configs.push_back(consumer_hw);
    
    // 消费者2：软件解码器
    ConsumerConfig consumer_sw;
    consumer_sw.consumer_name = "sw_decoder";
    consumer_sw.worker_config = configs[1];
    // 同样合并外部 flags
    if (flags & CONSUME_DISPLAY) {
        consumer_sw.worker_config.consumer_type.display.enable = true;
    }
    if (flags & CONSUME_SAVE_RAW) {
        consumer_sw.worker_config.consumer_type.save_raw.enable = true;
    }
    if (flags & CONSUME_SAVE_ENCODED) {
        consumer_sw.worker_config.consumer_type.save_encoded.enable = true;
    }
    group.consumer_configs.push_back(consumer_sw);
    
    // ========================================
    // 3. 配置连接器（ONE_TO_MANY + 比较回调）
    // ========================================
    ConnectorConfig connector;
    connector.mode = Connector::Mode::ONE_TO_MANY;
    connector.producer_names.push_back("packet_source");
    connector.consumer_names.push_back("hw_decoder");
    connector.consumer_names.push_back("sw_decoder");
    connector.enable_frame_sync = true;
    
    // 创建比较上下文（从 configs[0] 获取比较配置）
    auto compare_ctx = std::make_shared<CompareCallbackContext>();
    compare_ctx->initFromCompareType(configs[0].consumer_type.compare);
    compare_ctx->worker1_name = "hw_decoder";
    compare_ctx->worker2_name = "sw_decoder";
    
    // 添加比较回调到 callback_chain
    connector.callback_chain.push_back(
        WorkerSyncCoordinator::createDefaultCompareCallback(compare_ctx.get()));
    
    group.connector_configs.push_back(connector);
    
    // ========================================
    // 4. 组装配置
    // ========================================
    multi_config.groups.push_back(group);
    
    return {multi_config, compare_ctx};
}

// ============================================================
// 唯一对外接口
// ============================================================

ConsumeResult BufferConsumerService::start(
    const std::vector<WorkerConfig>& configs,
    ExecuteMode mode,
    uint32_t consume_flags
) {
    if (configs.empty()) {
        ConsumeResult result;
        result.success = false;
        result.error_message = "No configs provided";
        return result;
    }
    
    running_ = true;
    stop_requested_ = false;
    
    ConsumeResult result;
    
    switch (mode) {
        case ExecuteMode::SINGLE:
            LOG4CPLUS_INFO(logger_, "Starting SINGLE mode consumption");
            result = startProductionLine(configs[0], consume_flags);
            break;
            
        case ExecuteMode::COMPARE:
        {
            LOG4CPLUS_INFO_FMT(logger_, "Starting COMPARE mode with %zu workers, flags=0x%X", 
                              configs.size(), consume_flags);
            
            if (configs.size() < 2) {
                result.success = false;
                result.error_message = "COMPARE mode requires at least 2 configs";
                break;
            }
            
            // 转换为 MultiWorkerConfig
            auto [multi_config, compare_ctx] = buildMultiWorkerConfigForCompare(configs, consume_flags);
            
            // 使用 startMultiWorkerCompare
            result = startMultiWorkerCompare(multi_config);
            
            // compare_ctx 通过 shared_ptr 管理生命周期
        }
        break;
            
        case ExecuteMode::PARALLEL:
            LOG4CPLUS_INFO_FMT(logger_, "Starting PARALLEL mode with %zu workers", configs.size());
            result = startProductionLinesParallel(configs, consume_flags);
            break;
    }
    
    running_ = false;
    return result;
}

// ============================================================
// 控制接口
// ============================================================

void BufferConsumerService::requestStop() {
    stop_requested_ = true;
}

bool BufferConsumerService::isRunning() const {
    return running_.load();
}

// ============================================================
// SINGLE 模式实现
// ============================================================

ConsumeResult BufferConsumerService::startProductionLine(
    const WorkerConfig& config,
    uint32_t consume_flags
) {
    ConsumeResult result;
    auto start_time = std::chrono::steady_clock::now();
    
    try {
        // 1. 创建 VideoProductionLine
        VideoProductionLine producer;
        
        // 2. 启动生产线（配置在 start 中传入）
        if (!producer.start(config)) {
            result.success = false;
            result.error_message = "Failed to start VideoProductionLine";
            return result;
        }
        
        // 3. 获取 BufferPool（通过 Registry）
        auto pool_id = producer.getWorkingBufferPoolId();
        auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
        auto pool = pool_weak.lock();
        if (!pool) {
            result.success = false;
            result.error_message = "Failed to get BufferPool";
            producer.stop();
            return result;
        }
        
        // 4. 创建消费策略
        auto consumer = createConsumerFromFlags(consume_flags, config);
        if (!consumer) {
            result.success = false;
            result.error_message = "Failed to create consumer strategy";
            producer.stop();
            return result;
        }
        
        // 5. 执行消费循环
        consumeLoop(pool, consumer, config.consumer_type, result);
        
        // 6. 停止生产线（等待生产者完成所有帧的生产）
        producer.stop();
        
        // 7. 排空剩余的已填充 Buffer（drain 阶段）
        // 此时生产者已停止，BufferPool 中应该有所有剩余的 filled buffer
        LOG4CPLUS_DEBUG(logger_, "Draining remaining buffers after producer stopped...");
        int drained_count = 0;
        int drain_frame_index = result.frames_consumed;
        Buffer* remaining = nullptr;
        while ((remaining = pool->acquireFilled(false, 0)) != nullptr) {
            std::vector<Buffer*> buffers = {remaining};
            consumer->consume(buffers, drain_frame_index++);
            result.frames_consumed++;
            drained_count++;
            pool->releaseFilled(remaining);
        }
        if (drained_count > 0) {
            LOG4CPLUS_DEBUG_FMT(logger_, "Drained %d remaining buffers", drained_count);
        }
        
        // 8. 清理消费者
        consumer->finalize();
        
        result.success = true;
        
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("Exception: ") + e.what();
        LOG4CPLUS_ERROR_FMT(logger_, "startProductionLine exception: %s", e.what());
    }
    
    // 计算执行时间
    auto end_time = std::chrono::steady_clock::now();
    result.duration_seconds = std::chrono::duration<double>(end_time - start_time).count();
    if (result.duration_seconds > 0) {
        result.average_fps = result.frames_consumed / result.duration_seconds;
    }
    
    return result;
}

// ============================================================
// PARALLEL 模式实现
// ============================================================

ConsumeResult BufferConsumerService::startProductionLinesParallel(
    const std::vector<WorkerConfig>& configs,
    uint32_t consume_flags
) {
    ConsumeResult total_result;
    auto start_time = std::chrono::steady_clock::now();
    
    try {
        std::vector<std::future<ConsumeResult>> futures;
        
        // 获取线程池引用
        auto& thread_pool = thread_pool_ ? *thread_pool_ : GlobalThreadPool::getInstance().getThreadPool();
        
        // 为每个 config 提交任务到线程池
        for (const auto& config : configs) {
            auto future = thread_pool.submit_task([this, config, consume_flags]() {
                return startProductionLine(config, consume_flags);
            });
            futures.push_back(std::move(future));
        }
        
        // 等待所有任务完成，汇总结果
        total_result.success = true;
        for (auto& future : futures) {
            auto worker_result = future.get();
            total_result.worker_results.push_back(worker_result);
            
            // 汇总统计
            total_result.frames_consumed += worker_result.frames_consumed;
            total_result.frames_displayed += worker_result.frames_displayed;
            total_result.frames_saved += worker_result.frames_saved;
            
            if (!worker_result.success) {
                total_result.success = false;
            }
        }
        
    } catch (const std::exception& e) {
        total_result.success = false;
        total_result.error_message = std::string("Exception: ") + e.what();
        LOG4CPLUS_ERROR_FMT(logger_, "startProductionLinesParallel exception: %s", e.what());
    }
    
    // 计算执行时间
    auto end_time = std::chrono::steady_clock::now();
    total_result.duration_seconds = std::chrono::duration<double>(end_time - start_time).count();
    if (total_result.duration_seconds > 0) {
        total_result.average_fps = total_result.frames_consumed / total_result.duration_seconds;
    }
    
    return total_result;
}

// ============================================================
// MultiWorker Compare 辅助函数
// ============================================================

/**
 * @brief 从 ConsumerTypeConfig 生成消费 flags
 */
static uint32_t getConsumeFlagsFromConfig(const WorkerConfig::ConsumerTypeConfig& config) {
    uint32_t flags = 0;
    
    if (config.display.enable) {
        flags |= CONSUME_DISPLAY;
    }
    if (config.save_raw.enable) {
        flags |= CONSUME_SAVE_RAW;
    }
    if (config.save_encoded.enable) {
        flags |= CONSUME_SAVE_ENCODED;
    }
    // 注：compare 是执行模式（ExecuteMode），不是消费类型
    if (config.count.enable) {
        flags |= CONSUME_COUNT;
    }
    
    // 如果没有启用任何消费类型，默认启用 COUNT
    if (flags == 0) {
        flags = CONSUME_COUNT;
    }
    
    return flags;
}

/**
 * @brief 为单个 Worker 创建消费者
 */
static std::shared_ptr<IBufferConsumer> createConsumerForWorker(
    const WorkerConfig& worker_config,
    uint32_t flags,
    const AVCodecParameters* codec_params,
    const AVRational& time_base)
{
    // 如果没有指定 flags，从配置生成
    if (flags == 0) {
        flags = getConsumeFlagsFromConfig(worker_config.consumer_type);
    }
    
    const auto& config = worker_config.consumer_type;
    std::vector<std::shared_ptr<IBufferConsumer>> consumers;
    
    // 1. 显示消费者
    if (flags & CONSUME_DISPLAY) {
        consumers.push_back(std::make_shared<DisplayConsumer>(
            config.display.device_id));
    }
    
    // 2. 保存原始数据消费者
    if (flags & CONSUME_SAVE_RAW) {
        consumers.push_back(std::make_shared<SaveRawConsumer>(
            config.save_raw.output_paths,
            config.save_raw.max_frames_per_channel));
    }
    
    // 3. 保存编码数据消费者
    if (flags & CONSUME_SAVE_ENCODED) {
        consumers.push_back(std::make_shared<SaveEncodedConsumer>(
            config.save_encoded.output_path,
            codec_params,
            time_base));
    }
    
    // 4. 统计消费者（如果没有其他消费者或显式启用）
    if (consumers.empty() || (flags & CONSUME_COUNT)) {
        consumers.push_back(std::make_shared<CountConsumer>());
    }
    
    // 如果只有一个消费者，直接返回
    if (consumers.size() == 1) {
        return consumers[0];
    }
    
    // 多个消费者，使用 MultiConsumer 组合
    auto multi = std::make_shared<MultiConsumer>();
    for (auto& c : consumers) {
        multi->addStrategy(c);
    }
    return multi;
}

// ============================================================
// MultiWorker Compare 模式实现
// ============================================================

ConsumeResult BufferConsumerService::startMultiWorkerCompare(
    const MultiWorkerConfig& multi_config)
{
    ConsumeResult result;
    auto start_time = std::chrono::steady_clock::now();
    
    LOG4CPLUS_INFO(logger_, "========================================");
    LOG4CPLUS_INFO(logger_, "Starting MultiWorker Compare Mode");
    LOG4CPLUS_INFO(logger_, "========================================");
    
    if (multi_config.groups.empty()) {
        result.success = false;
        result.error_message = "No worker groups configured";
        return result;
    }
    
    try {
        // 1. 创建 MultiWorkerProductionLine（配置由测试 case 传入，包含 callback_chain）
        auto production_line = std::make_unique<MultiWorkerProductionLine>(multi_config);
        
        // 1.1 启动 MultiWorkerProductionLine（初始化 Worker 和 BufferPool）
        if (!production_line->start()) {
            result.success = false;
            result.error_message = "Failed to start MultiWorkerProductionLine: " + production_line->getLastError();
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to start MultiWorkerProductionLine: %s", 
                               production_line->getLastError().c_str());
            return result;
        }
        
        // 1.2 查找 CompareCallbackContext（用于获取比较结果）
        CompareCallbackContext* compare_ctx = nullptr;
        for (const auto& group_config : multi_config.groups) {
            for (const auto& conn_cfg : group_config.connector_configs) {
                if (conn_cfg.enable_frame_sync) {
                    for (const auto& cb_item : conn_cfg.callback_chain) {
                        if (cb_item.name == "default_compare_callback" && cb_item.context) {
                            compare_ctx = static_cast<CompareCallbackContext*>(cb_item.context);
                            LOG4CPLUS_DEBUG(logger_, "Found CompareCallbackContext in callback_chain");
                            break;
                        }
                    }
                }
                if (compare_ctx) break;
            }
            if (compare_ctx) break;
        }
        
        // 2. 获取所有 worker 的 BufferPool
        struct WorkerConsumeContext {
            std::string worker_name;
            std::shared_ptr<BufferPool> pool;
            std::shared_ptr<IBufferConsumer> consumer;
            int frame_count = 0;
        };
        
        std::vector<WorkerConsumeContext> worker_contexts;
        
        // 遍历所有 group 和 worker（consumer）
        for (size_t group_idx = 0; group_idx < multi_config.groups.size(); ++group_idx) {
            const auto& group_config = multi_config.groups[group_idx];
            
            for (size_t worker_idx = 0; worker_idx < group_config.consumer_configs.size(); ++worker_idx) {
                const auto& consumer_config = group_config.consumer_configs[worker_idx];
                const auto& worker_config = consumer_config.worker_config;
                
                // 获取 BufferPool ID
                uint64_t pool_id = production_line->getGroupConsumerBufferPoolId(
                    group_idx, worker_idx);
                
                if (pool_id == 0) {
                    LOG4CPLUS_WARN_FMT(logger_, 
                        "No BufferPool for group[%zu] worker[%zu]", 
                        group_idx, worker_idx);
                    continue;
                }
                
                // 获取 BufferPool
                auto pool = BufferPoolRegistry::getInstance().getPool(pool_id).lock();
                if (!pool) {
                    LOG4CPLUS_ERROR_FMT(logger_, 
                        "Failed to get BufferPool for group[%zu] worker[%zu]",
                        group_idx, worker_idx);
                    continue;
                }
                
                // 生成 flags（比较在 WorkerSyncCoordinator 内部完成，这里只处理消费类型）
                uint32_t flags = getConsumeFlagsFromConfig(worker_config.consumer_type);
                
                // 为该 worker 创建消费者
                auto consumer = createConsumerForWorker(
                    worker_config, 
                    flags,
                    worker_config.data_source.codec_params,
                    worker_config.data_source.time_base);
                
                WorkerConsumeContext ctx;
                ctx.worker_name = consumer_config.consumer_name.empty() 
                    ? ("worker_" + std::to_string(group_idx) + "_" + std::to_string(worker_idx))
                    : consumer_config.consumer_name;
                ctx.pool = pool;
                ctx.consumer = consumer;
                
                worker_contexts.push_back(std::move(ctx));
                
                LOG4CPLUS_INFO_FMT(logger_, 
                    "Added worker context: %s (pool_id=%llu, flags=0x%x)",
                    worker_contexts.back().worker_name.c_str(), 
                    (unsigned long long)pool_id, flags);
            }
        }
        
        if (worker_contexts.empty()) {
            result.success = false;
            result.error_message = "No worker contexts created";
            return result;
        }
        
        // 4. MultiWorkerProductionLine 已在上面启动（步骤 1.1）
        // 不需要再次调用 start()
        
        // 5. 消费循环 - 轮询所有 worker 的 BufferPool
        // 使用第一个 group 第一个 consumer 的配置作为默认
        const auto& first_config = multi_config.groups[0].consumer_configs[0].worker_config.consumer_type;
        int max_frames = first_config.max_frames;
        int timeout_ms = 5000;  // 固定 5s，确保有足够时间等待 Worker 处理（如 compare 回调）
        int max_timeout_count = first_config.max_timeout_count;
        
        int total_frames = 0;
        int consecutive_timeout = 0;
        
        running_ = true;
        
        while (!stop_requested_) {
            // 检查最大帧数
            if (max_frames > 0 && total_frames >= max_frames) {
                LOG4CPLUS_INFO_FMT(logger_, "Reached max frames: %d", max_frames);
                break;
            }
            
            bool any_consumed = false;
            
            // 轮询每个 worker 的 BufferPool
            for (auto& ctx : worker_contexts) {
                Buffer* buffer = ctx.pool->acquireFilled(true, timeout_ms);
                if (buffer) {
                    // 首帧初始化
                    if (ctx.frame_count == 0) {
                        std::vector<Buffer*> first_buffers = {buffer};
                        ctx.consumer->initialize(first_buffers);
                    }
                    
                    // 消费
                    std::vector<Buffer*> buffers = {buffer};
                    ctx.consumer->consume(buffers, ctx.frame_count);
                    ctx.frame_count++;
                    
                    // 释放
                    ctx.pool->releaseFilled(buffer);
                    
                    any_consumed = true;
                }
            }
            
            if (any_consumed) {
                total_frames++;
                consecutive_timeout = 0;
            } else {
                consecutive_timeout++;
                if (consecutive_timeout >= max_timeout_count) {
                    LOG4CPLUS_INFO_FMT(logger_, 
                        "Max timeout count reached: %d", consecutive_timeout);
                    break;
                }
                
                // 短暂休眠避免空转
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        
        running_ = false;
        
        // 6. 停止生产线（等待生产者完成所有帧的生产）
        production_line->stop();
        
        // 7. 排空剩余的已填充 Buffer（drain 阶段）
        // 每个 worker 独立 drain 自己的 BufferPool
        LOG4CPLUS_DEBUG(logger_, "Draining remaining buffers after production line stopped...");
        int total_drained = 0;
        for (auto& ctx : worker_contexts) {
            int drained_count = 0;
            Buffer* remaining = nullptr;
            while ((remaining = ctx.pool->acquireFilled(false, 0)) != nullptr) {
                // 首帧初始化（如果之前没有消费过）
                if (ctx.frame_count == 0) {
                    std::vector<Buffer*> first_buffers = {remaining};
                    ctx.consumer->initialize(first_buffers);
                }
                
                std::vector<Buffer*> buffers = {remaining};
                ctx.consumer->consume(buffers, ctx.frame_count++);
                ctx.pool->releaseFilled(remaining);
                drained_count++;
                total_frames++;
            }
            if (drained_count > 0) {
                LOG4CPLUS_DEBUG_FMT(logger_, "  %s: drained %d buffers", 
                    ctx.worker_name.c_str(), drained_count);
                total_drained += drained_count;
            }
        }
        if (total_drained > 0) {
            LOG4CPLUS_DEBUG_FMT(logger_, "Total drained: %d buffers", total_drained);
        }
        
        // 8. 清理消费者
        for (auto& ctx : worker_contexts) {
            if (ctx.frame_count > 0) {
                ctx.consumer->finalize();
            }
        }
        
        // 9. 填充结果
        result.success = true;
        result.frames_consumed = total_frames;
        
        // 9.1 填充比较结果（如果有）
        if (compare_ctx) {
            result.frames_compared = compare_ctx->total_frames.load();
            result.psnr_average = compare_ctx->getAveragePsnr();
            result.ssim_average = compare_ctx->getAverageSsim();
            result.compare_passed = compare_ctx->isPassed();
        }
        
        // 打印统计
        LOG4CPLUS_INFO(logger_, "========================================");
        LOG4CPLUS_INFO(logger_, "MultiWorker Compare Complete");
        LOG4CPLUS_INFO_FMT(logger_, "Total frames processed: %d", total_frames);
        for (const auto& ctx : worker_contexts) {
            LOG4CPLUS_INFO_FMT(logger_, "  %s: %d frames", 
                ctx.worker_name.c_str(), ctx.frame_count);
        }
        if (compare_ctx) {
            LOG4CPLUS_INFO_FMT(logger_, "Compare: %d frames, PSNR=%.2f dB, SSIM=%.4f, %s (pass_rate=%.1f%%)",
                result.frames_compared, result.psnr_average, result.ssim_average,
                result.compare_passed ? "PASSED" : "FAILED",
                compare_ctx->getPassRate());
        }
        LOG4CPLUS_INFO(logger_, "========================================");
        
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("Exception: ") + e.what();
        LOG4CPLUS_ERROR_FMT(logger_, "startMultiWorkerCompare exception: %s", e.what());
    }
    
    // 计算执行时间
    auto end_time = std::chrono::steady_clock::now();
    result.duration_seconds = std::chrono::duration<double>(end_time - start_time).count();
    if (result.duration_seconds > 0) {
        result.average_fps = result.frames_consumed / result.duration_seconds;
    }
    
    return result;
}

// ============================================================
// 辅助方法
// ============================================================

std::shared_ptr<IBufferConsumer> BufferConsumerService::createConsumerFromFlags(
    uint32_t flags,
    const WorkerConfig& config
) {
    // 统计有多少种消费类型被启用
    int type_count = 0;
    if (flags & CONSUME_COUNT) type_count++;
    if (flags & CONSUME_DISPLAY) type_count++;
    if (flags & CONSUME_SAVE_RAW) type_count++;
    if (flags & CONSUME_SAVE_ENCODED) type_count++;
    
    // 如果没有指定任何类型，默认使用 COUNT
    if (type_count == 0) {
        return std::make_shared<CountConsumer>();
    }
    
    // 如果只有一种类型，直接创建
    if (type_count == 1) {
        if (flags & CONSUME_COUNT) {
            return std::make_shared<CountConsumer>();
        }
        if (flags & CONSUME_DISPLAY) {
            return std::make_shared<DisplayConsumer>(
                config.consumer_type.display.device_id);
        }
        if (flags & CONSUME_SAVE_RAW) {
            return std::make_shared<SaveRawConsumer>(
                config.consumer_type.save_raw.output_paths,
                config.consumer_type.save_raw.max_frames_per_channel
            );
        }
        if (flags & CONSUME_SAVE_ENCODED) {
            return std::make_shared<SaveEncodedConsumer>(
                config.consumer_type.save_encoded.output_path,
                config.data_source.codec_params,
                config.data_source.time_base
            );
        }
    }
    
    // 多种类型叠加，使用 MultiConsumer
    auto multi = std::make_shared<MultiConsumer>();
    
    // 注意：COUNT 通常与其他类型组合使用，所以总是添加
    multi->addStrategy(std::make_shared<CountConsumer>());
    
    if (flags & CONSUME_DISPLAY) {
        multi->addStrategy(std::make_shared<DisplayConsumer>(
            config.consumer_type.display.device_id));
    }
    if (flags & CONSUME_SAVE_RAW) {
        multi->addStrategy(std::make_shared<SaveRawConsumer>(
            config.consumer_type.save_raw.output_paths,
            config.consumer_type.save_raw.max_frames_per_channel
        ));
    }
    if (flags & CONSUME_SAVE_ENCODED) {
        multi->addStrategy(std::make_shared<SaveEncodedConsumer>(
            config.consumer_type.save_encoded.output_path,
            config.data_source.codec_params,
            config.data_source.time_base
        ));
    }
    
    return multi;
}

void BufferConsumerService::consumeLoop(
    std::shared_ptr<BufferPool> pool,
    std::shared_ptr<IBufferConsumer> consumer,
    const WorkerConfig::ConsumerTypeConfig& config,
    ConsumeResult& result
) {
    int frame_index = 0;
    int timeout_count = 0;
    bool initialized = false;
    
    LOG4CPLUS_DEBUG(logger_, "Starting consume loop");
    
    while (!stop_requested_) {
        // 检查最大帧数限制
        if (config.max_frames > 0 && frame_index >= config.max_frames) {
            LOG4CPLUS_DEBUG_FMT(logger_, "Reached max frames: %d", config.max_frames);
            break;
        }
        
        // 获取填充的 Buffer
        Buffer* buffer = pool->acquireFilled(true, config.timeout_ms);
        
        if (!buffer) {
            timeout_count++;
            if (timeout_count >= config.max_timeout_count) {
                LOG4CPLUS_DEBUG_FMT(logger_, "Max timeout count reached: %d", timeout_count);
                break;
            }
            continue;
        }
        
        timeout_count = 0;
        
        // 首帧初始化
        if (!initialized) {
            std::vector<Buffer*> first_buffers = {buffer};
            if (!consumer->initialize(first_buffers)) {
                LOG4CPLUS_ERROR(logger_, "Consumer initialization failed");
                pool->releaseFilled(buffer);
                break;
            }
            initialized = true;
        }
        
        // 消费 Buffer
        std::vector<Buffer*> buffers = {buffer};
        bool continue_consume = consumer->consume(buffers, frame_index);
        
        // 归还 Buffer
        pool->releaseFilled(buffer);
        
        frame_index++;
        result.frames_consumed++;
        
        if (!continue_consume) {
            LOG4CPLUS_DEBUG(logger_, "Consumer requested stop");
            break;
        }
    }
    
    // 注意：drain 和 finalize 由调用者负责（在 producer.stop() 之后执行）
    LOG4CPLUS_DEBUG_FMT(logger_, "Consume loop finished, %d frames consumed", result.frames_consumed);
}

// ============================================================
// 工具方法
// ============================================================

void BufferConsumerService::printResult(const std::string& test_name, const ConsumeResult& result) {
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("consumer.BufferConsumerService"));
    
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "───────────────────────────────────────────────────────");
    LOG4CPLUS_INFO_FMT(logger, "  Test: %s", test_name.c_str());
    LOG4CPLUS_INFO(logger, "───────────────────────────────────────────────────────");
    LOG4CPLUS_INFO_FMT(logger, "  Status:          %s", result.success ? "PASSED" : "FAILED");
    LOG4CPLUS_INFO_FMT(logger, "  Result:          %s", result.getOverallResult() ? "PASSED" : "FAILED");
    LOG4CPLUS_INFO_FMT(logger, "  Frames:          %d", result.frames_consumed);
    LOG4CPLUS_INFO_FMT(logger, "  Duration:        %.2f s", result.duration_seconds);
    LOG4CPLUS_INFO_FMT(logger, "  Average FPS:     %.2f", result.average_fps);
    
    if (result.frames_displayed > 0) {
        LOG4CPLUS_INFO_FMT(logger, "  Displayed:       %d", result.frames_displayed);
    }
    if (result.frames_saved > 0) {
        LOG4CPLUS_INFO_FMT(logger, "  Saved:           %d", result.frames_saved);
    }
    if (result.frames_compared > 0) {
        LOG4CPLUS_INFO_FMT(logger, "  Compared:        %d", result.frames_compared);
        LOG4CPLUS_INFO_FMT(logger, "  Avg PSNR:        %.2f dB", result.psnr_average);
        LOG4CPLUS_INFO_FMT(logger, "  Avg SSIM:        %.4f", result.ssim_average);
        LOG4CPLUS_INFO_FMT(logger, "  Quality:         %s", result.compare_passed ? "PASSED" : "FAILED");
    }
    if (!result.output_file.empty()) {
        LOG4CPLUS_INFO_FMT(logger, "  Output:          %s", result.output_file.c_str());
    }
    if (!result.error_message.empty()) {
        LOG4CPLUS_INFO_FMT(logger, "  Error:           %s", result.error_message.c_str());
    }
    
    // PARALLEL 模式显示每个 Worker 的结果
    if (!result.worker_results.empty()) {
        LOG4CPLUS_INFO(logger, "  Worker Results:");
        for (size_t i = 0; i < result.worker_results.size(); i++) {
            const auto& wr = result.worker_results[i];
            LOG4CPLUS_INFO_FMT(logger, "    [%zu] %s, %d frames, %.2f fps",
                i, wr.success ? "OK" : "FAIL", wr.frames_consumed, wr.average_fps);
        }
    }
    
    LOG4CPLUS_INFO(logger, "───────────────────────────────────────────────────────");
    LOG4CPLUS_INFO(logger, "");
}

void BufferConsumerService::printHeader(const std::string& test_name, const WorkerConfig& config) {
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("consumer.BufferConsumerService"));
    
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO_FMT(logger, "  %s", test_name.c_str());
    LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO_FMT(logger, "  Input:           %s", config.data_source.path.c_str());
    if (config.decoder.name.has_value()) {
        LOG4CPLUS_INFO_FMT(logger, "  Decoder:         %s", config.decoder.name.value().c_str());
    }
    LOG4CPLUS_INFO_FMT(logger, "  Max Frames:      %d", config.consumer_type.max_frames);
    if (config.consumer_type.display.enable) {
        LOG4CPLUS_INFO(logger, "  Display:         enabled");
    }
    if (config.consumer_type.save_raw.enable) {
        LOG4CPLUS_INFO_FMT(logger, "  Save Raw:        to %s (max %d frames/channel)", 
            config.consumer_type.save_raw.getOutputPath(0).c_str(),
            config.consumer_type.save_raw.getMaxFrames(0));
    }
    if (config.consumer_type.save_encoded.enable) {
        LOG4CPLUS_INFO_FMT(logger, "  Save Encoded:    to %s", 
            config.consumer_type.save_encoded.output_path.c_str());
    }
    LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
}

} // namespace consumer
