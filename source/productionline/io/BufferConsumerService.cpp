/**
 * @file BufferConsumerService.cpp
 * @brief Buffer 消费服务实现
 */

#include "productionline/io/BufferConsumerService.hpp"
#include "common/GlobalThreadPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"

#include <future>
#include <sstream>
#include <iomanip>

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
            LOG4CPLUS_INFO_FMT(logger_, "Starting COMPARE mode with %zu workers", configs.size());
            result = startProductionLinesCompare(configs);
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
        consumeLoop(pool, consumer, config.consumer, result);
        
        // 6. 停止生产线
        producer.stop();
        
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
    const std::vector<WorkerConfig>& configs
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
        
        // 2. 创建 CompareConsumer
        const auto& consumer_config = configs[0].consumer;
        auto compare_consumer = std::make_shared<CompareConsumer>(
            consumer_config.min_psnr,
            consumer_config.min_ssim,
            consumer_config.enable_psnr,
            consumer_config.enable_ssim
        );
        
        // 3. 执行同步消费循环
        consumeLoopCompare(pools, compare_consumer, consumer_config, result);
        
        // 4. 获取比较结果
        result.psnr_average = compare_consumer->getAveragePsnr();
        result.ssim_average = compare_consumer->getAverageSsim();
        result.compare_passed = compare_consumer->isPassed();
        result.frames_compared = compare_consumer->getComparedCount();
        
        // 5. 停止所有生产线
        for (auto& producer : producers) {
            producer->stop();
        }
        
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
            return std::make_shared<DisplayConsumer>();
        }
        if (flags & CONSUME_SAVE_RAW) {
            return std::make_shared<SaveRawConsumer>(
                config.consumer.output_path,
                config.consumer.save_frames
            );
        }
        if (flags & CONSUME_SAVE_ENCODED) {
            return std::make_shared<SaveEncodedConsumer>(
                config.consumer.output_path,
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
        multi->addStrategy(std::make_shared<DisplayConsumer>());
    }
    if (flags & CONSUME_SAVE_RAW) {
        multi->addStrategy(std::make_shared<SaveRawConsumer>(
            config.consumer.output_path,
            config.consumer.save_frames
        ));
    }
    if (flags & CONSUME_SAVE_ENCODED) {
        multi->addStrategy(std::make_shared<SaveEncodedConsumer>(
            config.consumer.output_path,
            config.data_source.codec_params,
            config.data_source.time_base
        ));
    }
    
    return multi;
}

void BufferConsumerService::consumeLoop(
    std::shared_ptr<BufferPool> pool,
    std::shared_ptr<IBufferConsumer> consumer,
    const WorkerConfig::ConsumerConfig& config,
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
    
    // 清理
    if (initialized) {
        consumer->finalize();
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Consume loop finished, %d frames consumed", result.frames_consumed);
}

void BufferConsumerService::consumeLoopCompare(
    const std::vector<std::shared_ptr<BufferPool>>& pools,
    std::shared_ptr<IBufferConsumer> consumer,
    const WorkerConfig::ConsumerConfig& config,
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
    
    // 清理
    if (initialized) {
        consumer->finalize();
    }
    
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
    LOG4CPLUS_INFO_FMT(logger, "  Max Frames:      %d", config.consumer.max_frames);
    if (config.consumer.enable_display) {
        LOG4CPLUS_INFO(logger, "  Display:         enabled");
    }
    if (config.consumer.save_frames != 0) {
        LOG4CPLUS_INFO_FMT(logger, "  Save:            %d frames to %s", 
            config.consumer.save_frames, config.consumer.output_path.c_str());
    }
    LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
}

} // namespace consumer
