/**
 * @file BufferConsumerService.cpp
 * @brief Buffer 消费服务实现
 */

#include "productionline/io/BufferConsumerService.hpp"
#include "productionline/MultiWorkerProductionLine.hpp"
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
            LOG4CPLUS_INFO_FMT(logger_, "Starting COMPARE mode with %zu workers, flags=0x%X", 
                              configs.size(), consume_flags);
            result = startProductionLinesCompare(configs, consume_flags);
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
// COMPARE 模式实现
// ============================================================

ConsumeResult BufferConsumerService::startProductionLinesCompare(
    const std::vector<WorkerConfig>& configs,
    uint32_t consume_flags
) {
    ConsumeResult result;
    auto start_time = std::chrono::steady_clock::now();
    
    if (configs.size() < 2) {
        result.success = false;
        result.error_message = "COMPARE mode requires at least 2 configs";
        return result;
    }
    
    try {
        std::vector<std::unique_ptr<VideoProductionLine>> producers;
        std::vector<std::shared_ptr<BufferPool>> pools;
        
        // 1. 创建并启动所有生产线
        for (const auto& config : configs) {
            auto producer = std::make_unique<VideoProductionLine>();
            
            if (!producer->start(config)) {
                result.success = false;
                result.error_message = "Failed to start VideoProductionLine";
                // 停止已启动的生产线
                for (auto& p : producers) {
                    p->stop();
                }
                return result;
            }
            
            // 通过 Registry 获取 BufferPool
            auto pool_id = producer->getWorkingBufferPoolId();
            auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
            auto pool = pool_weak.lock();
            if (!pool) {
                result.success = false;
                result.error_message = "Failed to get BufferPool";
                for (auto& p : producers) {
                    p->stop();
                }
                return result;
            }
            
            pools.push_back(pool);
            producers.push_back(std::move(producer));
        }
        
        // 2. 创建 CompareConsumer（COMPARE 模式核心）
        const auto& consumer_type_config = configs[0].consumer_type;
        auto compare_consumer = std::make_shared<CompareConsumer>(
            consumer_type_config.compare.min_psnr,
            consumer_type_config.compare.min_ssim,
            consumer_type_config.compare.enable_psnr,
            consumer_type_config.compare.enable_ssim
        );
        
        // 3. 根据 consume_flags 决定是否叠加其他消费类型
        std::shared_ptr<IBufferConsumer> consumer;
        
        // 检查是否有额外的消费类型（DISPLAY、SAVE_RAW、SAVE_ENCODED）
        uint32_t extra_flags = consume_flags & (CONSUME_DISPLAY | CONSUME_SAVE_RAW | CONSUME_SAVE_ENCODED);
        
        if (extra_flags != 0) {
            // 使用 MultiConsumer 组合多个消费策略
            auto multi = std::make_shared<MultiConsumer>();
            
            // CompareConsumer 作为核心（第一个添加）
            multi->addStrategy(compare_consumer);
            
            // 叠加其他消费类型（使用第一个 config 的配置）
            if (consume_flags & CONSUME_DISPLAY) {
                multi->addStrategy(std::make_shared<DisplayConsumer>(
                    consumer_type_config.display.device_id));
                LOG4CPLUS_DEBUG(logger_, "COMPARE mode: added DisplayConsumer");
            }
            if (consume_flags & CONSUME_SAVE_RAW) {
                multi->addStrategy(std::make_shared<SaveRawConsumer>(
                    consumer_type_config.save_raw.output_path,
                    consumer_type_config.save_raw.max_frames
                ));
                LOG4CPLUS_DEBUG_FMT(logger_, "COMPARE mode: added SaveRawConsumer to %s", 
                                   consumer_type_config.save_raw.output_path.c_str());
            }
            if (consume_flags & CONSUME_SAVE_ENCODED) {
                multi->addStrategy(std::make_shared<SaveEncodedConsumer>(
                    consumer_type_config.save_encoded.output_path,
                    configs[0].data_source.codec_params,
                    configs[0].data_source.time_base
                ));
                LOG4CPLUS_DEBUG_FMT(logger_, "COMPARE mode: added SaveEncodedConsumer to %s", 
                                   consumer_type_config.save_encoded.output_path.c_str());
            }
            
            consumer = multi;
        } else {
            // 只有 CompareConsumer
            consumer = compare_consumer;
        }
        
        // 4. 执行同步消费循环
        consumeLoopCompare(pools, consumer, consumer_type_config, result);
        
        // 5. 停止所有生产线（等待生产者完成所有帧的生产）
        for (auto& producer : producers) {
            producer->stop();
        }
        
        // 6. 排空剩余的已填充 Buffer（drain 阶段）
        // 此时所有生产者已停止，BufferPool 中应该有所有剩余的 filled buffer
        LOG4CPLUS_DEBUG(logger_, "Draining remaining buffers after producers stopped...");
        int drained_count = 0;
        int drain_frame_index = result.frames_consumed;
        bool has_remaining = true;
        while (has_remaining) {
            std::vector<Buffer*> buffers;
            has_remaining = true;
            
            // 尝试从所有 pool 获取 buffer（非阻塞）
            for (auto& pool : pools) {
                Buffer* buffer = pool->acquireFilled(false, 0);
                if (!buffer) {
                    has_remaining = false;
                    break;
                }
                buffers.push_back(buffer);
            }
            
            if (has_remaining && buffers.size() == pools.size()) {
                // 成功获取所有 buffer，进行消费
                consumer->consume(buffers, drain_frame_index++);
                result.frames_consumed++;
                drained_count++;
                // 释放所有 buffer
                for (size_t i = 0; i < buffers.size(); i++) {
                    pools[i]->releaseFilled(buffers[i]);
                }
            } else {
                // 未能获取所有 buffer，释放已获取的并退出
                for (size_t i = 0; i < buffers.size(); i++) {
                    pools[i]->releaseFilled(buffers[i]);
                }
                break;
            }
        }
        if (drained_count > 0) {
            LOG4CPLUS_DEBUG_FMT(logger_, "Drained %d remaining buffer sets", drained_count);
        }
        
        // 7. 清理消费者
        consumer->finalize();
        
        // 8. 获取比较结果（从 CompareConsumer 获取）
        result.psnr_average = compare_consumer->getAveragePsnr();
        result.ssim_average = compare_consumer->getAverageSsim();
        result.compare_passed = compare_consumer->isPassed();
        result.frames_compared = compare_consumer->getComparedCount();
        
        result.success = true;
        
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("Exception: ") + e.what();
        LOG4CPLUS_ERROR_FMT(logger_, "startProductionLinesCompare exception: %s", e.what());
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
            config.save_raw.output_path,
            config.save_raw.max_frames));
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
    const MultiWorkerConfig& multi_config,
    CompareResultCallback compare_callback)
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
        
        // 4. 启动 ProductionLine
        if (!production_line->start()) {
            result.success = false;
            result.error_message = "Failed to start MultiWorkerProductionLine";
            return result;
        }
        
        // 5. 消费循环 - 轮询所有 worker 的 BufferPool
        // 使用第一个 group 第一个 consumer 的配置作为默认
        const auto& first_config = multi_config.groups[0].consumer_configs[0].worker_config.consumer_type;
        int max_frames = first_config.max_frames;
        int timeout_ms = first_config.timeout_ms;
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
                Buffer* buffer = ctx.pool->acquireFilled(false, timeout_ms);
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
        
        // 6. 清理消费者
        for (auto& ctx : worker_contexts) {
            if (ctx.frame_count > 0) {
                ctx.consumer->finalize();
            }
        }
        
        // 7. 停止
        production_line->stop();
        
        // 8. 填充结果
        result.success = true;
        result.frames_consumed = total_frames;
        
        // 打印统计
        LOG4CPLUS_INFO(logger_, "========================================");
        LOG4CPLUS_INFO(logger_, "MultiWorker Compare Complete");
        LOG4CPLUS_INFO_FMT(logger_, "Total frames processed: %d", total_frames);
        for (const auto& ctx : worker_contexts) {
            LOG4CPLUS_INFO_FMT(logger_, "  %s: %d frames", 
                ctx.worker_name.c_str(), ctx.frame_count);
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
                config.consumer_type.save_raw.output_path,
                config.consumer_type.save_raw.max_frames
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
            config.consumer_type.save_raw.output_path,
            config.consumer_type.save_raw.max_frames
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

void BufferConsumerService::consumeLoopCompare(
    const std::vector<std::shared_ptr<BufferPool>>& pools,
    std::shared_ptr<IBufferConsumer> consumer,
    const WorkerConfig::ConsumerTypeConfig& config,
    ConsumeResult& result
) {
    int frame_index = 0;
    int timeout_count = 0;
    bool initialized = false;
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Starting compare loop with %zu pools", pools.size());
    
    while (!stop_requested_) {
        // 检查最大帧数限制
        if (config.max_frames > 0 && frame_index >= config.max_frames) {
            LOG4CPLUS_DEBUG_FMT(logger_, "Reached max frames: %d", config.max_frames);
            break;
        }
        
        // 同步获取所有 BufferPool 的 Buffer
        std::vector<Buffer*> buffers;
        bool all_acquired = true;
        
        for (auto& pool : pools) {
            Buffer* buffer = pool->acquireFilled(true, config.timeout_ms);
            if (!buffer) {
                all_acquired = false;
                break;
            }
            buffers.push_back(buffer);
        }
        
        if (!all_acquired) {
            // 释放已获取的 Buffer
            for (size_t i = 0; i < buffers.size(); i++) {
                pools[i]->releaseFilled(buffers[i]);
            }
            
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
            if (!consumer->initialize(buffers)) {
                LOG4CPLUS_ERROR(logger_, "Consumer initialization failed");
                for (size_t i = 0; i < buffers.size(); i++) {
                    pools[i]->releaseFilled(buffers[i]);
                }
                break;
            }
            initialized = true;
        }
        
        // 消费 Buffers
        bool continue_consume = consumer->consume(buffers, frame_index);
        
        // 归还所有 Buffer
        for (size_t i = 0; i < buffers.size(); i++) {
            pools[i]->releaseFilled(buffers[i]);
        }
        
        frame_index++;
        result.frames_consumed++;
        
        if (!continue_consume) {
            LOG4CPLUS_DEBUG(logger_, "Consumer requested stop");
            break;
        }
    }
    
    // 注意：drain 和 finalize 由调用者负责（在 producers.stop() 之后执行）
    LOG4CPLUS_DEBUG_FMT(logger_, "Compare loop finished, %d frames consumed", result.frames_consumed);
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
        LOG4CPLUS_INFO_FMT(logger, "  Save Raw:        to %s (max %d frames)", 
            config.consumer_type.save_raw.output_path.c_str(),
            config.consumer_type.save_raw.max_frames);
    }
    if (config.consumer_type.save_encoded.enable) {
        LOG4CPLUS_INFO_FMT(logger, "  Save Encoded:    to %s", 
            config.consumer_type.save_encoded.output_path.c_str());
    }
    LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
}

} // namespace consumer
