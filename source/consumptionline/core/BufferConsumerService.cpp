/**
 * @file BufferConsumerService.cpp
 * @brief Buffer 消费服务实现
 */

#include "consumptionline/core/BufferConsumerService.hpp"
#include "productionline/worker/config/ConfigBuilders.hpp"
#include "productionline/worker/config/MgDatasourceProducerType.hpp"
#include "consumptionline/config/ConsumerTypeConfigBuilder.hpp"
#include "productionline/line/MultiWorkerProductionLine.hpp"
#include "productionline/line/WorkerSyncCoordinator.hpp"
#include "common/GlobalThreadPool.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include "consumptionline/types/npu/NpuInferenceConsumer.hpp"
#include "common/PerfFileWriter.hpp"

#include <future>
#include <sstream>
#include <iomanip>
#include <thread>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

namespace consumer {

namespace {

/// Append WorkerConfig::extra_consumer (e.g. WebUI PreviewFrameTap) if set.
static std::shared_ptr<IBufferConsumer> attachExtraConsumer(
    std::shared_ptr<IBufferConsumer> base,
    const WorkerConfig& config)
{
    if (!config.extra_consumer) {
        return base;
    }
    if (!base) {
        return config.extra_consumer;
    }
    auto multi = std::make_shared<MultiConsumer>();
    multi->addStrategy(std::move(base));
    multi->addStrategy(config.extra_consumer);
    return multi;
}

} // namespace

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
 * @brief 按 MgDatasourceProducerType 构造 MultiWorker datasource 生产者 WorkerConfig
 *
 * UNSPECIFIED / PACKET_RECORDER → 现有 Recorder 逻辑。
 * DECODE_THEN_ENCODE → Decode + consumer_type.video_encode（共享源由 MultiWorker 桥接到编码池）。
 */
static WorkerConfig buildCompareDatasourceProducer(
    const WorkerConfig& seed,
    MgDatasourceProducerType type)
{
    const int buffer_count = seed.data_source.buffer_count > 0
        ? seed.data_source.buffer_count : 32;

    auto data_source = DataSourceConfigBuilder()
        .setPath(seed.data_source.path)
        .setBufferCount(buffer_count)
        .setMaxFrames(seed.data_source.max_frames)
        .setLoop(seed.data_source.loop)
        .setLoopCount(seed.data_source.loop_count)
        .setRawFrameSize(seed.data_source.raw_frame_width,
                         seed.data_source.raw_frame_height)
        .build();

    if (type == MgDatasourceProducerType::UNSPECIFIED ||
        type == MgDatasourceProducerType::FFMPEG_PACKET_RECORDER) {
        return WorkerConfigBuilder()
            .setGlobalConfig(
                WorkerGlobalConfigBuilder()
                    .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
                    .build())
            .setDataSourceConfig(data_source)
            .build();
    }

    if (type == MgDatasourceProducerType::FFMPEG_DECODE ||
        type == MgDatasourceProducerType::FFMPEG_DECODE_THEN_ENCODE) {
        WorkerConfig producer = WorkerConfigBuilder()
            .setGlobalConfig(
                WorkerGlobalConfigBuilder()
                    .setWorkerType(WorkerType::FFMPEG_DECODE)
                    .build())
            .setDataSourceConfig(data_source)
            .setDecoderConfig(seed.decoder)
            .build();

        if (type == MgDatasourceProducerType::FFMPEG_DECODE_THEN_ENCODE) {
            ConsumerTypeConfig::VideoEncodeType ve = seed.consumer_type.video_encode;
            ve.enable = true;
            if (ve.encoder_name.empty()) {
                ve.encoder_name = "h264_taco";
            }
            if (seed.encoder.bit_rate > 0) {
                ve.bit_rate = seed.encoder.bit_rate;
            }
            if (seed.encoder.gop_size > 0) {
                ve.gop_size = seed.encoder.gop_size;
            }
            if (seed.encoder.framerate_num > 0) {
                ve.framerate_num = seed.encoder.framerate_num;
                ve.framerate_den = seed.encoder.framerate_den > 0
                    ? seed.encoder.framerate_den : 1;
            }
            if (seed.encoder.name.has_value() && !seed.encoder.name->empty()) {
                ve.encoder_name = *seed.encoder.name;
            }
            producer.consumer_type = ConsumerTypeConfigBuilder(producer.consumer_type)
                .setVideoEncodeConfig(ve)
                .build();
        }
        return producer;
    }

    if (type == MgDatasourceProducerType::FFMPEG_ENCODE) {
        WorkerConfig producer = WorkerConfigBuilder()
            .setGlobalConfig(
                WorkerGlobalConfigBuilder()
                    .setWorkerType(WorkerType::FFMPEG_ENCODE)
                    .build())
            .setDataSourceConfig(data_source)
            .build();
        producer.encoder = seed.encoder;
        return producer;
    }

    return WorkerConfigBuilder()
        .setGlobalConfig(
            WorkerGlobalConfigBuilder()
                .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
                .build())
        .setDataSourceConfig(data_source)
        .build();
}

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
    GroupConfig group("compare_group");

    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("consumer.BufferConsumerService"));

    bool parse_ok = true;
    const MgDatasourceProducerType ds_type =
        parseMgDatasourceProducerType(configs[0].mg_datasource_producer_type, &parse_ok);
    if (!parse_ok) {
        LOG4CPLUS_WARN_FMT(logger,
            "未知 --mg-datasource-producer-type='%s'，回退 UNSPECIFIED(PACKET_RECORDER)",
            configs[0].mg_datasource_producer_type.c_str());
    } else if (ds_type != MgDatasourceProducerType::UNSPECIFIED) {
        LOG4CPLUS_INFO_FMT(logger,
            "COMPARE datasource producer type: %s",
            mgDatasourceProducerTypeToString(ds_type));
    }

    // 1. 配置生产者（datasource）
    group.producers["packet_source"] = buildCompareDatasourceProducer(configs[0], ds_type);

    // 2. 配置消费者（Decoder Workers）
    {
        WorkerConfig hw_wc = configs[0];
        auto builder = ConsumerTypeConfigBuilder(hw_wc.consumer_type);
        if (flags & CONSUME_DISPLAY) {
            builder.setDisplayConfig(DisplayConsumerConfigBuilder(hw_wc.consumer_type.display)
                .setEnable(true).build());
        }
        if (flags & CONSUME_SAVE_RAW) {
            builder.setSaveRawConfig(SaveRawConfigBuilder(hw_wc.consumer_type.save_raw)
                .setEnable(true).build());
        }
        if (flags & CONSUME_SAVE_ENCODED) {
            builder.setSaveEncodedConfig(SaveEncodedConfigBuilder(hw_wc.consumer_type.save_encoded)
                .setEnable(true).build());
        }
        builder.setVideoEncodeConfig(VideoEncodeConfigBuilder()
            .setEnable(false).build());
        hw_wc.consumer_type = builder.build();
        hw_wc.mg_datasource_producer_type.clear();
        group.consumers["hw_decoder"] = hw_wc;
    }

    {
        WorkerConfig sw_wc = configs[1];
        auto builder = ConsumerTypeConfigBuilder(sw_wc.consumer_type);
        builder.setDisplayConfig(DisplayConsumerConfigBuilder(sw_wc.consumer_type.display)
            .setEnable(false).build());
        if (flags & CONSUME_SAVE_RAW) {
            builder.setSaveRawConfig(SaveRawConfigBuilder(sw_wc.consumer_type.save_raw)
                .setEnable(true).build());
        }
        if (flags & CONSUME_SAVE_ENCODED) {
            builder.setSaveEncodedConfig(SaveEncodedConfigBuilder(sw_wc.consumer_type.save_encoded)
                .setEnable(true).build());
        }
        builder.setVideoEncodeConfig(VideoEncodeConfigBuilder()
            .setEnable(false).build());
        sw_wc.consumer_type = builder.build();
        sw_wc.mg_datasource_producer_type.clear();
        group.consumers["sw_decoder"] = sw_wc;
    }

    // 3. 配置组模式和回调
    group.mode = GroupConfig::Mode::ONE_TO_MANY;
    group.enable_frame_sync = true;

    const auto& consumer_type = configs[0].consumer_type;

    std::shared_ptr<CompareCallbackContext> compare_ctx = nullptr;

    if (consumer_type.opencv.enable) {
        LOG4CPLUS_INFO_FMT(logger,
            "使用 OpenCV 模式，操作类型: %d",
            static_cast<int>(consumer_type.opencv.op_type));

        auto opencv_ctx = std::make_shared<OpenCVCallbackContext>();
        opencv_ctx->config = consumer_type.opencv;
        opencv_ctx->worker1_name = "hw_decoder";
        opencv_ctx->worker2_name = "sw_decoder";

        group.callback_chain.push_back(
            WorkerSyncCoordinator::createOpenCVCallback(opencv_ctx.get()));
    }
    else {
        LOG4CPLUS_INFO(logger, "使用 Compare 模式，计算 PSNR/SSIM");

        compare_ctx = std::make_shared<CompareCallbackContext>();
        compare_ctx->initFromCompareType(consumer_type.compare);
        compare_ctx->worker1_name = "hw_decoder";
        compare_ctx->worker2_name = "sw_decoder";

        group.callback_chain.push_back(
            WorkerSyncCoordinator::createDefaultCompareCallback(compare_ctx.get()));
    }

    // 4. 组装配置
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
        VideoProductionLine producer(config.data_source.loop);
        
        // 2. 启动生产线（配置在 start 中传入）
        if (!producer.start(config)) {
            result.success = false;
            result.error_message = "Failed to start VideoProductionLine";
            return result;
        }
        
        // 3. 获取 BufferPool（通过 Registry）
        auto pool_id = producer.getWorkingBufferPoolId();
        auto pool_weak = ComponentTopology::getInstance().getPool(pool_id);
        auto pool = pool_weak.lock();
        if (!pool) {
            result.success = false;
            result.error_message = "Failed to get BufferPool";
            producer.stop();
            return result;
        }
        
        // 3.5 从 Worker 获取 codec_params/time_base 供消费者使用
        // SINGLE 模式下 config 中的 codec_params 可能为 nullptr，
        // 但 Worker 启动后已从数据源获取到有效值
        WorkerConfig consumer_config = config;
        auto worker = producer.getWorker();
        if (worker) {
            if (!consumer_config.data_source.codec_params) {
                consumer_config.data_source.codec_params = worker->getSourceCodecParameters();
            }
            if (consumer_config.data_source.time_base.num == 0) {
                consumer_config.data_source.time_base = worker->getTimeBase();
            }
        }
        
        // 4. 创建消费策略
        auto consumer = createConsumerFromFlags(consume_flags, consumer_config);
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

        // 9. 收集 Worker 层面的阶段计时（如编码器 FFmpegEncodeWorker 的计时）
        if (worker) {
            auto* encode_worker = dynamic_cast<FFmpegEncodeWorker*>(worker.get());
            if (encode_worker) {
                auto enc_timings = encode_worker->getStageTimings();
                result.stage_timings.insert(result.stage_timings.end(),
                    enc_timings.begin(), enc_timings.end());
            }
        }
        
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

ConsumeResult BufferConsumerService::consumeExternalPool(
    std::shared_ptr<BufferPool> pool,
    const WorkerConfig& config,
    uint32_t consume_flags)
{
    ConsumeResult result;
    auto start_time = std::chrono::steady_clock::now();
    stop_requested_ = false;
    running_.store(true);

    if (!pool) {
        result.success = false;
        result.error_message = "consumeExternalPool: BufferPool is null";
        running_.store(false);
        return result;
    }

    try {
        auto consumer = createConsumerFromFlags(consume_flags, config);
        if (!consumer) {
            result.success = false;
            result.error_message = "Failed to create consumer strategy";
            running_.store(false);
            return result;
        }

        consumeLoop(pool, consumer, config.consumer_type, result);

        LOG4CPLUS_DEBUG(logger_, "consumeExternalPool: draining remaining filled buffers...");
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
            LOG4CPLUS_DEBUG_FMT(logger_, "consumeExternalPool: drained %d buffers", drained_count);
        }

        consumer->finalize();
        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("consumeExternalPool exception: ") + e.what();
        LOG4CPLUS_ERROR_FMT(logger_, "%s", result.error_message.c_str());
    }

    running_.store(false);
    auto end_time = std::chrono::steady_clock::now();
    result.duration_seconds = std::chrono::duration<double>(end_time - start_time).count();
    if (result.duration_seconds > 0.0) {
        result.average_fps = static_cast<double>(result.frames_consumed) / result.duration_seconds;
    }
    return result;
}

// ============================================================
// PARALLEL 模式实现
// ============================================================

static uint32_t getConsumeFlagsFromConfig(const WorkerConfig::ConsumerTypeConfig& config);

ConsumeResult BufferConsumerService::startProductionLinesParallel(
    const std::vector<WorkerConfig>& configs,
    uint32_t consume_flags
) {
    (void)consume_flags;  // ABI compat; PARALLEL derives flags per WorkerConfig
    ConsumeResult total_result;
    auto start_time = std::chrono::steady_clock::now();
    
    try {
        std::vector<std::future<ConsumeResult>> futures;
        
        // 获取线程池引用
        auto& thread_pool = thread_pool_ ? *thread_pool_ : GlobalThreadPool::getInstance().getThreadPool();
        
        // 为每个 config 提交任务到线程池（显式复制 config，确保每个任务获得独立副本）
        for (size_t idx = 0; idx < configs.size(); idx++) {
            WorkerConfig cfg_copy = configs[idx];
            LOG4CPLUS_DEBUG_FMT(logger_, "PARALLEL worker[%zu] save_raw.output_paths[0]=%s",
                idx, cfg_copy.consumer_type.save_raw.output_paths.empty()
                    ? "(none)" : cfg_copy.consumer_type.save_raw.output_paths[0].c_str());
            auto future = thread_pool.submit_task([this, cfg_copy]() {
                uint32_t worker_flags = getConsumeFlagsFromConfig(cfg_copy.consumer_type);
                return startProductionLine(cfg_copy, worker_flags);
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
    if (config.npu_inference.enable) {
        flags |= CONSUME_NPU_INFERENCE;
    }
    if (config.jpeg_encode.enable) {
        flags |= CONSUME_JPEG_ENCODE;
    }
    if (config.video_encode.enable) {
        flags |= CONSUME_VIDEO_ENCODE;
    }
    // 注：compare 是执行模式（ExecuteMode），不是消费类型
    if (config.count.enable) {
        flags |= CONSUME_COUNT;
    }
    if (config.opencv.enable) {
        flags |= CONSUME_OPENCV;
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
        consumers.push_back(std::make_shared<DisplayConsumer>(config.display));
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

    // 4. OpenCV 消费者（Buffer→Mat 转换 + PSNR/SSIM）
    if (flags & CONSUME_OPENCV) {
        consumers.push_back(std::make_shared<OpencvConsumer>(worker_config));
    }

    
    // 4. NPU 推理消费者
    if (flags & CONSUME_NPU_INFERENCE) {
        NpuInferenceConfig npu_config;
        npu_config.model_path       = config.npu_inference.model_path;
        npu_config.algorithm        = config.npu_inference.algorithm;
        npu_config.conf_threshold   = config.npu_inference.conf_threshold;
        npu_config.nms_threshold    = config.npu_inference.nms_threshold;
        npu_config.npu_core_index   = config.npu_inference.npu_core_index;
        npu_config.input_mode       = config.npu_inference.use_physical_addr
            ? NpuInferenceConfig::InputMode::PHYSICAL_ADDR
            : NpuInferenceConfig::InputMode::VIRTUAL_ADDR;
        npu_config.enable_draw      = config.npu_inference.enable_draw;
        npu_config.inference_interval = config.npu_inference.inference_interval;
        consumers.push_back(std::make_shared<NpuInferenceConsumer>(npu_config));
    }
    
    // 5. JPEG 编码消费者
    if (flags & CONSUME_JPEG_ENCODE) {
        consumers.push_back(std::make_shared<JpegEncodeConsumer>(config.jpeg_encode));
    }

    // 5.5 视频编码消费者（输出包池可供 MultiWorker 共享）
    if (flags & CONSUME_VIDEO_ENCODE) {
        consumers.push_back(std::make_shared<VideoEncodeConsumer>(config.video_encode));
    }
    
    // 6. 统计消费者（如果没有其他消费者或显式启用）
    if (consumers.empty() || (flags & CONSUME_COUNT)) {
        consumers.push_back(std::make_shared<CountConsumer>());
    }

    // WebUI PreviewFrameTap (independent of JPEG flags)
    if (worker_config.extra_consumer) {
        consumers.push_back(worker_config.extra_consumer);
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
        // 数据源有限循环由 EncodedPacketSourceFromFile.loop_count 负责；
        // VideoProductionLine.loop_ 仅用于无限循环（data_source.loop=true）。
        bool ds_loop = false;
        if (!multi_config.groups.empty() && !multi_config.groups[0].producers.empty()) {
            ds_loop = multi_config.groups[0].producers.begin()->second.data_source.loop;
        }
        auto production_line = std::make_unique<MultiWorkerProductionLine>(multi_config, ds_loop);
        
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
            if (group_config.enable_frame_sync) {
                for (const auto& cb_item : group_config.callback_chain) {
                    if (cb_item.name == "default_compare_callback" && cb_item.context) {
                        compare_ctx = static_cast<CompareCallbackContext*>(cb_item.context);
                        LOG4CPLUS_DEBUG(logger_, "Found CompareCallbackContext in callback_chain");
                        break;
                    }
                }
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
            
            size_t worker_idx = 0;
            for (const auto& [consumer_name, worker_config] : group_config.consumers) {
                uint64_t pool_id = production_line->getGroupConsumerBufferPoolId(
                    group_idx, worker_idx);
                
                if (pool_id == 0) {
                    LOG4CPLUS_WARN_FMT(logger_, 
                        "No BufferPool for group[%zu] worker[%zu]", 
                        group_idx, worker_idx);
                    worker_idx++;
                    continue;
                }
                
                auto pool = ComponentTopology::getInstance().getPool(pool_id).lock();
                if (!pool) {
                    LOG4CPLUS_ERROR_FMT(logger_, 
                        "Failed to get BufferPool for group[%zu] worker[%zu]",
                        group_idx, worker_idx);
                    worker_idx++;
                    continue;
                }
                
                uint32_t flags = getConsumeFlagsFromConfig(worker_config.consumer_type);
                
                auto consumer = createConsumerForWorker(
                    worker_config, 
                    flags,
                    worker_config.data_source.codec_params,
                    worker_config.data_source.time_base);
                
                WorkerConsumeContext ctx;
                ctx.worker_name = consumer_name.empty() 
                    ? ("worker_" + std::to_string(group_idx) + "_" + std::to_string(worker_idx))
                    : consumer_name;
                ctx.pool = pool;
                ctx.consumer = consumer;
                
                worker_contexts.push_back(std::move(ctx));
                
                LOG4CPLUS_INFO_FMT(logger_, 
                    "Added worker context: %s (pool_id=%llu, flags=0x%x)",
                    worker_contexts.back().worker_name.c_str(), 
                    (unsigned long long)pool_id, flags);
                worker_idx++;
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
        const auto& first_config = multi_config.groups[0].consumers.begin()->second.consumer_type;
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
                // 编码/共享源 EOS 后 MultiWorker consumer 会正常退出；勿再空等 10×5s
                if (production_line->areAllConsumerWorkersFinished()) {
                    LOG4CPLUS_INFO(logger_,
                        "All MultiWorker consumers finished (EOS), ending compare loop");
                    break;
                }
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
    if (flags & CONSUME_OPENCV) type_count++;

    if (flags & CONSUME_NPU_INFERENCE) type_count++;
    if (flags & CONSUME_JPEG_ENCODE) type_count++;
    if (flags & CONSUME_VIDEO_ENCODE) type_count++;
    
    // 如果没有指定任何类型，默认使用 COUNT
    if (type_count == 0) {
        return attachExtraConsumer(std::make_shared<CountConsumer>(), config);
    }

    // 如果只有一种类型，直接创建
    if (type_count == 1) {
        std::shared_ptr<IBufferConsumer> single;
        if (flags & CONSUME_COUNT) {
            single = std::make_shared<CountConsumer>();
        } else if (flags & CONSUME_DISPLAY) {
            single = std::make_shared<DisplayConsumer>(config.consumer_type.display);
        } else if (flags & CONSUME_SAVE_RAW) {
            single = std::make_shared<SaveRawConsumer>(
                config.consumer_type.save_raw.output_paths,
                config.consumer_type.save_raw.max_frames_per_channel
            );
        } else if (flags & CONSUME_SAVE_ENCODED) {
            single = std::make_shared<SaveEncodedConsumer>(
                config.consumer_type.save_encoded.output_path,
                config.data_source.codec_params,
                config.data_source.time_base
            );
        } else if (flags & CONSUME_OPENCV) {
            single = std::make_shared<OpencvConsumer>(config);
        } else if (flags & CONSUME_NPU_INFERENCE) {
            NpuInferenceConfig npu_cfg;
            npu_cfg.model_path     = config.consumer_type.npu_inference.model_path;
            npu_cfg.algorithm      = config.consumer_type.npu_inference.algorithm;
            npu_cfg.conf_threshold = config.consumer_type.npu_inference.conf_threshold;
            npu_cfg.nms_threshold  = config.consumer_type.npu_inference.nms_threshold;
            npu_cfg.npu_core_index = config.consumer_type.npu_inference.npu_core_index;
            npu_cfg.input_mode     = config.consumer_type.npu_inference.use_physical_addr
                ? NpuInferenceConfig::InputMode::PHYSICAL_ADDR
                : NpuInferenceConfig::InputMode::VIRTUAL_ADDR;
            npu_cfg.enable_draw    = config.consumer_type.npu_inference.enable_draw;
            npu_cfg.inference_interval = config.consumer_type.npu_inference.inference_interval;
            single = std::make_shared<NpuInferenceConsumer>(npu_cfg);
        } else if (flags & CONSUME_JPEG_ENCODE) {
            single = std::make_shared<JpegEncodeConsumer>(config.consumer_type.jpeg_encode);
        } else if (flags & CONSUME_VIDEO_ENCODE) {
            single = std::make_shared<VideoEncodeConsumer>(config.consumer_type.video_encode);
        }
        return attachExtraConsumer(std::move(single), config);
    }

    // 多种类型叠加，使用 MultiConsumer
    // 顺序：NPU推理(画框) → JpegEncode(编码带框画面) → Display → Save → Count
    auto multi = std::make_shared<MultiConsumer>();
    
    // NPU 推理必须在 Display 之前：先画框再显示
    if (flags & CONSUME_NPU_INFERENCE) {
        NpuInferenceConfig npu_cfg;
        npu_cfg.model_path     = config.consumer_type.npu_inference.model_path;
        npu_cfg.algorithm      = config.consumer_type.npu_inference.algorithm;
        npu_cfg.conf_threshold = config.consumer_type.npu_inference.conf_threshold;
        npu_cfg.nms_threshold  = config.consumer_type.npu_inference.nms_threshold;
        npu_cfg.npu_core_index = config.consumer_type.npu_inference.npu_core_index;
        npu_cfg.input_mode     = config.consumer_type.npu_inference.use_physical_addr
            ? NpuInferenceConfig::InputMode::PHYSICAL_ADDR
            : NpuInferenceConfig::InputMode::VIRTUAL_ADDR;
        npu_cfg.enable_draw    = config.consumer_type.npu_inference.enable_draw;
        npu_cfg.inference_interval = config.consumer_type.npu_inference.inference_interval;
        multi->addStrategy(std::make_shared<NpuInferenceConsumer>(npu_cfg));
    }
    
    // JPEG 编码在 NPU 之后（编码带框画面）、Display 之前
    if (flags & CONSUME_JPEG_ENCODE) {
        multi->addStrategy(std::make_shared<JpegEncodeConsumer>(config.consumer_type.jpeg_encode));
    }

    if (flags & CONSUME_VIDEO_ENCODE) {
        multi->addStrategy(std::make_shared<VideoEncodeConsumer>(config.consumer_type.video_encode));
    }
    
    if (flags & CONSUME_DISPLAY) {
        multi->addStrategy(std::make_shared<DisplayConsumer>(config.consumer_type.display));
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
    if (flags & CONSUME_OPENCV) {
        multi->addStrategy(std::make_shared<OpencvConsumer>(config));
    }

    
    // COUNT 放在最后
    multi->addStrategy(std::make_shared<CountConsumer>());

    // WebUI PreviewFrameTap last (idle fast-path; must not stop the loop)
    if (config.extra_consumer) {
        multi->addStrategy(config.extra_consumer);
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
    perf::StageTimer consume_timer("consume");
    int timeout_count = 0;
    bool initialized = false;
    Buffer* held_buffer = nullptr;
    auto loop_start = std::chrono::steady_clock::now();

    // --perf-file 周期写入控制
    const bool perf_file_enabled = !config.perf_file_path.empty();
    int perf_write_interval = 30;  // 每 30 帧写入一次
    
    LOG4CPLUS_DEBUG(logger_, "Starting consume loop");
    
    while (!stop_requested_) {
        if (config.max_frames > 0 && frame_index >= config.max_frames) {
            LOG4CPLUS_DEBUG_FMT(logger_, "Reached max frames: %d", config.max_frames);
            break;
        }
        
        Buffer* buffer = nullptr;

        if (held_buffer) {
            buffer = held_buffer;
            held_buffer = nullptr;
        } else {
            buffer = pool->acquireFilled(true, config.timeout_ms);
            if (!buffer) {
                timeout_count++;
                if (timeout_count >= config.max_timeout_count) {
                    if (pool->isRunning()) {
                        // Pool 仍活跃（生产者可能正在 seek/重置），继续等待
                        timeout_count = 0;
                        continue;
                    }
                    LOG4CPLUS_DEBUG_FMT(logger_, "Max timeout count reached: %d", timeout_count);
                    break;
                }
                continue;
            }
        }
        
        timeout_count = 0;
        
        if (!initialized) {
            std::vector<Buffer*> first_buffers = {buffer};
            if (!consumer->initialize(first_buffers)) {
                LOG4CPLUS_ERROR(logger_, "Consumer initialization failed");
                pool->releaseFilled(buffer);
                break;
            }
            initialized = true;
        }

        std::vector<Buffer*> buffers = {buffer};
        bool continue_consume;
        {
            perf::StageTimer::ScopedRecord _sr(consume_timer);
            continue_consume = consumer->consume(buffers, frame_index);
        }
        
        if (consumer->shouldRetainBuffer()) {
            held_buffer = buffer;
        } else {
            pool->releaseFilled(buffer);
            frame_index++;
            result.frames_consumed++;

            // ── 周期性写入性能快照文件 ──
            if (perf_file_enabled && (result.frames_consumed % perf_write_interval == 0)) {
                auto now = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(now - loop_start).count();
                double fps = (elapsed > 0) ? result.frames_consumed / elapsed : 0;

                std::vector<perf::StageTiming> timings;
                timings.push_back(consume_timer.summarize());

                std::string snapshot = perf::PerfFileWriter::formatSnapshot(
                    "LIVE", fps, elapsed, result.frames_consumed, timings);
                perf::PerfFileWriter::writeToFile(
                    config.perf_file_path, snapshot);
            }
        }
        
        if (!continue_consume) {
            LOG4CPLUS_DEBUG(logger_, "Consumer requested stop");
            break;
        }
    }
    
    if (held_buffer) {
        pool->releaseFilled(held_buffer);
        held_buffer = nullptr;
    }
    
    result.stage_timings.push_back(consume_timer.summarize());
    // 收集伴随消费者（display/npu/opencv 等）的阶段计时
    auto companion_timings = consumer->collectStageTimings();
    result.stage_timings.insert(result.stage_timings.end(),
        companion_timings.begin(), companion_timings.end());
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
            config.consumer_type.save_raw.output_paths.empty()
                ? "" : config.consumer_type.save_raw.output_paths[0].c_str(),
            config.consumer_type.save_raw.max_frames_per_channel.empty()
                ? -1 : config.consumer_type.save_raw.max_frames_per_channel[0]);
    }
    if (config.consumer_type.save_encoded.enable) {
        LOG4CPLUS_INFO_FMT(logger, "  Save Encoded:    to %s", 
            config.consumer_type.save_encoded.output_path.c_str());
    }
    LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
}

} // namespace consumer
