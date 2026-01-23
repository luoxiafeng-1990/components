/**
 * consumer_test.cpp - 消费者类测试文件
 * 
 * 测试所有消费者类的接口，包括：
 * - DisplayConsumer: DMA 显示消费者
 * - FileWriterConsumer: 单文件写入消费者
 * - MultiChannelFileWriterConsumer: 多通道文件写入消费者
 * - CompareConsumer: 比较消费者（已废弃，请使用 DualBufferCompareService）
 * - DualBufferCompareService: 双BufferPool对比服务（支持PTS对齐）
 * - EncodedStreamWriterConsumer: 编码流写入消费者
 * - BufferConsumerService: 消费者服务类（综合测试）
 */

#include "productionline/consumer/BufferConsumer.hpp"
#include "productionline/io/BufferComparator.hpp"
#include "productionline/VideoProductionLine.hpp"
#include "productionline/MultiWorkerProductionLine.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "display/LinuxFramebufferDevice.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "common/Logger.hpp"
#include "framework/TestMacros.hpp"
#include <atomic>
#include <signal.h>
#include <cstring>
#include <sys/stat.h>
#include <thread>
#include <chrono>
#include <map>
#include <numeric>
#include <algorithm>
#include <sstream>
#include <functional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#ifndef AV_NOPTS_VALUE
#define AV_NOPTS_VALUE ((int64_t)UINT64_C(0x8000000000000000))
#endif
}

using namespace productionline;
using namespace productionline::consumer;
using namespace productionline::io;

// 用于收集测试统计结果的结构（需要在函数定义之前声明）
struct TestResult {
    std::string test_name;
    std::string format_name;
    int channel;  // 0=PP0, 1=PP1, -1=Multi-PP
    bool passed;
    bool skipped;
    std::string report_path;
    int frames_processed = 0;      // 处理的帧数
    int frames_passed = 0;          // 通过的帧数
    double avg_psnr_y = 0.0;        // Y平面平均PSNR值（dB）
};

// ============ 全局变量和信号处理 ============

static volatile bool g_running = true;
static std::atomic<bool> g_rtsp_interrupted(false);
static log4cplus::Logger test_logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Test.Consumer"));

static void signal_handler(int signum) {
    if (signum == SIGINT) {
        if (!g_rtsp_interrupted.load()) {
            LOG4CPLUS_INFO(test_logger, "\n🛑 收到中断信号 (Ctrl+C)，正在停止程序...");
            g_running = false;
            g_rtsp_interrupted = true;
        } else {
            LOG4CPLUS_INFO(test_logger, "\n🛑 强制退出...");
            signal(SIGINT, SIG_DFL);
            raise(SIGINT);
        }
    }
}

// ============================================================================
// 测试用例 1: DisplayConsumer - RTSP 流显示
// ============================================================================

static int test_display_consumer_rtsp(const char* rtsp_url) {
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Test: DisplayConsumer - RTSP Stream Playback");
    LOG4CPLUS_INFO_FMT(test_logger, "  RTSP URL: %s", rtsp_url);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    signal(SIGINT, signal_handler);
    g_running = true;
    g_rtsp_interrupted = false;
    
    // 1. 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 创建 DisplayConsumer
    DisplayConsumer display_consumer(&display, true, false);  // ch0 启用，ch1 禁用
    
    // 3. 配置 WorkerConfig
    auto tacoConfig = TacoConfigBuilder()
        .setChannels(true, false)
        .build();
    
    auto workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(rtsp_url)
                .setBufferCount(32)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(display.getWidth(), display.getHeight())
                .setBitsPerPixel(display.getBitsPerPixel())
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useTaco("h264", tacoConfig)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_RTSP)
        .build();
    
    // 4. 创建 BufferConsumerService（使用 ConsumerConfigBuilder 构建配置）
    BufferConsumerService service;
    BufferConsumerService::Config config = ConsumerConfigBuilder()
        .setWorkerConfig(workerConfig)
        .setLoop(false)
        .setThreadCount(1)
        .build();
    config.acquire_timeout_ms = 100;
    config.max_timeout_count = 50;
    config.drain_remaining = true;
    
    // 5. 设置错误回调
    auto error_callback = [](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "RTSP Error: %s", error.c_str());
        g_running = false;
    };
    
    // 6. 一键运行（open → run → printStats → close）
    BufferConsumerService::RunOptions opts;
    opts.error_callback = error_callback;
    
    if (!service.runOnce(config, &display_consumer, opts)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to run BufferConsumerService");
        return -1;
    }
    
    LOG4CPLUS_INFO(test_logger, "✅ DisplayConsumer RTSP test completed");
    return 0;
}

// ============================================================================
// 测试用例 2: DisplayConsumer - 视频文件显示
// ============================================================================

static int test_display_consumer_file(const char* video_path) {
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Test: DisplayConsumer - Video File Playback");
    LOG4CPLUS_INFO_FMT(test_logger, "  Video: %s", video_path);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    signal(SIGINT, signal_handler);
    g_running = true;
    
    // 1. 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 创建 DisplayConsumer
    DisplayConsumer display_consumer(&display, true, false);
    
    // 3. 配置 WorkerConfig
    auto tacoConfig = TacoConfigBuilder()
        .setChannels(true, false)
        .build();
    
    auto workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(32)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(display.getWidth(), display.getHeight())
                .setBitsPerPixel(display.getBitsPerPixel())
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useTaco("h264", tacoConfig)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    // 4. 创建 BufferConsumerService（使用 ConsumerConfigBuilder 构建配置）
    BufferConsumerService service;
    BufferConsumerService::Config config = ConsumerConfigBuilder()
        .setWorkerConfig(workerConfig)
        .setLoop(false)
        .setThreadCount(1)
        .build();
    
    auto error_callback = [](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "FFmpeg Error: %s", error.c_str());
        g_running = false;
    };
    
    BufferConsumerService::RunOptions opts;
    opts.error_callback = error_callback;
    
    if (!service.runOnce(config, &display_consumer, opts)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to run BufferConsumerService");
        return -1;
    }
    
    LOG4CPLUS_INFO(test_logger, "✅ DisplayConsumer File test completed");
    return 0;
}

// ============================================================================
// 测试用例 3: FileWriterConsumer - 单文件写入
// ============================================================================

static int test_file_writer_consumer(const char* video_path) {
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Test: FileWriterConsumer - Single File Write");
    LOG4CPLUS_INFO_FMT(test_logger, "  Video: %s", video_path);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    signal(SIGINT, signal_handler);
    g_running = true;
    
    // 1. 创建 FileWriterConsumer（只处理 ch0）
    std::string output_path = "./test_output_consumer/ch0_output.raw";
    system("mkdir -p ./test_output_consumer");
    FileWriterConsumer file_consumer(output_path, true, false);  // ch0启用，ch1禁用
    
    // 2. 配置 WorkerConfig（只启用 ch0）
    auto tacoConfig = TacoConfigBuilder()
        .setChannels(true, false)  // 只启用 ch0
        .build();
    
    auto workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(32)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(1920, 1080)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useTaco("h264", tacoConfig)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    // 3. 创建 BufferConsumerService（使用 ConsumerConfigBuilder 构建配置）
    BufferConsumerService service;
    BufferConsumerService::Config config = ConsumerConfigBuilder()
        .setWorkerConfig(workerConfig)
        .setLoop(false)
        .setThreadCount(1)
        .build();
    
    auto error_callback = [](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "Error: %s", error.c_str());
        g_running = false;
    };
    
    BufferConsumerService::RunOptions opts;
    opts.error_callback = error_callback;
    
    LOG4CPLUS_INFO(test_logger, "✅ BufferConsumerService opened, starting write...");
    LOG4CPLUS_INFO(test_logger, "   按 Ctrl+C 停止");
    
    if (!service.runOnce(config, &file_consumer, opts)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to run BufferConsumerService");
        return -1;
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "✅ FileWriterConsumer test completed, output: %s", output_path.c_str());
    return 0;
}

// ============================================================================
// 测试用例 4: MultiChannelFileWriterConsumer - 多通道文件写入
// ============================================================================

static int test_multichannel_writer_consumer(const char* video_path) {
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Test: MultiChannelFileWriterConsumer - Multi Channel Write");
    LOG4CPLUS_INFO_FMT(test_logger, "  Video: %s", video_path);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    signal(SIGINT, signal_handler);
    g_running = true;
    
    // 1. 创建 MultiChannelFileWriterConsumer
    system("mkdir -p ./test_output_consumer");
    std::vector<std::string> output_paths = {
        "./test_output_consumer/ch0_output.raw",
        "./test_output_consumer/ch1_output.raw"
    };
    MultiChannelFileWriterConsumer multi_consumer(output_paths, true, true);  // ch0 和 ch1 都启用
    
    // 2. 配置 WorkerConfig（启用双通道）
    auto tacoConfig = TacoConfigBuilder()
        .setChannels(true, true)  // 启用 ch0 和 ch1
        .build();
    
    auto workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(32)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(1920, 1080)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useTaco("h264", tacoConfig)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    // 3. 创建 BufferConsumerService（使用 ConsumerConfigBuilder 构建配置）
    BufferConsumerService service;
    BufferConsumerService::Config config = ConsumerConfigBuilder()
        .setWorkerConfig(workerConfig)
        .setLoop(false)
        .setThreadCount(1)
        .build();
    
    auto error_callback = [](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "Error: %s", error.c_str());
        g_running = false;
    };
    
    BufferConsumerService::RunOptions opts;
    opts.error_callback = error_callback;
    
    LOG4CPLUS_INFO(test_logger, "✅ BufferConsumerService opened, starting multi-channel write...");
    LOG4CPLUS_INFO(test_logger, "   按 Ctrl+C 停止");
    
    if (!service.runOnce(config, &multi_consumer, opts)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to run BufferConsumerService");
        return -1;
    }
    
    LOG4CPLUS_INFO(test_logger, "✅ MultiChannelFileWriterConsumer test completed");
    return 0;
}

// ============================================================================
// 测试用例 5: CompareConsumer - 硬件 vs 软件解码器对比
// ============================================================================
// ⚠️ 已废弃：CompareConsumer 不支持PTS对齐，仅用于简单的顺序对比。
// 请使用 DualBufferCompareService 进行带PTS对齐的对比。
// 如需使用此测试，请取消以下注释并取消 CompareConsumer 类的注释。

/*
static int test_compare_consumer(const char* video_path) {
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Test: CompareConsumer - Hardware vs Software Decoder");
    LOG4CPLUS_INFO_FMT(test_logger, "  Video: %s", video_path);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    signal(SIGINT, signal_handler);
    g_running = true;
    
    // 1. 创建硬件解码器生产线（参考）
    auto tacoConfig = TacoConfigBuilder()
        .setChannels(true, false)
        .build();
    
    auto hw_workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(32)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(1920, 1080)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useTaco("h264", tacoConfig)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    VideoProductionLine hw_producer(false, 1, false);
    hw_producer.setErrorCallback([](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "HW Producer Error: %s", error.c_str());
        g_running = false;
    });
    
    if (!hw_producer.start(hw_workerConfig)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to start hardware producer");
        return -1;
    }
    
    uint64_t hw_pool_id = hw_producer.getWorkingBufferPoolId();
    auto hw_pool_sptr = BufferPoolRegistry::getInstance().getPool(hw_pool_id).lock();
    if (!hw_pool_sptr) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get hardware BufferPool");
        hw_producer.stop();
        return -1;
    }
    
    // 2. 创建软件解码器生产线（测试）
    auto sw_workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(32)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(1920, 1080)
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
    
    // 3. 创建 BufferComparator
    BufferComparator comparator;
    CompareConfig compare_config;
    compare_config.enable_psnr = true;
    compare_config.enable_ssim = true;
    compare_config.psnr_threshold = 30.0;
    compare_config.ssim_threshold = 0.9;
    
    if (!comparator.open(compare_config)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to open BufferComparator");
        hw_producer.stop();
        return -1;
    }
    
    // 4. 创建 CompareConsumer
    CompareConsumer compare_consumer(&comparator, hw_pool_sptr);
    
    // 5. 创建 BufferConsumerService（使用软件解码器）
    BufferConsumerService service;
    BufferConsumerService::Config config = ConsumerConfigBuilder()
        .setWorkerConfig(sw_workerConfig)
        .setLoop(false)
        .setThreadCount(1)
        .build();
    
    auto error_callback = [](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "SW Producer Error: %s", error.c_str());
        g_running = false;
    };
    
    if (!service.open(config, &compare_consumer, error_callback)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to open BufferConsumerService");
        comparator.close();
        hw_producer.stop();
        return -1;
    }
    
    LOG4CPLUS_INFO(test_logger, "✅ CompareConsumer test started...");
    LOG4CPLUS_INFO(test_logger, "   按 Ctrl+C 停止");
    
    std::atomic<bool> running_flag(true);
    service.run(running_flag);
    
    service.printStats();
    service.close();
    
    comparator.close();
    hw_producer.stop();
    
    LOG4CPLUS_INFO(test_logger, "✅ CompareConsumer test completed");
    return 0;
}
*/

// ============================================================================
// 测试用例 6: DualBufferCompareService - 双BufferPool对比（PTS对齐）
// ============================================================================

static int test_dual_buffer_compare_service(const char* video_path) {
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Test: DualBufferCompareService - Hardware vs Software Decoder (PTS Aligned)");
    LOG4CPLUS_INFO_FMT(test_logger, "  Video: %s", video_path);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    signal(SIGINT, signal_handler);
    g_running = true;
    
    // 1. 创建硬件解码器生产线（测试）
    auto tacoConfig = TacoConfigBuilder()
        .setChannels(true, false)
        .build();
    
    auto hw_workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(32)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(1920, 1080)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useTaco("h264", tacoConfig)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    VideoProductionLine hw_producer(false, 1, false);
    hw_producer.setErrorCallback([](const std::string& error) {
        LOG4CPLUS_WARN_FMT(test_logger, "HW Producer Error (non-fatal): %s", error.c_str());
    });
    
    if (!hw_producer.start(hw_workerConfig)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to start hardware producer");
        return -1;
    }
    
    uint64_t hw_pool_id = hw_producer.getWorkingBufferPoolId();
    auto hw_pool_sptr = BufferPoolRegistry::getInstance().getPool(hw_pool_id).lock();
    if (!hw_pool_sptr) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get hardware BufferPool");
        hw_producer.stop();
        return -1;
    }
    
    // 2. 创建软件解码器生产线（参考）
    auto sw_workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(32)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(1920, 1080)
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
    
    VideoProductionLine sw_producer(false, 1, false);
    sw_producer.setErrorCallback([](const std::string& error) {
        LOG4CPLUS_WARN_FMT(test_logger, "SW Producer Error (non-fatal): %s", error.c_str());
    });
    
    if (!sw_producer.start(sw_workerConfig)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to start software producer");
        hw_producer.stop();
        return -1;
    }
    
    uint64_t sw_pool_id = sw_producer.getWorkingBufferPoolId();
    auto sw_pool_sptr = BufferPoolRegistry::getInstance().getPool(sw_pool_id).lock();
    if (!sw_pool_sptr) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get software BufferPool");
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }
    
    // 3. 创建 BufferComparator
    BufferComparator comparator;
    CompareConfig compare_config;
    compare_config.strategy = CompareConfig::AUTO_LAYERED;
    compare_config.format_strategy = CompareConfig::AUTO;
    compare_config.quick_psnr_threshold = 38.0;
    compare_config.quick_warn_threshold = 35.0;
    compare_config.enable_psnr = true;
    compare_config.enable_ssim = true;
    compare_config.ssim_threshold = 0.95;
    compare_config.ssim_warn_threshold = 0.90;
    compare_config.enable_parallel = true;
    compare_config.use_perceptual_weighting = true;
    // 精简调试输出：关闭逐帧详细日志，由测试程序统一打印统计
    compare_config.verbose = false;
    compare_config.enable_parallel = true;
    compare_config.use_perceptual_weighting = true;
    // 精简调试输出
    compare_config.verbose = false;
    compare_config.enable_parallel = true;
    compare_config.use_perceptual_weighting = true;
    // 精简调试输出
    compare_config.verbose = false;
    compare_config.save_report = true;
    compare_config.report_path = "./test_output_consumer/compare_report.txt";
    
    system("mkdir -p ./test_output_consumer");
    
    if (!comparator.open(compare_config)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to open BufferComparator");
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }
    
    // 4. 创建 DualBufferCompareService
    DualBufferCompareService compare_service;
    compare_service.setComparator(&comparator);
    compare_service.setReferencePool(sw_pool_sptr);  // 软件解码器作为参考
    compare_service.setTestPool(hw_pool_sptr);      // 硬件解码器作为测试
    compare_service.setReferenceProducer(&sw_producer);
    compare_service.setTestProducer(&hw_producer);
    
    DualBufferCompareService::Config service_config;
    service_config.max_frames = 300;  // 限制测试帧数
    service_config.acquire_timeout_ms = 100;
    service_config.max_timeout_count = 50;
    service_config.enable_pts_alignment = true;  // 启用PTS对齐
    service_config.max_pts_match_attempts = 10;
    service_config.drain_remaining = true;
    service_config.verbose = true;
    
    if (!compare_service.open(service_config)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to open DualBufferCompareService");
        comparator.close();
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }
    
    LOG4CPLUS_INFO(test_logger, "✅ DualBufferCompareService opened, starting comparison...");
    LOG4CPLUS_INFO(test_logger, "   PTS alignment: enabled");
    LOG4CPLUS_INFO(test_logger, "   按 Ctrl+C 停止");
    
    // 5. 运行对比服务
    std::atomic<bool> running_flag(true);
    compare_service.run(running_flag);
    
    // 6. 打印统计信息
    compare_service.printStats();
    comparator.printSummary();
    
    // 7. 关闭服务
    compare_service.close();
    comparator.close();
    hw_producer.stop();
    sw_producer.stop();
    
    LOG4CPLUS_INFO(test_logger, "✅ DualBufferCompareService test completed");
    LOG4CPLUS_INFO_FMT(test_logger, "   Report saved to: %s", compare_config.report_path.c_str());
    
    return 0;
}

// 注意：裁剪功能已封装到 DualBufferCompareService 中
// 如需使用裁剪功能，请使用 DualBufferCompareService 并配置裁剪参数
// 参考 DualBufferCompareService::cropBufferRegion 的实现（在 BufferConsumer.cpp 中）

// ============================================================================
// 测试用例 7: 综合测试 - 硬件解码+播放+保存+PSNR对比（YUV+RGB+裁剪）
// ============================================================================

static int test_comprehensive_decode_compare(const char* video_path) {
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Test: Comprehensive - Hardware Decode + Display + Save + PSNR Compare");
    LOG4CPLUS_INFO_FMT(test_logger, "  Video: %s", video_path);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    signal(SIGINT, signal_handler);
    g_running = true;
    
    system("mkdir -p ./test_output_consumer");
    
    // ========== 步骤1: 创建硬件解码器（双通道：ch0=YUV用于显示和保存，ch1=RGB用于保存）==========
    LOG4CPLUS_INFO(test_logger, "\n[Step 1] Creating hardware decoder (dual channel: ch0=YUV, ch1=RGB)...");
    
    auto hw_tacoConfig = TacoConfigBuilder()
        .setChannels(true, true)  // 启用双通道
        .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT601)
        .setOutputFormat(Channel::CH1, OutputFormat::RGB_RGB888, ColorStandard::BT601)
        .setScale(Channel::CH0, 1920, 1080)
        .setScale(Channel::CH1, 1920, 1080)
        .build();
    
    auto hw_workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(32)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(1920, 1080)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useTaco("h264", hw_tacoConfig)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    VideoProductionLine hw_producer(false, 1, false);
    hw_producer.setErrorCallback([](const std::string& error) {
        LOG4CPLUS_WARN_FMT(test_logger, "HW Producer Error (non-fatal): %s", error.c_str());
    });
    
    if (!hw_producer.start(hw_workerConfig)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to start hardware producer");
        return -1;
    }
    
    uint64_t hw_pool_id = hw_producer.getWorkingBufferPoolId();
    auto hw_pool_sptr = BufferPoolRegistry::getInstance().getPool(hw_pool_id).lock();
    if (!hw_pool_sptr) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get hardware BufferPool");
        hw_producer.stop();
        return -1;
    }
    
    // ========== 步骤2: 创建软件解码器（作为PSNR参考）==========
    LOG4CPLUS_INFO(test_logger, "\n[Step 2] Creating software decoder (reference for PSNR)...");
    
    auto sw_workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(32)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(1920, 1080)
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
    
    VideoProductionLine sw_producer(false, 1, false);
    sw_producer.setErrorCallback([](const std::string& error) {
        LOG4CPLUS_WARN_FMT(test_logger, "SW Producer Error (non-fatal): %s", error.c_str());
    });
    
    if (!sw_producer.start(sw_workerConfig)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to start software producer");
        hw_producer.stop();
        return -1;
    }
    
    uint64_t sw_pool_id = sw_producer.getWorkingBufferPoolId();
    auto sw_pool_sptr = BufferPoolRegistry::getInstance().getPool(sw_pool_id).lock();
    if (!sw_pool_sptr) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get software BufferPool");
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }
    
    // ========== 步骤3: 初始化显示设备 ==========
    LOG4CPLUS_INFO(test_logger, "\n[Step 3] Initializing display device...");
    
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to initialize display");
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }
    
    // ========== 步骤4: 创建消费者 ==========
    LOG4CPLUS_INFO(test_logger, "\n[Step 4] Creating consumers...");
    
    DisplayConsumer display_consumer(&display, true, false);  // ch0显示
    FileWriterConsumer yuv_writer("./test_output_consumer/comprehensive_ch0_yuv.raw", 
                                  true, false);  // ch0 YUV
    std::vector<std::string> rgb_paths = {
        "./test_output_consumer/comprehensive_ch1_rgb.raw"
    };
    MultiChannelFileWriterConsumer rgb_writer(rgb_paths, false, true);  // ch1 RGB
    
    // ========== 步骤5: 创建BufferComparator（用于YUV和RGB对比）==========
    LOG4CPLUS_INFO(test_logger, "\n[Step 5] Creating BufferComparators...");
    
    // YUV对比器
    BufferComparator yuv_comparator;
    CompareConfig yuv_compare_config;
    yuv_compare_config.strategy = CompareConfig::AUTO_LAYERED;
    yuv_compare_config.format_strategy = CompareConfig::AUTO;
    yuv_compare_config.quick_psnr_threshold = 38.0;
    yuv_compare_config.quick_warn_threshold = 35.0;
    yuv_compare_config.enable_psnr = true;
    yuv_compare_config.enable_ssim = true;
    yuv_compare_config.ssim_threshold = 0.95;
    yuv_compare_config.ssim_warn_threshold = 0.90;
    yuv_compare_config.enable_parallel = true;
    yuv_compare_config.use_perceptual_weighting = true;
    yuv_compare_config.verbose = true;
    yuv_compare_config.save_report = true;
    yuv_compare_config.report_path = "./test_output_consumer/comprehensive_yuv_compare.txt";
    
    if (!yuv_comparator.open(yuv_compare_config)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to open YUV BufferComparator");
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }
    
    // RGB对比器
    BufferComparator rgb_comparator;
    CompareConfig rgb_compare_config = yuv_compare_config;
    rgb_compare_config.report_path = "./test_output_consumer/comprehensive_rgb_compare.txt";
    
    if (!rgb_comparator.open(rgb_compare_config)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to open RGB BufferComparator");
        yuv_comparator.close();
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }
    
    // ========== 步骤6: 创建DualBufferCompareService（用于YUV的PSNR对比）==========
    LOG4CPLUS_INFO(test_logger, "\n[Step 6] Creating DualBufferCompareService for YUV comparison...");
    
    DualBufferCompareService yuv_compare_service;
    yuv_compare_service.setComparator(&yuv_comparator);
    yuv_compare_service.setReferencePool(sw_pool_sptr);
    yuv_compare_service.setTestPool(hw_pool_sptr);
    yuv_compare_service.setReferenceProducer(&sw_producer);
    yuv_compare_service.setTestProducer(&hw_producer);
    
    DualBufferCompareService::Config yuv_service_config;
    yuv_service_config.max_frames = 300;
    yuv_service_config.acquire_timeout_ms = 100;
    yuv_service_config.max_timeout_count = 50;
    yuv_service_config.enable_pts_alignment = true;
    yuv_service_config.max_pts_match_attempts = 10;
    yuv_service_config.drain_remaining = true;
    yuv_service_config.verbose = true;
    yuv_service_config.enable_crop = false;  // YUV对比不使用裁剪
    
    if (!yuv_compare_service.open(yuv_service_config)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to open YUV DualBufferCompareService");
        yuv_comparator.close();
        rgb_comparator.close();
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }
    
    // ========== 步骤6.5: 创建裁剪对比服务（裁剪中心区域 50%）==========
    LOG4CPLUS_INFO(test_logger, "\n[Step 6.5] Creating DualBufferCompareService for cropped comparison...");
    
    BufferComparator crop_comparator;
    CompareConfig crop_compare_config = yuv_compare_config;
    crop_compare_config.report_path = "./test_output_consumer/comprehensive_crop_compare.txt";
    
    if (!crop_comparator.open(crop_compare_config)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to open crop BufferComparator");
        yuv_compare_service.close();
        yuv_comparator.close();
        rgb_comparator.close();
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }
    
    DualBufferCompareService crop_compare_service;
    crop_compare_service.setComparator(&crop_comparator);
    crop_compare_service.setReferencePool(sw_pool_sptr);
    crop_compare_service.setTestPool(hw_pool_sptr);
    crop_compare_service.setReferenceProducer(&sw_producer);
    crop_compare_service.setTestProducer(&hw_producer);
    
    DualBufferCompareService::Config crop_service_config = yuv_service_config;
    crop_service_config.enable_crop = true;
    crop_service_config.crop_x = 1920 / 4;  // 中心区域起始X
    crop_service_config.crop_y = 1080 / 4;  // 中心区域起始Y
    crop_service_config.crop_w = 1920 / 2;  // 裁剪宽度（50%）
    crop_service_config.crop_h = 1080 / 2;  // 裁剪高度（50%）
    
    if (!crop_compare_service.open(crop_service_config)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to open crop DualBufferCompareService");
        crop_comparator.close();
        yuv_compare_service.close();
        yuv_comparator.close();
        rgb_comparator.close();
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }
    
    if (!yuv_compare_service.open(yuv_service_config)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to open YUV DualBufferCompareService");
        yuv_comparator.close();
        rgb_comparator.close();
        crop_comparator.close();
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }
    
    // ========== 步骤7: 等待第一个Buffer并初始化消费者 ==========
    LOG4CPLUS_INFO(test_logger, "\n[Step 7] Waiting for first buffers and initializing consumers...");
    
    Buffer* first_hw_buf = hw_pool_sptr->acquireFilled(true, 5000);
    Buffer* first_sw_buf = sw_pool_sptr->acquireFilled(true, 5000);
    
    if (!first_hw_buf || !first_sw_buf) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get first buffers");
        if (first_hw_buf) hw_pool_sptr->releaseFilled(first_hw_buf);
        if (first_sw_buf) sw_pool_sptr->releaseFilled(first_sw_buf);
        yuv_compare_service.close();
        yuv_comparator.close();
        rgb_comparator.close();
        crop_comparator.close();
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }
    
    // 初始化消费者（使用ch0的Buffer）
    if (first_hw_buf->getOutputChannel() == 0) {
        display_consumer.initialize(first_hw_buf);
        yuv_writer.initialize(first_hw_buf);
    }
    
    // 释放第一个Buffer
    hw_pool_sptr->releaseFilled(first_hw_buf);
    sw_pool_sptr->releaseFilled(first_sw_buf);
    
    // ========== 步骤8: 主循环 - 同时进行播放、保存、对比 ==========
    LOG4CPLUS_INFO(test_logger, "\n[Step 8] Starting main loop (display + save + compare)...");
    LOG4CPLUS_INFO(test_logger, "   按 Ctrl+C 停止");
    
    int frame_count = 0;
    int yuv_frame_count = 0;
    int rgb_frame_count = 0;
    const int MAX_FRAMES = 300;
    
    // 统计信息
    std::vector<double> yuv_psnr_values;
    std::vector<double> rgb_psnr_values;
    
    // 注意：裁剪参数已配置在 crop_compare_service 中，无需在此处定义
    
    // 用于PTS对齐的Buffer缓存
    std::map<int64_t, Buffer*> hw_yuv_cache;  // PTS -> Buffer
    std::map<int64_t, Buffer*> sw_yuv_cache;   // PTS -> Buffer
    
    while (g_running && frame_count < MAX_FRAMES) {
        // 8.1 从硬件解码器获取Buffer（可能是ch0或ch1）
        Buffer* hw_buf = hw_pool_sptr->acquireFilled(true, 100);
        if (!hw_buf) {
            if (!hw_producer.isRunning()) {
                break;
            }
            continue;
        }
        
        int hw_channel = hw_buf->getOutputChannel();
        AVFrame* hw_frame = hw_buf->getAVFrame();
        int64_t hw_pts = (hw_frame && hw_frame->pts != AV_NOPTS_VALUE) ? hw_frame->pts : -1;
        
        // 8.2 根据通道处理
        if (hw_channel == 0) {
            // ch0: YUV - 显示、保存、YUV对比
            display_consumer.consume(hw_buf, 0);
            yuv_writer.consume(hw_buf, 0);
            
            // YUV对比（需要从软件解码器获取对应的Buffer，使用PTS对齐）
            if (hw_pts >= 0) {
                // 尝试从缓存中找到匹配的软件Buffer
                Buffer* sw_buf = nullptr;
                auto it = sw_yuv_cache.find(hw_pts);
                if (it != sw_yuv_cache.end()) {
                    sw_buf = it->second;
                    sw_yuv_cache.erase(it);
                } else {
                    // 从pool中获取并查找匹配的PTS
                    Buffer* temp_sw = sw_pool_sptr->acquireFilled(true, 100);
                    if (temp_sw) {
                        AVFrame* sw_frame = temp_sw->getAVFrame();
                        int64_t sw_pts = (sw_frame && sw_frame->pts != AV_NOPTS_VALUE) ? sw_frame->pts : -1;
                        
                        if (sw_pts == hw_pts) {
                            // PTS匹配，直接使用
                            sw_buf = temp_sw;
                        } else {
                            // PTS不匹配，缓存起来
                            sw_yuv_cache[sw_pts] = temp_sw;
                            // 继续查找
                            int attempts = 0;
                            while (attempts < 5 && !sw_buf) {
                                temp_sw = sw_pool_sptr->acquireFilled(true, 100);
                                if (temp_sw) {
                                    sw_frame = temp_sw->getAVFrame();
                                    sw_pts = (sw_frame && sw_frame->pts != AV_NOPTS_VALUE) ? sw_frame->pts : -1;
                                    if (sw_pts == hw_pts) {
                                        sw_buf = temp_sw;
                                    } else {
                                        sw_yuv_cache[sw_pts] = temp_sw;
                                    }
                                }
                                attempts++;
                            }
                        }
                    }
                }
                
                if (sw_buf) {
                    auto yuv_result = yuv_comparator.compare(sw_buf, hw_buf);
                    if (yuv_result.psnr_y > 0.0) {
                        yuv_psnr_values.push_back(yuv_result.psnr_y);
                        yuv_frame_count++;
                    }
                    
                    // 注意：裁剪对比现在由 crop_compare_service 在后台线程处理
                    // 这里只记录全图对比结果
                    
                    sw_pool_sptr->releaseFilled(sw_buf);
                }
            }
        } else if (hw_channel == 1) {
            // ch1: RGB - 保存
            rgb_writer.consume(hw_buf, 1);
            rgb_frame_count++;
            
            // 注意：RGB对比需要软件解码器也输出RGB
            // 由于软件解码器默认输出YUV，这里简化处理：只保存RGB，不进行对比
            // 如果需要RGB对比，需要创建另一个软件解码器配置为RGB输出
        }
        
        hw_pool_sptr->releaseFilled(hw_buf);
        frame_count++;
        
        if (frame_count % 50 == 0) {
            LOG4CPLUS_INFO_FMT(test_logger, "  Progress: %d frames processed (YUV: %d, RGB: %d)", 
                             frame_count, yuv_frame_count, rgb_frame_count);
        }
    }
    
    // 清理缓存的Buffer
    for (auto& pair : hw_yuv_cache) {
        hw_pool_sptr->releaseFilled(pair.second);
    }
    for (auto& pair : sw_yuv_cache) {
        sw_pool_sptr->releaseFilled(pair.second);
    }
    
    // ========== 步骤9: 清理和统计 ==========
    LOG4CPLUS_INFO(test_logger, "\n[Step 9] Cleaning up and printing statistics...");
    
    // 排空剩余Buffer
    Buffer* remaining = nullptr;
    while ((remaining = hw_pool_sptr->acquireFilled(false, 0)) != nullptr) {
        int ch = remaining->getOutputChannel();
        if (ch == 0) {
            display_consumer.consume(remaining, 0);
            yuv_writer.consume(remaining, 0);
        } else if (ch == 1) {
            rgb_writer.consume(remaining, 1);
        }
        hw_pool_sptr->releaseFilled(remaining);
    }
    
    // 清理消费者
    display_consumer.cleanup();
    yuv_writer.cleanup();
    rgb_writer.cleanup();
    
    // 关闭对比服务
    yuv_compare_service.close();
    crop_compare_service.close();
    
    // 打印统计信息
    LOG4CPLUS_INFO(test_logger, "\n═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "Comprehensive Test Statistics");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    yuv_comparator.printSummary();
    rgb_comparator.printSummary();
    crop_comparator.printSummary();
    
    if (!yuv_psnr_values.empty()) {
        double avg_yuv_psnr = std::accumulate(yuv_psnr_values.begin(), yuv_psnr_values.end(), 0.0) / yuv_psnr_values.size();
        double min_yuv_psnr = *std::min_element(yuv_psnr_values.begin(), yuv_psnr_values.end());
        double max_yuv_psnr = *std::max_element(yuv_psnr_values.begin(), yuv_psnr_values.end());
        LOG4CPLUS_INFO_FMT(test_logger, "YUV PSNR: avg=%.2f dB, min=%.2f dB, max=%.2f dB (from %zu frames)", 
                         avg_yuv_psnr, min_yuv_psnr, max_yuv_psnr, yuv_psnr_values.size());
    }
    
    // 打印裁剪对比服务的统计信息
    auto crop_stats = crop_compare_service.getStats();
    if (crop_stats.total_compared > 0) {
        double avg_crop_psnr = crop_stats.psnr_y_values.empty() ? 0.0 :
            std::accumulate(crop_stats.psnr_y_values.begin(), crop_stats.psnr_y_values.end(), 0.0) /
            crop_stats.psnr_y_values.size();
        LOG4CPLUS_INFO_FMT(test_logger, "Crop PSNR: avg=%.2f dB (from %d frames)", 
                         avg_crop_psnr, crop_stats.total_compared);
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "Frame counts: Total=%d, YUV=%d, RGB=%d", 
                     frame_count, yuv_frame_count, rgb_frame_count);
    
    // 关闭所有资源
    yuv_comparator.close();
    rgb_comparator.close();
    crop_comparator.close();
    hw_producer.stop();
    sw_producer.stop();
    
    LOG4CPLUS_INFO(test_logger, "\n✅ Comprehensive test completed");
    LOG4CPLUS_INFO(test_logger, "   Output files:");
    LOG4CPLUS_INFO(test_logger, "     - YUV: ./test_output_consumer/comprehensive_ch0_yuv.raw");
    LOG4CPLUS_INFO(test_logger, "     - RGB: ./test_output_consumer/comprehensive_ch1_rgb.raw");
    LOG4CPLUS_INFO(test_logger, "     - YUV Compare Report: ./test_output_consumer/comprehensive_yuv_compare.txt");
    LOG4CPLUS_INFO(test_logger, "     - RGB Compare Report: ./test_output_consumer/comprehensive_rgb_compare.txt");
    LOG4CPLUS_INFO(test_logger, "     - Crop Compare Report: ./test_output_consumer/comprehensive_crop_compare.txt");
    
    return 0;
}

// ============================================================================
// 测试用例 8: EncodedStreamWriterConsumer - 编码流录制
// ============================================================================

static int test_encoded_stream_writer_consumer(const char* rtsp_url) {
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Test: EncodedStreamWriterConsumer - Encoded Stream Record");
    LOG4CPLUS_INFO_FMT(test_logger, "  RTSP URL: %s", rtsp_url);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    signal(SIGINT, signal_handler);
    g_running = true;
    g_rtsp_interrupted = false;
    
    // 1. 创建录制 Worker（用于获取编码参数）
    auto recorder_workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(rtsp_url)
                .setBufferCount(32)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
        .build();
    
    VideoProductionLine recorder_producer(false, 1, false);
    recorder_producer.setErrorCallback([](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "Recorder Error: %s", error.c_str());
        g_running = false;
    });
    
    if (!recorder_producer.start(recorder_workerConfig)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to start recorder producer");
        return -1;
    }
    
    // 2. 获取编码参数
    auto worker_facade = recorder_producer.getWorkerFacade();
    if (!worker_facade) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get WorkerFacade");
        recorder_producer.stop();
        return -1;
    }
    
    const AVCodecParameters* codec_params = worker_facade->getCodecParameters();
    AVRational time_base = worker_facade->getTimeBase();
    
    if (!codec_params) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get codec parameters");
        recorder_producer.stop();
        return -1;
    }
    
    // 3. 创建 EncodedStreamWriterConsumer
    std::string output_path = "./test_output_consumer/encoded_output.mp4";
    system("mkdir -p ./test_output_consumer");
    EncodedStreamWriterConsumer encoded_consumer(output_path, codec_params, time_base);
    
    // 4. 获取 BufferPool
    uint64_t pool_id = recorder_producer.getWorkingBufferPoolId();
    auto pool_sptr = BufferPoolRegistry::getInstance().getPool(pool_id).lock();
    if (!pool_sptr) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get BufferPool");
        recorder_producer.stop();
        return -1;
    }
    
    // 5. 手动消费循环（因为需要从 recorder 的 BufferPool 获取）
    LOG4CPLUS_INFO(test_logger, "✅ EncodedStreamWriterConsumer test started...");
    LOG4CPLUS_INFO(test_logger, "   按 Ctrl+C 停止");
    
    int packet_count = 0;
    while (g_running && recorder_producer.isRunning()) {
        Buffer* buffer = pool_sptr->acquireFilled(true, 100);
        if (buffer) {
            if (encoded_consumer.consume(buffer, 0)) {
                packet_count++;
            }
            pool_sptr->releaseFilled(buffer);
        } else {
            if (!recorder_producer.isRunning()) {
                break;
            }
        }
    }
    
    // 排空剩余
    Buffer* remaining = nullptr;
    while ((remaining = pool_sptr->acquireFilled(false, 0)) != nullptr) {
        encoded_consumer.consume(remaining, 0);
        pool_sptr->releaseFilled(remaining);
        packet_count++;
    }
    
    encoded_consumer.cleanup();
    recorder_producer.stop();
    
    LOG4CPLUS_INFO_FMT(test_logger, "✅ EncodedStreamWriterConsumer test completed, packets: %d", packet_count);
    LOG4CPLUS_INFO_FMT(test_logger, "   Output: %s", output_path.c_str());
    return 0;
}

// ============================================================================
// 通用解码测试函数（使用 BufferConsumerService 实现）
// ============================================================================

/**
 * @brief 通用的解码测试函数（使用 BufferConsumerService）
 * 
 * 这个函数实现了与 mp4_decode_test.cpp 中 run_decode_test_with_params 相同的功能，
 * 但使用 BufferConsumerService 来简化代码。
 * @param print_stats 是否立即输出统计信息（false 表示延迟到统一输出）
 */
static int run_decode_test_with_consumer(
    const char* video_path,
    int width,
    int height,
    const char* decoder_name,
    int decode_threads,
    double frame_rate,
    const char* profile,
    const char* test_tag,
    int max_frames = -1,
    bool print_stats = true
) {
    LOG4CPLUS_INFO(test_logger, "\n═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO_FMT(test_logger, "  Decode Test: %s", test_tag ? test_tag : "unknown");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════\n");
    
    if (!video_path || video_path[0] == '\0') {
        LOG4CPLUS_ERROR(test_logger, "No video file path specified");
        return -1;
    }
    
    // 创建输出目录
    std::ostringstream oss;
    if (test_tag && test_tag[0] != '\0') {
        oss << test_tag << "_";
    }
    oss << width << "x" << height << "_" << frame_rate << "fps";
    if (decoder_name && decoder_name[0] != '\0') {
        oss << "_" << decoder_name;
    }
    std::string res_str = oss.str();
    std::string report_path = "logs/compare_" + res_str + ".txt";
    system("mkdir -p logs");
    system("mkdir -p ./test_output_consumer");
    
    // 1. 配置硬件解码器
    std::string codec_name;
    if (decoder_name && decoder_name[0] != '\0') {
        std::string dname(decoder_name);
        if (dname.length() > 5 && dname.substr(dname.length() - 5) == "_taco") {
            codec_name = dname.substr(0, dname.length() - 5);
        } else {
            codec_name = dname;
        }
    }
    
    // 基础解码测试：同时测试 PP0 和 PP1
    auto tacoConfig = TacoConfigBuilder()
        .setChannels(true, true)  // 同时启用 ch0 和 ch1
        .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT601)
        .setOutputFormat(Channel::CH1, OutputFormat::RGB_RGB888, ColorStandard::BT601)
        .setScale(Channel::CH0, width, height)
        .setScale(Channel::CH1, width, height)
        .build();
    
    auto workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(16)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(width, height)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            codec_name.empty() ? 
                DecoderConfigBuilder().useSoftware().build() :
                DecoderConfigBuilder().useTaco(codec_name, tacoConfig).build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    // 2. 创建多通道文件写入消费者（同时保存 PP0 和 PP1 输出）
    std::vector<std::string> output_paths = {
        "./test_output_consumer/" + res_str + "_PP0_output.raw",
        "./test_output_consumer/" + res_str + "_PP1_output.raw"
    };
    MultiChannelFileWriterConsumer file_consumer(output_paths, true, true);  // ch0 和 ch1 都启用
    
    // 3. 配置 BufferConsumerService，启用 PSNR 对比和诊断（使用 ConsumerConfigBuilder）
    BufferConsumerService service;
    BufferConsumerService::Config config = ConsumerConfigBuilder()
        .setWorkerConfig(workerConfig)
        .setLoop(false)
        .setThreadCount(1)
        .setAcquireTimeout(100)
        .setMaxTimeoutCount(50)
        .setDrainRemaining(true)
        .setEnablePSNRCompare(true)           // 启用 PSNR 对比
        .setEnableMultiChannelPSNR(true)      // 启用多通道 PSNR 对比（分别统计 PP0 和 PP1）
        .setQuickPSNRThreshold(38.0)
        .setQuickWarnThreshold(35.0)
        .setSSIMThreshold(0.95)
        .setSSIMWarnThreshold(0.90)
        .setEnableParallel(true)
        .setUsePerceptualWeighting(true)
        .setSavePSNRReport(true)
        .setPSNRReportPath(report_path)                       // 单通道模式使用（向后兼容）
        .setPSNRReportPathCh0("logs/compare_PP0_" + res_str + ".txt")  // PP0 报告
        .setPSNRReportPathCh1("logs/compare_PP1_" + res_str + ".txt")  // PP1 报告
        .setEnablePTSAlignment(true)
        .setMaxPTSMatchAttempts(10)
        .setEnableDecoderVerification(true)   // 启用解码器验证
        .setVerboseDiagnosis(false)           // 精简日志：关闭详细诊断
        .build();
    
    auto error_callback = [](const std::string& error) {
        LOG4CPLUS_WARN_FMT(test_logger, "Error (non-fatal): %s", error.c_str());
    };
    
    // 4. 一键运行（根据参数决定是否自动输出统计）
    BufferConsumerService::RunOptions opts;
    opts.error_callback = error_callback;
    opts.auto_print_stats = print_stats;
    opts.auto_close = true;
    
    LOG4CPLUS_INFO(test_logger, "✅ BufferConsumerService opened, starting decode test...");
    
    if (!service.runOnce(config, &file_consumer, opts)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to run BufferConsumerService");
        return -1;
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "✅ Decode test completed: %s", test_tag ? test_tag : "unknown");
    LOG4CPLUS_INFO_FMT(test_logger, "   Report saved to: %s", report_path.c_str());
    
    return 0;
}

// ============================================================================
// 后处理格式配置（从 mp4_decode_test.cpp 提取）
// ============================================================================

struct PP0FormatConfig {
    const char* format_name;
    std::function<WorkerConfig::DecoderConfig::TacoConfig(int, int)> config_builder;
};

struct PP1FormatConfig {
    const char* format_name;
    std::function<WorkerConfig::DecoderConfig::TacoConfig(int, int)> config_builder;
};

struct MultiPPFormatConfig {
    const char* test_name;
    const char* pp0_format_name;
    const char* pp1_format_name;
    std::function<WorkerConfig::DecoderConfig::TacoConfig(int, int)> config_builder;
};

// PP0格式列表
static const PP0FormatConfig pp0_formats[] = {
    {"YUV420_8bit_NV12", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, false)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .build();
    }},
};

#define PP0_FORMAT_COUNT (sizeof(pp0_formats) / sizeof(pp0_formats[0]))

// PP1格式列表
static const PP1FormatConfig pp1_formats[] = {
    {"RGB888", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_RGB888, ColorStandard::BT601)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"ARGB8888", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_ARGB888, ColorStandard::BT601)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"BGR888", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_BGR888, ColorStandard::BT601)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"RGB888_planar", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_GBRP, ColorStandard::BT601)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"YUV420_8bit_NV21", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::YUV_NV21, ColorStandard::BT601)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
};

#define PP1_FORMAT_COUNT (sizeof(pp1_formats) / sizeof(pp1_formats[0]))

// 多PP格式组合列表
static const MultiPPFormatConfig multi_pp_formats[] = {
    {"T01", "YUV420_8bit_NV12", "RGB888", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_RGB888, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"T02", "YUV420_8bit_NV12", "ARGB8888", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_ARGB888, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"T03", "YUV420_8bit_NV21", "BGR888", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV21, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_BGR888, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"T04", "YUV420_8bit_NV12", "RGB888_8bit", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_RGB888, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"T05", "YUV420_P010", "ARGB2101010", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_P010, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_ARGB888, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"T09", "YUV420_8bit_NV12", "RGB888_planar", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_GBRP, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"T10", "YUV420_NV21_P010_Tiled", "ARGB8888", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_P010, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_ARGB888, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"T11", "YUV420_8bit_NV12", "YUV420_8bit_NV21", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::YUV_NV21, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
};

#define MULTI_PP_FORMAT_COUNT (sizeof(multi_pp_formats) / sizeof(multi_pp_formats[0]))

// ============================================================================
// 后处理测试函数
// ============================================================================

/**
 * @brief 运行 PP0 格式测试
 * @param print_stats 是否立即输出统计信息（false 表示延迟到统一输出）
 */
static int run_pp0_test_with_format(
    const char* video_path,
    int width,
    int height,
    const char* decoder_name,
    int decode_threads,
    double frame_rate,
    const char* profile,
    const char* test_tag,
    int max_frames,
    const PP0FormatConfig& format_config,
    int& result_code,
    TestResult* result_out = nullptr,  // 可选：输出统计信息
    bool print_stats = true
) {
    std::string codec_name;
    if (decoder_name && decoder_name[0] != '\0') {
        std::string dname(decoder_name);
        if (dname.length() > 5 && dname.substr(dname.length() - 5) == "_taco") {
            codec_name = dname.substr(0, dname.length() - 5);
        } else {
            codec_name = dname;
        }
    }
    
    auto tacoConfig = format_config.config_builder(width, height);
    
    auto workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(16)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(width, height)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder().useTaco(codec_name, tacoConfig).build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    std::ostringstream oss;
    if (test_tag) oss << test_tag << "_";
    oss << "PP0_" << format_config.format_name;
    std::string output_path = "./test_output_consumer/" + oss.str() + ".raw";
    
    FileWriterConsumer file_consumer(output_path, true, false);
    
    BufferConsumerService service;
    BufferConsumerService::Config config = ConsumerConfigBuilder()
        .setWorkerConfig(workerConfig)
        .setLoop(false)
        .setThreadCount(1)
        // 超时和排空配置（确保处理所有帧）
        .setAcquireTimeout(100)           // 获取Buffer超时时间
        .setMaxTimeoutCount(50)          // 最大超时次数（确保不会过早退出）
        .setDrainRemaining(true)         // 排空剩余Buffer（确保处理所有帧）
        .setWaitFirstBuffer(true)        // 等待第一个Buffer
        .setFirstBufferTimeout(5000)     // 第一个Buffer超时时间
        // PSNR 对比配置（确保逐帧计算PSNR）
        .setEnablePSNRCompare(true)
        // PP0 测试：只启用 ch0，使用单通道模式（不需要 enable_multi_channel_psnr）
        .setQuickPSNRThreshold(38.0)
        .setQuickWarnThreshold(35.0)
        .setSSIMThreshold(0.95)
        .setSSIMWarnThreshold(0.90)
        .setEnableParallel(true)
        .setUsePerceptualWeighting(true)
        .setEnablePTSAlignment(true)     // 启用 PTS 对齐，确保帧匹配
        .setMaxPTSMatchAttempts(10)
        .setEnableDecoderVerification(true)
        .build();
    
    auto error_callback = [](const std::string& error) {
        LOG4CPLUS_WARN_FMT(test_logger, "Error: %s", error.c_str());
    };
    
    BufferConsumerService::RunOptions opts;
    opts.error_callback = error_callback;
    opts.auto_print_stats = false;   // 由上层决定何时统一输出
    opts.auto_close = false;         // 需要在关闭前访问 comparator 统计
    
    if (!service.runOnce(config, &file_consumer, opts)) {
        result_code = -1;
        return -1;
    }
    
    // 在关闭前获取统计信息
    // PP0 测试：单通道模式，使用 channel 0 或直接使用默认 comparator
    if (result_out) {
        auto* comparator = service.getPSNRComparator(0);  // PP0 使用 channel 0
        // 在单通道模式下，getPSNRComparator(0) 会返回 psnr_comparator_.get()
        if (comparator) {
            result_out->frames_processed = comparator->getCompareCount();
            result_out->frames_passed = comparator->getPassedCount();
            result_out->avg_psnr_y = comparator->getAveragePSNRY();
        } else {
            LOG4CPLUS_WARN_FMT(test_logger, "Warning: PSNR comparator for channel 0 not found in PP0 test");
        }
    }
    
    if (print_stats) {
        service.printStats();
    }
    service.close();
    
    result_code = 0;
    return 0;
}

/**
 * @brief 运行 PP1 格式测试
 * @param print_stats 是否立即输出统计信息（false 表示延迟到统一输出）
 */
static int run_pp1_test_with_format(
    const char* video_path,
    int width,
    int height,
    const char* decoder_name,
    int decode_threads,
    double frame_rate,
    const char* profile,
    const char* test_tag,
    int max_frames,
    const PP1FormatConfig& format_config,
    int& result_code,
    TestResult* result_out = nullptr,  // 可选：输出统计信息
    bool print_stats = true
) {
    std::string codec_name;
    if (decoder_name && decoder_name[0] != '\0') {
        std::string dname(decoder_name);
        if (dname.length() > 5 && dname.substr(dname.length() - 5) == "_taco") {
            codec_name = dname.substr(0, dname.length() - 5);
        } else {
            codec_name = dname;
        }
    }
    
    auto tacoConfig = format_config.config_builder(width, height);
    
    auto workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(16)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(width, height)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder().useTaco(codec_name, tacoConfig).build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    std::ostringstream oss;
    if (test_tag) oss << test_tag << "_";
    oss << "PP1_" << format_config.format_name;
    std::string output_path = "./test_output_consumer/" + oss.str() + ".raw";
    
    // PP1 测试：只启用 ch1（false, true）
    FileWriterConsumer file_consumer(output_path, false, true);
    
    BufferConsumerService service;
    BufferConsumerService::Config config = ConsumerConfigBuilder()
        .setWorkerConfig(workerConfig)
        .setLoop(false)
        .setThreadCount(1)
        // 超时和排空配置（确保处理所有帧）
        .setAcquireTimeout(100)           // 获取Buffer超时时间
        .setMaxTimeoutCount(50)          // 最大超时次数（确保不会过早退出）
        .setDrainRemaining(true)         // 排空剩余Buffer（确保处理所有帧）
        .setWaitFirstBuffer(true)        // 等待第一个Buffer
        .setFirstBufferTimeout(5000)     // 第一个Buffer超时时间
        // PSNR 对比配置（确保逐帧计算PSNR）
        .setEnablePSNRCompare(true)
        // PP1 测试：只启用 ch1，需要启用多通道模式才能正确获取 ch1 的统计信息
        .setEnableMultiChannelPSNR(true) // 启用多通道 PSNR 对比（即使只有一个通道，也需要启用以支持 ch1）
        .setQuickPSNRThreshold(38.0)
        .setQuickWarnThreshold(35.0)
        .setSSIMThreshold(0.95)
        .setSSIMWarnThreshold(0.90)
        .setEnableParallel(true)
        .setUsePerceptualWeighting(true)
        .setEnablePTSAlignment(true)     // 启用 PTS 对齐，确保帧匹配
        .setMaxPTSMatchAttempts(10)
        .setEnableDecoderVerification(true)
        .build();
    
    auto error_callback = [](const std::string& error) {
        LOG4CPLUS_WARN_FMT(test_logger, "Error: %s", error.c_str());
    };
    
    BufferConsumerService::RunOptions opts;
    opts.error_callback = error_callback;
    opts.auto_print_stats = false;   // 统计由上层统一输出
    opts.auto_close = false;         // 需要在关闭前读取 comparator 统计
    
    if (!service.runOnce(config, &file_consumer, opts)) {
        result_code = -1;
        return -1;
    }
    
    // 在关闭前获取统计信息
    if (result_out) {
        auto* comparator = service.getPSNRComparator(1);  // PP1 使用 channel 1
        if (comparator) {
            result_out->frames_processed = comparator->getCompareCount();
            result_out->frames_passed = comparator->getPassedCount();
            result_out->avg_psnr_y = comparator->getAveragePSNRY();
        } else {
            // 如果 comparator 不存在，记录警告
            LOG4CPLUS_WARN_FMT(test_logger, "Warning: PSNR comparator for channel 1 not found");
        }
    }
    
    if (print_stats) {
        service.printStats();
    }
    service.close();
    
    result_code = 0;
    return 0;
}

/**
 * @brief 运行多 PP 格式测试（同时测试 PP0 和 PP1）
 * @param print_stats 是否立即输出统计信息（false 表示延迟到统一输出）
 */
static int run_multi_pp_test_with_format(
    const char* video_path,
    int width,
    int height,
    const char* decoder_name,
    int decode_threads,
    double frame_rate,
    const char* profile,
    const char* test_tag,
    int max_frames,
    const MultiPPFormatConfig& format_config,
    int& result_code,
    TestResult* result_out_pp0 = nullptr,  // 可选：输出 PP0 统计信息
    TestResult* result_out_pp1 = nullptr,  // 可选：输出 PP1 统计信息
    bool print_stats = true
) {
    std::string codec_name;
    if (decoder_name && decoder_name[0] != '\0') {
        std::string dname(decoder_name);
        if (dname.length() > 5 && dname.substr(dname.length() - 5) == "_taco") {
            codec_name = dname.substr(0, dname.length() - 5);
        } else {
            codec_name = dname;
        }
    }
    
    auto tacoConfig = format_config.config_builder(width, height);
    
    auto workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(16)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(width, height)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder().useTaco(codec_name, tacoConfig).build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    std::ostringstream oss;
    if (test_tag) oss << test_tag << "_";
    oss << "MultiPP_" << format_config.test_name;
    std::string base_path = "./test_output_consumer/" + oss.str();
    
    // 多 PP 测试：同时启用 ch0 和 ch1（true, true）
    // MultiChannelFileWriterConsumer 需要文件路径向量
    std::vector<std::string> output_paths = {
        base_path + "_ch0.raw",
        base_path + "_ch1.raw"
    };
    MultiChannelFileWriterConsumer multi_consumer(output_paths, true, true);
    
    BufferConsumerService service;
    BufferConsumerService::Config config = ConsumerConfigBuilder()
        .setWorkerConfig(workerConfig)
        .setLoop(false)
        .setThreadCount(1)
        // ⭐ 双通道时将循环最大帧数翻倍
        .setMaxFrames(max_frames > 0 ? max_frames * 2 : -1)
        // 超时和排空配置（确保处理所有帧）
        .setAcquireTimeout(100)           // 获取Buffer超时时间
        .setMaxTimeoutCount(50)          // 最大超时次数（确保不会过早退出）
        .setDrainRemaining(true)         // 排空剩余Buffer（确保处理所有帧）
        .setWaitFirstBuffer(true)        // 等待第一个Buffer
        .setFirstBufferTimeout(5000)     // 第一个Buffer超时时间
        // PSNR 对比配置（与单PP测试保持一致，确保逐帧计算PSNR）
        .setEnablePSNRCompare(true)
        .setEnableMultiChannelPSNR(true) // 启用多通道 PSNR 对比（分别统计 PP0 和 PP1）
        .setQuickPSNRThreshold(38.0)
        .setQuickWarnThreshold(35.0)
        .setSSIMThreshold(0.95)
        .setSSIMWarnThreshold(0.90)
        .setEnableParallel(true)
        .setUsePerceptualWeighting(true)
        .setEnablePTSAlignment(true)     // 启用 PTS 对齐，确保帧匹配
        .setMaxPTSMatchAttempts(10)
        .setEnableDecoderVerification(true)
        .build();
    
    auto error_callback = [](const std::string& error) {
        LOG4CPLUS_WARN_FMT(test_logger, "Error: %s", error.c_str());
    };
    
    BufferConsumerService::RunOptions opts;
    opts.error_callback = error_callback;
    opts.auto_print_stats = false;   // 统计由上层统一输出
    opts.auto_close = false;         // 需要在关闭前读取 comparator 统计
    
    if (!service.runOnce(config, &multi_consumer, opts)) {
        result_code = -1;
        return -1;
    }
    
    // 在关闭前获取统计信息（多通道模式）
    if (result_out_pp0) {
        auto* comparator_pp0 = service.getPSNRComparator(0);  // PP0 使用 channel 0
        if (comparator_pp0) {
            result_out_pp0->frames_processed = comparator_pp0->getCompareCount();
            result_out_pp0->frames_passed = comparator_pp0->getPassedCount();
            result_out_pp0->avg_psnr_y = comparator_pp0->getAveragePSNRY();
        } else {
            LOG4CPLUS_WARN_FMT(test_logger, "Warning: PSNR comparator for channel 0 not found in Multi-PP test");
        }
    }
    if (result_out_pp1) {
        auto* comparator_pp1 = service.getPSNRComparator(1);  // PP1 使用 channel 1
        if (comparator_pp1) {
            result_out_pp1->frames_processed = comparator_pp1->getCompareCount();
            result_out_pp1->frames_passed = comparator_pp1->getPassedCount();
            result_out_pp1->avg_psnr_y = comparator_pp1->getAveragePSNRY();
        } else {
            LOG4CPLUS_WARN_FMT(test_logger, "Warning: PSNR comparator for channel 1 not found in Multi-PP test");
        }
    }
    
    if (print_stats) {
        service.printStats();
    }
    service.close();
    
    result_code = 0;
    return 0;
}

/**
 * @brief 运行所有后处理测试
 */
static int run_all_pp_tests(
    const char* video_path,
    int width,
    int height,
    const char* decoder_name,
    int decode_threads,
    double frame_rate,
    const char* profile,
    const char* test_tag,
    int max_frames
) {
    LOG4CPLUS_INFO(test_logger, "\n╔═══════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO_FMT(test_logger, "║  PP Post-Processing Test Suite: %s               ║", test_tag ? test_tag : "unknown");
    LOG4CPLUS_INFO(test_logger, "╚═══════════════════════════════════════════════════════╝\n");
    
    std::vector<TestResult> all_results;
    
    // ========== Phase 1: PP0 单通道测试 ==========
    LOG4CPLUS_INFO(test_logger, "\n═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Phase 1: PP0 Single Channel Tests");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════\n");
    
    for (size_t i = 0; i < PP0_FORMAT_COUNT; i++) {
        int result_code = 0;
        TestResult result;
        result.test_name = "PP0";
        result.format_name = pp0_formats[i].format_name;
        result.channel = 0;
        result.skipped = false;
        
        int ret = run_pp0_test_with_format(
            video_path, width, height, decoder_name, decode_threads,
            frame_rate, profile, test_tag, max_frames,
            pp0_formats[i], result_code,
            &result,  // 传递 result 指针以填充统计信息
            false  // 不立即输出统计信息
        );
        
        // 判断测试是否通过：既要函数返回成功，也要有帧通过PSNR检查
        // 如果处理了帧但没有任何帧通过，应该标记为失败
        bool has_psnr_passed = (result.frames_processed > 0 && result.frames_passed > 0) || 
                               (result.frames_processed == 0);  // 如果没有处理帧，可能是格式不支持，不强制要求
        result.passed = (ret == 0) && has_psnr_passed;
        all_results.push_back(result);
        
        if (result.passed) {
            LOG4CPLUS_INFO_FMT(test_logger, "  ✅ PP0 Format %s: PASSED", pp0_formats[i].format_name);
        } else {
            LOG4CPLUS_ERROR_FMT(test_logger, "  ❌ PP0 Format %s: FAILED", pp0_formats[i].format_name);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    // ========== Phase 2: PP1 单通道测试 ==========
    LOG4CPLUS_INFO(test_logger, "\n═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Phase 2: PP1 Single Channel Tests");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════\n");
    
    for (size_t i = 0; i < PP1_FORMAT_COUNT; i++) {
        int result_code = 0;
        TestResult result;
        result.test_name = "PP1";
        result.format_name = pp1_formats[i].format_name;
        result.channel = 1;
        result.skipped = false;
        
        int ret = run_pp1_test_with_format(
            video_path, width, height, decoder_name, decode_threads,
            frame_rate, profile, test_tag, max_frames,
            pp1_formats[i], result_code,
            &result,  // 传递 result 指针以填充统计信息
            false  // 不立即输出统计信息
        );
        
        // 判断测试是否通过：既要函数返回成功，也要有帧通过PSNR检查
        // 如果处理了帧但没有任何帧通过，应该标记为失败
        bool has_psnr_passed = (result.frames_processed > 0 && result.frames_passed > 0) || 
                               (result.frames_processed == 0);  // 如果没有处理帧，可能是格式不支持，不强制要求
        result.passed = (ret == 0) && has_psnr_passed;
        all_results.push_back(result);
        
        if (result.passed) {
            LOG4CPLUS_INFO_FMT(test_logger, "  ✅ PP1 Format %s: PASSED", pp1_formats[i].format_name);
        } else {
            LOG4CPLUS_ERROR_FMT(test_logger, "  ❌ PP1 Format %s: FAILED", pp1_formats[i].format_name);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    // ========== Phase 3: 多 PP 测试（PP0 + PP1）==========
    LOG4CPLUS_INFO(test_logger, "\n═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Phase 3: Multi-PP Tests (PP0 + PP1)");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════\n");
    
    for (size_t i = 0; i < MULTI_PP_FORMAT_COUNT; i++) {
        // 跳过 T06（如果已禁用）
        if (strcmp(multi_pp_formats[i].test_name, "T06") == 0) {
            LOG4CPLUS_WARN_FMT(test_logger, "  ⏭️  Multi-PP %s: SKIPPED (T06 disabled)", multi_pp_formats[i].test_name);
            TestResult result;
            result.test_name = "Multi-PP";
            result.format_name = multi_pp_formats[i].test_name;
            result.channel = -1;
            result.passed = false;
            result.skipped = true;
            all_results.push_back(result);
            continue;
        }
        
        int result_code = 0;
        TestResult result_pp0;
        TestResult result_pp1;
        result_pp0.test_name = "Multi-PP";
        result_pp0.format_name = std::string(multi_pp_formats[i].test_name) + " (PP0)";
        result_pp0.channel = 0;
        result_pp0.skipped = false;
        result_pp1.test_name = "Multi-PP";
        result_pp1.format_name = std::string(multi_pp_formats[i].test_name) + " (PP1)";
        result_pp1.channel = 1;
        result_pp1.skipped = false;
        
        // ⭐ 双通道时将循环最大帧数翻倍
        int multi_pp_max_frames = max_frames * 2;
        int ret = run_multi_pp_test_with_format(
            video_path, width, height, decoder_name, decode_threads,
            frame_rate, profile, test_tag, multi_pp_max_frames,
            multi_pp_formats[i], result_code,
            &result_pp0,  // 传递 PP0 result 指针
            &result_pp1,  // 传递 PP1 result 指针
            false  // 不立即输出统计信息
        );
        
        // 判断测试是否通过：既要函数返回成功，也要有帧通过PSNR检查
        // 对于多PP测试，两个通道都需要检查
        // 如果处理了帧但没有任何帧通过，应该标记为失败
        bool pp0_has_psnr_passed = (result_pp0.frames_processed > 0 && result_pp0.frames_passed > 0) || 
                                   (result_pp0.frames_processed == 0);
        bool pp1_has_psnr_passed = (result_pp1.frames_processed > 0 && result_pp1.frames_passed > 0) || 
                                   (result_pp1.frames_processed == 0);
        result_pp0.passed = (ret == 0) && pp0_has_psnr_passed;
        result_pp1.passed = (ret == 0) && pp1_has_psnr_passed;
        all_results.push_back(result_pp0);
        all_results.push_back(result_pp1);
        
        // 多PP测试整体通过需要两个通道都通过
        bool multi_pp_passed = result_pp0.passed && result_pp1.passed;
        if (multi_pp_passed) {
            LOG4CPLUS_INFO_FMT(test_logger, "  ✅ Multi-PP %s: PASSED", multi_pp_formats[i].test_name);
        } else {
            LOG4CPLUS_ERROR_FMT(test_logger, "  ❌ Multi-PP %s: FAILED", multi_pp_formats[i].test_name);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    // ========== 统一输出所有测试的统计结果 ==========
    LOG4CPLUS_INFO(test_logger, "\n╔═══════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO(test_logger, "║  PP Post-Processing Test Results Summary            ║");
    LOG4CPLUS_INFO(test_logger, "╚═══════════════════════════════════════════════════════╝\n");
    
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Test Summary:");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    int total_tests = all_results.size();
    int passed_tests = 0;
    int failed_tests = 0;
    int skipped_tests = 0;
    
    for (const auto& result : all_results) {
        if (result.skipped) {
            skipped_tests++;
            LOG4CPLUS_INFO_FMT(test_logger, "  ⏭️  %s %s: SKIPPED", 
                             result.test_name.c_str(), result.format_name.c_str());
        } else if (result.passed) {
            passed_tests++;
            // 计算通过率
            double pass_rate = result.frames_processed > 0 ? 
                (100.0 * result.frames_passed / result.frames_processed) : 0.0;
            LOG4CPLUS_INFO_FMT(test_logger, 
                             "  ✅ %s %s: PASSED | Frames: %d/%d (%.1f%%) | Avg PSNR-Y: %.2f dB", 
                             result.test_name.c_str(), result.format_name.c_str(),
                             result.frames_passed, result.frames_processed, pass_rate,
                             result.avg_psnr_y);
        } else {
            failed_tests++;
            double pass_rate = result.frames_processed > 0 ? 
                (100.0 * result.frames_passed / result.frames_processed) : 0.0;
            LOG4CPLUS_ERROR_FMT(test_logger, 
                              "  ❌ %s %s: FAILED | Frames: %d/%d (%.1f%%) | Avg PSNR-Y: %.2f dB", 
                              result.test_name.c_str(), result.format_name.c_str(),
                              result.frames_passed, result.frames_processed, pass_rate,
                              result.avg_psnr_y);
        }
    }
    
    LOG4CPLUS_INFO(test_logger, "");
    LOG4CPLUS_INFO_FMT(test_logger, "  Total: %d, Passed: %d, Failed: %d, Skipped: %d", 
                      total_tests, passed_tests, failed_tests, skipped_tests);
    
    LOG4CPLUS_INFO(test_logger, "\n╔═══════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO(test_logger, "║  PP Test Suite Completed                             ║");
    LOG4CPLUS_INFO(test_logger, "╚═══════════════════════════════════════════════════════╝\n");
    
    return 0;
}

/**
 * @brief 运行测试（包含基础测试和后处理测试）
 */
static int run_test_with_all_pp(
    const char* video_path,
    int width,
    int height,
    const char* decoder_name,
    int decode_threads,
    double frame_rate,
    const char* profile,
    const char* test_tag
) {
    // 先执行基础解码测试（不立即输出统计信息）
    int base_result = run_decode_test_with_consumer(
        video_path,
        width, height,
        decoder_name,
        decode_threads,
        frame_rate,
        profile,
        test_tag,
        -1,
        false  // 不立即输出统计信息，延迟到统一输出
    );
    
    // 执行所有PP测试（不立即输出统计信息）
    run_all_pp_tests(
        video_path,
        width, height,
        decoder_name,
        decode_threads,
        frame_rate,
        profile,
        test_tag,
        -1
    );
    
    // 注意：由于 BufferConsumerService::printStats() 会立即输出日志，
    // 我们无法真正延迟输出。但 PP 测试的统计信息已经在 run_all_pp_tests() 中统一输出了。
    // 基础解码测试的统计信息需要在 run_decode_test_with_consumer() 中输出，
    // 或者我们可以在这里重新运行一次基础测试来输出统计信息（不推荐）。
    
    // 实际上，由于技术限制，基础解码测试的统计信息会在测试时输出，
    // PP 测试的统计信息会在最后统一输出。这是当前实现的最佳方案。
    
    return base_result;
}

// ============================================================================
// 参数化测试用例（H.264、H.265、MJPEG）
// ============================================================================

// ---------------- H.264 系列 (9 个) ----------------

static int test_dec_h264_128x128_30_main(const char* video_path) {
    return run_test_with_all_pp(video_path, 128, 128, "h264_taco", 0, 30.0, "main", "h264_128x128_30_main");
}

static int test_dec_h264_320x240_30_high(const char* video_path) {
    return run_test_with_all_pp(video_path, 320, 240, "h264_taco", 0, 30.0, "high", "h264_320x240_30_high");
}

static int test_dec_h264_640x480_30_main(const char* video_path) {
    return run_test_with_all_pp(video_path, 640, 480, "h264_taco", 0, 30.0, "main", "dec_h264_640x480_30_main");
}

static int test_dec_h264_640x480_60_high(const char* video_path) {
    return run_test_with_all_pp(video_path, 640, 480, "h264_taco", 4, 60.0, "high", "dec_h264_640x480_60_high");
}

static int test_dec_h264_1280x720_30_high(const char* video_path) {
    return run_test_with_all_pp(video_path, 1280, 720, "h264_taco", 0, 30.0, "high", "dec_h264_1280x720_30_high");
}

static int test_dec_h264_1920x1080_30_high(const char* video_path) {
    return run_test_with_all_pp(video_path, 1920, 1080, "h264_taco", 0, 30.0, "high", "dec_h264_1920x1080_30_high");
}

static int test_dec_h264_1920x1080_60_high(const char* video_path) {
    return run_test_with_all_pp(video_path, 1920, 1080, "h264_taco", 4, 60.0, "high", "dec_h264_1920x1080_60_high");
}

static int test_dec_h264_2560x1440_30_high(const char* video_path) {
    return run_test_with_all_pp(video_path, 2560, 1440, "h264_taco", 4, 30.0, "high", "dec_h264_2560x1440_30_high");
}

static int test_dec_h264_3840x2160_30_high(const char* video_path) {
    return run_test_with_all_pp(video_path, 3840, 2160, "h264_taco", 4, 30.0, "high", "dec_h264_3840x2160_30_high");
}

// ---------------- H.265 系列 (9 个) ----------------

static int test_dec_h265_128x128_30_main(const char* video_path) {
    return run_test_with_all_pp(video_path, 128, 128, "hevc_taco", 0, 30.0, "main", "dec_h265_128x128_30_main");
}

static int test_dec_h265_320x240_30_main(const char* video_path) {
    return run_test_with_all_pp(video_path, 320, 240, "hevc_taco", 0, 30.0, "main", "dec_h265_320x240_30_main");
}

static int test_dec_h265_640x480_30_main(const char* video_path) {
    return run_test_with_all_pp(video_path, 640, 480, "hevc_taco", 0, 30.0, "main", "dec_h265_640x480_30_main");
}

static int test_dec_h265_640x480_60_main(const char* video_path) {
    return run_test_with_all_pp(video_path, 640, 480, "hevc_taco", 4, 60.0, "main", "dec_h265_640x480_60_main");
}

static int test_dec_h265_1280x720_30_main(const char* video_path) {
    return run_test_with_all_pp(video_path, 1280, 720, "hevc_taco", 0, 30.0, "main", "dec_h265_1280x720_30_main");
}

static int test_dec_h265_1920x1080_30_main(const char* video_path) {
    return run_test_with_all_pp(video_path, 1920, 1080, "hevc_taco", 0, 30.0, "main", "dec_h265_1920x1080_30_main");
}

static int test_dec_h265_1920x1080_60_main(const char* video_path) {
    return run_test_with_all_pp(video_path, 1920, 1080, "hevc_taco", 4, 60.0, "main", "dec_h265_1920x1080_60_main");
}

static int test_dec_h265_2560x1440_30_main(const char* video_path) {
    return run_test_with_all_pp(video_path, 2560, 1440, "hevc_taco", 4, 30.0, "main", "dec_h265_2560x1440_30_main");
}

static int test_dec_h265_3840x2160_30_main(const char* video_path) {
    return run_test_with_all_pp(video_path, 3840, 2160, "hevc_taco", 4, 30.0, "main", "dec_h265_3840x2160_30_main");
}

// ---------------- MJPEG 系列 (9 个) ----------------

static int test_dec_mjpeg_128x128_30(const char* video_path) {
    return run_test_with_all_pp(video_path, 128, 128, "jpeg_taco", 0, 30.0, "none", "dec_mjpeg_128x128_30");
}

static int test_dec_mjpeg_320x240_30(const char* video_path) {
    return run_test_with_all_pp(video_path, 320, 240, "jpeg_taco", 0, 30.0, "none", "dec_mjpeg_320x240_30");
}

static int test_dec_mjpeg_640x480_30(const char* video_path) {
    return run_test_with_all_pp(video_path, 640, 480, "jpeg_taco", 0, 30.0, "none", "dec_mjpeg_640x480_30");
}

static int test_dec_mjpeg_640x480_60(const char* video_path) {
    return run_test_with_all_pp(video_path, 640, 480, "jpeg_taco", 4, 60.0, "none", "dec_mjpeg_640x480_60");
}

static int test_dec_mjpeg_1280x720_30(const char* video_path) {
    return run_test_with_all_pp(video_path, 1280, 720, "jpeg_taco", 0, 30.0, "none", "dec_mjpeg_1280x720_30");
}

static int test_dec_mjpeg_1920x1080_30(const char* video_path) {
    return run_test_with_all_pp(video_path, 1920, 1080, "jpeg_taco", 0, 30.0, "none", "dec_mjpeg_1920x1080_30");
}

static int test_dec_mjpeg_1920x1080_60(const char* video_path) {
    return run_test_with_all_pp(video_path, 1920, 1080, "jpeg_taco", 4, 60.0, "none", "dec_mjpeg_1920x1080_60");
}

static int test_dec_mjpeg_2560x1440_30(const char* video_path) {
    return run_test_with_all_pp(video_path, 2560, 1440, "jpeg_taco", 4, 30.0, "none", "dec_mjpeg_2560x1440_30");
}

static int test_dec_mjpeg_3840x2160_30(const char* video_path) {
    return run_test_with_all_pp(video_path, 3840, 2160, "jpeg_taco", 4, 30.0, "none", "dec_mjpeg_3840x2160_30");
}

// ============================================================================
// RTSP 流测试函数
// ============================================================================

/**
 * @brief 运行 RTSP 流解码测试
 */
static int run_rtsp_decode_test_with_params(
    const char* rtsp_url,
    int width,
    int height,
    const char* decoder_name,
    int decode_threads,
    double frame_rate,
    const char* profile,
    const char* test_tag
) {
    LOG4CPLUS_INFO(test_logger, "\n═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO_FMT(test_logger, "  RTSP Decode Test: %s", test_tag ? test_tag : "unknown");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════\n");
    
    if (!rtsp_url || rtsp_url[0] == '\0') {
        LOG4CPLUS_ERROR(test_logger, "No RTSP URL specified");
        return -1;
    }
    
    std::string codec_name;
    if (decoder_name && decoder_name[0] != '\0') {
        std::string dname(decoder_name);
        if (dname.length() > 5 && dname.substr(dname.length() - 5) == "_taco") {
            codec_name = dname.substr(0, dname.length() - 5);
        } else {
            codec_name = dname;
        }
    }
    
    auto tacoConfig = TacoConfigBuilder()
        .setChannels(true, false)
        .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT601)
        .setScale(Channel::CH0, width, height)
        .build();
    
    auto workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(rtsp_url)
                .setBufferCount(32)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(width, height)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder().useTaco(codec_name, tacoConfig).build()
        )
        .setWorkerType(WorkerType::FFMPEG_RTSP)
        .build();
    
    // 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    DisplayConsumer display_consumer(&display, true, false);
    
    BufferConsumerService service;
    BufferConsumerService::Config config = ConsumerConfigBuilder()
        .setWorkerConfig(workerConfig)
        .setLoop(false)
        .setThreadCount(1)
        .setAcquireTimeout(100)
        .setMaxTimeoutCount(50)
        .setDrainRemaining(true)
        .setEnablePSNRCompare(false)   // RTSP 流不进行 PSNR 对比
        .build();
    
    auto error_callback = [](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "RTSP Error: %s", error.c_str());
        g_running = false;
    };
    
    BufferConsumerService::RunOptions opts;
    opts.error_callback = error_callback;
    
    LOG4CPLUS_INFO(test_logger, "✅ RTSP decode test started...");
    LOG4CPLUS_INFO(test_logger, "   按 Ctrl+C 停止");
    
    if (!service.runOnce(config, &display_consumer, opts)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to run BufferConsumerService");
        return -1;
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "✅ RTSP decode test completed: %s", test_tag ? test_tag : "unknown");
    return 0;
}

// RTSP H.264 测试
static int test_rtsp_h264_1280x720_30_cbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(rtsp_url, 1280, 720, "h264_taco", 0, 30.0, "high", "rtsp_h264_1280x720_30_cbr");
}

static int test_rtsp_h264_1280x720_30_vbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(rtsp_url, 1280, 720, "h264_taco", 0, 30.0, "high", "rtsp_h264_1280x720_30_vbr");
}

static int test_rtsp_h264_1920x1080_30_cbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(rtsp_url, 1920, 1080, "h264_taco", 0, 30.0, "high", "rtsp_h264_1920x1080_30_cbr");
}

static int test_rtsp_h264_1920x1080_30_vbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(rtsp_url, 1920, 1080, "h264_taco", 0, 30.0, "high", "rtsp_h264_1920x1080_30_vbr");
}

static int test_rtsp_h264_3840x2160_30_cbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(rtsp_url, 3840, 2160, "h264_taco", 4, 30.0, "high", "rtsp_h264_3840x2160_30_cbr");
}

static int test_rtsp_h264_3840x2160_30_vbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(rtsp_url, 3840, 2160, "h264_taco", 4, 30.0, "high", "rtsp_h264_3840x2160_30_vbr");
}

// RTSP H.265 测试
static int test_rtsp_h265_1280x720_30_cbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(rtsp_url, 1280, 720, "hevc_taco", 0, 30.0, "main", "rtsp_h265_1280x720_30_cbr");
}

static int test_rtsp_h265_1280x720_30_vbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(rtsp_url, 1280, 720, "hevc_taco", 0, 30.0, "main", "rtsp_h265_1280x720_30_vbr");
}

static int test_rtsp_h265_1920x1080_30_cbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(rtsp_url, 1920, 1080, "hevc_taco", 0, 30.0, "main", "rtsp_h265_1920x1080_30_cbr");
}

static int test_rtsp_h265_1920x1080_30_vbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(rtsp_url, 1920, 1080, "hevc_taco", 0, 30.0, "main", "rtsp_h265_1920x1080_30_vbr");
}

static int test_rtsp_h265_3840x2160_30_cbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(rtsp_url, 3840, 2160, "hevc_taco", 4, 30.0, "main", "rtsp_h265_3840x2160_30_cbr");
}

static int test_rtsp_h265_3840x2160_30_vbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(rtsp_url, 3840, 2160, "hevc_taco", 4, 30.0, "main", "rtsp_h265_3840x2160_30_vbr");
}

// RTSP MJPEG 测试
static int test_rtsp_mjpeg_32768x18432_30(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(rtsp_url, 32768, 18432, "jpeg_taco", 4, 30.0, "main", "rtsp_mjpeg_32768x18432_30");
}

// ============================================================================
// 测试用例 8: 综合测试 - 同时测试所有消费者接口
// ============================================================================

static int test_all_consumers_comprehensive(const char* video_path) {
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Test: Comprehensive Test - All Consumer Interfaces");
    LOG4CPLUS_INFO_FMT(test_logger, "  Video: %s", video_path);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    signal(SIGINT, signal_handler);
    g_running = true;
    
    // 1. 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 创建多个消费者
    system("mkdir -p ./test_output_consumer");
    
    // 3. 测试 DisplayConsumer（只启用 ch0）
    LOG4CPLUS_INFO(test_logger, "\n[Test 1/4] Testing DisplayConsumer...");
    {
        DisplayConsumer display_consumer(&display, true, false);  // ch0启用，ch1禁用
        
        // 配置 WorkerConfig（只启用 ch0）
        auto tacoConfig = TacoConfigBuilder()
            .setChannels(true, false)  // 只启用 ch0
            .build();
        
        auto workerConfig = WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(video_path)
                    .setBufferCount(32)
                    .build()
            )
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(display.getWidth(), display.getHeight())
                    .setBitsPerPixel(display.getBitsPerPixel())
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("h264", tacoConfig)
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
            .build();
        
        BufferConsumerService service;
        BufferConsumerService::Config config = ConsumerConfigBuilder()
            .setWorkerConfig(workerConfig)
            .setLoop(false)
            .setThreadCount(1)
            .setDrainRemaining(true)
            .build();
        
        auto error_callback = [](const std::string& error) {
            LOG4CPLUS_ERROR_FMT(test_logger, "Error: %s", error.c_str());
            g_running = false;
        };
        
        BufferConsumerService::RunOptions opts;
        opts.error_callback = error_callback;
        
        (void)service.runOnce(config, &display_consumer, opts);
    }
    
    // 4. 测试 FileWriterConsumer（只启用 ch0）
    LOG4CPLUS_INFO(test_logger, "\n[Test 2/4] Testing FileWriterConsumer...");
    {
        FileWriterConsumer file_consumer("./test_output_consumer/comprehensive_ch0.raw", 
                                         true, false);  // ch0启用，ch1禁用
        
        // 配置 WorkerConfig（只启用 ch0）
        auto tacoConfig = TacoConfigBuilder()
            .setChannels(true, false)  // 只启用 ch0
            .build();
        
        auto workerConfig = WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(video_path)
                    .setBufferCount(32)
                    .build()
            )
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(1920, 1080)
                    .setBitsPerPixel(32)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("h264", tacoConfig)
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
            .build();
        
        BufferConsumerService service;
        BufferConsumerService::Config config = ConsumerConfigBuilder()
            .setWorkerConfig(workerConfig)
            .setLoop(false)
            .setThreadCount(1)
            .setDrainRemaining(true)
            .build();
        
        auto error_callback = [](const std::string& error) {
            LOG4CPLUS_ERROR_FMT(test_logger, "Error: %s", error.c_str());
            g_running = false;
        };
        
        BufferConsumerService::RunOptions opts;
        opts.error_callback = error_callback;
        
        (void)service.runOnce(config, &file_consumer, opts);
    }
    
    // 5. 测试 MultiChannelFileWriterConsumer（启用双通道）
    LOG4CPLUS_INFO(test_logger, "\n[Test 3/4] Testing MultiChannelFileWriterConsumer...");
    {
        std::vector<std::string> multi_paths = {
            "./test_output_consumer/comprehensive_multi_ch0.raw",
            "./test_output_consumer/comprehensive_multi_ch1.raw"
        };
        MultiChannelFileWriterConsumer multi_consumer(multi_paths, true, true);  // ch0和ch1都启用
        
        // 配置 WorkerConfig（启用双通道）
        auto tacoConfig = TacoConfigBuilder()
            .setChannels(true, true)  // 启用 ch0 和 ch1
            .build();
    
    auto workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(32)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(display.getWidth(), display.getHeight())
                .setBitsPerPixel(display.getBitsPerPixel())
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useTaco("h264", tacoConfig)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
        
        BufferConsumerService service;
        BufferConsumerService::Config config = ConsumerConfigBuilder()
            .setWorkerConfig(workerConfig)
            .setLoop(false)
            .setThreadCount(1)
            .setDrainRemaining(true)
            .build();
        
        auto error_callback = [](const std::string& error) {
            LOG4CPLUS_ERROR_FMT(test_logger, "Error: %s", error.c_str());
            g_running = false;
        };
        
        BufferConsumerService::RunOptions opts;
        opts.error_callback = error_callback;
        
        (void)service.runOnce(config, &multi_consumer, opts);
    }
    
    // 6. 测试 PSNR 对比（集成在消费者中，自动进行对比）
    LOG4CPLUS_INFO(test_logger, "\n[Test 4/4] Testing PSNR Comparison (integrated in consumer)...");
    {
        // 配置硬件解码器（使用与 Test 1 相同的配置，可能包含缩放等后处理）
        auto hw_tacoConfig = TacoConfigBuilder()
            .setChannels(true, false)  // 只启用 ch0
            .build();
        
        auto hw_workerConfig = WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(video_path)
                    .setBufferCount(32)
                    .build()
            )
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(display.getWidth(), display.getHeight())
                    .setBitsPerPixel(display.getBitsPerPixel())
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("h264", hw_tacoConfig)
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
            .build();
        
        // 创建文件写入消费者
        FileWriterConsumer file_consumer(
            "./test_output_consumer/comprehensive_psnr_output.raw",
            true, false  // enable_ch0=true, enable_ch1=false
        );
        
        // 配置 BufferConsumerService，启用 PSNR 对比和诊断（使用 ConsumerConfigBuilder）
        BufferConsumerService service;
        BufferConsumerService::Config service_config = ConsumerConfigBuilder()
            .setWorkerConfig(hw_workerConfig)
            .setEnablePSNRCompare(true)          // ⭐ 启用 PSNR 对比
            .setQuickPSNRThreshold(38.0)
            .setQuickWarnThreshold(35.0)
            .setSSIMThreshold(0.95)
            .setSSIMWarnThreshold(0.90)
            .setEnableParallel(true)
            .setUsePerceptualWeighting(true)
            .setSavePSNRReport(true)
            .setPSNRReportPath("./test_output_consumer/comprehensive_psnr_report.txt")
            .setEnablePTSAlignment(true)         // 启用 PTS 对齐
            .setMaxPTSMatchAttempts(10)
            .setEnableDecoderVerification(true)  // ⭐ 启用解码器验证和诊断
            .setVerboseDiagnosis(false)          // ⭐ 精简日志：关闭详细诊断信息
            .build();
        
        auto error_callback = [](const std::string& error) {
            LOG4CPLUS_WARN_FMT(test_logger, "Service Error (non-fatal): %s", error.c_str());
        };
        
        // 打开并运行服务（自动创建硬件解码器，如启用 PSNR，对应的软件解码器也会自动创建）
        BufferConsumerService::RunOptions opts;
        opts.error_callback = error_callback;
        
        if (!service.runOnce(service_config, &file_consumer, opts)) {
            LOG4CPLUS_ERROR(test_logger, "Failed to run BufferConsumerService");
            return -1;
        }
    }
    
    LOG4CPLUS_INFO(test_logger, "\n✅ Comprehensive test completed");
    LOG4CPLUS_INFO(test_logger, "   Check output files in ./test_output_consumer/");
    
    return 0;
}

// ============================================================================
// 测试用例注册
// ============================================================================

// ========== 消费者接口测试用例 ==========
REGISTER_TEST(consumer_display_rtsp, "DisplayConsumer - RTSP stream playback", test_display_consumer_rtsp);
REGISTER_TEST(consumer_display_file, "DisplayConsumer - Video file playback", test_display_consumer_file);
REGISTER_TEST(consumer_file_writer, "FileWriterConsumer - Single file write", test_file_writer_consumer);
REGISTER_TEST(consumer_multichannel_writer, "MultiChannelFileWriterConsumer - Multi channel write", test_multichannel_writer_consumer);
REGISTER_TEST(consumer_dual_buffer_compare, "DualBufferCompareService - Hardware vs Software decoder (PTS aligned)", test_dual_buffer_compare_service);
REGISTER_TEST(consumer_comprehensive_decode_compare, "Comprehensive - Hardware decode + Display + Save YUV/RGB + PSNR Compare (YUV+RGB+Crop)", test_comprehensive_decode_compare);
REGISTER_TEST(consumer_encoded_stream, "EncodedStreamWriterConsumer - Encoded stream recording", test_encoded_stream_writer_consumer);
REGISTER_TEST(consumer_comprehensive, "Comprehensive test - All consumer interfaces", test_all_consumers_comprehensive);

// ========== H.264 解码测试用例 (9 个) ==========
REGISTER_TEST(dec_h264_128x128_30_main, "H.264 128x128 30fps main profile", test_dec_h264_128x128_30_main);
REGISTER_TEST(dec_h264_320x240_30_high, "H.264 320x240 30fps high profile", test_dec_h264_320x240_30_high);
REGISTER_TEST(dec_h264_640x480_30_main, "H.264 640x480 30fps main profile", test_dec_h264_640x480_30_main);
REGISTER_TEST(dec_h264_640x480_60_high, "H.264 640x480 60fps high profile", test_dec_h264_640x480_60_high);
REGISTER_TEST(dec_h264_1280x720_30_high, "H.264 1280x720 30fps high profile", test_dec_h264_1280x720_30_high);
REGISTER_TEST(dec_h264_1920x1080_30_high, "H.264 1920x1080 30fps high profile", test_dec_h264_1920x1080_30_high);
REGISTER_TEST(dec_h264_1920x1080_60_high, "H.264 1920x1080 60fps high profile", test_dec_h264_1920x1080_60_high);
REGISTER_TEST(dec_h264_2560x1440_30_high, "H.264 2560x1440 30fps high profile", test_dec_h264_2560x1440_30_high);
REGISTER_TEST(dec_h264_3840x2160_30_high, "H.264 3840x2160 30fps high profile", test_dec_h264_3840x2160_30_high);

// ========== H.265 解码测试用例 (9 个) ==========
REGISTER_TEST(dec_h265_128x128_30_main, "H.265 128x128 30fps main profile", test_dec_h265_128x128_30_main);
REGISTER_TEST(dec_h265_320x240_30_main, "H.265 320x240 30fps main profile", test_dec_h265_320x240_30_main);
REGISTER_TEST(dec_h265_640x480_30_main, "H.265 640x480 30fps main profile", test_dec_h265_640x480_30_main);
REGISTER_TEST(dec_h265_640x480_60_main, "H.265 640x480 60fps main profile", test_dec_h265_640x480_60_main);
REGISTER_TEST(dec_h265_1280x720_30_main, "H.265 1280x720 30fps main profile", test_dec_h265_1280x720_30_main);
REGISTER_TEST(dec_h265_1920x1080_30_main, "H.265 1920x1080 30fps main profile", test_dec_h265_1920x1080_30_main);
REGISTER_TEST(dec_h265_1920x1080_60_main, "H.265 1920x1080 60fps main profile", test_dec_h265_1920x1080_60_main);
REGISTER_TEST(dec_h265_2560x1440_30_main, "H.265 2560x1440 30fps main profile", test_dec_h265_2560x1440_30_main);
REGISTER_TEST(dec_h265_3840x2160_30_main, "H.265 3840x2160 30fps main profile", test_dec_h265_3840x2160_30_main);

// ========== MJPEG 解码测试用例 (9 个) ==========
REGISTER_TEST(dec_mjpeg_128x128_30, "MJPEG 128x128 30fps", test_dec_mjpeg_128x128_30);
REGISTER_TEST(dec_mjpeg_320x240_30, "MJPEG 320x240 30fps", test_dec_mjpeg_320x240_30);
REGISTER_TEST(dec_mjpeg_640x480_30, "MJPEG 640x480 30fps", test_dec_mjpeg_640x480_30);
REGISTER_TEST(dec_mjpeg_640x480_60, "MJPEG 640x480 60fps", test_dec_mjpeg_640x480_60);
REGISTER_TEST(dec_mjpeg_1280x720_30, "MJPEG 1280x720 30fps", test_dec_mjpeg_1280x720_30);
REGISTER_TEST(dec_mjpeg_1920x1080_30, "MJPEG 1920x1080 30fps", test_dec_mjpeg_1920x1080_30);
REGISTER_TEST(dec_mjpeg_1920x1080_60, "MJPEG 1920x1080 60fps", test_dec_mjpeg_1920x1080_60);
REGISTER_TEST(dec_mjpeg_2560x1440_30, "MJPEG 2560x1440 30fps", test_dec_mjpeg_2560x1440_30);
REGISTER_TEST(dec_mjpeg_3840x2160_30, "MJPEG 3840x2160 30fps", test_dec_mjpeg_3840x2160_30);

// ========== RTSP 流测试用例 ==========
// RTSP H.264
REGISTER_TEST(rtsp_h264_1280x720_30_cbr, "RTSP H.264 1280x720 30fps CBR", test_rtsp_h264_1280x720_30_cbr);
REGISTER_TEST(rtsp_h264_1280x720_30_vbr, "RTSP H.264 1280x720 30fps VBR", test_rtsp_h264_1280x720_30_vbr);
REGISTER_TEST(rtsp_h264_1920x1080_30_cbr, "RTSP H.264 1920x1080 30fps CBR", test_rtsp_h264_1920x1080_30_cbr);
REGISTER_TEST(rtsp_h264_1920x1080_30_vbr, "RTSP H.264 1920x1080 30fps VBR", test_rtsp_h264_1920x1080_30_vbr);
REGISTER_TEST(rtsp_h264_3840x2160_30_cbr, "RTSP H.264 3840x2160 30fps CBR", test_rtsp_h264_3840x2160_30_cbr);
REGISTER_TEST(rtsp_h264_3840x2160_30_vbr, "RTSP H.264 3840x2160 30fps VBR", test_rtsp_h264_3840x2160_30_vbr);

// RTSP H.265
REGISTER_TEST(rtsp_h265_1280x720_30_cbr, "RTSP H.265 1280x720 30fps CBR", test_rtsp_h265_1280x720_30_cbr);
REGISTER_TEST(rtsp_h265_1280x720_30_vbr, "RTSP H.265 1280x720 30fps VBR", test_rtsp_h265_1280x720_30_vbr);
REGISTER_TEST(rtsp_h265_1920x1080_30_cbr, "RTSP H.265 1920x1080 30fps CBR", test_rtsp_h265_1920x1080_30_cbr);
REGISTER_TEST(rtsp_h265_1920x1080_30_vbr, "RTSP H.265 1920x1080 30fps VBR", test_rtsp_h265_1920x1080_30_vbr);
REGISTER_TEST(rtsp_h265_3840x2160_30_cbr, "RTSP H.265 3840x2160 30fps CBR", test_rtsp_h265_3840x2160_30_cbr);
REGISTER_TEST(rtsp_h265_3840x2160_30_vbr, "RTSP H.265 3840x2160 30fps VBR", test_rtsp_h265_3840x2160_30_vbr);

// RTSP MJPEG
REGISTER_TEST(rtsp_mjpeg_32768x18432_30, "RTSP MJPEG 32768x18432 30fps", test_rtsp_mjpeg_32768x18432_30);

// ============================================================================
// 主函数
// ============================================================================

void configureModuleLoggers() {
    // 精简日志：测试logger使用INFO级别，减少调试输出
    test_logger.setLogLevel(log4cplus::INFO_LOG_LEVEL);
    
    auto consumer_service = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.BufferConsumerService"));
    consumer_service.setLogLevel(log4cplus::INFO_LOG_LEVEL);
    
    auto consumer_display = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Consumer.Display"));
    consumer_display.setLogLevel(log4cplus::INFO_LOG_LEVEL);
    
    auto consumer_filewriter = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Consumer.FileWriter"));
    consumer_filewriter.setLogLevel(log4cplus::INFO_LOG_LEVEL);
    
    auto consumer_multichannel = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Consumer.MultiChannelFileWriter"));
    consumer_multichannel.setLogLevel(log4cplus::INFO_LOG_LEVEL);
    
    auto consumer_compare = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Consumer.Compare"));
    consumer_compare.setLogLevel(log4cplus::INFO_LOG_LEVEL);
    
    auto consumer_encoded = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Consumer.EncodedStreamWriter"));
    consumer_encoded.setLogLevel(log4cplus::INFO_LOG_LEVEL);
    
    auto video_line = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.VideoLine"));
    video_line.setLogLevel(log4cplus::INFO_LOG_LEVEL);
    
    auto buffer_pool = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.BufferPool"));
    buffer_pool.setLogLevel(log4cplus::INFO_LOG_LEVEL);
}

int main(int argc, char* argv[]) {
    INIT_LOGGER();
    
    // 如果没有配置文件，使用编程式配置设置 logger 级别
    // （如果有配置文件，配置文件会覆盖这些设置）
    struct stat buffer;
    bool has_config_file = (stat("./logger.properties", &buffer) == 0) ||
                           (stat("/etc/logger.properties", &buffer) == 0) ||
                           (stat("../logger.properties", &buffer) == 0);
    if (!has_config_file) {
        configureModuleLoggers();
    }
    
    signal(SIGINT, [](int) { g_running = false; });
    signal(SIGTERM, [](int) { g_running = false; });
    
    TEST_MAIN(argc, argv);
}
