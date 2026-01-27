#include "productionline/consumer/BufferConsumer.hpp"
#include "productionline/io/BufferWriter.hpp"
#include "productionline/io/BufferComparator.hpp"
#include "productionline/worker/FfmpegDecodeVideoFileWorker.hpp"
#include "productionline/worker/WorkerBase.hpp"
#include "display/LinuxFramebufferDevice.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "common/Logger.hpp"
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <numeric>
#include <cstring>
#include <set>
#include <climits>
#include <cmath>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace productionline {
namespace consumer {

// ============================================================================
// BufferConsumerService 实现
// ============================================================================

BufferConsumerService::BufferConsumerService()
    : is_open_(false)
    , consumer_(nullptr)
    , error_callback_(nullptr)
    , psnr_initialized_(false)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.BufferConsumerService")))
{
    // Stats 结构体的 std::atomic 成员已在定义时初始化
    // PSNR 统计的 std::atomic 成员也已初始化
}

void BufferConsumerService::reportError(ConsumerErrorCode code,
                                        const std::string& message,
                                        const std::string& location,
                                        int line,
                                        const std::map<std::string, std::string>& context) {
    ConsumerErrorInfo error;
    error.code = code;
    error.message = message;
    error.location = location;
    error.line = line;
    error.context = context;
    
    // 记录日志
    LOG4CPLUS_ERROR_FMT(logger_, "%s", error.toString().c_str());
    
    // 调用错误回调
    if (error_callback_) {
        error_callback_(error);
    }
}

ErrorCallback BufferConsumerService::wrapSimpleErrorCallback(SimpleErrorCallback simple_callback) {
    if (!simple_callback) {
        return nullptr;
    }
    
    return [simple_callback](const ConsumerErrorInfo& error) {
        simple_callback(error.toString());
    };
}

bool BufferConsumerService::open(const Config& config, 
                                 IBufferConsumer* consumer,
                                 SimpleErrorCallback simple_error_callback) {
    ErrorCallback error_callback = wrapSimpleErrorCallback(simple_error_callback);
    return open(config, consumer, error_callback);
}

BufferConsumerService::~BufferConsumerService() {
    if (is_open_) {
        close();
    }
}

bool BufferConsumerService::open(const Config& config, 
                                 IBufferConsumer* consumer,
                                 ErrorCallback error_callback) {
    if (is_open_) {
        reportError(ConsumerErrorCode::ERROR_ALREADY_OPEN,
                   "Service is already opened",
                   "BufferConsumerService::open");
        return false;
    }
    
    if (!consumer) {
        reportError(ConsumerErrorCode::ERROR_CONSUMER_NULL,
                   "Consumer is nullptr",
                   "BufferConsumerService::open");
        return false;
    }
    
    // 验证配置
    std::string validation_error = config.validate();
    if (!validation_error.empty()) {
        reportError(ConsumerErrorCode::ERROR_INVALID_CONFIG,
                   "Configuration validation failed: " + validation_error,
                   "BufferConsumerService::open");
        return false;
    }
    
    config_ = config;
    consumer_ = consumer;
    
    // 设置错误回调
    error_callback_ = error_callback;
    // 重置统计信息（手动重置每个 std::atomic 成员）
    stats_.total_consumed = 0;
    stats_.success_count = 0;
    stats_.failed_count = 0;
    stats_.skipped_count = 0;
    stats_.drained_count = 0;
    stats_.avg_fps = 0.0;
    stats_.ch0_consumed = 0;
    stats_.ch1_consumed = 0;
    stats_.ch0_skipped = 0;
    stats_.ch1_skipped = 0;
    
    // 1. 初始化生产线
    if (!initializeProducer(error_callback)) {
        return false;
    }
    
    // 2. 初始化 BufferPool
    if (!initializeBufferPool()) {
        producer_->stop();
        return false;
    }
    
    // 3. 初始化消费者（等待第一个 Buffer）
    if (config_.runtime.wait_first_buffer) {
        if (!initializeConsumer()) {
            producer_->stop();
            return false;
        }
    }
    
    // 4. 初始化 PSNR 对比（如果启用）
    if (config_.psnr.enable) {
        if (!initializePSNRCompare(error_callback)) {
            LOG4CPLUS_WARN(logger_, "Failed to initialize PSNR compare, continuing without it");
            // 不返回 false，允许继续运行（PSNR 对比是可选的）
        }
    }
    
    is_open_ = true;
    LOG4CPLUS_INFO(logger_, "Opened successfully");
    return true;
}

bool BufferConsumerService::initializeProducer(ErrorCallback error_callback) {
    // 创建生产线
    producer_ = std::make_unique<VideoProductionLine>(
        config_.production_line.loop, 
        config_.production_line.thread_count, 
        config_.production_line.enable_monitor);
    
    // 设置错误回调（转换为简单回调以兼容 VideoProductionLine）
    if (error_callback) {
        SimpleErrorCallback simple_callback = [error_callback](const std::string& msg) {
            ConsumerErrorInfo error;
            error.code = ConsumerErrorCode::ERROR_PRODUCER_START_FAILED;
            error.message = msg;
            error.location = "VideoProductionLine";
            error_callback(error);
        };
        producer_->setErrorCallback(simple_callback);
    }
    
    // 启动生产线
    if (!producer_->start(config_.worker_config)) {
        reportError(ConsumerErrorCode::ERROR_PRODUCER_START_FAILED,
                   "Failed to start production line",
                   "BufferConsumerService::initializeProducer");
        return false;
    }
    
    return true;
}

bool BufferConsumerService::initializeBufferPool() {
    uint64_t pool_id = producer_->getWorkingBufferPoolId();
    if (pool_id == 0) {
        reportError(ConsumerErrorCode::ERROR_BUFFER_POOL_NOT_FOUND,
                   "No working BufferPool ID available",
                   "BufferConsumerService::initializeBufferPool");
        return false;
    }
    
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    pool_sptr_ = pool_weak.lock();
    if (!pool_sptr_) {
        reportError(ConsumerErrorCode::ERROR_BUFFER_POOL_DESTROYED,
                   "BufferPool not found or destroyed",
                   "BufferConsumerService::initializeBufferPool",
                   0,
                   {{"pool_id", std::to_string(pool_id)}});
        return false;
    }
    
    LOG4CPLUS_INFO_FMT(logger_, "BufferPool: '%s' (ID: %lu)", 
                      pool_sptr_->getName().c_str(), pool_id);
    return true;
}

bool BufferConsumerService::initializeConsumer() {
    Buffer* first_buffer = pool_sptr_->acquireFilled(
        true, config_.runtime.first_buffer_timeout_ms);
    
    if (!first_buffer) {
        reportError(ConsumerErrorCode::ERROR_FIRST_BUFFER_TIMEOUT,
                   "Failed to get first buffer",
                   "BufferConsumerService::initializeConsumer",
                   0,
                   {{"timeout_ms", std::to_string(config_.runtime.first_buffer_timeout_ms)}});
        return false;
    }
    
    bool success = consumer_->initialize(first_buffer);
    pool_sptr_->releaseFilled(first_buffer);
    
    if (!success) {
        reportError(ConsumerErrorCode::ERROR_CONSUMER_INIT_FAILED,
                   "Failed to initialize consumer",
                   "BufferConsumerService::initializeConsumer");
        return false;
    }
    
    LOG4CPLUS_INFO(logger_, "Consumer initialized successfully");
    return true;
}

void BufferConsumerService::run(std::atomic<bool>& running_flag) {
    if (!is_open_ || !consumer_) {
        LOG4CPLUS_ERROR(logger_, "Not opened or consumer is nullptr");
        return;
    }
    
    consumeLoop(running_flag);
    
    if (config_.drain_remaining) {
        drainRemainingBuffers();
    }
    
    consumer_->cleanup();
}

bool BufferConsumerService::runOnce(
    const Config& config,
    IBufferConsumer* consumer,
    RunOptions options
) {
    if (!consumer) {
        reportError(ConsumerErrorCode::ERROR_CONSUMER_NULL,
                   "Consumer is nullptr",
                   "BufferConsumerService::runOnce");
        return false;
    }
    
    // 如果已经打开，先关闭，避免状态残留影响本次运行
    if (is_open_) {
        LOG4CPLUS_WARN(logger_, "runOnce: service is already open, closing previous session first");
        close();
    }
    
    // 处理错误回调：优先使用增强的错误回调，否则使用简单回调
    ErrorCallback error_callback = options.error_callback;
    if (!error_callback && options.simple_error_callback) {
        error_callback = wrapSimpleErrorCallback(options.simple_error_callback);
    }
    
    // 打开服务
    if (!open(config, consumer, error_callback)) {
        LOG4CPLUS_ERROR(logger_, "runOnce: failed to open BufferConsumerService");
        return false;
    }
    
    // 选择运行标志
    std::atomic<bool> local_running_flag(true);
    std::atomic<bool>* running_flag_ptr =
        options.running_flag ? options.running_flag : &local_running_flag;
    
    // 运行消费循环（阻塞，直到 running_flag 变为 false 或内部条件触发停止）
    run(*running_flag_ptr);
    
    // 运行结束后按需打印统计
    if (options.auto_print_stats) {
        printStats();
    }
    
    // 按需关闭服务
    if (options.auto_close) {
        close();
    }
    
    return true;
}

bool BufferConsumerService::execute(
    const Config& service_config,
    const ConsumerConfig& consumer_config,
    std::atomic<bool>* running_flag,
    std::function<void(const std::string&)> error_callback) {
    
    // 1. 通过 ConsumerFactory 创建策略实例（第二部分：策略选择配置部分）
    auto consumer = ConsumerFactory::create(consumer_config);
    if (!consumer) {
        LOG4CPLUS_ERROR(logger_, "execute: failed to create consumer strategy instance");
        return false;
    }
    
    // 2. 使用策略实例执行消费流程（第二部分：上下文，策略模式）
    RunOptions opts;
    opts.running_flag = running_flag;
    opts.error_callback = error_callback;
    opts.auto_print_stats = true;
    opts.auto_close = true;
    
    return runOnce(service_config, consumer.get(), opts);
}

bool BufferConsumerService::execute(const Config& service_config,
                                     const ConsumerConfig& consumer_config,
                                     std::atomic<bool>* running_flag,
                                     SimpleErrorCallback simple_error_callback) {
    ErrorCallback error_callback = wrapSimpleErrorCallback(simple_error_callback);
    return execute(service_config, consumer_config, running_flag, error_callback);
}

void BufferConsumerService::consumeLoop(std::atomic<bool>& running_flag) {
    int timeout_count = 0;
    
    while (running_flag.load()) {
        // ⭐ 检查是否达到最大帧数限制
        if (config_.runtime.max_frames > 0 && stats_.total_consumed.load() >= config_.runtime.max_frames) {
            LOG4CPLUS_INFO_FMT(logger_, "Reached max_frames limit (%d), stopping", config_.runtime.max_frames);
            // ⭐ 处理等待队列中剩余的buffer（达到帧数限制时，可能还有未处理的buffer）
            processPendingBuffers();
            break;
        }
        
        // 检查生产者状态
        if (!producer_->isRunning()) {
            LOG4CPLUS_INFO(logger_, "Producer stopped naturally");
            // ⭐ 处理等待队列中剩余的buffer（producer停止时，可能还有未处理的buffer）
            processPendingBuffers();
            break;
        }
        
        // 获取 Buffer
        Buffer* buffer = pool_sptr_->acquireFilled(
            true, config_.runtime.acquire_timeout_ms);
        
        if (buffer) {
            // 使用统一的辅助方法处理单个 Buffer（包含通道检查、PSNR 和消费逻辑）
            processSingleBuffer(buffer, /*from_drain=*/false);
            // 更新统计（FPS 依赖 producer 统计）
            stats_.avg_fps = producer_->getAverageFPS();
            timeout_count = 0;
        } else {
            timeout_count++;
            if (timeout_count >= config_.runtime.max_timeout_count) {
                LOG4CPLUS_INFO(logger_, "Consumer timeout, stopping");
                // ⭐ 处理等待队列中剩余的buffer（超时停止时，可能还有未处理的buffer）
                processPendingBuffers();
                break;
            }
        }
    }
}

void BufferConsumerService::drainRemainingBuffers() {
    // ⭐ 首先处理等待队列中剩余的buffer
    if (config_.psnr.enable_multi_channel) {
        processPendingBuffers();
    }
    
    Buffer* buffer = nullptr;
    
    while ((buffer = pool_sptr_->acquireFilled(false, 0)) != nullptr) {
        // 复用与主循环相同的单 Buffer 处理逻辑，确保行为一致
        processSingleBuffer(buffer, /*from_drain=*/true);
        // 如果 buffer 被放入等待队列，将在 processPendingBuffers 中统一处理
    }
    
    // ⭐ 再次处理等待队列（处理drain过程中新加入的buffer）
    if (config_.psnr.enable_multi_channel) {
        processPendingBuffers();
    }
    
    int drained = stats_.drained_count.load();
    if (drained > 0) {
        LOG4CPLUS_INFO_FMT(logger_, "Drained %d remaining buffers", drained);
    }
}

void BufferConsumerService::processSingleBuffer(Buffer* buffer, bool from_drain) {
    if (!buffer) {
        return;
    }
    
    int channel_id = buffer->getOutputChannel();
    
    // 检查是否应该消费该通道
    if (!consumer_->shouldConsumeChannel(channel_id)) {
        pool_sptr_->releaseFilled(buffer);
        stats_.skipped_count++;
        // 按通道统计跳过数量
        if (channel_id == 0) {
            stats_.ch0_skipped++;
        } else if (channel_id == 1) {
            stats_.ch1_skipped++;
        }
        return;
    }
    
    // 执行 PSNR 对比（如果启用）
    bool buffer_in_waiting_queue = false;
    if (psnr_initialized_ && config_.psnr.enable) {
        // ⭐ 多通道模式：performPSNRCompare 可能会将 buffer 加入等待队列
        // 如果 buffer 在等待队列中，会在所有通道到达后统一处理，这里不释放
        buffer_in_waiting_queue = performPSNRCompare(buffer);
    }
    
    // 如果 buffer 不在等待队列中，正常消费和释放
    if (!buffer_in_waiting_queue) {
        // 消费 Buffer
        if (consumer_->consume(buffer, channel_id)) {
            stats_.success_count++;
        } else {
            stats_.failed_count++;
        }
        
        pool_sptr_->releaseFilled(buffer);
        
        // 统计更新
        if (from_drain) {
            stats_.drained_count++;
        }
        stats_.total_consumed++;
        
        // 按通道统计消费数量
        if (channel_id == 0) {
            stats_.ch0_consumed++;
        } else if (channel_id == 1) {
            stats_.ch1_consumed++;
        }
    }
    // 如果 buffer 在等待队列中，会在 processPendingBuffers 中统一处理
}

bool BufferConsumerService::initializePSNRCompare(ErrorCallback error_callback) {
    LOG4CPLUS_INFO(logger_, "Initializing PSNR comparison (auto software decoder)...");
    
    // 1. 获取硬件解码器的实际输出格式和分辨率（包括后处理）
    Buffer* first_hw_buf = pool_sptr_->acquireFilled(true, 5000);
    if (!first_hw_buf) {
        LOG4CPLUS_ERROR(logger_, "Failed to get first hardware buffer for PSNR initialization");
        return false;
    }
    
    AVPixelFormat hw_format = AV_PIX_FMT_NONE;
    int hw_width = 0;
    int hw_height = 0;
    
    if (first_hw_buf->hasImageMetadata()) {
        hw_format = first_hw_buf->getImageFormat();
        hw_width = first_hw_buf->getImageWidth();
        hw_height = first_hw_buf->getImageHeight();
    } else {
        LOG4CPLUS_WARN(logger_, "Hardware buffer has no image metadata, using defaults");
        hw_format = AV_PIX_FMT_NV12;
        hw_width = 1920;
        hw_height = 1080;
    }
    
    pool_sptr_->releaseFilled(first_hw_buf);
    
    LOG4CPLUS_INFO_FMT(logger_, "  Hardware output: %s, %dx%d (after post-processing)",
                     av_get_pix_fmt_name(hw_format), hw_width, hw_height);
    
    // 2. 创建软件解码器配置（使用硬件解码器的输出分辨率）
    auto sw_workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(config_.worker_config.data_source.path)
                .setBufferCount(config_.worker_config.data_source.buffer_count)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(hw_width, hw_height)  // 匹配硬件输出分辨率
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useSoftware()
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    // 3. 创建并启动软件解码器
    sw_producer_ = std::make_unique<VideoProductionLine>(false, 1, false);
    
    if (error_callback) {
        sw_producer_->setErrorCallback(error_callback);
    }
    
    if (!sw_producer_->start(sw_workerConfig)) {
        LOG4CPLUS_ERROR(logger_, "Failed to start software producer for PSNR");
        return false;
    }
    
    uint64_t sw_pool_id = sw_producer_->getWorkingBufferPoolId();
    if (sw_pool_id == 0) {
        LOG4CPLUS_ERROR(logger_, "Software producer failed to create BufferPool");
        sw_producer_->stop();
        return false;
    }
    
    sw_pool_sptr_ = BufferPoolRegistry::getInstance().getPool(sw_pool_id).lock();
    if (!sw_pool_sptr_) {
        LOG4CPLUS_ERROR(logger_, "Failed to get software BufferPool");
        sw_producer_->stop();
        return false;
    }
    
    // 4. 初始化 BufferComparator（单通道或多通道模式）
    if (config_.psnr.enable_multi_channel) {
        // 多通道模式：为每个通道创建独立的 comparator
        LOG4CPLUS_INFO(logger_, "  Multi-channel PSNR mode: creating separate comparators for each channel");
        
        // 检测需要哪些通道（通过获取多个 buffer 来检测）
        std::set<int> detected_channels;
        int detect_attempts = 0;
        const int MAX_DETECT_ATTEMPTS = 50;
        
        while (detect_attempts < MAX_DETECT_ATTEMPTS && detected_channels.size() < 2) {
            Buffer* test_buf = pool_sptr_->acquireFilled(true, 100);
            if (test_buf) {
                int ch = test_buf->getOutputChannel();
                detected_channels.insert(ch);
                pool_sptr_->releaseFilled(test_buf);
            }
            detect_attempts++;
        }
        
        if (detected_channels.empty()) {
            LOG4CPLUS_WARN(logger_, "No channels detected, falling back to single-channel mode");
            detected_channels.insert(0);  // 默认使用 ch0
        }
        
        // 为每个检测到的通道创建 comparator
        for (int ch : detected_channels) {
            auto comparator = std::make_unique<io::BufferComparator>();
            
            io::CompareConfig compare_config;
            compare_config.strategy = io::CompareConfig::AUTO_LAYERED;
            compare_config.format_strategy = io::CompareConfig::AUTO;
            compare_config.quick_psnr_threshold = config_.psnr.quick_psnr_threshold;
            compare_config.quick_warn_threshold = config_.psnr.quick_warn_threshold;
            compare_config.enable_psnr = true;
            compare_config.enable_ssim = true;
            compare_config.ssim_threshold = config_.psnr.ssim_threshold;
            compare_config.ssim_warn_threshold = config_.psnr.ssim_warn_threshold;
            compare_config.enable_parallel = config_.psnr.enable_parallel;
            compare_config.use_perceptual_weighting = config_.psnr.use_perceptual_weighting;
            compare_config.verbose = false;
            compare_config.save_report = config_.psnr.save_report;
            
            // 设置通道特定的报告路径
            if (ch == 0) {
                compare_config.report_path = config_.psnr.report_path_ch0;
            } else if (ch == 1) {
                compare_config.report_path = config_.psnr.report_path_ch1;
            } else {
                compare_config.report_path = config_.psnr.report_path + "_ch" + std::to_string(ch) + ".txt";
            }
            
            if (!comparator->open(compare_config)) {
                LOG4CPLUS_ERROR_FMT(logger_, "Failed to open BufferComparator for channel %d", ch);
                // 清理已创建的 comparators
                psnr_comparators_.clear();
                sw_producer_->stop();
                sw_producer_.reset();
                sw_pool_sptr_.reset();
                return false;
            }
            
            psnr_comparators_[ch] = std::move(comparator);
            
            // 初始化通道统计
            psnr_compare_count_by_channel_[ch] = 0;
            psnr_pass_count_by_channel_[ch] = 0;
            psnr_warn_count_by_channel_[ch] = 0;
            psnr_fail_count_by_channel_[ch] = 0;
            
            LOG4CPLUS_INFO_FMT(logger_, "  ✅ Channel %d comparator initialized (report: %s)", 
                             ch, compare_config.report_path.c_str());
        }
        
        LOG4CPLUS_INFO_FMT(logger_, "  Multi-channel PSNR: %zu channel(s) configured", 
                         psnr_comparators_.size());
    } else {
        // 单通道模式：使用单个 comparator
        psnr_comparator_ = std::make_unique<io::BufferComparator>();
        
        io::CompareConfig compare_config;
        compare_config.strategy = io::CompareConfig::AUTO_LAYERED;
        compare_config.format_strategy = io::CompareConfig::AUTO;
        compare_config.quick_psnr_threshold = config_.psnr.quick_psnr_threshold;
        compare_config.quick_warn_threshold = config_.psnr.quick_warn_threshold;
        compare_config.enable_psnr = true;
        compare_config.enable_ssim = true;
        compare_config.ssim_threshold = config_.psnr.ssim_threshold;
        compare_config.ssim_warn_threshold = config_.psnr.ssim_warn_threshold;
        compare_config.enable_parallel = config_.psnr.enable_parallel;
        compare_config.use_perceptual_weighting = config_.psnr.use_perceptual_weighting;
        compare_config.verbose = false;
        compare_config.save_report = config_.psnr.save_report;
        compare_config.report_path = config_.psnr.report_path;
        
        if (!psnr_comparator_->open(compare_config)) {
            LOG4CPLUS_ERROR(logger_, "Failed to open BufferComparator");
            sw_producer_->stop();
            sw_producer_.reset();
            sw_pool_sptr_.reset();
            return false;
        }
        
        LOG4CPLUS_INFO(logger_, "  Single-channel PSNR mode");
    }
    
    psnr_initialized_ = true;
    LOG4CPLUS_INFO(logger_, "✅ PSNR comparison initialized successfully");
    LOG4CPLUS_INFO(logger_, "  Software decoder will be used as reference");
    LOG4CPLUS_INFO(logger_, "  PTS alignment: enabled");
    
    // 5. 验证解码器（如果启用）
    if (config_.psnr.enable_decoder_verification) {
        verifyDecoders();
    }
    
    return true;
}

io::BufferComparator* BufferConsumerService::getPSNRComparator(int channel_id) const {
    if (config_.psnr.enable_multi_channel) {
        // 多通道模式：根据 channel_id 返回对应的 comparator
        auto it = psnr_comparators_.find(channel_id);
        if (it != psnr_comparators_.end()) {
            return it->second.get();
        }
        return nullptr;  // 该通道未配置 comparator
    } else {
        // 单通道模式：返回统一的 comparator
        return psnr_comparator_.get();
    }
}

bool BufferConsumerService::performPSNRCompare(Buffer* hw_buffer) {
    if (!psnr_initialized_ || !hw_buffer || !sw_pool_sptr_) {
        return false;
    }
    
    // 获取硬件 buffer 的通道 ID
    int channel_id = hw_buffer->getOutputChannel();
    
    // 获取对应的 comparator
    io::BufferComparator* comparator = getPSNRComparator(channel_id);
    if (!comparator) {
        // 该通道未配置 comparator（多通道模式下可能某些通道未启用）
        return false;
    }
    
    // 获取硬件 buffer 的 PTS（从 AVFrame）
    int64_t hw_pts = AV_NOPTS_VALUE;
    AVFrame* hw_frame = hw_buffer->getAVFrame();
    if (hw_frame) {
        hw_pts = hw_frame->pts;
    }
    
    // ⭐ 多通道模式优化方案：等待同一PTS的所有通道buffer都到达后，一次性计算所有PSNR
    if (config_.psnr.enable_multi_channel && hw_pts != AV_NOPTS_VALUE) {
        std::lock_guard<std::mutex> lock(pending_hw_buffers_mutex_);
        
        // 将当前硬件buffer加入等待队列
        auto& pending = pending_hw_buffers_[hw_pts];
        pending.channel_buffers[channel_id] = hw_buffer;
        pending.arrived_channels.insert(channel_id);
        
        // 检查是否所有启用的通道都已到达
        size_t enabled_channels = psnr_comparators_.size();
        if (pending.arrived_channels.size() >= enabled_channels) {
            // 所有通道都已到达，可以一次性计算所有PSNR
            // 从等待队列中移除
            PendingHwBuffers ready_buffers = std::move(pending);
            pending_hw_buffers_.erase(hw_pts);
            
            // 释放锁，避免长时间持有
            lock.~lock_guard();
            
            // 获取软件buffer（通过PTS对齐）
            Buffer* sw_buffer = acquireSoftwareBufferByPTS(hw_pts);
            if (!sw_buffer) {
                LOG4CPLUS_WARN_FMT(logger_, "Failed to acquire software buffer for PTS=%ld, releasing hardware buffers", (long)hw_pts);
                // ⭐ 即使无法获取软件buffer，也要统计硬件buffer的消费（确保帧数统计正确）
                for (auto& pair : ready_buffers.channel_buffers) {
                    int ch_id = pair.first;
                    Buffer* ch_hw_buffer = pair.second;
                    
                    // ⭐ 检查是否应该消费该通道（避免统计不应该消费的通道）
                    if (!consumer_->shouldConsumeChannel(ch_id)) {
                        // 该通道不应该被消费，直接释放buffer并跳过
                        pool_sptr_->releaseFilled(ch_hw_buffer);
                        stats_.skipped_count++;
                        // 按通道统计跳过数量
                        if (ch_id == 0) {
                            stats_.ch0_skipped++;
                        } else if (ch_id == 1) {
                            stats_.ch1_skipped++;
                        }
                        continue;
                    }
                    
                    // 消费硬件buffer（即使没有PSNR对比）
                    if (consumer_->consume(ch_hw_buffer, ch_id)) {
                        stats_.success_count++;
                    } else {
                        stats_.failed_count++;
                    }
                    
                    // 释放硬件buffer
                    pool_sptr_->releaseFilled(ch_hw_buffer);
                    stats_.total_consumed++;
                    
                    // 按通道统计消费数量
                    if (ch_id == 0) {
                        stats_.ch0_consumed++;
                    } else if (ch_id == 1) {
                        stats_.ch1_consumed++;
                    }
                }
                return false;
            }
            
            // 同时计算所有通道的PSNR
            bool all_success = true;
            for (auto& pair : ready_buffers.channel_buffers) {
                int ch_id = pair.first;
                Buffer* ch_hw_buffer = pair.second;
                
                io::BufferComparator* ch_comparator = getPSNRComparator(ch_id);
                if (ch_comparator) {
                    // 执行PSNR对比（软件作为参考，硬件作为测试）
                    io::FrameCompareResult result = ch_comparator->compare(sw_buffer, ch_hw_buffer);
                    
                    // 统计结果
                    psnr_compare_count_by_channel_[ch_id]++;
                    if (result.passed) {
                        psnr_pass_count_by_channel_[ch_id]++;
                    } else if (result.level == io::FrameCompareResult::WARN) {
                        psnr_warn_count_by_channel_[ch_id]++;
                    } else {
                        psnr_fail_count_by_channel_[ch_id]++;
                    }
                    
                    // 全局统计
                    psnr_compare_count_++;
                    if (result.passed) {
                        psnr_pass_count_++;
                    } else if (result.level == io::FrameCompareResult::WARN) {
                        psnr_warn_count_++;
                    } else {
                        psnr_fail_count_++;
                    }
                    
                } else {
                    all_success = false;
                }
            }
            
            // 释放软件buffer（只释放一次）
            sw_pool_sptr_->releaseFilled(sw_buffer);
            
            // 消费和释放所有硬件buffer
            for (auto& pair : ready_buffers.channel_buffers) {
                int ch_id = pair.first;
                Buffer* ch_hw_buffer = pair.second;
                
                // ⭐ 检查是否应该消费该通道（避免统计不应该消费的通道）
                if (!consumer_->shouldConsumeChannel(ch_id)) {
                    // 该通道不应该被消费，直接释放buffer并跳过
                    pool_sptr_->releaseFilled(ch_hw_buffer);
                    stats_.skipped_count++;
                    // 按通道统计跳过数量
                    if (ch_id == 0) {
                        stats_.ch0_skipped++;
                    } else if (ch_id == 1) {
                        stats_.ch1_skipped++;
                    }
                    continue;
                }
                
                // 消费硬件buffer
                if (consumer_->consume(ch_hw_buffer, ch_id)) {
                    stats_.success_count++;
                } else {
                    stats_.failed_count++;
                }
                
                // 释放硬件buffer
                pool_sptr_->releaseFilled(ch_hw_buffer);
                stats_.total_consumed++;
                
                // 按通道统计消费数量
                if (ch_id == 0) {
                    stats_.ch0_consumed++;
                } else if (ch_id == 1) {
                    stats_.ch1_consumed++;
                }
            }
            
            
            return all_success;
        } else {
            // 还有通道未到达，等待（当前buffer保留在等待队列中，不释放）
            // 注意：这里不释放hw_buffer，它会被保留在等待队列中
            // 当所有通道都到达时，会在上面的分支中统一处理
            return true;  // 返回true表示已加入等待队列
        }
    }
    
    // 单通道模式：直接处理（使用旧的逻辑作为fallback）
    // 注意：多通道模式应该已经在上面处理了，这里只处理单通道模式
    if (!config_.psnr.enable_multi_channel || hw_pts == AV_NOPTS_VALUE) {
        // 获取软件buffer
        Buffer* sw_buffer = acquireSoftwareBufferByPTS(hw_pts);
        if (!sw_buffer) {
            if (config_.psnr.enable_multi_channel) {
                LOG4CPLUS_WARN_FMT(logger_, "Channel %d: Failed to acquire software buffer for PSNR comparison (hw_pts=%ld)", 
                                 channel_id, (long)hw_pts);
            }
            return false;
        }
        
        // 执行PSNR对比
        io::FrameCompareResult result = comparator->compare(sw_buffer, hw_buffer);
        
        // 释放软件buffer
        sw_pool_sptr_->releaseFilled(sw_buffer);
        
        // 统计结果
        if (config_.psnr.enable_multi_channel) {
            psnr_compare_count_by_channel_[channel_id]++;
            if (result.passed) {
                psnr_pass_count_by_channel_[channel_id]++;
            } else if (result.level == io::FrameCompareResult::WARN) {
                psnr_warn_count_by_channel_[channel_id]++;
            } else {
                psnr_fail_count_by_channel_[channel_id]++;
            }
        }
        
        psnr_compare_count_++;
        if (result.passed) {
            psnr_pass_count_++;
        } else if (result.level == io::FrameCompareResult::WARN) {
            psnr_warn_count_++;
        } else {
            psnr_fail_count_++;
        }
        
        return true;
    }
    
    // 不应该到达这里（多通道模式应该已经在上面处理了）
    return false;
}

Buffer* BufferConsumerService::acquireSoftwareBufferByPTS(int64_t hw_pts) {
    if (!sw_pool_sptr_ || hw_pts == AV_NOPTS_VALUE) {
        return nullptr;
    }
    
    Buffer* sw_buffer = nullptr;
    
    if (config_.psnr.enable_pts_alignment) {
        // PTS 对齐模式：尝试找到匹配的 PTS
        int attempts = 0;
        int64_t best_pts_diff = INT64_MAX;
        Buffer* best_buffer = nullptr;
        std::vector<Buffer*> candidates_to_release;
        
        while (attempts < config_.psnr.max_pts_match_attempts) {
            Buffer* candidate = sw_pool_sptr_->acquireFilled(true, config_.runtime.acquire_timeout_ms);
            if (!candidate) {
                break;
            }
            
            int64_t sw_pts = AV_NOPTS_VALUE;
            AVFrame* sw_frame = candidate->getAVFrame();
            if (sw_frame) {
                sw_pts = sw_frame->pts;
            }
            
            if (sw_pts == AV_NOPTS_VALUE) {
                // 无法比较PTS，直接使用这个buffer
                for (Buffer* b : candidates_to_release) {
                    sw_pool_sptr_->releaseFilled(b);
                }
                if (best_buffer) {
                    sw_pool_sptr_->releaseFilled(best_buffer);
                }
                sw_buffer = candidate;
                break;
            }
            
            if (sw_pts == hw_pts) {
                // PTS 完全匹配，使用这个buffer
                for (Buffer* b : candidates_to_release) {
                    sw_pool_sptr_->releaseFilled(b);
                }
                if (best_buffer) {
                    sw_pool_sptr_->releaseFilled(best_buffer);
                }
                sw_buffer = candidate;
                break;
            }
            
            // PTS 不匹配，记录最接近的buffer
            int64_t pts_diff = std::abs(sw_pts - hw_pts);
            if (pts_diff < best_pts_diff) {
                if (best_buffer) {
                    candidates_to_release.push_back(best_buffer);
                }
                best_buffer = candidate;
                best_pts_diff = pts_diff;
            } else {
                candidates_to_release.push_back(candidate);
            }
            
            attempts++;
        }
        
        // 如果找到了最接近的buffer，使用它
        if (!sw_buffer && best_buffer) {
            int64_t pts_tolerance = 450000;  // 5秒容差（90kHz时间基）
            if (best_pts_diff < pts_tolerance) {
                sw_buffer = best_buffer;
                for (Buffer* b : candidates_to_release) {
                    sw_pool_sptr_->releaseFilled(b);
                }
            } else {
                for (Buffer* b : candidates_to_release) {
                    sw_pool_sptr_->releaseFilled(b);
                }
                sw_pool_sptr_->releaseFilled(best_buffer);
            }
        } else if (!sw_buffer) {
            for (Buffer* b : candidates_to_release) {
                sw_pool_sptr_->releaseFilled(b);
            }
        }
    } else {
        // 非 PTS 对齐模式：直接获取下一个 buffer
        sw_buffer = sw_pool_sptr_->acquireFilled(true, config_.runtime.acquire_timeout_ms);
    }
    
    return sw_buffer;
}

void BufferConsumerService::processPendingBuffers() {
    // ⭐ 处理等待队列中剩余的buffer（当producer停止或超时时）
    if (!config_.psnr.enable_multi_channel) {
        return;  // 单通道模式不需要处理等待队列
    }
    
    std::lock_guard<std::mutex> lock(pending_hw_buffers_mutex_);
    
    if (pending_hw_buffers_.empty()) {
        return;  // 等待队列为空，无需处理
    }
    
    LOG4CPLUS_INFO_FMT(logger_, "Processing %zu pending buffer groups from waiting queue", pending_hw_buffers_.size());
    
    // 处理所有等待的buffer组
    for (auto it = pending_hw_buffers_.begin(); it != pending_hw_buffers_.end();) {
        int64_t hw_pts = it->first;
        PendingHwBuffers& pending = it->second;
        
        // 获取软件buffer（通过PTS对齐）
        Buffer* sw_buffer = acquireSoftwareBufferByPTS(hw_pts);
        
        if (sw_buffer) {
            // 处理所有已到达的通道
            for (auto& pair : pending.channel_buffers) {
                int ch_id = pair.first;
                Buffer* ch_hw_buffer = pair.second;
                
                io::BufferComparator* ch_comparator = getPSNRComparator(ch_id);
                if (ch_comparator) {
                    // 执行PSNR对比
                    io::FrameCompareResult result = ch_comparator->compare(sw_buffer, ch_hw_buffer);
                    
                    // 统计结果
                    psnr_compare_count_by_channel_[ch_id]++;
                    if (result.passed) {
                        psnr_pass_count_by_channel_[ch_id]++;
                    } else if (result.level == io::FrameCompareResult::WARN) {
                        psnr_warn_count_by_channel_[ch_id]++;
                    } else {
                        psnr_fail_count_by_channel_[ch_id]++;
                    }
                    
                    // 全局统计
                    psnr_compare_count_++;
                    if (result.passed) {
                        psnr_pass_count_++;
                    } else if (result.level == io::FrameCompareResult::WARN) {
                        psnr_warn_count_++;
                    } else {
                        psnr_fail_count_++;
                    }
                }
                
                // ⭐ 检查是否应该消费该通道（避免统计不应该消费的通道）
                if (!consumer_->shouldConsumeChannel(ch_id)) {
                    // 该通道不应该被消费，直接释放buffer并跳过
                    pool_sptr_->releaseFilled(ch_hw_buffer);
                    stats_.skipped_count++;
                    // 按通道统计跳过数量
                    if (ch_id == 0) {
                        stats_.ch0_skipped++;
                    } else if (ch_id == 1) {
                        stats_.ch1_skipped++;
                    }
                    continue;
                }
                
                // 消费硬件buffer
                if (consumer_->consume(ch_hw_buffer, ch_id)) {
                    stats_.success_count++;
                } else {
                    stats_.failed_count++;
                }
                
                pool_sptr_->releaseFilled(ch_hw_buffer);
                stats_.total_consumed++;
                
                // 按通道统计消费数量
                if (ch_id == 0) {
                    stats_.ch0_consumed++;
                } else if (ch_id == 1) {
                    stats_.ch1_consumed++;
                }
            }
            
            // 释放软件buffer
            sw_pool_sptr_->releaseFilled(sw_buffer);
        } else {
            // ⭐ 无法获取软件buffer，但也要统计硬件buffer的消费（确保帧数统计正确）
            LOG4CPLUS_WARN_FMT(logger_, "Failed to acquire software buffer for PTS=%ld, releasing hardware buffers", (long)hw_pts);
            for (auto& pair : pending.channel_buffers) {
                int ch_id = pair.first;
                Buffer* ch_hw_buffer = pair.second;
                
                // ⭐ 检查是否应该消费该通道（避免统计不应该消费的通道）
                if (!consumer_->shouldConsumeChannel(ch_id)) {
                    // 该通道不应该被消费，直接释放buffer并跳过
                    pool_sptr_->releaseFilled(ch_hw_buffer);
                    stats_.skipped_count++;
                    // 按通道统计跳过数量
                    if (ch_id == 0) {
                        stats_.ch0_skipped++;
                    } else if (ch_id == 1) {
                        stats_.ch1_skipped++;
                    }
                    continue;
                }
                
                // 消费硬件buffer（即使没有PSNR对比）
                if (consumer_->consume(ch_hw_buffer, ch_id)) {
                    stats_.success_count++;
                } else {
                    stats_.failed_count++;
                }
                
                // 释放硬件buffer
                pool_sptr_->releaseFilled(ch_hw_buffer);
                stats_.total_consumed++;
                
                // 按通道统计消费数量
                if (ch_id == 0) {
                    stats_.ch0_consumed++;
                } else if (ch_id == 1) {
                    stats_.ch1_consumed++;
                }
            }
        }
        
        // 移除已处理的buffer组
        it = pending_hw_buffers_.erase(it);
    }
    
    LOG4CPLUS_INFO(logger_, "All pending buffers processed");
}

void BufferConsumerService::verifyDecoders() {
    if (!producer_ || !sw_producer_) {
        LOG4CPLUS_WARN(logger_, "⚠️  Cannot verify decoders: producers not initialized");
        return;
    }
    
    LOG4CPLUS_INFO(logger_, "");
    LOG4CPLUS_INFO(logger_, "🔍 Decoder Verification:");
    
    // 获取 WorkerFacade
    auto hw_worker_facade = producer_->getWorkerFacade();
    auto sw_worker_facade = sw_producer_->getWorkerFacade();
    
    if (!hw_worker_facade || !sw_worker_facade) {
        LOG4CPLUS_WARN(logger_, "  ⚠️  Failed to get worker facades for codec verification");
        return;
    }
    
    // 通过 dynamic_cast 获取实际的 Worker 并调用 getCodecName()
    // 注意：FfmpegDecodeVideoFileWorker 和 WorkerBase 在全局命名空间
    auto* hw_worker = dynamic_cast<FfmpegDecodeVideoFileWorker*>(
        const_cast<WorkerBase*>(hw_worker_facade->getWorkerBase())
    );
    auto* sw_worker = dynamic_cast<FfmpegDecodeVideoFileWorker*>(
        const_cast<WorkerBase*>(sw_worker_facade->getWorkerBase())
    );
    
    if (!hw_worker || !sw_worker) {
        LOG4CPLUS_WARN(logger_, "  ⚠️  Failed to get worker instances for codec verification");
        LOG4CPLUS_WARN(logger_, "     (Workers may not be FfmpegDecodeVideoFileWorker type)");
        return;
    }
    
    const char* hw_codec = hw_worker->getCodecName();
    const char* sw_codec = sw_worker->getCodecName();
    
    LOG4CPLUS_INFO_FMT(logger_, "  Hardware decoder actual codec: %s", 
                      hw_codec ? hw_codec : "unknown");
    LOG4CPLUS_INFO_FMT(logger_, "  Software decoder actual codec: %s", 
                      sw_codec ? sw_codec : "unknown");
    
    // 检查两个解码器是否相同
    if (hw_codec && sw_codec && strcmp(hw_codec, sw_codec) == 0) {
        LOG4CPLUS_ERROR(logger_, "  ❌ ERROR: Both decoders are using the same codec!");
        LOG4CPLUS_ERROR_FMT(logger_, "    Hardware decoder: %s", hw_codec);
        LOG4CPLUS_ERROR_FMT(logger_, "    Software decoder: %s", sw_codec);
        LOG4CPLUS_ERROR(logger_, "    This will result in PSNR = 100dB (identical output)");
        
        if (config_.verbose_diagnosis) {
            LOG4CPLUS_ERROR(logger_, "    Possible causes:");
            LOG4CPLUS_ERROR(logger_, "      1. Hardware decoder initialization failed, fell back to software decoder");
            LOG4CPLUS_ERROR(logger_, "      2. Hardware decoder not available or not properly configured");
            LOG4CPLUS_ERROR(logger_, "      3. Video file has issues that prevent hardware decoding");
            LOG4CPLUS_ERROR(logger_, "         - Missing SPS/PPS or other critical metadata");
            LOG4CPLUS_ERROR(logger_, "         - Stream corruption (Frame Num gaps)");
            LOG4CPLUS_ERROR(logger_, "         - Format incompatibility with hardware decoder");
            
            // 检查是否是 RTSP 流
            if (config_.worker_config.worker_type == WorkerType::FFMPEG_RTSP) {
                LOG4CPLUS_ERROR(logger_, "    💡 RTSP-specific diagnosis:");
                LOG4CPLUS_ERROR(logger_, "       - Check if recorded MP4 has low frame rate or packet ratio warnings");
                LOG4CPLUS_ERROR(logger_, "       - Try using a different RTSP stream or recording duration");
            }
        }
        
        LOG4CPLUS_ERROR(logger_, "    ⚠️  Test will continue, but PSNR results will be invalid!");
    } else {
        LOG4CPLUS_INFO(logger_, "  ✅ Decoders are using different codecs (expected)");
        
        // 检查硬件解码器是否回退到软件解码器
        if (hw_codec) {
            // 检查配置的解码器名称（从 worker_config 中获取）
            std::string expected_decoder = "";
            if (config_.worker_config.decoder.name.has_value()) {
                expected_decoder = config_.worker_config.decoder.name.value();
            }
            
            // 如果配置了硬件解码器（包含 "taco"），但实际 codec 不包含 "taco"
            if (!expected_decoder.empty() && 
                (expected_decoder.find("taco") != std::string::npos || 
                 expected_decoder.find("_taco") != std::string::npos) &&
                strstr(hw_codec, "taco") == nullptr) {
                LOG4CPLUS_WARN(logger_, "  ⚠️  WARNING: Hardware decoder was configured but actual codec does not contain 'taco'!");
                LOG4CPLUS_WARN_FMT(logger_, "    Expected: %s, Actual: %s", 
                                  expected_decoder.c_str(), hw_codec);
                LOG4CPLUS_WARN(logger_, "    Hardware decoder may have failed to initialize and fell back to software decoder");
                
                if (config_.psnr.verbose_diagnosis) {
                    LOG4CPLUS_WARN(logger_, "    💡 This is likely due to video file format issues:");
                    LOG4CPLUS_WARN(logger_, "       - Missing or corrupted SPS/PPS in the file");
                    LOG4CPLUS_WARN(logger_, "       - Stream corruption that prevents hardware decoder initialization");
                    LOG4CPLUS_WARN(logger_, "       - Format incompatibility (hardware decoder requires specific structure)");
                    
                    if (config_.worker_config.worker_type == WorkerType::FFMPEG_RTSP) {
                        LOG4CPLUS_WARN(logger_, "    💡 RTSP-specific diagnosis:");
                        LOG4CPLUS_WARN(logger_, "       - Check if recorded MP4 has format issues");
                        LOG4CPLUS_WARN(logger_, "       - Try using a different RTSP stream or recording duration");
                    }
                }
            } else {
                LOG4CPLUS_INFO(logger_, "  ✅ Hardware decoder is using expected codec");
            }
        }
    }
    
    // 输出 BufferPool 信息
    if (pool_sptr_ && sw_pool_sptr_) {
        LOG4CPLUS_INFO(logger_, "");
        LOG4CPLUS_INFO_FMT(logger_, "  Hardware BufferPool: '%s' (ID: %lu)", 
                          pool_sptr_->getName().c_str(), 
                          pool_sptr_->getRegistryId());
        LOG4CPLUS_INFO_FMT(logger_, "  Software BufferPool: '%s' (ID: %lu)", 
                          sw_pool_sptr_->getName().c_str(), 
                          sw_pool_sptr_->getRegistryId());
    }
}

void BufferConsumerService::close() {
    if (!is_open_) {
        return;
    }
    
    // ⭐ 清理等待队列（多通道模式）
    {
        std::lock_guard<std::mutex> lock(pending_hw_buffers_mutex_);
        for (auto& pair : pending_hw_buffers_) {
            // 释放所有等待的硬件buffer
            for (auto& ch_pair : pair.second.channel_buffers) {
                if (ch_pair.second && pool_sptr_) {
                    pool_sptr_->releaseFilled(ch_pair.second);
                }
            }
        }
        pending_hw_buffers_.clear();
    }
    
    // 关闭 PSNR 对比器
    // 关闭单通道 comparator
    if (psnr_comparator_) {
        psnr_comparator_->close();
        psnr_comparator_.reset();
    }
    
    // 关闭多通道 comparators
    for (auto& pair : psnr_comparators_) {
        if (pair.second) {
            pair.second->close();
        }
    }
    psnr_comparators_.clear();
    
    // 停止软件解码器
    if (sw_producer_) {
        sw_producer_->stop();
        sw_producer_.reset();
    }
    
    sw_pool_sptr_.reset();
    
    // 停止硬件解码器
    if (producer_) {
        producer_->stop();
    }
    
    is_open_ = false;
    psnr_initialized_ = false;
    LOG4CPLUS_INFO(logger_, "Closed");
}

void BufferConsumerService::printStats() const {
    LOG4CPLUS_INFO(logger_, "╔═══════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO(logger_, "║  BufferConsumerService Summary                       ║");
    LOG4CPLUS_INFO(logger_, "╚═══════════════════════════════════════════════════════╝");
    LOG4CPLUS_INFO_FMT(logger_, "  Total consumed: %d", stats_.total_consumed.load());
    LOG4CPLUS_INFO_FMT(logger_, "  Success: %d ✅", stats_.success_count.load());
    LOG4CPLUS_INFO_FMT(logger_, "  Failed: %d ❌", stats_.failed_count.load());
    LOG4CPLUS_INFO_FMT(logger_, "  Skipped: %d ⏭️", stats_.skipped_count.load());
    LOG4CPLUS_INFO_FMT(logger_, "  Drained: %d 📦", stats_.drained_count.load());
    LOG4CPLUS_INFO_FMT(logger_, "  Average FPS: %.2f", stats_.avg_fps.load());
    
    // 按通道统计（如果有数据）
    int ch0_consumed = stats_.ch0_consumed.load();
    int ch1_consumed = stats_.ch1_consumed.load();
    int ch0_skipped = stats_.ch0_skipped.load();
    int ch1_skipped = stats_.ch1_skipped.load();
    
    if (ch0_consumed > 0 || ch1_consumed > 0 || ch0_skipped > 0 || ch1_skipped > 0) {
        LOG4CPLUS_INFO(logger_, "");
        LOG4CPLUS_INFO(logger_, "  Channel Statistics:");
        if (ch0_consumed > 0 || ch0_skipped > 0) {
            LOG4CPLUS_INFO_FMT(logger_, "    ch0: consumed=%d, skipped=%d", 
                             ch0_consumed, ch0_skipped);
        }
        if (ch1_consumed > 0 || ch1_skipped > 0) {
            LOG4CPLUS_INFO_FMT(logger_, "    ch1: consumed=%d, skipped=%d", 
                             ch1_consumed, ch1_skipped);
        }
    }
    
    if (consumer_) {
        std::string consumer_stats = consumer_->getStats();
        if (!consumer_stats.empty()) {
            LOG4CPLUS_INFO(logger_, "");
            LOG4CPLUS_INFO(logger_, "  Consumer Stats:");
            LOG4CPLUS_INFO_FMT(logger_, "    %s", consumer_stats.c_str());
        }
    }
    
    // PSNR 对比统计（如果启用）
    if (psnr_initialized_) {
        LOG4CPLUS_INFO(logger_, "");
        LOG4CPLUS_INFO(logger_, "═══════════════════════════════════════════════════════");
        LOG4CPLUS_INFO(logger_, "  Decoder Comparison Results");
        LOG4CPLUS_INFO(logger_, "═══════════════════════════════════════════════════════");
        
        if (config_.psnr.enable_multi_channel && !psnr_comparators_.empty()) {
            // 多通道模式：分别输出每个通道的统计
            for (const auto& pair : psnr_comparators_) {
                int ch = pair.first;
                const auto& comparator = pair.second;
                
                LOG4CPLUS_INFO(logger_, "");
                LOG4CPLUS_INFO_FMT(logger_, "  ─── Channel %d (PP%d) ───", ch, ch);
                
                // 输出该通道的详细统计（printSummary会根据格式自动输出YUV或RGB的通道平均值）
                comparator->printSummary();
                
                // 输出该通道的报告路径（如果保存了报告）
                if (config_.psnr.save_report) {
                    std::string report_path;
                    if (ch == 0) {
                        report_path = config_.psnr.report_path_ch0;
                    } else if (ch == 1) {
                        report_path = config_.psnr.report_path_ch1;
                    } else {
                        report_path = config_.psnr.report_path + "_ch" + std::to_string(ch) + ".txt";
                    }
                    if (!report_path.empty()) {
                        LOG4CPLUS_INFO_FMT(logger_, "📄 Channel %d report saved to: %s", 
                                        ch, report_path.c_str());
                    }
                }
            }
            
            // 输出汇总统计
            LOG4CPLUS_INFO(logger_, "");
            LOG4CPLUS_INFO(logger_, "  ─── Summary (All Channels) ───");
            LOG4CPLUS_INFO_FMT(logger_, "    Total comparisons: %d", 
                             psnr_compare_count_.load());
            LOG4CPLUS_INFO_FMT(logger_, "    Passed: %d ✅", 
                             psnr_pass_count_.load());
            LOG4CPLUS_INFO_FMT(logger_, "    Warned: %d ⚠️", 
                             psnr_warn_count_.load());
            LOG4CPLUS_INFO_FMT(logger_, "    Failed: %d ❌", 
                             psnr_fail_count_.load());
        } else if (psnr_comparator_) {
            // 单通道模式：输出统一统计（printSummary会根据格式自动输出YUV或RGB的通道平均值）
            psnr_comparator_->printSummary();
            
            // 输出报告路径（如果保存了报告）
            if (config_.psnr.save_report && !config_.psnr.report_path.empty()) {
                LOG4CPLUS_INFO_FMT(logger_, "📄 Detailed comparison report saved to: %s", 
                                  config_.psnr.report_path.c_str());
            }
        }
        
        LOG4CPLUS_INFO(logger_, "═══════════════════════════════════════════════════════");
    }
    
    LOG4CPLUS_INFO(logger_, "╚═══════════════════════════════════════════════════════╝");
}

// ============================================================================
// DisplayConsumer 实现
// ============================================================================

// ============================================================================
// 第三部分：策略实现部分（已移至独立的策略库文件）
// ============================================================================
// 策略实现代码已移至 BufferConsumerStrategies.cpp

// ============================================================================
// DualBufferCompareService 实现
// ============================================================================

DualBufferCompareService::DualBufferCompareService()
    : is_open_(false)
    , comparator_(nullptr)
    , reference_pool_(nullptr)
    , test_pool_(nullptr)
    , reference_producer_(nullptr)
    , test_producer_(nullptr)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Consumer.DualBufferCompare")))
{
}

DualBufferCompareService::~DualBufferCompareService() {
    close();
}

bool DualBufferCompareService::open(const Config& config) {
    if (is_open_) {
        LOG4CPLUS_WARN(logger_, "Service already open");
        return false;
    }
    
    if (!comparator_) {
        LOG4CPLUS_ERROR(logger_, "Comparator is nullptr");
        return false;
    }
    
    if (!reference_pool_ || !test_pool_) {
        LOG4CPLUS_ERROR(logger_, "Reference pool or test pool is nullptr");
        return false;
    }
    
    // 验证裁剪参数
    if (config.enable_crop) {
        if (config.crop_w <= 0 || config.crop_h <= 0) {
            LOG4CPLUS_ERROR(logger_, "Crop enabled but crop_w or crop_h is invalid");
            return false;
        }
    }
    
    config_ = config;
    is_open_ = true;
    
    std::string crop_info = config.enable_crop ? 
        (std::to_string(config.crop_w) + "x" + std::to_string(config.crop_h) + 
         "@(" + std::to_string(config.crop_x) + "," + std::to_string(config.crop_y) + ")") :
        "disabled";
    
    LOG4CPLUS_INFO_FMT(logger_, "DualBufferCompareService opened: max_frames=%d, pts_alignment=%s, crop=%s",
                      config_.max_frames, 
                      config_.enable_pts_alignment ? "enabled" : "disabled",
                      crop_info.c_str());
    
    return true;
}

void DualBufferCompareService::close() {
    if (!is_open_) {
        return;
    }
    
    if (config_.drain_remaining) {
        drainRemainingBuffers();
    }
    
    is_open_ = false;
    LOG4CPLUS_INFO(logger_, "DualBufferCompareService closed");
}

int64_t DualBufferCompareService::getBufferPTS(Buffer* buffer) {
    if (!buffer) {
        return AV_NOPTS_VALUE;
    }
    
    AVFrame* avframe = buffer->getAVFrame();
    if (!avframe) {
        return AV_NOPTS_VALUE;
    }
    
    if (avframe->pts != AV_NOPTS_VALUE) {
        return avframe->pts;
    }
    
    return avframe->best_effort_timestamp;
}

bool DualBufferCompareService::acquireAlignedBuffers(Buffer*& ref_buf, Buffer*& test_buf, int acquire_timeout) {
    ref_buf = nullptr;
    test_buf = nullptr;
    
    if (!config_.enable_pts_alignment) {
        // 不使用PTS对齐，直接顺序获取
        ref_buf = reference_pool_->acquireFilled(true, acquire_timeout);
        test_buf = test_pool_->acquireFilled(true, acquire_timeout);
        return (ref_buf != nullptr && test_buf != nullptr);
    }
    
    // 使用PTS对齐
    int pts_match_attempts = 0;
    const int MAX_PTS_MATCH_ATTEMPTS = config_.max_pts_match_attempts;
    
    while (pts_match_attempts < MAX_PTS_MATCH_ATTEMPTS) {
        // 如果没有buffer，获取新的buffer
        if (!ref_buf) {
            ref_buf = reference_pool_->acquireFilled(true, acquire_timeout);
        }
        if (!test_buf) {
            test_buf = test_pool_->acquireFilled(true, acquire_timeout);
        }
        
        // 如果任一pool没有数据，返回false
        if (!ref_buf || !test_buf) {
            if (ref_buf) {
                reference_pool_->releaseFilled(ref_buf);
                ref_buf = nullptr;
            }
            if (test_buf) {
                test_pool_->releaseFilled(test_buf);
                test_buf = nullptr;
            }
            return false;
        }
        
        // 获取两个buffer的PTS
        int64_t ref_pts = getBufferPTS(ref_buf);
        int64_t test_pts = getBufferPTS(test_buf);
        
        // 如果PTS都有效且匹配，找到对齐的帧
        if (ref_pts != AV_NOPTS_VALUE && test_pts != AV_NOPTS_VALUE) {
            if (ref_pts == test_pts) {
                // PTS匹配，可以进行比较
                if (pts_match_attempts > 0 && config_.verbose) {
                }
                return true;
            } else {
                // PTS不匹配，释放PTS较小的buffer，获取下一个
                if (ref_pts < test_pts) {
                    // 参考PTS较小，释放参考buffer获取下一个
                    reference_pool_->releaseFilled(ref_buf);
                    ref_buf = nullptr;
                    pts_match_attempts++;
                    if (pts_match_attempts <= 3 && config_.verbose) {
                    }
                } else {
                    // 测试PTS较小，释放测试buffer获取下一个
                    test_pool_->releaseFilled(test_buf);
                    test_buf = nullptr;
                    pts_match_attempts++;
                    if (pts_match_attempts <= 3 && config_.verbose) {
                    }
                }
                continue;  // 继续尝试匹配
            }
        } else {
            // PTS无效，无法按PTS对齐，直接使用（向后兼容）
            if (pts_match_attempts == 0 && config_.verbose) {
                LOG4CPLUS_WARN_FMT(logger_, "PTS unavailable (Ref=%s, Test=%s), using sequential matching",
                                   ref_pts != AV_NOPTS_VALUE ? std::to_string(ref_pts).c_str() : "NOPTS",
                                   test_pts != AV_NOPTS_VALUE ? std::to_string(test_pts).c_str() : "NOPTS");
            }
            return true;  // 使用这两个buffer（向后兼容）
        }
    }
    
    // 如果尝试多次仍未找到匹配，记录警告但继续处理
    if (pts_match_attempts >= MAX_PTS_MATCH_ATTEMPTS) {
        LOG4CPLUS_WARN_FMT(logger_, "Failed to match PTS after %d attempts, using current buffers",
                           pts_match_attempts);
        stats_.pts_alignment_failures++;
        return true;  // 仍然返回true，使用当前buffer
    }
    
    return false;
}

void DualBufferCompareService::run(std::atomic<bool>& running_flag) {
    if (!is_open_) {
        LOG4CPLUS_ERROR(logger_, "Service not open");
        return;
    }
    
    int frame_count = 0;
    int timeout_count = 0;
    
    // 动态计算超时时间（根据BufferPool状态）
    auto calculateTimeout = [this](int& ref_filled, int& ref_free, int& test_filled, int& test_free) -> int {
        ref_filled = reference_pool_->getFilledCount();
        ref_free = reference_pool_->getFreeCount();
        test_filled = test_pool_->getFilledCount();
        test_free = test_pool_->getFreeCount();
        
        if (ref_filled >= 5 && test_filled >= 5) {
            return 100;  // 两个都有足够数据，快速响应
        } else if (ref_filled > 0 && test_filled > 0) {
            return 500;  // 两个都有数据但不多，中等等待
        } else {
            // 如果解码器仍在运行但 filled=0 且 free=0，说明所有缓冲区都被占用
            if ((ref_filled == 0 && ref_free == 0 && reference_producer_ && reference_producer_->isRunning()) ||
                (test_filled == 0 && test_free == 0 && test_producer_ && test_producer_->isRunning())) {
                return 5000;  // 等待5秒，给解码器时间处理并提交帧
            } else {
                return 2000;  // 至少一个没有数据，等待更长时间
            }
        }
    };
    
    while (running_flag.load() && (config_.max_frames < 0 || frame_count < config_.max_frames)) {
        // 检查解码器是否已完成
        int ref_filled, ref_free, test_filled, test_free;
        int acquire_timeout = calculateTimeout(ref_filled, ref_free, test_filled, test_free);
        
        if (reference_producer_ && test_producer_ &&
            !reference_producer_->isRunning() && !test_producer_->isRunning()) {
            if (ref_filled == 0 && test_filled == 0) {
                LOG4CPLUS_INFO_FMT(logger_, "Decoders finished naturally (all frames processed), total=%d",
                                  frame_count);
                break;
            }
        }
        
        // 获取PTS对齐的Buffer对
        Buffer* ref_buf = nullptr;
        Buffer* test_buf = nullptr;
        
        if (!acquireAlignedBuffers(ref_buf, test_buf, acquire_timeout)) {
            timeout_count++;
            
            if (timeout_count == 1) {
                LOG4CPLUS_WARN_FMT(logger_, "First timeout: Ref filled=%d (free=%d), Test filled=%d (free=%d)",
                                  ref_filled, ref_free, test_filled, test_free);
            }
            
            if (timeout_count >= config_.max_timeout_count) {
                LOG4CPLUS_INFO_FMT(logger_, "Max timeout reached (%d), stopping comparison",
                                   config_.max_timeout_count);
                break;
            }
            
            // 检查是否自然结束
            if (reference_producer_ && test_producer_ &&
                !reference_producer_->isRunning() && !test_producer_->isRunning()) {
                LOG4CPLUS_INFO_FMT(logger_, "Decoders finished naturally, total=%d, timeouts=%d",
                                  frame_count, timeout_count);
                break;
            }
            
            continue;
        }
        
        timeout_count = 0;
        
        // 执行对比（如果启用裁剪，先对Buffer进行裁剪）
        if (ref_buf && test_buf && comparator_) {
            Buffer* ref_buf_to_compare = ref_buf;
            Buffer* test_buf_to_compare = test_buf;
            std::unique_ptr<AVFrame, void(*)(AVFrame*)> ref_cropped_frame(nullptr, [](AVFrame* f) { if (f) av_frame_free(&f); });
            std::unique_ptr<AVFrame, void(*)(AVFrame*)> test_cropped_frame(nullptr, [](AVFrame* f) { if (f) av_frame_free(&f); });
            
            // 如果启用裁剪，创建裁剪后的Buffer
            if (config_.enable_crop && config_.crop_w > 0 && config_.crop_h > 0) {
                AVFrame* ref_cropped = cropBufferRegion(ref_buf, config_.crop_x, config_.crop_y, config_.crop_w, config_.crop_h);
                AVFrame* test_cropped = cropBufferRegion(test_buf, config_.crop_x, config_.crop_y, config_.crop_w, config_.crop_h);
                
                if (ref_cropped && test_cropped) {
                    // 创建临时Buffer用于对比（简化实现：使用原始Buffer，但记录裁剪信息）
                    // 实际项目中，可以扩展BufferComparator以支持裁剪区域参数
                    // 或者创建临时Buffer包装裁剪后的AVFrame
                    // 这里简化：使用原始Buffer对比，但记录这是裁剪后的对比需求
                    ref_cropped_frame.reset(ref_cropped);
                    test_cropped_frame.reset(test_cropped);
                    
                    // 注意：由于Buffer的创建比较复杂，这里暂时使用原始Buffer进行对比
                    // 实际项目中，可以扩展BufferComparator以支持裁剪区域参数
                    // 或者创建临时Buffer包装裁剪后的AVFrame
                    // 这里简化：使用原始Buffer对比，但记录这是裁剪后的对比需求
                } else {
                    if (config_.verbose) {
                        LOG4CPLUS_WARN_FMT(logger_, "Failed to crop buffers, using original buffers");
                    }
                }
            }
            
            auto result = comparator_->compare(ref_buf_to_compare, test_buf_to_compare);
            
            // 记录统计
            stats_.total_compared++;
            if (result.passed) {
                stats_.passed_count++;
            } else if (result.level == io::FrameCompareResult::WARN) {
                stats_.warned_count++;
            } else {
                stats_.failed_count++;
            }
            
            // 记录PSNR/SSIM值
            if (result.psnr_y > 0.0) {
                stats_.psnr_y_values.push_back(result.psnr_y);
            }
            if (result.psnr_avg > 0.0) {
                stats_.psnr_avg_values.push_back(result.psnr_avg);
            }
            if (result.ssim_y > 0.0) {
                stats_.ssim_y_values.push_back(result.ssim_y);
            }
            if (result.ssim_avg > 0.0) {
                stats_.ssim_avg_values.push_back(result.ssim_avg);
            }
            
            // 检查帧类型是否一致（用于诊断）
            AVFrame* ref_avframe = ref_buf->getAVFrame();
            AVFrame* test_avframe = test_buf->getAVFrame();
            if (ref_avframe && test_avframe && ref_avframe->pict_type != test_avframe->pict_type) {
                stats_.frame_type_mismatches++;
            }
        }
        
        // 释放Buffer
        if (ref_buf) {
            reference_pool_->releaseFilled(ref_buf);
        }
        if (test_buf) {
            test_pool_->releaseFilled(test_buf);
        }
        
        frame_count++;
        stats_.timeout_count = timeout_count;
        
        // 每50帧打印一次进度
        if (frame_count % 50 == 0 && config_.verbose) {
            double avg_psnr_y = stats_.psnr_y_values.empty() ? 0.0 :
                std::accumulate(stats_.psnr_y_values.begin(), stats_.psnr_y_values.end(), 0.0) /
                stats_.psnr_y_values.size();
            LOG4CPLUS_INFO_FMT(logger_, "Progress: %d frames | PSNR-Y=%.2f dB | Passed: %d, Warned: %d, Failed: %d",
                              frame_count, avg_psnr_y,
                              stats_.passed_count, stats_.warned_count, stats_.failed_count);
        }
    }
}

AVFrame* DualBufferCompareService::cropBufferRegion(Buffer* buffer, int crop_x, int crop_y, int crop_w, int crop_h) {
    if (!buffer || !buffer->hasImageMetadata()) {
        return nullptr;
    }
    
    int src_width = buffer->getImageWidth();
    int src_height = buffer->getImageHeight();
    AVPixelFormat src_format = buffer->getImageFormat();
    
    // 验证裁剪参数
    if (crop_x < 0 || crop_y < 0 || crop_w <= 0 || crop_h <= 0 ||
        crop_x + crop_w > src_width || crop_y + crop_h > src_height) {
        LOG4CPLUS_ERROR_FMT(logger_, "Invalid crop parameters: x=%d, y=%d, w=%d, h=%d (src: %dx%d)",
                          crop_x, crop_y, crop_w, crop_h, src_width, src_height);
        return nullptr;
    }
    
    // 创建目标AVFrame
    AVFrame* dst_frame = av_frame_alloc();
    if (!dst_frame) {
        return nullptr;
    }
    
    dst_frame->format = src_format;
    dst_frame->width = crop_w;
    dst_frame->height = crop_h;
    
    if (av_frame_get_buffer(dst_frame, 32) < 0) {
        av_frame_free(&dst_frame);
        return nullptr;
    }
    
    // 获取源AVFrame
    AVFrame* src_frame = buffer->getAVFrame();
    if (!src_frame) {
        av_frame_free(&dst_frame);
        return nullptr;
    }
    
    // 使用sws_scale进行裁剪
    SwsContext* sws_ctx = sws_getContext(
        crop_w, crop_h, src_format,  // 源尺寸和格式（裁剪区域）
        crop_w, crop_h, src_format,  // 目标尺寸和格式（相同）
        SWS_POINT,                    // 最近邻采样（裁剪不需要插值）
        nullptr, nullptr, nullptr
    );
    
    if (!sws_ctx) {
        av_frame_free(&dst_frame);
        return nullptr;
    }
    
    // 计算源区域的起始指针和stride
    const uint8_t* src_data[4] = {nullptr};
    int src_linesize[4] = {0};
    const int* src_linesizes = buffer->getImageLinesize();
    
    if (!src_linesizes) {
        sws_freeContext(sws_ctx);
        av_frame_free(&dst_frame);
        return nullptr;
    }
    
    // 获取源plane数据
    for (int i = 0; i < 4; i++) {
        uint8_t* plane_data = buffer->getImagePlaneData(i);
        if (plane_data) {
            // 计算裁剪后的起始位置
            int plane_x = crop_x;
            int plane_y = crop_y;
            
            // 对于UV平面，需要按比例缩放坐标
            if (i > 0 && (src_format == AV_PIX_FMT_NV12 || src_format == AV_PIX_FMT_NV21 || 
                          src_format == AV_PIX_FMT_YUV420P || src_format == AV_PIX_FMT_YUV422P)) {
                plane_x = crop_x / (i == 1 ? 2 : 1);  // U/V平面宽度减半
                plane_y = crop_y / (src_format == AV_PIX_FMT_YUV420P ? 2 : 1);  // YUV420P高度减半
            }
            
            src_data[i] = plane_data + plane_y * src_linesizes[i] + plane_x * 
                         (src_format == AV_PIX_FMT_NV12 || src_format == AV_PIX_FMT_NV21 ? 1 : 
                          (i == 0 ? 1 : (src_format == AV_PIX_FMT_YUV420P ? 1 : 2)));
            src_linesize[i] = src_linesizes[i];
        }
    }
    
    // 执行裁剪（实际上是复制指定区域）
    sws_scale(sws_ctx,
              src_data, src_linesize, 0, crop_h,
              dst_frame->data, dst_frame->linesize);
    
    sws_freeContext(sws_ctx);
    
    // 复制PTS等元数据
    dst_frame->pts = src_frame->pts;
    dst_frame->pkt_dts = src_frame->pkt_dts;
    dst_frame->pict_type = src_frame->pict_type;
    
    return dst_frame;
}

Buffer* DualBufferCompareService::createCroppedBuffer(Buffer* src_buffer, int crop_x, int crop_y, int crop_w, int crop_h) {
    // 简化实现：由于Buffer的创建比较复杂，这里返回nullptr
    // 实际项目中，可以扩展BufferComparator以支持裁剪区域参数
    // 或者创建临时Buffer包装裁剪后的AVFrame
    (void)src_buffer;
    (void)crop_x;
    (void)crop_y;
    (void)crop_w;
    (void)crop_h;
    return nullptr;
}

void DualBufferCompareService::drainRemainingBuffers() {
    if (!reference_pool_ || !test_pool_) {
        return;
    }
    
    int ref_drained = 0;
    Buffer* remaining = nullptr;
    while ((remaining = reference_pool_->acquireFilled(false, 0)) != nullptr) {
        reference_pool_->releaseFilled(remaining);
        ref_drained++;
    }
    
    int test_drained = 0;
    while ((remaining = test_pool_->acquireFilled(false, 0)) != nullptr) {
        test_pool_->releaseFilled(remaining);
        test_drained++;
    }
    
    if (ref_drained > 0 || test_drained > 0) {
        LOG4CPLUS_INFO_FMT(logger_, "Drained remaining buffers: ref=%d, test=%d",
                          ref_drained, test_drained);
    }
}

void DualBufferCompareService::printStats() const {
    LOG4CPLUS_INFO(logger_, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(logger_, "DualBufferCompareService Statistics");
    LOG4CPLUS_INFO(logger_, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO_FMT(logger_, "Total compared: %d", stats_.total_compared);
    LOG4CPLUS_INFO_FMT(logger_, "Passed: %d (%.1f%%)", stats_.passed_count,
                       stats_.total_compared > 0 ? 100.0 * stats_.passed_count / stats_.total_compared : 0.0);
    LOG4CPLUS_INFO_FMT(logger_, "Warned: %d (%.1f%%)", stats_.warned_count,
                       stats_.total_compared > 0 ? 100.0 * stats_.warned_count / stats_.total_compared : 0.0);
    LOG4CPLUS_INFO_FMT(logger_, "Failed: %d (%.1f%%)", stats_.failed_count,
                       stats_.total_compared > 0 ? 100.0 * stats_.failed_count / stats_.total_compared : 0.0);
    LOG4CPLUS_INFO_FMT(logger_, "Timeouts: %d", stats_.timeout_count);
    LOG4CPLUS_INFO_FMT(logger_, "PTS alignment failures: %d", stats_.pts_alignment_failures);
    LOG4CPLUS_INFO_FMT(logger_, "Frame type mismatches: %d", stats_.frame_type_mismatches);
    
    if (!stats_.psnr_y_values.empty()) {
        double avg_psnr_y = std::accumulate(stats_.psnr_y_values.begin(), stats_.psnr_y_values.end(), 0.0) /
                           stats_.psnr_y_values.size();
        auto minmax = std::minmax_element(stats_.psnr_y_values.begin(), stats_.psnr_y_values.end());
        LOG4CPLUS_INFO_FMT(logger_, "PSNR-Y: avg=%.2f dB, min=%.2f dB, max=%.2f dB",
                          avg_psnr_y, *minmax.first, *minmax.second);
    }
    
    if (!stats_.ssim_y_values.empty()) {
        double avg_ssim_y = std::accumulate(stats_.ssim_y_values.begin(), stats_.ssim_y_values.end(), 0.0) /
                           stats_.ssim_y_values.size();
        LOG4CPLUS_INFO_FMT(logger_, "SSIM-Y: avg=%.4f", avg_ssim_y);
    }
    
    LOG4CPLUS_INFO(logger_, "═══════════════════════════════════════════════════════");
}

// ============================================================================
// ConsumerConfigBuilder 实现（第三部分：配置/工厂，策略模式）
// ============================================================================

// 注意：ConsumerConfigBuilder::createConsumer() 已删除
// 请使用 ConsumerFactory::create() 创建消费者

// ============================================================================
// ConsumerConfigBuilder 实现
// ============================================================================

ConsumerConfigBuilder& ConsumerConfigBuilder::setWorkerConfig(const WorkerConfig& worker_config) {
    config_.worker_config = worker_config;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setLoop(bool loop) {
    config_.production_line.loop = loop;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setThreadCount(int thread_count) {
    config_.production_line.thread_count = thread_count;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setEnableMonitor(bool enable) {
    config_.production_line.enable_monitor = enable;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setMaxFrames(int max_frames) {
    config_.runtime.max_frames = max_frames;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setAcquireTimeout(int timeout_ms) {
    config_.runtime.acquire_timeout_ms = timeout_ms;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setMaxTimeoutCount(int count) {
    config_.runtime.max_timeout_count = count;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setDrainRemaining(bool drain) {
    config_.runtime.drain_remaining = drain;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setWaitFirstBuffer(bool wait) {
    config_.runtime.wait_first_buffer = wait;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setFirstBufferTimeout(int timeout_ms) {
    config_.runtime.first_buffer_timeout_ms = timeout_ms;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setEnablePSNRCompare(bool enable) {
    config_.psnr.enable = enable;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setEnableMultiChannelPSNR(bool enable) {
    config_.psnr.enable_multi_channel = enable;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setQuickPSNRThreshold(double threshold) {
    config_.psnr.quick_psnr_threshold = threshold;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setQuickWarnThreshold(double threshold) {
    config_.psnr.quick_warn_threshold = threshold;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setSSIMThreshold(double threshold) {
    config_.psnr.ssim_threshold = threshold;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setSSIMWarnThreshold(double threshold) {
    config_.psnr.ssim_warn_threshold = threshold;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setEnableParallel(bool enable) {
    config_.psnr.enable_parallel = enable;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setUsePerceptualWeighting(bool enable) {
    config_.psnr.use_perceptual_weighting = enable;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setSavePSNRReport(bool save) {
    config_.psnr.save_report = save;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setPSNRReportPath(const std::string& path) {
    config_.psnr.report_path = path;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setPSNRReportPathCh0(const std::string& path) {
    config_.psnr.report_path_ch0 = path;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setPSNRReportPathCh1(const std::string& path) {
    config_.psnr.report_path_ch1 = path;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setEnablePTSAlignment(bool enable) {
    config_.psnr.enable_pts_alignment = enable;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setMaxPTSMatchAttempts(int attempts) {
    config_.psnr.max_pts_match_attempts = attempts;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setEnableDecoderVerification(bool enable) {
    config_.psnr.enable_decoder_verification = enable;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setVerboseDiagnosis(bool verbose) {
    config_.psnr.verbose_diagnosis = verbose;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setProductionLine(const ProductionLineConfig& config) {
    config_.production_line = config;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setRuntime(const RuntimeConfig& config) {
    config_.runtime = config;
    return *this;
}

ConsumerConfigBuilder& ConsumerConfigBuilder::setPSNR(const PSNRConfig& config) {
    config_.psnr = config;
    return *this;
}

std::string ConsumerConfigBuilder::validate() const {
    return config_.validate();
}

BufferConsumerService::Config ConsumerConfigBuilder::build(bool validate_config) const {
    if (validate_config) {
        std::string error = config_.validate();
        if (!error.empty()) {
            throw std::runtime_error("Configuration validation failed: " + error);
        }
    }
    return config_;
}

// 注意：消费者配置方法已删除
// 请使用 ConsumerStrategyConfigBuilder 构建消费者配置，使用 ConsumerFactory 创建消费者


// ============================================================================
// 第二部分：策略选择配置部分 - ConsumerFactory 实现
// ============================================================================

log4cplus::Logger ConsumerFactory::logger_ = 
    log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.ConsumerFactory"));

std::unique_ptr<IBufferConsumer> ConsumerFactory::create(const ConsumerConfig& config) {
    switch (config.type) {
        case ConsumerConfig::Type::DISPLAY: {
            if (!config.display_device) {
                LOG4CPLUS_ERROR(logger_, "DisplayConsumer: display_device is nullptr");
                return nullptr;
            }
            return std::make_unique<DisplayConsumer>(
                config.display_device,
                config.display_ch0_enable,
                config.display_ch1_enable);
        }
        
        case ConsumerConfig::Type::FILE_WRITER: {
            if (config.file_output_path.empty()) {
                LOG4CPLUS_ERROR(logger_, "FileWriterConsumer: file_output_path is empty");
                return nullptr;
            }
            return std::make_unique<FileWriterConsumer>(
                config.file_output_path,
                config.file_ch0_enable,
                config.file_ch1_enable);
        }
        
        case ConsumerConfig::Type::MULTI_CHANNEL_FILE: {
            if (config.multi_file_output_paths.empty()) {
                LOG4CPLUS_ERROR(logger_, "MultiChannelFileWriterConsumer: multi_file_output_paths is empty");
                return nullptr;
            }
            return std::make_unique<MultiChannelFileWriterConsumer>(
                config.multi_file_output_paths,
                config.multi_file_ch0_enable,
                config.multi_file_ch1_enable);
        }
        
        case ConsumerConfig::Type::BUFFER_COMPARE: {
            if (!config.reference_pool) {
                LOG4CPLUS_ERROR(logger_, "BufferCompareConsumer: reference_pool is nullptr");
                return nullptr;
            }
            // 注意：BufferCompareConsumer 暂未实现，请使用 DualBufferCompareService
            LOG4CPLUS_WARN(logger_, "BufferCompareConsumer not yet implemented, use DualBufferCompareService instead");
            return nullptr;
        }
        
        case ConsumerConfig::Type::ENCODED_STREAM: {
            if (config.encoded_output_path.empty()) {
                LOG4CPLUS_ERROR(logger_, "EncodedStreamWriterConsumer: encoded_output_path is empty");
                return nullptr;
            }
            if (!config.codec_params) {
                LOG4CPLUS_ERROR(logger_, "EncodedStreamWriterConsumer: codec_params is nullptr");
                return nullptr;
            }
            return std::make_unique<EncodedStreamWriterConsumer>(
                config.encoded_output_path,
                config.codec_params,
                config.time_base);
        }
        
        default: {
            LOG4CPLUS_ERROR(logger_, "Unknown consumer type");
            return nullptr;
        }
    }
}

std::unique_ptr<IBufferConsumer> ConsumerFactory::createDisplayConsumer(
    LinuxFramebufferDevice* display,
    bool ch0_enable,
    bool ch1_enable) {
    return std::make_unique<DisplayConsumer>(display, ch0_enable, ch1_enable);
}

std::unique_ptr<IBufferConsumer> ConsumerFactory::createFileWriterConsumer(
    const std::string& output_path,
    bool ch0_enable,
    bool ch1_enable) {
    return std::make_unique<FileWriterConsumer>(output_path, ch0_enable, ch1_enable);
}

std::unique_ptr<IBufferConsumer> ConsumerFactory::createMultiChannelFileWriterConsumer(
    const std::vector<std::string>& output_paths,
    bool ch0_enable,
    bool ch1_enable) {
    return std::make_unique<MultiChannelFileWriterConsumer>(
        output_paths, ch0_enable, ch1_enable);
}

std::unique_ptr<IBufferConsumer> ConsumerFactory::createBufferCompareConsumer(
    std::shared_ptr<BufferPool> reference_pool,
    const io::CompareConfig& compare_config,
    bool ch0_enable,
    bool ch1_enable) {
    // 注意：BufferCompareConsumer 暂未实现，请使用 DualBufferCompareService
    LOG4CPLUS_WARN(logger_, "BufferCompareConsumer not yet implemented, use DualBufferCompareService instead");
    (void)reference_pool;
    (void)compare_config;
    (void)ch0_enable;
    (void)ch1_enable;
    return nullptr;
}

std::unique_ptr<IBufferConsumer> ConsumerFactory::createEncodedStreamConsumer(
    const std::string& output_path,
    const AVCodecParameters* codec_params,
    AVRational time_base) {
    return std::make_unique<EncodedStreamWriterConsumer>(output_path, codec_params, time_base);
}

// ============================================================================
// ProductionLineTestService 实现
// ============================================================================

ProductionLineTestService::ProductionLineTestService()
    : logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.ProductionLineTestService")))
{
}

ProductionLineTestService::~ProductionLineTestService() {
    if (producer_) {
        producer_->stop();
    }
}

ProductionLineTestService::Result ProductionLineTestService::run(
    const WorkerConfig& worker_config,
    IBufferConsumer* consumer,
    std::atomic<bool>* running_flag,
    const Options& options,
    std::function<void(const std::string&)> error_callback) {
    
    Result result;
    
    if (!consumer) {
        LOG4CPLUS_ERROR(logger_, "Consumer is nullptr");
        return result;
    }
    
    // 1. 初始化生产线
    if (!initializeProducer(worker_config, options, error_callback)) {
        return result;
    }
    
    // 2. 初始化BufferPool
    if (!initializeBufferPool()) {
        producer_->stop();
        return result;
    }
    
    // 3. 初始化消费者
    if (options.wait_first_buffer) {
        if (!initializeConsumer(consumer, options)) {
            producer_->stop();
            return result;
        }
    }
    
    // 4. 创建运行标志
    std::atomic<bool> local_running_flag(true);
    std::atomic<bool>* running_flag_ptr = running_flag ? running_flag : &local_running_flag;
    
    // 5. 消费循环
    consumeLoop(options, consumer, *running_flag_ptr, result);
    
    // 6. 排空剩余Buffer
    if (options.drain_remaining) {
        drainRemainingBuffers(consumer, result);
    }
    
    // 7. 清理消费者
    consumer->cleanup();
    
    // 8. 获取平均帧率
    result.avg_fps = producer_->getAverageFPS();
    
    // 9. 停止生产线
    producer_->stop();
    
    result.success = true;
    LOG4CPLUS_INFO_FMT(logger_, "Test completed: consumed=%d, success=%d, failed=%d",
                      result.total_consumed, result.success_count, result.failed_count);
    
    return result;
}

bool ProductionLineTestService::initializeProducer(
    const WorkerConfig& worker_config,
    const Options& options,
    std::function<void(const std::string&)> error_callback) {
    
    producer_ = std::make_unique<VideoProductionLine>(
        options.loop, options.thread_count, options.enable_monitor);
    
    if (error_callback) {
        producer_->setErrorCallback(error_callback);
    }
    
    if (!producer_->start(worker_config)) {
        LOG4CPLUS_ERROR(logger_, "Failed to start production line");
        return false;
    }
    
    LOG4CPLUS_INFO(logger_, "Production line started successfully");
    return true;
}

bool ProductionLineTestService::initializeBufferPool() {
    uint64_t pool_id = producer_->getWorkingBufferPoolId();
    if (pool_id == 0) {
        LOG4CPLUS_ERROR(logger_, "No working BufferPool ID available");
        return false;
    }
    
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    pool_sptr_ = pool_weak.lock();
    if (!pool_sptr_) {
        LOG4CPLUS_ERROR(logger_, "BufferPool not found or destroyed");
        return false;
    }
    
    LOG4CPLUS_INFO_FMT(logger_, "BufferPool: '%s' (ID: %lu)",
                      pool_sptr_->getName().c_str(), pool_id);
    return true;
}

bool ProductionLineTestService::initializeConsumer(IBufferConsumer* consumer,
                                                  const Options& options) {
    Buffer* first_buffer = pool_sptr_->acquireFilled(
        true, options.first_buffer_timeout_ms);
    
    if (!first_buffer) {
        LOG4CPLUS_ERROR_FMT(logger_,
                          "Failed to get first buffer after %d ms timeout",
                          options.first_buffer_timeout_ms);
        return false;
    }
    
    bool success = consumer->initialize(first_buffer);
    pool_sptr_->releaseFilled(first_buffer);
    
    if (!success) {
        LOG4CPLUS_ERROR(logger_, "Failed to initialize consumer");
        return false;
    }
    
    LOG4CPLUS_INFO(logger_, "Consumer initialized successfully");
    return true;
}

void ProductionLineTestService::consumeLoop(
    const Options& options,
    IBufferConsumer* consumer,
    std::atomic<bool>& running_flag,
    Result& result) {
    
    int timeout_count = 0;
    
    while (running_flag.load()) {
        // 检查最大帧数限制
        if (options.max_frames > 0 && result.total_consumed >= options.max_frames) {
            LOG4CPLUS_INFO_FMT(logger_, "Reached max_frames limit (%d), stopping",
                              options.max_frames);
            break;
        }
        
        // 检查生产者状态
        if (!producer_->isRunning()) {
            LOG4CPLUS_INFO(logger_, "Producer stopped naturally");
            break;
        }
        
        // 获取Buffer
        Buffer* buffer = pool_sptr_->acquireFilled(true, options.acquire_timeout_ms);
        
        if (buffer) {
            int channel_id = buffer->getOutputChannel();
            
            // 检查是否应该消费该通道
            if (!consumer->shouldConsumeChannel(channel_id)) {
                pool_sptr_->releaseFilled(buffer);
                result.skipped_count++;
                continue;
            }
            
            // 消费Buffer
            if (consumer->consume(buffer, channel_id)) {
                result.success_count++;
            } else {
                result.failed_count++;
            }
            
            pool_sptr_->releaseFilled(buffer);
            result.total_consumed++;
            timeout_count = 0;
        } else {
            timeout_count++;
            if (timeout_count >= options.max_timeout_count) {
                LOG4CPLUS_INFO(logger_, "Consumer timeout, stopping");
                break;
            }
        }
    }
    
    result.timeout_count = timeout_count;
}

void ProductionLineTestService::drainRemainingBuffers(IBufferConsumer* consumer,
                                                     Result& result) {
    Buffer* buffer = nullptr;
    
    while ((buffer = pool_sptr_->acquireFilled(false, 0)) != nullptr) {
        int channel_id = buffer->getOutputChannel();
        
        if (!consumer->shouldConsumeChannel(channel_id)) {
            pool_sptr_->releaseFilled(buffer);
            continue;
        }
        
        if (consumer->consume(buffer, channel_id)) {
            result.success_count++;
        } else {
            result.failed_count++;
        }
        
        pool_sptr_->releaseFilled(buffer);
        result.total_consumed++;
        result.drained_count++;
    }
    
    if (result.drained_count > 0) {
        LOG4CPLUS_INFO_FMT(logger_, "Drained %d remaining buffers", result.drained_count);
    }
}

} // namespace consumer
} // namespace productionline
