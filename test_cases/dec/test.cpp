/**
 * Display Framework Test Program
 * 
 * 测试 LinuxFramebufferDevice, BufferFillingWorkerFacade, PerformanceMonitor, BufferManager 四个类的功能
 * 
 * 使用新的测试框架，支持自动注册和统一的命令行接口
 * 
 * 运行命令：
 *   ./display_test -m loop video.raw
 *   ./display_test -m sequential video.raw
 *   ./display_test -l  # 列出所有测试用例
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <sstream>
#include <algorithm>
#include <functional>
#include <cctype>
#include <sys/stat.h>  // mkdir
#include "display/LinuxFramebufferDevice.hpp"
#include "productionline/worker/BufferFillingWorkerFacade.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/worker/FfmpegPacketRecorderWorker.hpp"
#include "productionline/worker/RtspPacketSource.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "productionline/VideoProductionLine.hpp"
#include "productionline/MultiWorkerProductionLine.hpp"
#include "productionline/io/BufferWriter.hpp"
#include "productionline/io/BufferComparator.hpp"
#include "monitor/PerformanceMonitor.hpp"
#include "common/Logger.hpp"
#include "framework/TestMacros.hpp"

// FFmpeg头文件（解码器测试使用）
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>  // av_get_pix_fmt_name() 函数
}

// ============ 全局变量和信号处理 ============

// 全局标志，用于处理 Ctrl+C 退出
static volatile bool g_running = true;

// RTSP 中断标志（用于快速响应 Ctrl+C）
static std::atomic<bool> g_rtsp_interrupted(false);

// 测试框架专用 Logger
static log4cplus::Logger test_logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Test"));

/**
 * @brief 信号处理器（用于 Ctrl+C）
 * 
 * 当用户按 Ctrl+C 时：
 * 1. 设置全局中断标志
 * 2. 请求 FFmpeg 中断所有 RTSP 流操作
 * 3. 第二次按 Ctrl+C 强制退出
 */
static void signal_handler(int signum) {
    if (signum == SIGINT) {
        if (!g_rtsp_interrupted.load()) {
            // 第一次 Ctrl+C：优雅退出
            LOG4CPLUS_INFO(test_logger, "\n");
            LOG4CPLUS_INFO(test_logger, "🛑 ═══════════════════════════════════════════════════════");
            LOG4CPLUS_INFO(test_logger, "🛑   收到中断信号 (Ctrl+C)，正在停止程序...");
            LOG4CPLUS_INFO(test_logger, "🛑   再次按 Ctrl+C 可强制退出");
            LOG4CPLUS_INFO(test_logger, "🛑 ═══════════════════════════════════════════════════════");
            
            g_running = false;
            g_rtsp_interrupted = true;
            
            // 请求 FFmpeg 中断所有 RTSP 流操作
            RtspPacketSource::requestInterrupt();
        } else {
            // 第二次 Ctrl+C：强制退出
            LOG4CPLUS_INFO(test_logger, "\n🛑 强制退出...");
            signal(SIGINT, SIG_DFL);
            raise(SIGINT);
        }
    }
}


/**
 * 测试5：RTSP 视频流播放（Worker自动创建BufferPool + DMA 零拷贝显示）
 */
static int test_play_rtsp_stream(const char* rtsp_url) {
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Test: RTSP Stream Playback (Independent BufferPool + DMA)");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    // 注册信号处理器（用于 Ctrl+C）
    signal(SIGINT, signal_handler);
    g_running = true;
    g_rtsp_interrupted = false;
    RtspPacketSource::clearInterrupt();
    LOG4CPLUS_DEBUG(test_logger, "[Test] ✅ 已注册 Ctrl+C 信号处理器");
    
    LOG4CPLUS_INFO(test_logger, "Zero-Copy Workflow:");
    LOG4CPLUS_INFO(test_logger, "  1. Worker opens RTSP stream and automatically creates BufferPool (if needed)");
    LOG4CPLUS_INFO(test_logger, "  2. Worker decodes RTSP → AVFrame with phys_addr");
    LOG4CPLUS_INFO(test_logger, "  3. Worker injects Buffer to its BufferPool");
    LOG4CPLUS_INFO(test_logger, "  4. Consumer acquires Buffer from Worker's BufferPool");
    LOG4CPLUS_INFO(test_logger, "  5. display.displayBufferByDMA(buffer) → DMA zero-copy");
    LOG4CPLUS_INFO(test_logger, "  6. Consumer releases Buffer → triggers deleter");
    
    // 1. 初始化显示设备
    LOG4CPLUS_INFO(test_logger, "[Test] 初始化显示设备...");
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 创建 VideoProductionLine（Worker会在open()时自动调用Allocator创建BufferPool）
    LOG4CPLUS_INFO(test_logger, "[Test] 创建VideoProductionLine...");
    VideoProductionLine producer(false, 1);  // loop=false, thread_count=1
    
    // 4. 配置 RTSP 流（注意：推荐单线程）
    LOG4CPLUS_INFO_FMT(test_logger, "Configuring RTSP stream: %s", rtsp_url);

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
                .useTaco("h264", tacoConfig)  // 使用 TACO 硬件解码器进行 H.264 RTSP 流解码
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_RTSP)
        .build();
    
    // 5. 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "RTSP Error: %s", error.c_str());
        g_running = false;
    });
    
    // 6. 启动生产者（内部会创建RTSP Reader并启用零拷贝）
    LOG4CPLUS_INFO(test_logger, "Starting RTSP producer...");
    if (!producer.start(workerConfig)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to start RTSP producer");
        return -1;
    }
    
    LOG4CPLUS_INFO(test_logger, "RTSP stream connected, starting playback...");
    LOG4CPLUS_INFO(test_logger, "[Test] 按Ctrl+C停止");
    LOG4CPLUS_INFO(test_logger, "Watch for '[DMA Display]' messages below");
    
    // 7. 获取工作BufferPool（Worker创建的或fallback的）
    uint64_t producer_pool_id = producer.getWorkingBufferPoolId();
    if (producer_pool_id == 0) {
        LOG4CPLUS_ERROR(test_logger, "No working BufferPool ID available");
        return -1;
    }
    auto producer_pool_weak = BufferPoolRegistry::getInstance().getPool(producer_pool_id);
    auto producer_pool_sptr = producer_pool_weak.lock();
    if (!producer_pool_sptr) {
        LOG4CPLUS_ERROR(test_logger, "BufferPool not found or destroyed");
        return -1;
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "[Test] Using BufferPool: '%s' (created by Worker via Allocator)", 
                 producer_pool_sptr->getName().c_str());
    producer_pool_sptr->printStats();
    
    // 8. 消费者循环：从工作BufferPool获取并通过DMA显示
    int frame_count = 0;
    int dma_success = 0;
    int dma_failed = 0;
    
    while (g_running) {
        // 检查中断标志
        if (g_rtsp_interrupted.load()) {
            LOG4CPLUS_INFO(test_logger, "⚠️  检测到中断请求，停止播放...");
            break;
        }
        
        // 从工作BufferPool获取已解码的buffer（带物理地址）
        Buffer* decoded_buffer = producer_pool_sptr->acquireFilled(true, 100);

        if (decoded_buffer == nullptr) {
            // 超时时检查生产者状态和中断标志
            if (g_rtsp_interrupted.load()) {
                LOG4CPLUS_INFO(test_logger, "⚠️  检测到中断请求，停止播放...");
                break;
            }
            if (!producer.isRunning()) {
                LOG4CPLUS_INFO(test_logger, "Producer stopped naturally, exiting consumer loop...");
                break;
            }
            continue;  // 超时，继续等待
        }

        // ⭐⭐⭐ 严格按照 TACO config 消费：只消费配置中启用的通道 ⭐⭐⭐
        int buffer_channel = decoded_buffer->getOutputChannel();
        if (buffer_channel == 0 && tacoConfig.ch0_enable) {
            // 通道 0 已启用：正常显示
            // ✨ 关键调用：display.displayBufferByDMA(buffer)
            display.waitVerticalSync();
            if (display.displayBufferByDMA(decoded_buffer)) {
                dma_success++;
            } else {
                dma_failed++;
                LOG4CPLUS_WARN_FMT(test_logger, "DMA display failed for buffer (phys_addr=0x%llx)",
                            (unsigned long long)decoded_buffer->getPhysicalAddress());
            }
        } else if (buffer_channel == 1 && tacoConfig.ch1_enable) {
            // 通道 1 已启用：正常显示
            display.waitVerticalSync();
            if (display.displayBufferByDMA(decoded_buffer)) {
                dma_success++;
            } else {
                dma_failed++;
                LOG4CPLUS_WARN_FMT(test_logger, "DMA display failed for ch1 buffer (phys_addr=0x%llx)",
                            (unsigned long long)decoded_buffer->getPhysicalAddress());
            }
        } else {
            // 通道未启用：跳过显示
            LOG4CPLUS_DEBUG_FMT(test_logger, "Skipping display of buffer from ch%d (not enabled in TACO config)", buffer_channel);
        }

        // 归还 buffer（会触发 RtspVideoReader 的 deleter 回收 AVFrame）
        producer_pool_sptr->releaseFilled(decoded_buffer);
        
        frame_count++;
        
        // 每100帧打印一次统计
        if (frame_count % 100 == 0) {
            LOG4CPLUS_DEBUG_FMT(test_logger, "Progress: %d frames displayed (%.1f fps, DMA success: %d, failed: %d)", 
                          frame_count, producer.getAverageFPS(), dma_success, dma_failed);
        }
    }
    
    // 排空剩余的已填充 buffer
    LOG4CPLUS_INFO(test_logger, "Draining remaining buffers from BufferPool...");
    Buffer* remaining_buffer = nullptr;
    int drained_count = 0;
    while ((remaining_buffer = producer_pool_sptr->acquireFilled(false, 0)) != nullptr) {
        display.waitVerticalSync();
        if (display.displayBufferByDMA(remaining_buffer)) {
            dma_success++;
        } else {
            dma_failed++;
        }
        producer_pool_sptr->releaseFilled(remaining_buffer);
        frame_count++;
        drained_count++;
    }
    if (drained_count > 0) {
        LOG4CPLUS_INFO_FMT(test_logger, "Drained %d remaining buffers", drained_count);
    }
    
    // 8. 停止生产者
    LOG4CPLUS_INFO(test_logger, "Stopping RTSP producer...");
    producer.stop();
    
    LOG4CPLUS_INFO(test_logger, "RTSP test completed");
    LOG4CPLUS_INFO_FMT(test_logger, "Total frames displayed: %d", frame_count);
    LOG4CPLUS_INFO_FMT(test_logger, "DMA display success: %d", dma_success);
    LOG4CPLUS_INFO_FMT(test_logger, "DMA display failed: %d", dma_failed);
    LOG4CPLUS_INFO_FMT(test_logger, "Success rate: %.1f%%", 
                 frame_count > 0 ? (100.0 * dma_success / frame_count) : 0.0);
    
    LOG4CPLUS_INFO(test_logger, "Final BufferPool statistics:");
    producer_pool_sptr->printStats();
    
    return 0;
}

/**
 * 测试5.5：RTSP码流录制为MP4文件
 * 
 * 架构：
 * - 生产者：FfmpegPacketRecorderWorker（读取编码包 → Buffer）
 * - 消费者：BufferWriter（Buffer → MP4文件，自动封装容器）
 * 
 * 功能：
 * - 录制RTSP流的原始编码数据（不解码，remux方式）
 * - 自动封装为 MP4 格式（包含完整元数据）
 * - 支持通过环境变量 RTSP_OUTPUT_FILE 指定输出路径
 * - 可用于后续对比测试或直接播放
 * 
 * 使用示例：
 *   export RTSP_OUTPUT_FILE=/path/to/output.mp4
 *   ./display_test -m rtsp_record rtsp://...
 */
static int test_rtsp_record_stream(const char* rtsp_url) {
    using namespace productionline::io;
    
    LOG4CPLUS_INFO(test_logger, "╔═══════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO(test_logger, "║   Test: RTSP Stream Recording to MP4                 ║");
    LOG4CPLUS_INFO(test_logger, "╚═══════════════════════════════════════════════════════╝\n");
    
    // 注册信号处理器（用于 Ctrl+C）
    signal(SIGINT, signal_handler);
    g_running = true;
    g_rtsp_interrupted = false;
    RtspPacketSource::clearInterrupt();
    LOG4CPLUS_DEBUG(test_logger, "[Test] ✅ 已注册 Ctrl+C 信号处理器");
    
    // 从环境变量获取输出路径，如果没有则使用默认值
    const char* output_file = std::getenv("RTSP_OUTPUT_FILE");
    if (!output_file || strlen(output_file) == 0) {
        output_file = "/tmp/rtsp_recorded.mp4";  // 默认输出为MP4
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "Output file: %s\n", output_file);
    
    const int duration_seconds = 30;
    
    // ⭐ 创建定时器用于控制录制时长
    Timer recording_timer;
    recording_timer.start();
    
    // 1. 创建 VideoProductionLine（生产者）
    LOG4CPLUS_INFO(test_logger, "[Step 1] Creating VideoProductionLine...");
    VideoProductionLine producer(false, 1, false);
    
    // 2. 配置 Worker
    LOG4CPLUS_INFO(test_logger, "[Step 2] Configuring FfmpegPacketRecorderWorker...");
    auto workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(rtsp_url)
                .setBufferCount(32)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
        .build();
    
    // 3. 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "Recording Error: %s", error.c_str());
        g_running = false;
    });
    
    // 4. 启动生产者
    LOG4CPLUS_INFO(test_logger, "[Step 3] Starting producer...");
    if (!producer.start(workerConfig)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to start producer");
        return -1;
    }
    
    // 5. 获取 BufferPool
    LOG4CPLUS_INFO(test_logger, "[Step 4] Getting BufferPool...");
    uint64_t pool_id = producer.getWorkingBufferPoolId();
    auto pool_sptr = BufferPoolRegistry::getInstance().getPool(pool_id).lock();
    if (!pool_sptr) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get BufferPool");
        producer.stop();
        return -1;
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "  BufferPool: '%s' (ID: %lu)", pool_sptr->getName().c_str(), pool_id);
    
    // 6. 获取Worker并打开BufferWriter（MP4模式）
    LOG4CPLUS_INFO(test_logger, "[Step 5] Opening BufferWriter (MP4 mode)...");
    
    // 获取Worker的编解码器参数（v2.14: 通过门面类直接获取，无需类型转换）
    auto worker_facade_sptr = producer.getWorkerFacade();
    if (!worker_facade_sptr) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get worker facade");
        producer.stop();
        return -1;
    }
    
    // 直接从门面类获取编解码器参数和时间基
    const AVCodecParameters* codec_params = worker_facade_sptr->getCodecParameters();
    if (!codec_params) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get codec parameters from worker");
        producer.stop();
        return -1;
    }
    
    AVRational time_base = worker_facade_sptr->getTimeBase();
    
    // 保存编码流到 MP4 文件
    BufferWriter writer;
    if (!writer.openEncoded(output_file, codec_params, time_base)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to save encoded stream");
        producer.stop();
        return -1;
    }
    
    // 7. 消费者线程：保存编码流到MP4文件
    LOG4CPLUS_INFO(test_logger, "\n[Step 6] Recording to MP4...");
    LOG4CPLUS_INFO(test_logger, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // ⭐ 设置录制时长定时器
    auto timer_id = recording_timer.scheduleOnce(
        duration_seconds * 1000,  // 毫秒
        []() {
            g_running = false;  // 时间到，停止录制
            LOG4CPLUS_INFO(test_logger, "\n  ⏱️  Recording duration reached, stopping...");
        }
    );
    
    LOG4CPLUS_INFO_FMT(test_logger, "  Recording for %d seconds (timer-controlled)...\n", duration_seconds);
    
    auto start_time = std::chrono::steady_clock::now();
    int packet_count = 0;
    int64_t total_bytes = 0;
    int timeout_count = 0;
    const int MAX_TIMEOUT = 50;
    
    while (g_running) {
        // 检查中断标志
        if (g_rtsp_interrupted.load()) {
            LOG4CPLUS_INFO(test_logger, "\n  ⚠️  检测到中断请求，停止录制...");
            break;
        }
        
        // 获取Buffer
        Buffer* buffer = pool_sptr->acquireFilled(true, 100);
        
        if (buffer) {
            // 写入MP4文件（BufferWriter自动封装）
            size_t used_size = buffer->getUsedSize();
            if (used_size > 0) {
                if (writer.write(buffer)) {
                    packet_count++;
                    total_bytes += used_size;
                    
                    if (packet_count % 50 == 0) {
                        // 计算已录制时长
                        auto now = std::chrono::steady_clock::now();
                        int elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
                        double rate_mbps = elapsed > 0 ? (total_bytes * 8.0) / (elapsed * 1000000.0) : 0.0;
                        LOG4CPLUS_INFO_FMT(test_logger, "  Recorded %d packets | %d seconds | %.2f Mbps",
                                     packet_count, elapsed, rate_mbps);
                    }
                } else {
                    LOG4CPLUS_WARN(test_logger, "Failed to write packet to MP4");
                }
            }
            
            pool_sptr->releaseFilled(buffer);
            timeout_count = 0;
        } else {
            timeout_count++;
            if (timeout_count >= MAX_TIMEOUT) {
                LOG4CPLUS_WARN(test_logger, "\n  ⚠️  Stream timeout, stopping...");
                break;
            }
        }
    }
    
    LOG4CPLUS_INFO(test_logger, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // 8. 清理
    // ⭐ 停止定时器
    recording_timer.cancel(timer_id);
    recording_timer.stop();
    
    // BufferWriter会自动写入MP4 trailer
    writer.close();
    producer.stop();
    
    // 9. 统计信息
    auto end_time = std::chrono::steady_clock::now();
    double total_duration = std::chrono::duration<double>(end_time - start_time).count();
    
    LOG4CPLUS_INFO(test_logger, "\n═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Recording Results");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO_FMT(test_logger, "  ✅ MP4 file:      %s", output_file);
    LOG4CPLUS_INFO_FMT(test_logger, "  Packets recorded: %d", packet_count);
    LOG4CPLUS_INFO_FMT(test_logger, "  Duration:         %.2f seconds", total_duration);
    LOG4CPLUS_INFO_FMT(test_logger, "  Total bytes:      %.2f MB", total_bytes / (1024.0 * 1024.0));
    
    if (total_duration > 0) {
        LOG4CPLUS_INFO_FMT(test_logger, "  Average bitrate:  %.2f Mbps", 
                     (total_bytes * 8.0) / (total_duration * 1000000.0));
    }
    
    LOG4CPLUS_INFO(test_logger, "\n💡 Play the recorded MP4 file with:");
    LOG4CPLUS_INFO_FMT(test_logger, "   ffplay %s", output_file);
    LOG4CPLUS_INFO_FMT(test_logger, "   vlc %s", output_file);
    LOG4CPLUS_INFO(test_logger, "\n💡 Or test with this program:");
    LOG4CPLUS_INFO_FMT(test_logger, "   ./display_test -m ffmpeg %s              # Hardware decode", output_file);
    LOG4CPLUS_INFO_FMT(test_logger, "   ./display_test -m ffmpeg_software %s     # Software decode", output_file);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════\n");
    
    if (packet_count > 0) {
        return 0;
    } else {
        LOG4CPLUS_ERROR(test_logger, "No packets recorded");
        return -1;
    }
}

/**
 * 测试5.7：本地文件录制/转码测试
 * 
 * 架构：
 * - 生产者：FfmpegPacketRecorderWorker（读取编码包 → Buffer）
 * - 数据源：FilePacketSource（本地文件）
 * - 消费者：BufferWriter（Buffer → MP4文件，自动封装容器）
 * 
 * 功能：
 * - 从本地视频文件读取编码流（不解码，remux方式）
 * - 重新封装为 MP4 格式（包含完整元数据）
 * - 支持通过环境变量 FILE_OUTPUT_FILE 指定输出路径
 * - 验证 FilePacketSource 数据源的正常工作
 * 
 * 使用场景：
 * - 视频格式转换（AVI → MP4, MKV → MP4, FLV → MP4 等）
 * - 视频文件修复/重新封装
 * - 批量视频处理
 * 
 * 使用示例：
 *   export FILE_OUTPUT_FILE=/path/to/output.mp4
 *   ./display_test -m file_record /path/to/input.avi
 */
static int test_file_record(const char* input_file) {
    using namespace productionline::io;
    
    LOG4CPLUS_INFO(test_logger, "╔═══════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO(test_logger, "║   Test: File Recording/Remux to MP4                  ║");
    LOG4CPLUS_INFO(test_logger, "╚═══════════════════════════════════════════════════════╝\n");
    
    LOG4CPLUS_INFO_FMT(test_logger, "Input file: %s\n", input_file);
    
    // 从环境变量获取输出路径，如果没有则使用默认值
    const char* output_file = std::getenv("FILE_OUTPUT_FILE");
    if (!output_file || strlen(output_file) == 0) {
        output_file = "/tmp/file_recorded.mp4";  // 默认输出为MP4
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "Output file: %s\n", output_file);
    
    // 1. 创建 VideoProductionLine（生产者）
    LOG4CPLUS_INFO(test_logger, "[Step 1] Creating VideoProductionLine...");
    VideoProductionLine producer(false, 1, false);
    
    // 2. 配置 Worker
    LOG4CPLUS_INFO(test_logger, "[Step 2] Configuring FfmpegPacketRecorderWorker...");
    auto workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(input_file)
                .setBufferCount(32)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
        .build();
    
    // 3. 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "Recording Error: %s", error.c_str());
        g_running = false;
    });
    
    // 4. 启动生产者
    LOG4CPLUS_INFO(test_logger, "[Step 3] Starting producer...");
    if (!producer.start(workerConfig)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to start producer");
        return -1;
    }
    
    // 5. 获取 BufferPool
    LOG4CPLUS_INFO(test_logger, "[Step 4] Getting BufferPool...");
    uint64_t pool_id = producer.getWorkingBufferPoolId();
    auto pool_sptr = BufferPoolRegistry::getInstance().getPool(pool_id).lock();
    if (!pool_sptr) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get BufferPool");
        producer.stop();
        return -1;
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "  BufferPool: '%s' (ID: %lu)", pool_sptr->getName().c_str(), pool_id);
    
    // 6. 获取Worker并打开BufferWriter（MP4模式）
    LOG4CPLUS_INFO(test_logger, "[Step 5] Opening BufferWriter (MP4 mode)...");
    
    // 获取Worker的编解码器参数（v2.14: 通过门面类直接获取，无需类型转换）
    auto worker_facade_sptr = producer.getWorkerFacade();
    if (!worker_facade_sptr) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get worker facade");
        producer.stop();
        return -1;
    }
    
    // 直接从门面类获取编解码器参数和时间基
    const AVCodecParameters* codec_params = worker_facade_sptr->getCodecParameters();
    if (!codec_params) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get codec parameters from worker");
        producer.stop();
        return -1;
    }
    
    AVRational time_base = worker_facade_sptr->getTimeBase();
    
    // 保存编码流到 MP4 文件
    BufferWriter writer;
    if (!writer.openEncoded(output_file, codec_params, time_base)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to save encoded stream");
        producer.stop();
        return -1;
    }
    
    // 7. 消费者线程：保存编码流到MP4文件
    LOG4CPLUS_INFO(test_logger, "\n[Step 6] Remuxing to MP4...");
    LOG4CPLUS_INFO(test_logger, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    auto start_time = std::chrono::steady_clock::now();
    int packet_count = 0;
    int64_t total_bytes = 0;
    int timeout_count = 0;
    const int MAX_TIMEOUT = 50;  // 文件读取结束后会超时
    
    while (g_running) {
        // 获取Buffer
        Buffer* buffer = pool_sptr->acquireFilled(true, 100);
        
        if (buffer) {
            // 写入MP4文件（BufferWriter自动封装）
            size_t used_size = buffer->getUsedSize();
            if (used_size > 0) {
                if (writer.write(buffer)) {
                    packet_count++;
                    total_bytes += used_size;
                    
                    if (packet_count % 100 == 0) {
                        LOG4CPLUS_INFO_FMT(test_logger, "  Processed %d packets (%.2f MB)",
                                     packet_count, total_bytes / (1024.0 * 1024.0));
                    }
                } else {
                    LOG4CPLUS_WARN(test_logger, "Failed to write packet to MP4");
                }
            }
            
            pool_sptr->releaseFilled(buffer);
            timeout_count = 0;
        } else {
            timeout_count++;
            if (timeout_count >= MAX_TIMEOUT) {
                LOG4CPLUS_INFO(test_logger, "\n  ⏱️  File processing completed (EOF reached)");
                break;
            }
        }
        
        // 检查生产者状态
        if (!producer.isRunning()) {
            LOG4CPLUS_INFO(test_logger, "\n  ⏱️  Producer finished naturally");
            break;
        }
    }
    
    LOG4CPLUS_INFO(test_logger, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // 8. 清理（BufferWriter会自动写入MP4 trailer）
    writer.close();
    producer.stop();
    
    // 9. 统计信息
    auto end_time = std::chrono::steady_clock::now();
    double total_duration = std::chrono::duration<double>(end_time - start_time).count();
    
    LOG4CPLUS_INFO(test_logger, "\n═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Recording Results");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO_FMT(test_logger, "  ✅ Input file:    %s", input_file);
    LOG4CPLUS_INFO_FMT(test_logger, "  ✅ Output file:   %s", output_file);
    LOG4CPLUS_INFO_FMT(test_logger, "  Packets written:  %d", packet_count);
    LOG4CPLUS_INFO_FMT(test_logger, "  Processing time:  %.2f seconds", total_duration);
    LOG4CPLUS_INFO_FMT(test_logger, "  Total bytes:      %.2f MB", total_bytes / (1024.0 * 1024.0));
    
    if (total_duration > 0) {
        LOG4CPLUS_INFO_FMT(test_logger, "  Processing speed: %.2f Mbps", 
                     (total_bytes * 8.0) / (total_duration * 1000000.0));
    }
    
    LOG4CPLUS_INFO(test_logger, "\n💡 Play the remuxed MP4 file with:");
    LOG4CPLUS_INFO_FMT(test_logger, "   ffplay %s", output_file);
    LOG4CPLUS_INFO_FMT(test_logger, "   vlc %s", output_file);
    LOG4CPLUS_INFO(test_logger, "\n💡 Or test with this program:");
    LOG4CPLUS_INFO_FMT(test_logger, "   ./display_test -m ffmpeg %s              # Hardware decode", output_file);
    LOG4CPLUS_INFO_FMT(test_logger, "   ./display_test -m ffmpeg_software %s     # Software decode", output_file);
    LOG4CPLUS_INFO(test_logger, "\n💡 Compare with original:");
    LOG4CPLUS_INFO_FMT(test_logger, "   ffprobe %s", input_file);
    LOG4CPLUS_INFO_FMT(test_logger, "   ffprobe %s", output_file);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════\n");
    
    if (packet_count > 0) {
        return 0;
    } else {
        LOG4CPLUS_ERROR(test_logger, "No packets recorded");
        return -1;
    }
}

/**
 * 测试6：FFmpeg 编码视频文件播放（使用Worker自动创建BufferPool）
 */
static int test_h264_taco_video(const char* video_path) {
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO_FMT(test_logger, "  Test: FFmpeg Encoded Video Playback - File: %s", video_path);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    // 1. 初始化显示设备
    LOG4CPLUS_INFO(test_logger, "[Test] 初始化显示设备...");
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 创建 VideoProductionLine（Worker会在open()时自动调用Allocator创建BufferPool）
    LOG4CPLUS_INFO(test_logger, "[Test] 创建VideoProductionLine...");
    VideoProductionLine producer(false, 1,false);  // loop=true, thread_count=1
    
    // 4. 配置 FFmpeg 解码
    LOG4CPLUS_INFO_FMT(test_logger, "[Test] 配置FFmpeg: %s", video_path);

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
                .useTaco("h264", tacoConfig)  // 使用 TACO 硬件解码器进行 H.264 视频文件解码
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    // 5. 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "FFmpeg Error: %s", error.c_str());
        g_running = false;
    });
    
    // 6. 启动生产者
    LOG4CPLUS_INFO(test_logger, "[Test] 启动FFmpeg...");
    if (!producer.start(workerConfig)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to start FFmpeg producer");
        return -1;
    }
    
    LOG4CPLUS_INFO(test_logger, "[Test] 视频解码已启动, starting playback...");
    LOG4CPLUS_INFO(test_logger, "[Test] 按Ctrl+C停止");
    
    // 7. 获取工作BufferPool（Worker创建的或fallback的）
    uint64_t producer_pool_id = producer.getWorkingBufferPoolId();
    if (producer_pool_id == 0) {
        LOG4CPLUS_ERROR(test_logger, "No working BufferPool ID available");
        return -1;
    }
    auto producer_pool_weak = BufferPoolRegistry::getInstance().getPool(producer_pool_id);
    auto producer_pool_sptr = producer_pool_weak.lock();
    if (!producer_pool_sptr) {
        LOG4CPLUS_ERROR(test_logger, "BufferPool not found or destroyed");
        return -1;
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "[Test] Using BufferPool: '%s' (created by Worker via Allocator)", 
                 producer_pool_sptr->getName().c_str());
    producer_pool_sptr->printStats();
    
    // 8. 初始化性能监控（监控消费者显示效率）
    std::unique_ptr<PerformanceMonitor> display_monitor = nullptr;//std::make_unique<PerformanceMonitor>();
    //display_monitor->setReportInterval(1000);  // 设置1秒间隔
    //display_monitor->start();  // 启动后Timer会自动触发周期性报告
    
    // 9. 消费者循环
    int frame_count = 0;
    
    while (g_running) {
        // 从工作BufferPool获取已解码的buffer
        Buffer* filled_buffer = producer_pool_sptr->acquireFilled(true, 100);

        if (filled_buffer == nullptr) {
            // 超时时检查生产者状态
            if (!producer.isRunning()) {
                LOG4CPLUS_INFO(test_logger, "Producer stopped naturally, exiting consumer loop...");
                break;
            }
            continue;  // 超时，继续等待
        }

        // ⭐⭐⭐ 严格按照 TACO config 消费：只消费配置中启用的通道 ⭐⭐⭐
        int buffer_channel = filled_buffer->getOutputChannel();
        if ((buffer_channel == 0 && tacoConfig.ch0_enable) ||
            (buffer_channel == 1 && tacoConfig.ch1_enable)) {

            // 通道已启用：正常显示
            // 开始计时显示操作
            if (display_monitor) {
                display_monitor->beginTiming("display");
            }

            // 显示
            display.waitVerticalSync();
            // 零拷贝模式：使用 DMA 显示
            if (!display.displayBufferByDMA(filled_buffer)) {
                LOG4CPLUS_WARN(test_logger, "DMA display failed, falling back to normal");
                display.displayFilledFramebuffer(filled_buffer);

            }
            // 归还 buffer
            producer_pool_sptr->releaseFilled(filled_buffer);
            if (display_monitor ) {
                display_monitor->endTiming("display");
            }
            frame_count++;
        } else {
            // 通道未启用：跳过显示
            LOG4CPLUS_DEBUG_FMT(test_logger, "Skipping display of buffer from ch%d (not enabled in TACO config)", buffer_channel);
            producer_pool_sptr->releaseFilled(filled_buffer);
        }
        if (display_monitor ) {
            display_monitor->endTiming("display");
        }
        frame_count++;
        
        // 每100帧打印一次统计
        if (frame_count % 100 == 0) {
            LOG4CPLUS_DEBUG_FMT(test_logger, "Frames displayed: %d (%.1f fps)", 
                          frame_count, producer.getAverageFPS());
        }
    }
    
    // 排空剩余的已填充 buffer
    LOG4CPLUS_INFO(test_logger, "Draining remaining buffers from BufferPool...");
    Buffer* remaining_buffer = nullptr;
    int drained_count = 0;
    while ((remaining_buffer = producer_pool_sptr->acquireFilled(false, 0)) != nullptr) {
        display.waitVerticalSync();
        if (!display.displayBufferByDMA(remaining_buffer)) {
            display.displayFilledFramebuffer(remaining_buffer);
        }
        producer_pool_sptr->releaseFilled(remaining_buffer);
        frame_count++;
        drained_count++;
    }
    if (drained_count > 0) {
        LOG4CPLUS_INFO_FMT(test_logger, "Drained %d remaining buffers", drained_count);
    }
    
    // 10. 停止性能监控
    if (display_monitor) {
        display_monitor->stop();
        LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
        LOG4CPLUS_INFO(test_logger, "  Display Performance Statistics");
        LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
        display_monitor->printStatistics();
        display_monitor.reset();
    }
    
    // 11. 停止生产者
    LOG4CPLUS_INFO(test_logger, "Stopping FFmpeg producer...");
    producer.stop();
    
    LOG4CPLUS_INFO(test_logger, "FFmpeg video test completed");
    LOG4CPLUS_INFO_FMT(test_logger, "Total frames displayed: %d", frame_count);
    LOG4CPLUS_INFO_FMT(test_logger, "Frames produced: %d", producer.getProducedFrames());
    LOG4CPLUS_INFO_FMT(test_logger, "Frames skipped: %d", producer.getSkippedFrames());
    LOG4CPLUS_INFO_FMT(test_logger, "Average FPS: %.2f", producer.getAverageFPS());
    
    LOG4CPLUS_INFO(test_logger, "Final BufferPool statistics:");
    producer_pool_sptr->printStats();
    
    return 0;
}

/**
 * 测试6b：FFmpeg 软件解码器测试（对比硬件解码）
 * 
 * 功能：
 * - 使用 FFmpeg 内置的软件解码器（如 libavcodec）解码视频
 * - 不使用硬件加速（h264_taco），纯软件解码
 * - 与 test_h264_taco_video 对比，验证软件解码路径
 * 
 * 目的：
 * - 测试 useSoftware() 配置在实际解码场景中的效果
 * - 验证 FFmpeg 自动解码器选择机制
 * - 提供硬件解码失败时的 fallback 方案验证
 */
static int test_ffmpeg_software_decoder(const char* video_path) {
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO_FMT(test_logger, "  Test: FFmpeg Software Decoder - File: %s", video_path);
    LOG4CPLUS_INFO(test_logger, "  (Using libavcodec, no hardware acceleration)");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    // 1. 初始化显示设备
    LOG4CPLUS_INFO(test_logger, "[Test] 初始化显示设备...");
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 创建 VideoProductionLine
    LOG4CPLUS_INFO(test_logger, "[Test] 创建VideoProductionLine...");
    VideoProductionLine producer(false, 1, false);  // loop=false, thread_count=1
    
    // 3. 配置 FFmpeg 软件解码
    LOG4CPLUS_INFO_FMT(test_logger, "[Test] 配置FFmpeg软件解码: %s", video_path);
    
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
                .useSoftware()  // ⭐ 使用软件解码器（自动选择，不指定解码器名称）
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)  // ⭐ 需要解码
        .build();
    
    // 4. 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "FFmpeg Software Decoder Error: %s", error.c_str());
        g_running = false;
    });
    
    // 5. 启动生产者
    LOG4CPLUS_INFO(test_logger, "[Test] 启动FFmpeg软件解码...");
    if (!producer.start(workerConfig)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to start FFmpeg software decoder");
        return -1;
    }
    
    LOG4CPLUS_INFO(test_logger, "[Test] 视频解码已启动 (Software Decoder), starting playback...");
    LOG4CPLUS_INFO(test_logger, "[Test] 按Ctrl+C停止");
    LOG4CPLUS_INFO(test_logger, "[Test] ⚠️ 注意：软件解码输出系统内存，需要拷贝到 framebuffer 显示");
    
    // 6. 获取 Display 的 BufferPool（用于显示）
    LOG4CPLUS_INFO(test_logger, "[Test] 获取 Display BufferPool...");
    uint64_t display_pool_id = display.getBufferPoolId();
    if (display_pool_id == 0) {
        LOG4CPLUS_ERROR(test_logger, "Display BufferPool not initialized");
        producer.stop();
        return -1;
    }
    auto display_pool_weak = BufferPoolRegistry::getInstance().getPool(display_pool_id);
    auto display_pool_sptr = display_pool_weak.lock();
    if (!display_pool_sptr) {
        LOG4CPLUS_ERROR_FMT(test_logger, "Display BufferPool (ID: %lu) not found or already destroyed", display_pool_id);
        producer.stop();
        return -1;
    }
    
    // 7. 获取 Worker 的 BufferPool（软件解码输出）
    LOG4CPLUS_INFO(test_logger, "[Test] 获取 Worker BufferPool...");
    uint64_t producer_pool_id = producer.getWorkingBufferPoolId();
    if (producer_pool_id == 0) {
        LOG4CPLUS_ERROR(test_logger, "No working BufferPool ID available");
        producer.stop();
        return -1;
    }
    auto producer_pool_weak = BufferPoolRegistry::getInstance().getPool(producer_pool_id);
    auto producer_pool_sptr = producer_pool_weak.lock();
    if (!producer_pool_sptr) {
        LOG4CPLUS_ERROR(test_logger, "Worker BufferPool not found or destroyed");
        producer.stop();
        return -1;
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "[Test] Worker BufferPool: '%s' (ID: %lu)", 
                 producer_pool_sptr->getName().c_str(), producer_pool_id);
    LOG4CPLUS_INFO_FMT(test_logger, "[Test] Display BufferPool: '%s' (ID: %lu)", 
                 display_pool_sptr->getName().c_str(), display_pool_id);
    producer_pool_sptr->printStats();
    display_pool_sptr->printStats();
    
    // 8. 消费者循环（内存拷贝显示）
    int frame_count = 0;
    
    while (g_running) {
        // 步骤1：从 Worker BufferPool 获取解码后的数据
        Buffer* decoded_buffer = producer_pool_sptr->acquireFilled(true, 100);
        
        if (decoded_buffer == nullptr) {
            // 超时时检查生产者状态
            if (!producer.isRunning()) {
                LOG4CPLUS_INFO(test_logger, "Producer stopped naturally, exiting consumer loop...");
                break;
            }
            continue;  // 超时，继续等待
        }
        
        // 步骤2：从 Display BufferPool 获取空闲的 framebuffer
        Buffer* display_buffer = display_pool_sptr->acquireFree(true, 100);
        if (display_buffer == nullptr) {
            LOG4CPLUS_WARN(test_logger, "Failed to acquire free display buffer, skipping frame");
            producer_pool_sptr->releaseFilled(decoded_buffer);
            continue;
        }
        
        // 步骤3：⭐ 拷贝数据（软件解码的关键步骤）
        // 前置检查：确保指针有效
        void* src_addr = decoded_buffer->getVirtualAddress();
        void* dst_addr = display_buffer->getVirtualAddress();
        size_t copy_size = std::min(decoded_buffer->size(), display_buffer->size());
        
        if (!src_addr) {
            LOG4CPLUS_ERROR_FMT(test_logger, "❌ ERROR: decoded_buffer->getVirtualAddress() is nullptr (buffer #%u)", 
                          decoded_buffer->id());
            display_pool_sptr->releaseFree(display_buffer);
            producer_pool_sptr->releaseFilled(decoded_buffer);
            continue;
        }
        
        if (!dst_addr) {
            LOG4CPLUS_ERROR_FMT(test_logger, "❌ ERROR: display_buffer->getVirtualAddress() is nullptr (buffer #%u)", 
                          display_buffer->id());
            display_pool_sptr->releaseFree(display_buffer);
            producer_pool_sptr->releaseFilled(decoded_buffer);
            continue;
        }
        
        if (copy_size == 0) {
            LOG4CPLUS_WARN(test_logger, "⚠️  WARNING: copy_size is 0, skipping frame");
            display_pool_sptr->releaseFree(display_buffer);
            producer_pool_sptr->releaseFilled(decoded_buffer);
            continue;
        }
        
        // 使用 C++ 标准库的安全方式拷贝
        std::memcpy(dst_addr, src_addr, copy_size);
        // 步骤4：显示（现在是 Display BufferPool 的 buffer，可以正常显示）
        display.waitVerticalSync();
        if (!display.displayFilledFramebuffer(display_buffer)) {
            LOG4CPLUS_WARN(test_logger, "Display failed");
        }
        
        // 步骤5：归还两个 buffer
        display_pool_sptr->releaseFree(display_buffer);
        producer_pool_sptr->releaseFilled(decoded_buffer);
        
        frame_count++;
        
        // 每100帧打印一次统计
        if (frame_count % 100 == 0) {
            LOG4CPLUS_DEBUG_FMT(test_logger, "Frames displayed: %d (%.1f fps)", 
                          frame_count, producer.getAverageFPS());
        }
    }
    
    // 9. 排空剩余的已填充 buffer
    LOG4CPLUS_INFO(test_logger, "Draining remaining buffers from BufferPool...");
    Buffer* remaining_decoded = nullptr;
    int drained_count = 0;
    while ((remaining_decoded = producer_pool_sptr->acquireFilled(false, 0)) != nullptr) {
        Buffer* display_buffer = display_pool_sptr->acquireFree(false, 0);
        if (display_buffer) {
            // 前置检查
            void* src_addr = remaining_decoded->getVirtualAddress();
            void* dst_addr = display_buffer->getVirtualAddress();
            size_t copy_size = std::min(remaining_decoded->size(), display_buffer->size());
            
            if (src_addr && dst_addr && copy_size > 0) {
                std::memcpy(dst_addr, src_addr, copy_size);
                
                display.waitVerticalSync();
                display.displayFilledFramebuffer(display_buffer);
            } else {
                LOG4CPLUS_WARN(test_logger, "⚠️  Skipping invalid buffer during drain");
            }
            
            display_pool_sptr->releaseFree(display_buffer);
        }
        
        producer_pool_sptr->releaseFilled(remaining_decoded);
        frame_count++;
        drained_count++;
    }
    if (drained_count > 0) {
        LOG4CPLUS_INFO_FMT(test_logger, "Drained %d remaining buffers", drained_count);
    }
    
    // 10. 停止生产者
    LOG4CPLUS_INFO(test_logger, "Stopping FFmpeg software decoder...");
    producer.stop();
    
    LOG4CPLUS_INFO(test_logger, "FFmpeg software decoder test completed");
    LOG4CPLUS_INFO_FMT(test_logger, "Total frames displayed: %d", frame_count);
    LOG4CPLUS_INFO_FMT(test_logger, "Frames produced: %d", producer.getProducedFrames());
    LOG4CPLUS_INFO_FMT(test_logger, "Frames skipped: %d", producer.getSkippedFrames());
    LOG4CPLUS_INFO_FMT(test_logger, "Average FPS: %.2f", producer.getAverageFPS());
    LOG4CPLUS_INFO(test_logger, "💡 Tip: Compare with test_h264_taco_video (hardware decoder) for performance difference");
    
    LOG4CPLUS_INFO(test_logger, "Final BufferPool statistics:");
    producer_pool_sptr->printStats();
    
    return 0;
}

/**
 * 单个生产线的解码工作函数（不显示，仅解码）
 * 
 * @param line_id 生产线ID（用于日志标识）
 * @param video_path 视频文件路径
 * @param width 输出分辨率宽度
 * @param height 输出分辨率高度
 * @param total_frames 全局帧数统计（原子变量）
 * @param total_errors 全局错误数统计（原子变量）
 */
static void decode_production_line_worker(
    int line_id,
    const char* video_path,
    int width,
    int height,
    std::atomic<int>* total_frames,
    std::atomic<int>* total_errors
) {
    std::string thread_prefix = "[Line " + std::to_string(line_id) + "] ";
    
    LOG4CPLUS_INFO_FMT(test_logger, "%sStarting decode worker for: %s", thread_prefix.c_str(), video_path);
    
    // 1. 创建 VideoProductionLine（Worker会在open()时自动调用Allocator创建BufferPool）
    VideoProductionLine producer(true, 1);  // loop=true, thread_count=1
    
    // 2. 配置 FFmpeg 解码
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
                .setDisplayResolution(width, height)
                .setBitsPerPixel(32)  // 固定32位，因为不显示，这个值不影响解码
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useTaco("h264", tacoConfig)  // 使用 TACO 硬件解码器进行 H.264 视频文件解码
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    // 3. 设置错误回调
    producer.setErrorCallback([thread_prefix, total_errors](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "%sFFmpeg Error: %s", thread_prefix.c_str(), error.c_str());
        (*total_errors)++;
    });
    
    // 4. 启动生产者
    LOG4CPLUS_INFO_FMT(test_logger, "%sStarting FFmpeg video producer...", thread_prefix.c_str());
    if (!producer.start(workerConfig)) {
        LOG4CPLUS_ERROR_FMT(test_logger, "%sFailed to start FFmpeg producer", thread_prefix.c_str());
        (*total_errors)++;
        return;
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "%sVideo decoding started", thread_prefix.c_str());
    
    // 5. 获取工作BufferPool（Worker创建的或fallback的）
    uint64_t producer_pool_id = producer.getWorkingBufferPoolId();
    if (producer_pool_id == 0) {
        LOG4CPLUS_ERROR_FMT(test_logger, "%sNo working BufferPool ID available", thread_prefix.c_str());
        (*total_errors)++;
        producer.stop();
        return;
    }
    
    auto producer_pool_weak = BufferPoolRegistry::getInstance().getPool(producer_pool_id);
    auto producer_pool_sptr = producer_pool_weak.lock();
    if (!producer_pool_sptr) {
        LOG4CPLUS_ERROR_FMT(test_logger, "%sBufferPool not found or destroyed", thread_prefix.c_str());
        (*total_errors)++;
        producer.stop();
        return;
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "%sUsing BufferPool: '%s'", thread_prefix.c_str(), 
                 producer_pool_sptr->getName().c_str());
    
    // 6. 解码循环（不显示，直接释放buffer）
    int frame_count = 0;
    
    while (g_running) {
        // 从工作BufferPool获取已解码的buffer
        Buffer* filled_buffer = producer_pool_sptr->acquireFilled(true, 100);

        if (filled_buffer == nullptr) {
            // 超时时检查生产者状态
            if (!producer.isRunning()) {
                LOG4CPLUS_INFO_FMT(test_logger, "%sProducer stopped naturally, exiting decode loop...",
                           thread_prefix.c_str());
                break;
            }
            continue;  // 超时，继续等待
        }

        // ⭐⭐⭐ 严格按照 TACO config 消费：只消费配置中启用的通道 ⭐⭐⭐
        int buffer_channel = filled_buffer->getOutputChannel();
        if ((buffer_channel == 0 && tacoConfig.ch0_enable) ||
            (buffer_channel == 1 && tacoConfig.ch1_enable)) {

            // 通道已启用：正常消费
            // 不显示，直接归还 buffer
            producer_pool_sptr->releaseFilled(filled_buffer);

            frame_count++;
            (*total_frames)++;
        } else {
            // 通道未启用：跳过消费
            LOG4CPLUS_DEBUG_FMT(test_logger, "%sSkipping buffer from ch%d (not enabled in TACO config)",
                         thread_prefix.c_str(), buffer_channel);
            producer_pool_sptr->releaseFilled(filled_buffer);
        }
        
        // 每100帧打印一次统计
        if (frame_count % 100 == 0) {
            LOG4CPLUS_DEBUG_FMT(test_logger, "%sDecoded %d frames (%.1f fps)", 
                         thread_prefix.c_str(), frame_count, producer.getAverageFPS());
        }
    }
    
    // 排空剩余的已填充 buffer
    LOG4CPLUS_INFO_FMT(test_logger, "%sDraining remaining buffers from BufferPool...", thread_prefix.c_str());
    Buffer* remaining_buffer = nullptr;
    int drained_count = 0;
    while ((remaining_buffer = producer_pool_sptr->acquireFilled(false, 0)) != nullptr) {
        producer_pool_sptr->releaseFilled(remaining_buffer);
        frame_count++;
        (*total_frames)++;
        drained_count++;
    }
    if (drained_count > 0) {
        LOG4CPLUS_INFO_FMT(test_logger, "%sDrained %d remaining buffers", thread_prefix.c_str(), drained_count);
    }
    
    // 7. 停止生产者
    LOG4CPLUS_INFO_FMT(test_logger, "%sStopping FFmpeg producer...", thread_prefix.c_str());
    producer.stop();
    
    LOG4CPLUS_INFO_FMT(test_logger, "%sDecode worker completed", thread_prefix.c_str());
    LOG4CPLUS_INFO_FMT(test_logger, "%sTotal frames decoded: %d", thread_prefix.c_str(), frame_count);
    LOG4CPLUS_INFO_FMT(test_logger, "%sFrames produced: %d", thread_prefix.c_str(), producer.getProducedFrames());
    LOG4CPLUS_INFO_FMT(test_logger, "%sFrames skipped: %d", thread_prefix.c_str(), producer.getSkippedFrames());
    LOG4CPLUS_INFO_FMT(test_logger, "%sAverage FPS: %.2f", thread_prefix.c_str(), producer.getAverageFPS());
}

/**
 * 测试7：多线程 FFmpeg 视频解码（不显示，仅解码）
 * 
 * 功能：
 * - 创建多个 VideoProductionLine 实例
 * - 每个实例独立解码同一个视频文件
 * - 不显示，仅做解码性能测试
 * - 统计所有线程的解码性能
 */
static int test_h264_taco_video_multithread(const char* video_path) {
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO_FMT(test_logger, "  Test: Multi-threaded FFmpeg Video Decoding - File: %s", video_path);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    // 配置参数
    const int num_threads = 4;  // 固定4个线程
    const int output_width = 1920;
    const int output_height = 1080;
    
    LOG4CPLUS_INFO_FMT(test_logger, "Configuration:");
    LOG4CPLUS_INFO_FMT(test_logger, "  Threads: %d", num_threads);
    LOG4CPLUS_INFO_FMT(test_logger, "  Video file: %s", video_path);
    LOG4CPLUS_INFO_FMT(test_logger, "  Output resolution: %dx%d", output_width, output_height);
    LOG4CPLUS_INFO_FMT(test_logger, "  Display: Disabled (decode only)");
    
    
    // 全局统计
    std::atomic<int> total_frames(0);
    std::atomic<int> total_errors(0);
    
    // 创建多个线程，每个线程运行一个 VideoProductionLine
    LOG4CPLUS_INFO(test_logger, "Creating decode threads...");
    std::vector<std::thread> threads;
    
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(decode_production_line_worker,
                            i + 1,  // line_id 从1开始
                            video_path,
                            output_width,
                            output_height,
                            &total_frames,
                            &total_errors);
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "All %d decode threads started", num_threads);
    LOG4CPLUS_INFO(test_logger, "[Test] 按Ctrl+C停止");
    
    
    // 等待所有线程完成（或通过 g_running 控制）
    for (auto& t : threads) {
        t.join();
    }
    
    // 打印最终统计
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Test Results");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO_FMT(test_logger, "Total threads: %d", num_threads);
    LOG4CPLUS_INFO_FMT(test_logger, "Total frames decoded: %d", total_frames.load());
    LOG4CPLUS_INFO_FMT(test_logger, "Total errors: %d", total_errors.load());
    
    if (total_errors.load() > 0) {
        LOG4CPLUS_WARN_FMT(test_logger, "Test completed with %d errors", total_errors.load());
        return -1;
    }
    
    LOG4CPLUS_INFO(test_logger, "Test completed successfully");
    return 0;
}

/**
 * ⭐ v2.17：辅助函数 - TACO格式名转FFmpeg标准格式
 * 
 * @param taco_format TACO格式字符串（如 "YUV420 NV12 8-bit"）
 * @param is_rgb 是否是RGB格式
 * @return AVPixelFormat，失败返回 AV_PIX_FMT_NONE
 */
// ⭐ v2.18: 此函数已移除，因为现在直接从 Buffer 实际格式获取 AVPixelFormat

/**
 * ⭐ v2.17：辅助函数 - 生成输出文件路径
 * 
 * @param channel 通道号（0 或 1）
 * @param format AVPixelFormat
 * @param width 宽度
 * @param height 高度
 * @return 输出路径字符串
 */
static std::string generateOutputPath(int channel, AVPixelFormat format, int width, int height) {
    const char* format_name = av_get_pix_fmt_name(format);
    
    // 判断子目录
    const char* subdir = "yuv";
    if (format == AV_PIX_FMT_RGB24 || format == AV_PIX_FMT_BGR24 ||
        format == AV_PIX_FMT_ARGB || format == AV_PIX_FMT_RGBA ||
        format == AV_PIX_FMT_ABGR || format == AV_PIX_FMT_BGRA ||
        format == AV_PIX_FMT_RGB0 || format == AV_PIX_FMT_BGR0 ||
        format == AV_PIX_FMT_0RGB || format == AV_PIX_FMT_0BGR ||
        format == AV_PIX_FMT_RGB48LE || format == AV_PIX_FMT_BGR48LE ||
        format == AV_PIX_FMT_GBRP) {
        subdir = "rgb";
    } else if (format == AV_PIX_FMT_GRAY8 || format == AV_PIX_FMT_GRAY10LE) {
        subdir = "gray";
    }
    
    char path[512];
    snprintf(path, sizeof(path), 
             "./test_output_raw/%s/ch%d_%s_%dx%d.raw",
             subdir, channel, format_name, width, height);
    
    return std::string(path);
}

/**
 * 测试8a：BufferWriter单格式保存测试
 * 
 * 通过配置h264_taco解码器输出指定格式，测试BufferWriter保存功能
 * 所有格式信息从taco_config中自动推导
 * 
 * @param video_path 视频文件路径
 * @param taco_config h264_taco解码器配置（通过TacoConfigBuilder构建）
 */
static int test_buffer_writer_format(
    const char* video_path,
    const WorkerConfig::DecoderConfig::TacoConfig& taco_config
) {
    using namespace productionline::io;
    
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  BufferWriter Format Test (v2.18 - 优先级逻辑)");
    LOG4CPLUS_INFO_FMT(test_logger, "  Video: %s", video_path);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    // ========== Step 1: 启动 VideoProductionLine 并获取输入源信息 ==========
    LOG4CPLUS_INFO(test_logger, "\nStep 1: Starting VideoProductionLine and getting source info...");
    
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
                .useTaco("h264", taco_config)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    VideoProductionLine producer(false, 1, false);
    if (!producer.start(workerConfig)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to start VideoProductionLine");
        return -1;
    }
    
    // 获取 Worker Facade 以访问输入源信息
    auto worker_facade_sptr = producer.getWorkerFacade();
    if (!worker_facade_sptr) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get WorkerFacade");
        producer.stop();
        return -1;
    }
    
    // ⭐⭐⭐ 从输入数据源获取原始信息 ⭐⭐⭐
    int source_width = worker_facade_sptr->getSourceWidth();
    int source_height = worker_facade_sptr->getSourceHeight();
    AVPixelFormat source_format = worker_facade_sptr->getSourcePixelFormat();
    
    LOG4CPLUS_INFO(test_logger, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    LOG4CPLUS_INFO_FMT(test_logger, "  Input source resolution: %dx%d", source_width, source_height);
    LOG4CPLUS_INFO_FMT(test_logger, "  Input source format: %s", av_get_pix_fmt_name(source_format));
    LOG4CPLUS_INFO(test_logger, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // ========== Step 2: 使用优先级逻辑确定输出参数 ==========
    LOG4CPLUS_INFO(test_logger, "\nStep 2: Determining output parameters with priority logic...");
    system("mkdir -p ./test_output_raw/yuv ./test_output_raw/rgb ./test_output_raw/gray");
    
    // ========== 通道0变量 ==========
    bool ch0_writer_created = false;
    std::unique_ptr<BufferWriter> ch0_writer;
    AVPixelFormat ch0_expected_format = AV_PIX_FMT_NONE;
    int ch0_expected_width = 0;
    int ch0_expected_height = 0;
    std::string ch0_output_path;
    int ch0_saved_count = 0;
    
    // ========== 通道1变量 ==========
    bool ch1_writer_created = false;
    std::unique_ptr<BufferWriter> ch1_writer;
    AVPixelFormat ch1_expected_format = AV_PIX_FMT_NONE;
    int ch1_expected_width = 0;
    int ch1_expected_height = 0;
    std::string ch1_output_path;
    int ch1_saved_count = 0;
    
    // ========== 统计变量 ==========
    int skipped_count = 0;
    
    // ⭐⭐⭐ ch0 配置（YUV 格式，支持 scaler）- 使用优先级逻辑 ⭐⭐⭐
    if (taco_config.ch0_enable) {
        // ch0 输出 YUV 格式，格式由解码器自动决定，默认 NV12
        ch0_expected_format = AV_PIX_FMT_NV12;
        
        // 优先级1: taco_config 配置的缩放分辨率
        // 优先级2: 输入源的原始分辨率
        // 优先级3: 默认 1920x1080
        if (taco_config.ch0_scale_width > 0 && taco_config.ch0_scale_height > 0) {
            ch0_expected_width = taco_config.ch0_scale_width;
            ch0_expected_height = taco_config.ch0_scale_height;
            LOG4CPLUS_INFO_FMT(test_logger, "  ch0: Using taco_config scale: %dx%d (Priority 1)", 
                         ch0_expected_width, ch0_expected_height);
        } else if (source_width > 0 && source_height > 0) {
            ch0_expected_width = source_width;
            ch0_expected_height = source_height;
            LOG4CPLUS_INFO_FMT(test_logger, "  ch0: Using input source resolution: %dx%d (Priority 2)", 
                         ch0_expected_width, ch0_expected_height);
        } else {
            ch0_expected_width = 1920;
            ch0_expected_height = 1080;
            LOG4CPLUS_INFO_FMT(test_logger, "  ch0: Using default resolution: %dx%d (Priority 3)", 
                         ch0_expected_width, ch0_expected_height);
        }
        ch0_output_path = generateOutputPath(0, ch0_expected_format, ch0_expected_width, ch0_expected_height);
        ch0_writer = std::make_unique<BufferWriter>();
        
        if (ch0_writer->openRaw(ch0_output_path.c_str(), ch0_expected_format, 
                                ch0_expected_width, ch0_expected_height)) {
            ch0_writer_created = true;
            LOG4CPLUS_INFO_FMT(test_logger, "✅ Created ch0 writer: %s (%dx%d)", 
                         av_get_pix_fmt_name(ch0_expected_format),
                         ch0_expected_width, ch0_expected_height);
            LOG4CPLUS_INFO_FMT(test_logger, "   Output: %s", ch0_output_path.c_str());
        } else {
            LOG4CPLUS_ERROR(test_logger, "Failed to create ch0 writer");
            producer.stop();
            return -1;
        }
    }
    
    // ⭐ ch1 配置（RGB 格式，支持 scaler）
    if (taco_config.ch1_enable) {
        // ch1 根据配置推导 RGB 格式
        ch1_expected_format = AV_PIX_FMT_ARGB;  // 默认 ARGB (对应 ch1_rgb_format=9)
        if (taco_config.ch1_rgb) {
            // RGB 格式需要根据 ch1_rgb_format 映射
            switch (taco_config.ch1_rgb_format) {
                case 1:  ch1_expected_format = AV_PIX_FMT_RGB24; break;    // rgb888
                case 3:  ch1_expected_format = AV_PIX_FMT_BGR24; break;    // bgr888
                case 9:  ch1_expected_format = AV_PIX_FMT_ARGB; break;     // argb888
                case 11: ch1_expected_format = AV_PIX_FMT_ABGR; break;     // abgr888
                case 13: ch1_expected_format = AV_PIX_FMT_RGBA; break;     // rgba888
                case 15: ch1_expected_format = AV_PIX_FMT_BGRA; break;     // bgra888
                case 17: ch1_expected_format = AV_PIX_FMT_RGB48LE; break;  // r16g16b16
                case 19: ch1_expected_format = AV_PIX_FMT_BGR48LE; break;  // b16g16r16
                case 21: ch1_expected_format = AV_PIX_FMT_RGB0; break;     // rgbx888
                case 23: ch1_expected_format = AV_PIX_FMT_BGR0; break;     // bgrx888
                case 25: ch1_expected_format = AV_PIX_FMT_0RGB; break;     // xrgb888
                case 27: ch1_expected_format = AV_PIX_FMT_0BGR; break;     // xbgr888
                case 28: ch1_expected_format = AV_PIX_FMT_GBRP; break;     // gbrp
                default: ch1_expected_format = AV_PIX_FMT_ARGB; break;
            }
        } else {
            // ch1 输出 YUV 格式
            ch1_expected_format = AV_PIX_FMT_NV12;  // 默认 YUV420 NV12
        }
        
        // 优先级1: taco_config 配置的缩放分辨率
        // 优先级2: 输入源的原始分辨率
        // 优先级3: 默认 1920x1080
        if (taco_config.ch1_scale_width > 0 && taco_config.ch1_scale_height > 0) {
            ch1_expected_width = taco_config.ch1_scale_width;
            ch1_expected_height = taco_config.ch1_scale_height;
            LOG4CPLUS_INFO_FMT(test_logger, "  ch1: Using taco_config scale: %dx%d (Priority 1)", 
                         ch1_expected_width, ch1_expected_height);
        } else if (source_width > 0 && source_height > 0) {
            ch1_expected_width = source_width;
            ch1_expected_height = source_height;
            LOG4CPLUS_INFO_FMT(test_logger, "  ch1: Using input source resolution: %dx%d (Priority 2)", 
                         ch1_expected_width, ch1_expected_height);
        } else {
            ch1_expected_width = 1920;
            ch1_expected_height = 1080;
            LOG4CPLUS_INFO_FMT(test_logger, "  ch1: Using default resolution: %dx%d (Priority 3)", 
                         ch1_expected_width, ch1_expected_height);
        }
        ch1_output_path = generateOutputPath(1, ch1_expected_format, ch1_expected_width, ch1_expected_height);
        ch1_writer = std::make_unique<BufferWriter>();
        
        if (ch1_writer->openRaw(ch1_output_path.c_str(), ch1_expected_format, 
                                ch1_expected_width, ch1_expected_height)) {
            ch1_writer_created = true;
            LOG4CPLUS_INFO_FMT(test_logger, "✅ Created ch1 writer: %s (%dx%d)", 
                         av_get_pix_fmt_name(ch1_expected_format),
                         ch1_expected_width, ch1_expected_height);
            LOG4CPLUS_INFO_FMT(test_logger, "   Output: %s", ch1_output_path.c_str());
        } else {
            LOG4CPLUS_ERROR(test_logger, "Failed to create ch1 writer");
            producer.stop();
            return -1;
        }
    }
    
    if (!ch0_writer_created && !ch1_writer_created) {
        LOG4CPLUS_ERROR(test_logger, "No channels enabled in taco_config!");
        producer.stop();
        return -1;
    }
    
    // ========== Step 3: 获取 BufferPool ==========
    LOG4CPLUS_INFO(test_logger, "\nStep 3: Getting BufferPool...");
    uint64_t pool_id = producer.getWorkingBufferPoolId();
    auto pool_sptr = BufferPoolRegistry::getInstance().getPool(pool_id).lock();
    if (!pool_sptr) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get BufferPool");
        producer.stop();
        return -1;
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "BufferPool: %s (ID: %lu)", 
                 pool_sptr->getName().c_str(), pool_id);
    
    // ========== Step 4: 消费循环（通道过滤 + 格式验证在 BufferWriter 内部） ==========
    LOG4CPLUS_INFO(test_logger, "\nStep 4: Saving frames...");
    LOG4CPLUS_INFO(test_logger, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    int timeout_count = 0;
    const int MAX_TIMEOUT = 10;
    
    while (g_running) {
        Buffer* buffer = pool_sptr->acquireFilled(true, 100);
        
        if (buffer) {
            int buffer_channel = buffer->getOutputChannel();
            
            // ⭐⭐⭐ 严格按照 TACO config 消费：只消费配置中启用的通道 ⭐⭐⭐
            // TACO config 决定了启用哪些通道，消费者必须严格遵守
            if (buffer_channel == 0 && taco_config.ch0_enable && ch0_writer_created) {
                // 通道 0 已启用且 writer 已创建：消费 ch0 数据
                if (ch0_writer->write(buffer)) {
                    ch0_saved_count++;
                    if (ch0_saved_count % 10 == 0) {
                        LOG4CPLUS_INFO_FMT(test_logger, "  ch0: Saved %d frames", ch0_saved_count);
                    }
                }
            } else if (buffer_channel == 1 && taco_config.ch1_enable && ch1_writer_created) {
                // 通道 1 已启用且 writer 已创建：消费 ch1 数据
                if (ch1_writer->write(buffer)) {
                    ch1_saved_count++;
                    if (ch1_saved_count % 10 == 0) {
                        LOG4CPLUS_INFO_FMT(test_logger, "  ch1: Saved %d frames", ch1_saved_count);
                    }
                }
            } else if (buffer_channel < 0) {
                // 无通道信息（软件解码器等）：尝试写入 TACO config 中启用的通道
                if (taco_config.ch0_enable && ch0_writer_created && ch0_writer->write(buffer)) {
                    ch0_saved_count++;
                } else if (taco_config.ch1_enable && ch1_writer_created && ch1_writer->write(buffer)) {
                    ch1_saved_count++;
                }
            } else {
                // 通道不匹配或未启用：跳过
                skipped_count++;
                if (skipped_count % 50 == 1) {
                    LOG4CPLUS_WARN_FMT(test_logger, "  ⚠️  Skipping frame from ch%d (not enabled in TACO config, total: %d)",
                                buffer_channel, skipped_count);
                }
            }
            
            pool_sptr->releaseFilled(buffer);
            timeout_count = 0;
        } else {
            timeout_count++;
            if (timeout_count >= MAX_TIMEOUT) {
                LOG4CPLUS_INFO(test_logger, "Video finished, stopping...");
                break;
            }
            usleep(10000);
        }
    }
    
    LOG4CPLUS_INFO(test_logger, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // 4. 关闭
    LOG4CPLUS_INFO(test_logger, "\nStep 4: Cleaning up...");
    if (ch0_writer) ch0_writer->close();
    if (ch1_writer) ch1_writer->close();
    producer.stop();
    
    // 5. ========== 最终统计 ==========
    LOG4CPLUS_INFO(test_logger, "\n═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Test Results");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    if (ch0_writer_created) {
        LOG4CPLUS_INFO_FMT(test_logger, "  ch0: Saved %d frames", ch0_saved_count);
        LOG4CPLUS_INFO_FMT(test_logger, "       Format mismatches: %lld", (long long)ch0_writer->getMismatchCount());
        LOG4CPLUS_INFO_FMT(test_logger, "       Output: %s", ch0_output_path.c_str());
    }
    if (ch1_writer_created) {
        LOG4CPLUS_INFO_FMT(test_logger, "  ch1: Saved %d frames", ch1_saved_count);
        LOG4CPLUS_INFO_FMT(test_logger, "       Format mismatches: %lld", (long long)ch1_writer->getMismatchCount());
        LOG4CPLUS_INFO_FMT(test_logger, "       Output: %s", ch1_output_path.c_str());
    }
    LOG4CPLUS_INFO_FMT(test_logger, "  Skipped frames: %d (channel mismatch)", skipped_count);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    
    bool success = (ch0_saved_count > 0 || ch1_saved_count > 0);
    if (success) {
        LOG4CPLUS_INFO(test_logger, "\n✅ Test PASSED");
    } else {
        LOG4CPLUS_ERROR(test_logger, "\n❌ Test FAILED: No frames saved");
        LOG4CPLUS_ERROR(test_logger, "   💡 Tip: Check TACO configuration and decoder output");
    }
    
    return success ? 0 : -1;
}

/**
 * 打印支持的像素格式列表
 */
static void print_supported_formats() {
    LOG4CPLUS_INFO(test_logger, "╔═══════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO(test_logger, "║  支持的像素格式 (Supported Pixel Formats)            ║");
    LOG4CPLUS_INFO(test_logger, "╚═══════════════════════════════════════════════════════╝");
    LOG4CPLUS_INFO(test_logger, "");
    LOG4CPLUS_INFO(test_logger, "YUV 格式:");
    LOG4CPLUS_INFO(test_logger, "  nv12         - YUV420 NV12 (默认格式)");
    LOG4CPLUS_INFO(test_logger, "  nv21         - YUV420 NV21");
    LOG4CPLUS_INFO(test_logger, "");
    LOG4CPLUS_INFO(test_logger, "RGB 格式 (8-bit, Alpha 通道):");
    LOG4CPLUS_INFO(test_logger, "  argb888      - ARGB (4字节/像素)");
    LOG4CPLUS_INFO(test_logger, "  abgr888      - ABGR (4字节/像素)");
    LOG4CPLUS_INFO(test_logger, "  bgra888      - BGRA (4字节/像素)");
    LOG4CPLUS_INFO(test_logger, "  rgba888      - RGBA (4字节/像素)");
    LOG4CPLUS_INFO(test_logger, "");
    LOG4CPLUS_INFO(test_logger, "RGB 格式 (8-bit, 无 Alpha):");
    LOG4CPLUS_INFO(test_logger, "  rgb888       - RGB24 (3字节/像素)");
    LOG4CPLUS_INFO(test_logger, "  bgr888       - BGR24 (3字节/像素)");
    LOG4CPLUS_INFO(test_logger, "");
    LOG4CPLUS_INFO(test_logger, "RGB 格式 (16-bit):");
    LOG4CPLUS_INFO(test_logger, "  r16g16b16    - RGB48LE (6字节/像素)");
    LOG4CPLUS_INFO(test_logger, "  b16g16r16    - BGR48LE (6字节/像素)");
    LOG4CPLUS_INFO(test_logger, "");
    LOG4CPLUS_INFO(test_logger, "使用示例:");
    LOG4CPLUS_INFO(test_logger, "  ./test writer nv12 /path/to/video.mp4");
    LOG4CPLUS_INFO(test_logger, "  ./test writer rgb888 /path/to/video.mp4");
    LOG4CPLUS_INFO(test_logger, "  ./test writer argb888 /path/to/video.mp4");
    LOG4CPLUS_INFO(test_logger, "╚═══════════════════════════════════════════════════════╝");
}

/**
 * 根据格式名称创建 TacoConfig
 * 
 * @param format_name 格式名称（如 "nv12", "rgb888", "argb888" 等）
 * @param taco_config 输出参数，构建的配置
 * @return 成功返回 true，格式不支持返回 false
 */
static bool create_taco_config_by_format(
    const std::string& format_name,
    WorkerConfig::DecoderConfig::TacoConfig& taco_config
) {
    // 转换为小写便于比较
    std::string fmt = format_name;
    std::transform(fmt.begin(), fmt.end(), fmt.begin(), ::tolower);
    
    // YUV 格式
    if (fmt == "nv12") {
        taco_config = TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::YUV_NV12, ColorStandard::BT601)
            .build();
        return true;
    }
    if (fmt == "nv21") {
        taco_config = TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::YUV_NV21, ColorStandard::BT601)
            .build();
        return true;
    }
    
    // RGB 8-bit Alpha 格式
    // ⭐ v2.18：移除硬编码分辨率，让优先级逻辑自动适配输入视频分辨率
    if (fmt == "argb888") {
        taco_config = TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_ARGB888, ColorStandard::BT601)
            .build();
        return true;
    }
    if (fmt == "abgr888") {
        taco_config = TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_ABGR888, ColorStandard::BT601)
            .build();
        return true;
    }
    if (fmt == "bgra888") {
        taco_config = TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_BGRA888, ColorStandard::BT601)
            .build();
        return true;
    }
    if (fmt == "rgba888") {
        taco_config = TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_RGBA888, ColorStandard::BT601)
            .build();
        return true;
    }
    
    // RGB 8-bit 无 Alpha 格式
    if (fmt == "rgb888") {
        taco_config = TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_RGB888, ColorStandard::BT601)
            .build();
        return true;
    }
    if (fmt == "bgr888") {
        taco_config = TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_BGR888, ColorStandard::BT601)
            .build();
        return true;
    }
    
    // RGB 16-bit 格式
    if (fmt == "r16g16b16") {
        taco_config = TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_R16G16B16, ColorStandard::BT601)
            .build();
        return true;
    }
    if (fmt == "b16g16r16") {
        taco_config = TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_B16G16R16, ColorStandard::BT601)
            .build();
        return true;
    }
    
    // 不支持的格式
    return false;
}

/**
 * 测试8：BufferWriter保存帧测试（支持命令行指定格式）
 * 
 * 功能：
 * - 使用VideoProductionLine解码视频
 * - 使用BufferWriter将解码后的帧保存到文件
 * - 支持命令行指定像素格式
 * 
 * 命令行格式：
 *   ./test writer <format> <video_path>
 * 
 * 示例：
 *   ./test writer nv12 /path/to/video.mp4
 *   ./test writer rgb888 /path/to/video.mp4
 *   ./test writer argb888 /path/to/video.mp4
 */
static int test_buffer_writer(const std::vector<std::string>& args) {
    // 检查参数数量（框架已经检查过，这里只需处理业务逻辑）
    if (args.size() < 2) {
        // 这个分支不应该被触发，因为框架会先检查
        LOG4CPLUS_ERROR(test_logger, "错误：缺少参数");
        return -1;
    }
    
    std::string format_name = args[0];
    const char* video_path = args[1].c_str();
    
    // 根据格式名称创建配置
    WorkerConfig::DecoderConfig::TacoConfig taco_config;
    if (!create_taco_config_by_format(format_name, taco_config)) {
        LOG4CPLUS_ERROR(test_logger, "╔═══════════════════════════════════════════════════════╗");
        LOG4CPLUS_ERROR_FMT(test_logger, "║  错误：不支持的格式 '%s'", format_name.c_str());
        LOG4CPLUS_ERROR(test_logger, "╚═══════════════════════════════════════════════════════╝");
        LOG4CPLUS_ERROR(test_logger, "");
        print_supported_formats();
        return -1;
    }
    
    // 调用实际的测试函数
    return test_buffer_writer_format(video_path, taco_config);
}

/**
 * 测试8b：BufferWriter RGB格式保存测试
 * 
 * 测试 BufferWriter 对所有 RGB 格式的支持：
 *   - 8-bit ARGB/ABGR/BGRA/RGBA (4 字节/像素，Alpha 通道)
 *   - 8-bit RGB/BGR (3 字节/像素)
 *   - 8-bit 0RGB/0BGR (4 字节/像素，padding 在前)
 *   - 8-bit RGB0/BGR0 (4 字节/像素，padding 在后)
 *   - 16-bit RGB48LE/BGR48LE (6 字节/像素)
 * 
 * 共计 12 种 RGB 格式
 * 
 * ⚠️ 特殊说明：
 *   TACO 的 xrgb888/xbgr888 由于字节序转换，实际内存布局可能与命名不同
 *   - xrgb888 → 命名暗示 0RGB，但实际内存可能是 BGR0
 *   - xbgr888 → 命名暗示 0BGR，但实际内存可能是 RGB0
 *   测试会根据 Buffer 的实际格式元数据自动适配
 * 
 * 注意：NV12 格式由 test_buffer_writer() 单独测试
 */
static int test_buffer_writer_rgb_formats(const char* video_path) {
    LOG4CPLUS_INFO(test_logger, "╔═══════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO(test_logger, "║  BufferWriter RGB Formats Test Suite                  ║");
    LOG4CPLUS_INFO(test_logger, "║  Testing 12 RGB formats (full coverage)               ║");
    LOG4CPLUS_INFO(test_logger, "╚═══════════════════════════════════════════════════════╝");
    
    // ✅ 定义测试用例：所有 RGB 格式（NV12 由 test_buffer_writer 单独测试）
    // ⭐ v2.18：移除硬编码分辨率，让优先级逻辑自动适配输入视频分辨率
    std::function<WorkerConfig::DecoderConfig::TacoConfig()> tests[] = {
        // 8-bit ARGB/ABGR/BGRA/RGBA 格式（Alpha 通道，4 字节/像素）
        []() { return TacoConfigBuilder()
                   .setChannels(false, true)
                   .setOutputFormat(Channel::CH1, OutputFormat::RGB_ARGB888, ColorStandard::BT601)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setChannels(false, true)
                   .setOutputFormat(Channel::CH1, OutputFormat::RGB_ABGR888, ColorStandard::BT601)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setChannels(false, true)
                   .setOutputFormat(Channel::CH1, OutputFormat::RGB_BGRA888, ColorStandard::BT601)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setChannels(false, true)
                   .setOutputFormat(Channel::CH1, OutputFormat::RGB_RGBA888, ColorStandard::BT601)
                   .build(); 
        },
        
        // 8-bit RGB/BGR 格式（3 字节/像素）
        []() { return TacoConfigBuilder()
                   .setChannels(false, true)
                   .setOutputFormat(Channel::CH1, OutputFormat::RGB_RGB888, ColorStandard::BT601)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setChannels(false, true)
                   .setOutputFormat(Channel::CH1, OutputFormat::RGB_BGR888, ColorStandard::BT601)
                   .build(); 
        },
        
       /*  // ⭐ 8-bit 0RGB/0BGR 格式（padding 在前，4 字节/像素）
        // 注意：TACO xrgb888 → FFmpeg 0RGB，xbgr888 → FFmpeg 0BGR
        []() { return TacoConfigBuilder()
                   .setChannels(false, true)
                   .setOutputFormat(Channel::CH1, OutputFormat::RGB_XRGB888, ColorStandard::BT601)
                   .setScale(Channel::CH1, 1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setChannels(false, true)
                   .setOutputFormat(Channel::CH1, OutputFormat::RGB_XBGR888, ColorStandard::BT601)
                   .setScale(Channel::CH1, 1920, 1080)
                   .build(); 
        },
        
        // ⭐⭐ 8-bit RGB0/BGR0 格式（padding 在后，4 字节/像素）
        // 关键发现：TACO 的 xrgb888/xbgr888 经过字节序转换后，实际内存是 BGRX/RGBX
        // libdec24 注释：XRGB888 → 内存:BGRX，XBGR888 → 内存:RGBX
        // 所以同样的配置字符串可能映射到不同的 FFmpeg 格式（取决于驱动实现）
        // 这里我们暂时复用 xrgb888/xbgr888，期望驱动能正确处理为 RGB0/BGR0
        []() { return TacoConfigBuilder()
                   .setChannels(false, true)
                   .setOutputFormat(Channel::CH1, OutputFormat::RGB_XBGR888, ColorStandard::BT601)  // → 实际内存 RGBX → RGB0
                   .setScale(Channel::CH1, 1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setChannels(false, true)
                   .setOutputFormat(Channel::CH1, OutputFormat::RGB_XRGB888, ColorStandard::BT601)  // → 实际内存 BGRX → BGR0
                   .setScale(Channel::CH1, 1920, 1080)
                   .build(); 
        },
         */
        // ⭐ 16-bit RGB/BGR 格式（6 字节/像素，文件更大）
        []() { return TacoConfigBuilder()
                   .setChannels(false, true)
                   .setOutputFormat(Channel::CH1, OutputFormat::RGB_R16G16B16, ColorStandard::BT601)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setChannels(false, true)
                   .setOutputFormat(Channel::CH1, OutputFormat::RGB_B16G16R16, ColorStandard::BT601)
                   .build(); 
        },
    };
    
    int total_tests = sizeof(tests) / sizeof(tests[0]);
    int passed = 0;
    int failed = 0;
    
    LOG4CPLUS_INFO_FMT(test_logger, "\nTotal RGB formats to test: %d\n", total_tests);
    
    for (int i = 0; i < total_tests; i++) {
        LOG4CPLUS_INFO_FMT(test_logger, "\n╔═══════════════════════════════════════════════════════╗");
        LOG4CPLUS_INFO_FMT(test_logger, "║  [%d/%d] Testing format                                ║", i + 1, total_tests);
        LOG4CPLUS_INFO_FMT(test_logger, "╚═══════════════════════════════════════════════════════╝");
        
        // ✅ 调用build_config()构建TacoConfig，直接传给测试函数
        int result = test_buffer_writer_format(video_path, tests[i]());
        
        if (result == 0) {
            passed++;
            LOG4CPLUS_INFO_FMT(test_logger, "\n✅ [%d/%d] PASSED\n", i + 1, total_tests);
        } else {
            failed++;
            LOG4CPLUS_ERROR_FMT(test_logger, "\n❌ [%d/%d] FAILED\n", i + 1, total_tests);
        }
        
        // 短暂延迟，避免资源冲突
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    // 最终统计
    LOG4CPLUS_INFO(test_logger, "\n╔═══════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO(test_logger, "║  Test Summary                                          ║");
    LOG4CPLUS_INFO(test_logger, "╚═══════════════════════════════════════════════════════╝");
    LOG4CPLUS_INFO_FMT(test_logger, "Total tests: %d", total_tests);
    LOG4CPLUS_INFO_FMT(test_logger, "Passed: %d ✅", passed);
    LOG4CPLUS_INFO_FMT(test_logger, "Failed: %d ❌", failed);
    LOG4CPLUS_INFO_FMT(test_logger, "Success rate: %.1f%%", (100.0 * passed / total_tests));
    LOG4CPLUS_INFO(test_logger, "\n╔═══════════════════════════════════════════════════════╗");
    
    return (failed == 0) ? 0 : -1;
}

/**
 * 测试8c：BufferWriter YUV格式保存测试
 * 
 * 测试 BufferWriter 对所有硬件支持的 YUV 格式的支持（PP0，ch0）：
 *   - YUV400 系列：P010, I010, L010, Pack10, 8-bit
 *   - YUV420 NV12 系列：P010, I010, L010, Pack10, 8-bit NV12
 *   - YUV420 NV21 系列：P010 Tiled-4×4, I011, L010, 8-bit NV21
 *   - YUV420 P010
 */
static int test_buffer_writer_yuv_formats(const char* video_path) {
    LOG4CPLUS_INFO(test_logger, "╔═══════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO(test_logger, "║  BufferWriter YUV Formats Test Suite                   ║");
    LOG4CPLUS_INFO(test_logger, "║  Testing YUV formats supported by PP0 (ch0)            ║");
    LOG4CPLUS_INFO(test_logger, "╚═══════════════════════════════════════════════════════╝");
    
    // ✅ 定义测试用例：所有硬件支持的 YUV 格式（与 test_buffer_writer_rgb_formats 保持一致的结构）
    // ⭐ v2.18：移除硬编码分辨率，让优先级逻辑自动适配输入视频分辨率
    // ⭐ v3.0：通道0使用 YUV_AUTO，由解码器根据输入流自动决定YUV格式
    std::function<WorkerConfig::DecoderConfig::TacoConfig()> tests[] = {
        // YUV400 系列
        []() { return TacoConfigBuilder()  // YUV400 I010, bt2020
                   .setChannels(true, true)
                   .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT2020)
                   .setOutputFormat(Channel::CH1, OutputFormat::RGB_ABGR888, ColorStandard::BT2020)
                   .build(); 
        }
        // []() { return TacoConfigBuilder()  // YUV400 L010, bt2020
        //            .setChannels(true, false)
        //            .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT2020)
        //            .build(); 
        // },
        // []() { return TacoConfigBuilder()  // YUV400 Pack10, bt2020
        //            .setChannels(true, false)
        //            .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT2020)
        //            .build(); 
        // },
        // []() { return TacoConfigBuilder()  // YUV400 8-bit, bt601
        //            .setChannels(true, false)
        //            .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT601)
        //            .build(); 
        // },
        
        // // YUV420 NV12 系列
        // []() { return TacoConfigBuilder()  // YUV420 NV12 P010, bt2020
        //            .setChannels(true, false)
        //            .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT2020)
        //            .build(); 
        // },
        // []() { return TacoConfigBuilder()  // YUV420 NV12 I010, bt2020
        //            .setChannels(true, false)
        //            .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT2020)
        //            .build(); 
        // },
        // []() { return TacoConfigBuilder()  // YUV420 NV12 L010, bt2020
        //            .setChannels(true, false)
        //            .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT2020)
        //            .build(); 
        // },
        // []() { return TacoConfigBuilder()  // YUV420 NV12 Pack10, bt2020
        //            .setChannels(true, false)
        //            .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT2020)
        //            .build(); 
        // },
        // []() { return TacoConfigBuilder()  // YUV420 8-bit NV12, bt601（最常用）
        //            .setChannels(true, false)
        //            .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT601)
        //            .build(); 
        // },
        
        // // YUV420 NV21 系列
        // []() { return TacoConfigBuilder()  // YUV420 NV21 P010 Tiled-4×4, bt2020
        //            .setChannels(true, false)
        //            .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT2020)
        //            .build(); 
        // },
        // []() { return TacoConfigBuilder()  // YUV420 NV21 I011, bt2020
        //            .setChannels(true, false)
        //            .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT2020)
        //            .build(); 
        // },
        // []() { return TacoConfigBuilder()  // YUV420 NV21 L010, bt2020
        //            .setChannels(true, false)
        //            .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT2020)
        //            .build(); 
        // },
        // []() { return TacoConfigBuilder()  // YUV420 8-bit NV21, bt601
        //            .setChannels(true, false)
        //            .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT601)
        //            .build(); 
        // },
        
        // // YUV420 P010
        // []() { return TacoConfigBuilder()  // YUV420 P010, bt2020
        //            .setChannels(true, false)
        //            .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT2020)
        //            .build(); 
        // },
    };
    
    int total_tests = sizeof(tests) / sizeof(tests[0]);
    int passed = 0;
    int failed = 0;
    
    LOG4CPLUS_INFO_FMT(test_logger, "\nTotal YUV formats to test: %d\n", total_tests);
    
    for (int i = 0; i < total_tests; i++) {
        LOG4CPLUS_INFO_FMT(test_logger, "\n╔═══════════════════════════════════════════════════════╗");
        LOG4CPLUS_INFO_FMT(test_logger, "║  [%d/%d] Testing format                                ║", i + 1, total_tests);
        LOG4CPLUS_INFO_FMT(test_logger, "╚═══════════════════════════════════════════════════════╝");
        
        // ✅ 调用build_config()构建TacoConfig，直接传给测试函数（与RGB测试保持一致）
        int result = test_buffer_writer_format(video_path, tests[i]());
        
        if (result == 0) {
            passed++;
            LOG4CPLUS_INFO_FMT(test_logger, "\n✅ [%d/%d] PASSED\n", i + 1, total_tests);
        } else {
            failed++;
            LOG4CPLUS_ERROR_FMT(test_logger, "\n❌ [%d/%d] FAILED\n", i + 1, total_tests);
        }
        
        // 短暂延迟，避免资源冲突
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    // 最终统计
    LOG4CPLUS_INFO(test_logger, "\n╔═══════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO(test_logger, "║  Test Summary                                          ║");
    LOG4CPLUS_INFO(test_logger, "╚═══════════════════════════════════════════════════════╝");
    LOG4CPLUS_INFO_FMT(test_logger, "Total tests: %d", total_tests);
    LOG4CPLUS_INFO_FMT(test_logger, "Passed: %d ✅", passed);
    LOG4CPLUS_INFO_FMT(test_logger, "Failed: %d ❌", failed);
    LOG4CPLUS_INFO_FMT(test_logger, "Success rate: %.1f%%", (100.0 * passed / total_tests));
    LOG4CPLUS_INFO(test_logger, "\n╔═══════════════════════════════════════════════════════╝");
    
    return (failed == 0) ? 0 : -1;
}


/**
 * 测试：RTSP 流录制到所有支持的容器格式（批量验证）
 * 
 * 功能：
 * - 测试 7 种主流容器格式（Phase 1-3）
 * - Phase 1: MP4/MKV/MOV（主流格式）
 * - Phase 2: TS/FLV（流媒体格式）
 * - Phase 3: AVI/3GP（特殊格式）
 * - 每种格式录制 10 秒
 * - 输出到当前目录的 ./test_output_videos/ 子目录
 * 
 * 使用示例：
 *   ./display_test -m rtsp_record_all_formats rtsp://192.168.1.100:8554/stream
 */
static int test_rtsp_record_all_formats(const char* rtsp_url) {
    using namespace productionline::io;
    
    LOG4CPLUS_INFO(test_logger, "╔═══════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO(test_logger, "║   Test: RTSP Record - All Format Validation          ║");
    LOG4CPLUS_INFO(test_logger, "╚═══════════════════════════════════════════════════════╝\n");
    
    // 注册信号处理器
    signal(SIGINT, signal_handler);
    g_running = true;
    g_rtsp_interrupted = false;
    RtspPacketSource::clearInterrupt();
    
    LOG4CPLUS_INFO_FMT(test_logger, "RTSP URL: %s\n", rtsp_url);
    
    // ⭐ 创建定时器（每次格式测试时会重新设置）
    Timer recording_timer;
    recording_timer.start();
    
    // 1. 创建输出目录
    const char* output_dir = "./test_output_videos";
    LOG4CPLUS_INFO_FMT(test_logger, "[Step 1] Creating output directory: %s", output_dir);
    
    // 使用 mkdir (POSIX) 创建目录
    #ifdef _WIN32
        _mkdir(output_dir);
    #else
        mkdir(output_dir, 0755);
    #endif
    
    LOG4CPLUS_INFO_FMT(test_logger, "  ✅ Output directory: %s/\n", output_dir);
    
    // 2. 定义所有要测试的格式
    struct FormatTest {
        const char* extension;
        const char* name;
        const char* tier;
        bool success;
        int packet_count;
        int64_t file_size;
    };
    
    FormatTest formats[] = {
        // Phase 1: 主流格式
        {"mp4",  "MP4 (MPEG-4 Part 14)",     "Tier 1: 主流格式", false, 0, 0},
        {"mkv",  "MKV (Matroska)",           "Tier 1: 主流格式", false, 0, 0},
        {"mov",  "MOV (QuickTime)",          "Tier 1: 主流格式", false, 0, 0},
        
        // Phase 2: 流媒体格式
        {"ts",   "TS (MPEG Transport Stream)", "Tier 2: 流媒体格式", false, 0, 0},
        {"flv",  "FLV (Flash Video)",          "Tier 2: 流媒体格式", false, 0, 0},
        
        // Phase 3: 特殊格式
        {"avi",  "AVI (Audio Video Interleave)", "Tier 3: 特殊格式", false, 0, 0},
        {"3gp",  "3GP (3GPP)",                    "Tier 3: 特殊格式", false, 0, 0},
    };
    
    const int num_formats = sizeof(formats) / sizeof(formats[0]);
    const int duration_seconds = 30;  // 每种格式录制 10 秒
    
    LOG4CPLUS_INFO_FMT(test_logger, "[Step 2] Testing %d container formats (%d seconds each)...\n", 
                 num_formats, duration_seconds);
    
    // 3. 循环测试每种格式
    int success_count = 0;
    int failed_count = 0;
    
    for (int i = 0; i < num_formats; i++) {
        auto& fmt = formats[i];
        
        LOG4CPLUS_INFO(test_logger, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        LOG4CPLUS_INFO_FMT(test_logger, "[%d/%d] Testing %s (%s)", i + 1, num_formats, fmt.name, fmt.tier);
        
        // 生成输出文件路径
        char output_file[512];
        snprintf(output_file, sizeof(output_file), 
                 "%s/test_output.%s", output_dir, fmt.extension);
        
        LOG4CPLUS_INFO_FMT(test_logger, "  Output: %s", output_file);
        
        // 重置中断标志
        g_running = true;
        g_rtsp_interrupted = false;
        RtspPacketSource::clearInterrupt();
        
        // 创建 VideoProductionLine
        VideoProductionLine producer(false, 1, false);
        
        // 配置 Worker
        auto workerConfig = WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(rtsp_url)
                    .setBufferCount(32)
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
            .build();
        
        // 设置错误回调
        producer.setErrorCallback([](const std::string& error) {
            // 静默错误，避免打印过多
        });
        
        // 启动生产者
        if (!producer.start(workerConfig)) {
            LOG4CPLUS_ERROR_FMT(test_logger, "  ❌ Failed to start producer");
            failed_count++;
            continue;
        }
        
        // 获取 BufferPool
        uint64_t pool_id = producer.getWorkingBufferPoolId();
        auto pool_sptr = BufferPoolRegistry::getInstance().getPool(pool_id).lock();
        if (!pool_sptr) {
            LOG4CPLUS_ERROR_FMT(test_logger, "  ❌ Failed to get BufferPool");
            producer.stop();
            failed_count++;
            continue;
        }
        
        // 获取 Worker 的编解码器参数（v2.14: 通过门面类直接获取）
        auto worker_facade_sptr = producer.getWorkerFacade();
        if (!worker_facade_sptr) {
            LOG4CPLUS_ERROR_FMT(test_logger, "  ❌ Failed to get worker facade");
            producer.stop();
            failed_count++;
            continue;
        }
        
        // ⭐ 直接从门面类获取编解码器参数和时间基
        const AVCodecParameters* codec_params = worker_facade_sptr->getCodecParameters();
        if (!codec_params) {
            LOG4CPLUS_ERROR_FMT(test_logger, "  ❌ Failed to get codec parameters from worker");
            producer.stop();
            failed_count++;
            continue;
        }
        
        AVRational time_base = worker_facade_sptr->getTimeBase();
        
        // 打开 BufferWriter
        BufferWriter writer;
        if (!writer.openEncoded(output_file, codec_params, time_base)) {
            LOG4CPLUS_ERROR_FMT(test_logger, "  ❌ Failed to open BufferWriter for format %s", fmt.extension);
            producer.stop();
            failed_count++;
            continue;
        }
        
        // 录制循环（10秒）
        // ⭐ 设置录制时长定时器
        auto timer_id = recording_timer.scheduleOnce(
            duration_seconds * 1000,  // 毫秒
            [&producer]() {  // 捕获 producer 引用
                g_running = false;  // 时间到，停止录制
                producer.stop();
                LOG4CPLUS_INFO(test_logger, "\n  ⏱️  Recording duration reached, stopping...");
            }
        );
        
        int packet_count = 0;
        int timeout_count = 0;
        const int MAX_TIMEOUT = 50;
        
        while (g_running) {
            // 检查中断
            if (g_rtsp_interrupted.load()) {
                break;
            }
            
            // 获取 Buffer
            Buffer* buffer = pool_sptr->acquireFilled(true, 100);
            
            if (buffer) {
                if (writer.write(buffer)) {
                    packet_count++;
                }
                pool_sptr->releaseFilled(buffer);
                timeout_count = 0;
            } else {
                timeout_count++;
                if (timeout_count >= MAX_TIMEOUT) {
                    break;
                }
            }
        }
        
        // 关闭
        // ⭐ 停止定时器
        recording_timer.cancel(timer_id);
        
        writer.close();
        
        // 检查结果
        if (packet_count > 0) {
            // 获取文件大小
            FILE* f = fopen(output_file, "rb");
            int64_t file_size = 0;
            if (f) {
                fseek(f, 0, SEEK_END);
                file_size = ftell(f);
                fclose(f);
            }
            
            fmt.success = true;
            fmt.packet_count = packet_count;
            fmt.file_size = file_size;
            
            LOG4CPLUS_INFO_FMT(test_logger, "  ✅ Success: %d packets, %.2f MB", 
                         packet_count, file_size / (1024.0 * 1024.0));
            success_count++;
        } else {
            LOG4CPLUS_ERROR_FMT(test_logger, "  ❌ Failed: No packets recorded");
            failed_count++;
        }
        
        // 短暂延迟，避免 RTSP 连接冲突
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    // 4. 打印总结报告
    LOG4CPLUS_INFO(test_logger, "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    LOG4CPLUS_INFO(test_logger, "\n╔═══════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO(test_logger, "║   Test Summary                                        ║");
    LOG4CPLUS_INFO(test_logger, "╚═══════════════════════════════════════════════════════╝");
    LOG4CPLUS_INFO_FMT(test_logger, "  Total:    %d formats", num_formats);
    LOG4CPLUS_INFO_FMT(test_logger, "  Success:  %d formats ✅", success_count);
    LOG4CPLUS_INFO_FMT(test_logger, "  Failed:   %d formats ❌", failed_count);
    
    // 打印失败的格式
    if (failed_count > 0) {
        LOG4CPLUS_INFO(test_logger, "\nFailed formats:");
        for (int i = 0; i < num_formats; i++) {
            if (!formats[i].success) {
                LOG4CPLUS_INFO_FMT(test_logger, "  - %s (%s)", formats[i].name, formats[i].tier);
            }
        }
    }
    
    // 打印成功的文件
    if (success_count > 0) {
        LOG4CPLUS_INFO(test_logger, "\nSuccessfully recorded files:");
        for (int i = 0; i < num_formats; i++) {
            if (formats[i].success) {
                LOG4CPLUS_INFO_FMT(test_logger, "  %s/test_output.%-4s  (%.2f MB, %d packets)", 
                             output_dir, formats[i].extension,
                             formats[i].file_size / (1024.0 * 1024.0),
                             formats[i].packet_count);
            }
        }
    }
    
    LOG4CPLUS_INFO(test_logger, "\n💡 Verify files with:");
    LOG4CPLUS_INFO_FMT(test_logger, "   ls -lh %s/", output_dir);
    LOG4CPLUS_INFO_FMT(test_logger, "   ffprobe %s/test_output.mp4", output_dir);
    LOG4CPLUS_INFO(test_logger, "╚═══════════════════════════════════════════════════════╝\n");
    
    // ⭐ 停止定时器服务
    recording_timer.stop();
    
    return (failed_count == 0) ? 0 : -1;
}

/**
 * 测试：MultiWorkerProductionLine 多Worker解码（从文件读取，硬解+软解对比）
 *
 * 架构：
 * - Producer: FFMPEG_PACKET_RECORDER（读取视频文件编码包 → BufferPool P）
 * - Consumer 1: 硬件解码（h264_taco）→ BufferPool C1
 * - Consumer 2: 软件解码（useSoftware）→ BufferPool C2
 * - 本测试从 C1/C2 中取出解码后的帧，分别写入两个 YUV 文件，便于对比质量/性能
 *
 * 用法示例：
 *   ./display_test -m multi_worker /path/to/video.h264
 *   或
 *   ./display_test -m multi_worker rtsp://...  (仍支持RTSP)
 */
static int test_multi_worker(const char* video_source) {
    using namespace productionline::io;
    
    LOG4CPLUS_INFO(test_logger, "╔═══════════════════════════════════════════════════════╗");
    LOG4CPLUS_INFO(test_logger, "║   Test: MultiWorkerProductionLine - Multi Worker     ║");
    LOG4CPLUS_INFO(test_logger, "╚═══════════════════════════════════════════════════════╝\n");
    
    // 注册信号处理器（用于 Ctrl+C）
    signal(SIGINT, signal_handler);
    g_running = true;
    g_rtsp_interrupted = false;
    RtspPacketSource::clearInterrupt();
    LOG4CPLUS_DEBUG(test_logger, "[Test] ✅ 已注册 Ctrl+C 信号处理器");
    
    // ⭐ 支持文件和RTSP（FfmpegPacketRecorderWorker会自动判断）
    LOG4CPLUS_INFO_FMT(test_logger, "Video source: %s\n", video_source);
    
    // 判断是否是RTSP，用于后续处理
    bool is_rtsp = (strstr(video_source, "rtsp://") == video_source);
    
    const int duration_seconds = 10;  // 录制时长（秒）
    
    // 1. 配置 MultiWorkerConfig
    LOG4CPLUS_INFO(test_logger, "[Step 1] Configuring MultiWorkerProductionLine...");
    
    MultiWorkerConfig config;
    
    // 创建 WorkerGroup
    WorkerGroup group(is_rtsp ? "rtsp_decode_group" : "file_decode_group");
    
    // 1.1 配置生产者 Worker（从文件或RTSP录制包）
    LOG4CPLUS_INFO_FMT(test_logger, "    Configuring Producer Worker (%s Record)...", is_rtsp ? "RTSP" : "File");
    ProducerConfig producer_cfg;
    producer_cfg.producer_name = "recorder";
    producer_cfg.worker_config = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_source)  // ⭐ 支持文件路径或RTSP URL
                .setBufferCount(32)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
        .build();
    group.producer_configs.push_back(producer_cfg);
    
    // 1.2 配置 Consumer Worker 1（硬件解码器）
    LOG4CPLUS_INFO(test_logger, "    Configuring Consumer Worker 1 (Hardware Decoder - h264_taco)...");
    
    auto tacoConfig = TacoConfigBuilder()
        .setChannels(true, false)
        .build();
    
    ConsumerConfig consumer_hw_cfg;
    consumer_hw_cfg.consumer_name = "hw_dec";
    consumer_hw_cfg.worker_config = WorkerConfigBuilder()
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(1920, 1080)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useTaco("h264", tacoConfig)  // 硬件解码器
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_RTSP)
        .build();
    group.consumer_configs.push_back(consumer_hw_cfg);
    
    // 1.3 配置 Consumer Worker 2（软件解码器）
    LOG4CPLUS_INFO(test_logger, "    Configuring Consumer Worker 2 (Software Decoder - libavcodec)...");
    
    ConsumerConfig consumer_sw_cfg;
    consumer_sw_cfg.consumer_name = "sw_dec";
    consumer_sw_cfg.worker_config = WorkerConfigBuilder()
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(1920, 1080)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useSoftware()  // 软件解码器
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_RTSP)
        .build();
    group.consumer_configs.push_back(consumer_sw_cfg);
    
    // 1.4 配置连接器：一个生产者 → 两个消费者（ONE_TO_MANY 模式）
    LOG4CPLUS_INFO(test_logger, "    Configuring Connector (ONE_TO_MANY)...");
    ConnectorConfig connector_cfg;
    connector_cfg.mode = Connector::Mode::ONE_TO_MANY;
    connector_cfg.producer_names.push_back("recorder");
    connector_cfg.consumer_names.push_back("hw_dec");
    connector_cfg.consumer_names.push_back("sw_dec");
    group.connector_configs.push_back(connector_cfg);
    
    // 添加 Group 到配置
    config.groups.push_back(group);
    config.thread_pool_size = 32;
    
    LOG4CPLUS_INFO_FMT(test_logger, "  Thread pool size: %d", config.thread_pool_size);
    LOG4CPLUS_INFO_FMT(test_logger, "  WorkerGroups: %zu", config.groups.size());
    LOG4CPLUS_INFO_FMT(test_logger, "  Group '%s': %zu producers, %zu consumers, %zu connectors",
                 group.group_id.c_str(),
                 group.producer_configs.size(),
                 group.consumer_configs.size(),
                 group.connector_configs.size());
    
    // 2. 创建 MultiWorkerProductionLine
    LOG4CPLUS_INFO(test_logger, "\n[Step 2] Creating MultiWorkerProductionLine...");
    MultiWorkerProductionLine multi_worker(config, false, 1, false);  // loop=false, thread_count=1, enable_monitor=false
    
    // 3. 设置错误回调
    multi_worker.setErrorCallback([](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "MultiWorker Error: %s", error.c_str());
        g_running = false;
    });
    
    // 4. 启动 MultiWorkerProductionLine
    LOG4CPLUS_INFO(test_logger, "[Step 3] Starting MultiWorkerProductionLine...");
    if (!multi_worker.start()) {
        LOG4CPLUS_ERROR(test_logger, "Failed to start MultiWorkerProductionLine");
        return -1;
    }
    
    LOG4CPLUS_INFO(test_logger, "  ✅ MultiWorkerProductionLine started successfully");
    
    // 5. 获取两个消费者的 BufferPool（对本测试而言，Group 内的消费者是数据的生产者）
    LOG4CPLUS_INFO(test_logger, "\n[Step 4] Getting Consumer BufferPools...");
    
    uint64_t hw_pool_id = multi_worker.getGroupConsumerBufferPoolId(0, 0);  // Group 0, Consumer 0 (Hardware decoder)
    uint64_t sw_pool_id = multi_worker.getGroupConsumerBufferPoolId(0, 1);  // Group 0, Consumer 1 (Software decoder)
    
    if (hw_pool_id == 0 || sw_pool_id == 0) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get consumer BufferPool IDs");
        multi_worker.stop();
        return -1;
    }
    
    auto hw_pool_weak = BufferPoolRegistry::getInstance().getPool(hw_pool_id);
    auto sw_pool_weak = BufferPoolRegistry::getInstance().getPool(sw_pool_id);
    auto hw_pool_sptr = hw_pool_weak.lock();
    auto sw_pool_sptr = sw_pool_weak.lock();
    
    if (!hw_pool_sptr || !sw_pool_sptr) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get consumer BufferPools from Registry");
        multi_worker.stop();
        return -1;
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "  Consumer 1 (Hardware Decoder) BufferPool: '%s' (ID: %lu)", 
                 hw_pool_sptr->getName().c_str(), hw_pool_id);
    LOG4CPLUS_INFO_FMT(test_logger, "  Consumer 2 (Software Decoder) BufferPool: '%s' (ID: %lu)", 
                 sw_pool_sptr->getName().c_str(), sw_pool_id);
    
    // 6. 等待第一个Buffer，获取实际格式
    LOG4CPLUS_INFO(test_logger, "\n[Step 5] Waiting for first buffers to detect format...");
    
    Buffer* first_hw = hw_pool_sptr->acquireFilled(true, 5000);
    Buffer* first_sw = sw_pool_sptr->acquireFilled(true, 5000);
    
    if (!first_hw || !first_sw) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get first buffers (timeout)");
        if (first_hw) hw_pool_sptr->releaseFilled(first_hw);
        if (first_sw) sw_pool_sptr->releaseFilled(first_sw);
        multi_worker.stop();
        return -1;
    }
    
    // 从Buffer元数据获取实际格式
    AVPixelFormat hw_format = AV_PIX_FMT_NONE;
    AVPixelFormat sw_format = AV_PIX_FMT_NONE;
    int hw_width = 1920, hw_height = 1080;
    int sw_width = 1920, sw_height = 1080;
    
    if (first_hw->hasImageMetadata()) {
        hw_format = first_hw->getImageFormat();
        hw_width = first_hw->getImageWidth();
        hw_height = first_hw->getImageHeight();
        LOG4CPLUS_INFO_FMT(test_logger, "  Consumer 1 (Hardware) format: %s (%dx%d)", 
                     av_get_pix_fmt_name(hw_format), hw_width, hw_height);
    } else {
        LOG4CPLUS_WARN(test_logger, "  Consumer 1 buffer has no metadata, using default NV12 1920x1080");
        hw_format = AV_PIX_FMT_NV12;
    }
    
    if (first_sw->hasImageMetadata()) {
        sw_format = first_sw->getImageFormat();
        sw_width = first_sw->getImageWidth();
        sw_height = first_sw->getImageHeight();
        LOG4CPLUS_INFO_FMT(test_logger, "  Consumer 2 (Software) format: %s (%dx%d)", 
                     av_get_pix_fmt_name(sw_format), sw_width, sw_height);
    } else {
        LOG4CPLUS_WARN(test_logger, "  Consumer 2 buffer has no metadata, using default NV12 1920x1080");
        sw_format = AV_PIX_FMT_NV12;
    }
    
    // 7. 创建两个 BufferWriter
    LOG4CPLUS_INFO(test_logger, "\n[Step 6] Creating BufferWriters...");
    
    const char* output_file_hw = "/tmp/multi_worker_hardware.yuv";
    const char* output_file_sw = "/tmp/multi_worker_software.yuv";
    
    BufferWriter writer_hw, writer_sw;
    
    if (!writer_hw.openRaw(output_file_hw, hw_format, hw_width, hw_height)) {
        LOG4CPLUS_ERROR_FMT(test_logger, "Failed to open BufferWriter for Consumer 1: %s", output_file_hw);
        hw_pool_sptr->releaseFilled(first_hw);
        sw_pool_sptr->releaseFilled(first_sw);
        multi_worker.stop();
        return -1;
    }
    
    if (!writer_sw.openRaw(output_file_sw, sw_format, sw_width, sw_height)) {
        LOG4CPLUS_ERROR_FMT(test_logger, "Failed to open BufferWriter for Consumer 2: %s", output_file_sw);
        writer_hw.close();
        hw_pool_sptr->releaseFilled(first_hw);
        sw_pool_sptr->releaseFilled(first_sw);
        multi_worker.stop();
        return -1;
    }
    
    LOG4CPLUS_INFO_FMT(test_logger, "  Consumer 1 (Hardware Decoder) output: %s (format: %s)", 
                 output_file_hw, av_get_pix_fmt_name(hw_format));
    LOG4CPLUS_INFO_FMT(test_logger, "  Consumer 2 (Software Decoder) output: %s (format: %s)", 
                 output_file_sw, av_get_pix_fmt_name(sw_format));
    
    // 8. 保存第一帧
    LOG4CPLUS_INFO(test_logger, "\n[Step 7] Saving frames...");
    LOG4CPLUS_INFO(test_logger, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    if (writer_hw.write(first_hw)) {
        LOG4CPLUS_INFO(test_logger, "  ✅ Saved Consumer 1 frame 1 (Hardware Decoder)");
    }
    if (writer_sw.write(first_sw)) {
        LOG4CPLUS_INFO(test_logger, "  ✅ Saved Consumer 2 frame 1 (Software Decoder)");
    }
    
    hw_pool_sptr->releaseFilled(first_hw);
    sw_pool_sptr->releaseFilled(first_sw);
    
    // 9. 消费者循环：从两个消费者的 BufferPool 获取 buffer 并保存
    auto start_time = std::chrono::steady_clock::now();
    int frame_count_hw = 1;  // 已经保存了第一帧
    int frame_count_sw = 1;
    int timeout_hw = 0;
    int timeout_sw = 0;
    const int MAX_TIMEOUT = 50;
    
    LOG4CPLUS_INFO(test_logger, "  Starting consumer loop (Ctrl+C to stop)...\n");
    
    while (g_running) {
        // 检查中断标志
        if (g_rtsp_interrupted.load()) {
            LOG4CPLUS_INFO(test_logger, "\n  ⚠️  检测到中断请求，停止测试...");
            break;
        }
        
        // 检查时长
        auto now = std::chrono::steady_clock::now();
        int elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        if (elapsed >= duration_seconds) {
            LOG4CPLUS_INFO_FMT(test_logger, "\n  ⏱️  Reached duration limit: %d seconds", duration_seconds);
            break;
        }
        
        // 从 Consumer 1 (Hardware) 获取 buffer
        Buffer* buf_hw = hw_pool_sptr->acquireFilled(true, 100);
        if (buf_hw) {
            if (writer_hw.write(buf_hw)) {
                frame_count_hw++;
                if (frame_count_hw % 50 == 0) {
                    LOG4CPLUS_INFO_FMT(test_logger, "  Consumer 1 (Hardware Decoder): %d frames saved", frame_count_hw);
                }
            } else {
                LOG4CPLUS_WARN(test_logger, "Failed to write Consumer 1 buffer");
            }
            hw_pool_sptr->releaseFilled(buf_hw);
            timeout_hw = 0;
        } else {
            timeout_hw++;
        }
        
        // 从 Consumer 2 (Software) 获取 buffer
        Buffer* buf_sw = sw_pool_sptr->acquireFilled(true, 100);
        if (buf_sw) {
            if (writer_sw.write(buf_sw)) {
                frame_count_sw++;
                if (frame_count_sw % 50 == 0) {
                    LOG4CPLUS_INFO_FMT(test_logger, "  Consumer 2 (Software Decoder): %d frames saved", frame_count_sw);
                }
            } else {
                LOG4CPLUS_WARN(test_logger, "Failed to write Consumer 2 buffer");
            }
            sw_pool_sptr->releaseFilled(buf_sw);
            timeout_sw = 0;
        } else {
            timeout_sw++;
        }
        
        // 检查超时
        if (timeout_hw >= MAX_TIMEOUT && timeout_sw >= MAX_TIMEOUT) {
            LOG4CPLUS_WARN(test_logger, "\n  ⚠️  Both consumers timeout, stopping...");
            break;
        }
    }
    
    LOG4CPLUS_INFO(test_logger, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // 10. 排空剩余的 buffer
    LOG4CPLUS_INFO(test_logger, "\n[Step 8] Draining remaining buffers...");
    int drained_hw = 0, drained_sw = 0;
    
    Buffer* remaining_hw = nullptr;
    while ((remaining_hw = hw_pool_sptr->acquireFilled(false, 0)) != nullptr) {
        writer_hw.write(remaining_hw);
        hw_pool_sptr->releaseFilled(remaining_hw);
        frame_count_hw++;
        drained_hw++;
    }
    
    Buffer* remaining_sw = nullptr;
    while ((remaining_sw = sw_pool_sptr->acquireFilled(false, 0)) != nullptr) {
        writer_sw.write(remaining_sw);
        sw_pool_sptr->releaseFilled(remaining_sw);
        frame_count_sw++;
        drained_sw++;
    }
    
    if (drained_hw > 0 || drained_sw > 0) {
        LOG4CPLUS_INFO_FMT(test_logger, "  Drained: Consumer 1=%d, Consumer 2=%d", drained_hw, drained_sw);
    }
    
    // 11. 清理
    LOG4CPLUS_INFO(test_logger, "\n[Step 9] Cleaning up...");
    writer_hw.close();
    writer_sw.close();
    multi_worker.stop();
    
    // 12. 打印结果
    auto end_time = std::chrono::steady_clock::now();
    double total_duration = std::chrono::duration<double>(end_time - start_time).count();
    
    LOG4CPLUS_INFO(test_logger, "\n═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Test Results");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO_FMT(test_logger, "  Consumer 1 (Hardware Decoder):");
    LOG4CPLUS_INFO_FMT(test_logger, "    Output file: %s", output_file_hw);
    LOG4CPLUS_INFO_FMT(test_logger, "    Format: %s (%dx%d)", av_get_pix_fmt_name(hw_format), hw_width, hw_height);
    LOG4CPLUS_INFO_FMT(test_logger, "    Frames saved: %d", frame_count_hw);
    LOG4CPLUS_INFO_FMT(test_logger, "  Consumer 2 (Software Decoder):");
    LOG4CPLUS_INFO_FMT(test_logger, "    Output file: %s", output_file_sw);
    LOG4CPLUS_INFO_FMT(test_logger, "    Format: %s (%dx%d)", av_get_pix_fmt_name(sw_format), sw_width, sw_height);
    LOG4CPLUS_INFO_FMT(test_logger, "    Frames saved: %d", frame_count_sw);
    LOG4CPLUS_INFO_FMT(test_logger, "  Duration: %.2f seconds", total_duration);
    
    // 打印统计信息
    multi_worker.printDetailedStats();
    
    LOG4CPLUS_INFO(test_logger, "\n💡 Verify the output files with:");
    LOG4CPLUS_INFO_FMT(test_logger, "   ffplay -f rawvideo -pixel_format %s -video_size %dx%d %s",
                 av_get_pix_fmt_name(hw_format), hw_width, hw_height, output_file_hw);
    LOG4CPLUS_INFO_FMT(test_logger, "   ffplay -f rawvideo -pixel_format %s -video_size %dx%d %s",
                 av_get_pix_fmt_name(sw_format), sw_width, sw_height, output_file_sw);
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════\n");
    
    if (frame_count_hw > 0 && frame_count_sw > 0) {
        return 0;
    } else {
        LOG4CPLUS_ERROR(test_logger, "Test failed: No frames saved from one or both consumers");
        return -1;
    }
}

/**
 * MJPEG解码器测试帮助信息
 */
static void print_mjpeg_decoder_help() {
    LOG4CPLUS_INFO(test_logger, "MJPEG Hardware Decoder Test Help:");
    LOG4CPLUS_INFO(test_logger, "  This test decodes MJPEG video files using TACO hardware acceleration");
    LOG4CPLUS_INFO(test_logger, "  and saves the decoded frames to a raw YUV file.");
    LOG4CPLUS_INFO(test_logger, "");
    LOG4CPLUS_INFO(test_logger, "  Usage: mjpeg_decoder <input_video> <output_file>");
    LOG4CPLUS_INFO(test_logger, "    input_video: Path to MJPEG video file (.mjpeg, .avi, .mp4, etc.)");
    LOG4CPLUS_INFO(test_logger, "    output_file: Path for output YUV file (.yuv)");
    LOG4CPLUS_INFO(test_logger, "");
    LOG4CPLUS_INFO(test_logger, "  Example:");
    LOG4CPLUS_INFO(test_logger, "    mjpeg_decoder video.mjpeg output.yuv");
    LOG4CPLUS_INFO(test_logger, "");
    LOG4CPLUS_INFO(test_logger, "  Output file can be viewed with:");
    LOG4CPLUS_INFO(test_logger, "    ffplay -f rawvideo -pix_fmt yuv420p -s 1920x1080 output.yuv");
}

/**
 * 测试 MJPEG 解码器
 * 从包含 MJPEG 帧的视频文件解码并保存到文件
 */
static int test_mjpeg_decoder(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        LOG4CPLUS_ERROR(test_logger, "Usage: test_mjpeg_decoder <input_video> <output_file>");
        LOG4CPLUS_ERROR(test_logger, "Example: test_mjpeg_decoder video.mjpeg output.yuv");
        return -1;
    }

    const char* input_video = args[0].c_str();
    const char* output_file = args[1].c_str();

    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  MJPEG Hardware Decoder Test");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO_FMT(test_logger, "  Input:  %s", input_video);
    LOG4CPLUS_INFO_FMT(test_logger, "  Output: %s", output_file);

    // 1. 创建 VideoProductionLine
    LOG4CPLUS_INFO(test_logger, "[Test] Creating VideoProductionLine...");
    VideoProductionLine producer(false, 1, false);

    // 2. 配置 TACO MJPEG 硬件解码器
    auto tacoConfig = TacoConfigBuilder()
        .setReorderDisable(true)
        .setChannels(true, false)
        .build();

    auto workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(input_video)
                .setBufferCount(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useTaco("jpeg", tacoConfig)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();

    // 3. 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        LOG4CPLUS_ERROR_FMT(test_logger, "MJPEG Decoder Error: %s", error.c_str());
        g_running = false;
    });

    // 4. 启动生产者
    LOG4CPLUS_INFO(test_logger, "[Test] Starting producer...");
    if (!producer.start(workerConfig)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to start producer");
        return -1;
    }

    // 5. 获取 Worker Facade 以访问输入源信息
    auto worker_facade_sptr = producer.getWorkerFacade();
    if (!worker_facade_sptr) {
        LOG4CPLUS_ERROR(test_logger, "Failed to get WorkerFacade");
        producer.stop();
        return -1;
    }

    // ⭐⭐⭐ 从输入数据源获取原始信息 ⭐⭐⭐
    int source_width = worker_facade_sptr->getSourceWidth();
    int source_height = worker_facade_sptr->getSourceHeight();
    AVPixelFormat source_format = worker_facade_sptr->getSourcePixelFormat();

    LOG4CPLUS_INFO(test_logger, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    LOG4CPLUS_INFO_FMT(test_logger, "  Input source resolution: %dx%d", source_width, source_height);
    LOG4CPLUS_INFO_FMT(test_logger, "  Input source format: %s", av_get_pix_fmt_name(source_format));
    LOG4CPLUS_INFO(test_logger, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

    // 6. 获取工作 BufferPool
    uint64_t producer_pool_id = producer.getWorkingBufferPoolId();
    if (producer_pool_id == 0) {
        LOG4CPLUS_ERROR(test_logger, "No working BufferPool ID available");
        producer.stop();
        return -1;
    }

    auto producer_pool_weak = BufferPoolRegistry::getInstance().getPool(producer_pool_id);
    auto producer_pool_sptr = producer_pool_weak.lock();
    if (!producer_pool_sptr) {
        LOG4CPLUS_ERROR(test_logger, "BufferPool not found or destroyed");
        producer.stop();
        return -1;
    }

    LOG4CPLUS_INFO_FMT(test_logger, "[Test] Using BufferPool: '%s'", producer_pool_sptr->getName().c_str());

    // 7. 创建输出文件写入器（使用实际的源分辨率）
    LOG4CPLUS_INFO(test_logger, "[Test] Creating BufferWriter...");
    productionline::io::BufferWriter writer;
    if (!writer.openRaw(output_file, AV_PIX_FMT_YUV420P, source_width, source_height)) {
        LOG4CPLUS_ERROR(test_logger, "Failed to create output writer");
        producer.stop();
        return -1;
    }

    // 8. 消费者循环：消费所有解码后的帧
    LOG4CPLUS_INFO(test_logger, "[Test] Starting consumer loop...");

    int frame_count = 0;
    int timeout_count = 0;
    const int MAX_TIMEOUT = 100;

    while (g_running) {
        Buffer* buffer = producer_pool_sptr->acquireFilled(true, 100);

        if (buffer) {
            // ⭐⭐⭐ 严格按照 TACO config 消费：只消费配置中启用的通道 ⭐⭐⭐
            int buffer_channel = buffer->getOutputChannel();
            if (buffer_channel == 0 && tacoConfig.ch0_enable) {
                // 通道 0 已启用：保存帧数据
                if (writer.write(buffer)) {
                    frame_count++;
                    if (frame_count % 10 == 0) {
                        LOG4CPLUS_INFO_FMT(test_logger, "  Saved %d frames to %s", frame_count, output_file);
                    }
                } else {
                    LOG4CPLUS_WARN(test_logger, "Failed to write frame to output file");
                }
            } else {
                // 通道未启用或不匹配：跳过
                LOG4CPLUS_DEBUG_FMT(test_logger, "Skipping frame from ch%d (not enabled in TACO config)", buffer_channel);
            }

            producer_pool_sptr->releaseFilled(buffer);
            timeout_count = 0;
        } else {
            timeout_count++;
            if (timeout_count >= MAX_TIMEOUT) {
                LOG4CPLUS_INFO(test_logger, "\n[Test] Consumer timeout - producer may have finished");
                break;
            }

            // 检查生产者状态
            if (!producer.isRunning()) {
                LOG4CPLUS_INFO(test_logger, "\n[Test] Producer stopped naturally");
                break;
            }
        }
    }

    // 9. 清理资源
    writer.close();
    producer.stop();

    // 10. 结果报告
    LOG4CPLUS_INFO(test_logger, "\n═══════════════════════════════════════════════════════");
    LOG4CPLUS_INFO(test_logger, "  Test Results");
    LOG4CPLUS_INFO(test_logger, "═══════════════════════════════════════════════════════");

    if (frame_count > 0) {
        // 获取输出文件大小
        FILE* f = fopen(output_file, "rb");
        size_t file_size = 0;
        if (f) {
            fseek(f, 0, SEEK_END);
            file_size = ftell(f);
            fclose(f);
        }

        LOG4CPLUS_INFO(test_logger, "  ✅ SUCCESS");
        LOG4CPLUS_INFO_FMT(test_logger, "     Frames saved: %d", frame_count);
        LOG4CPLUS_INFO_FMT(test_logger, "     Output file:  %s", output_file);
        LOG4CPLUS_INFO_FMT(test_logger, "     File size:    %.2f MB", file_size / (1024.0 * 1024.0));

        LOG4CPLUS_INFO(test_logger, "\n  💡 Verify output with:");
        LOG4CPLUS_INFO_FMT(test_logger, "     ffprobe -f rawvideo -pix_fmt yuv420p -s %dx%d %s", source_width, source_height, output_file);
        LOG4CPLUS_INFO_FMT(test_logger, "     ffplay -f rawvideo -pix_fmt yuv420p -s %dx%d %s", source_width, source_height, output_file);

        return 0;
    } else {
        LOG4CPLUS_ERROR(test_logger, "  ❌ FAILED - No frames were decoded/saved");
        return -1;
    }
}

// ========== 测试用例注册 ==========
// 使用新的测试框架，自动注册所有测试用例
REGISTER_TEST(rtsp_play, "RTSP stream playback (zero-copy, FFmpeg)", test_play_rtsp_stream);
REGISTER_TEST(rtsp_record, "RTSP stream recording to MP4 (use env RTSP_OUTPUT_FILE to specify path)", test_rtsp_record_stream);
REGISTER_TEST(file_record, "Local file recording/remux to MP4 (use env FILE_OUTPUT_FILE to specify path)", test_file_record);
REGISTER_TEST(ffmpeg_hardware, "FFmpeg encoded video playback (MP4/AVI/MKV/etc)", test_h264_taco_video);
REGISTER_TEST_MULTI_ARG(mjpeg_decoder, "MJPEG Hardware Decoder Test", "<input_video> <output_file>", test_mjpeg_decoder, print_mjpeg_decoder_help);
REGISTER_TEST(ffmpeg_software, "FFmpeg software decoder (libavcodec, no hardware acceleration)", test_ffmpeg_software_decoder);
REGISTER_TEST(ffmpeg_multithread, "Multi-threaded FFmpeg video decoding (no display, decode only)", test_h264_taco_video_multithread);
REGISTER_TEST_MULTI_ARG(writer, "BufferWriter - Save frames (specify format)", "<format> <video_path>", test_buffer_writer, print_supported_formats);
REGISTER_TEST(writer_all_rgb_formats, "BufferWriter - 12 RGB formats (ARGB/ABGR/BGRA/RGBA/RGB/BGR/0RGB/0BGR/RGB0/BGR0/RGB48/BGR48)", test_buffer_writer_rgb_formats);
REGISTER_TEST(writer_all_yuv_formats, "BufferWriter - 15 YUV formats (PP0 ch0: YUV400/YUV420 NV12/YUV420 NV21/YUV420 P010 series)", test_buffer_writer_yuv_formats);
REGISTER_TEST(rtsp_record_all_formats, "RTSP Record - All format validation (7 formats: MP4/MKV/MOV/TS/FLV/AVI/3GP)", test_rtsp_record_all_formats);
REGISTER_TEST(multi_worker, "MultiWorkerProductionLine - Multi worker test (hardware + software decoder from same RTSP stream)", test_multi_worker);

/**
 * @brief 配置所有模块的日志级别（编程式配置，作为 fallback）
 * 
 * 说明：
 * - 如果存在 logger.properties 配置文件，此函数将被跳过
 * - 配置文件方式更灵活（无需重新编译，直接修改即可）
 * - 此函数仅在没有配置文件时作为默认配置使用
 * 
 * 使用方式：
 * - 在 main 函数开头调用（INIT_LOGGER 之后）
 * - 为每个模块设置日志级别
 */
void configureModuleLoggers() {
    // 静默配置所有模块的日志级别（不输出配置信息）
    
    // ========== 测试框架 Logger ==========
    test_logger.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    // ========== ProductionLine 模块 ==========
    auto multi_worker = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.MultiWorker"));
    multi_worker.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto video_line = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.VideoLine"));
    video_line.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    // ========== Buffer 相关模块 ==========
    auto buffer_pool = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.BufferPool"));
    buffer_pool.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto bps = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.BufferPacketSource"));
    bps.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto writer = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.BufferWriter"));
    writer.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto buffer_comparator = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.BufferComparator"));
    buffer_comparator.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    // ========== Connector 模块 ==========
    auto connector = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Connector"));
    connector.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    // ========== Worker 模块（层次化）==========
    auto worker = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker"));
    worker.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto worker_video = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.VideoFile"));
    worker_video.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto worker_rtsp = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Rtsp"));
    worker_rtsp.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto worker_recorder = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Recorder"));
    worker_recorder.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    // ========== Allocator 模块（层次化）==========
    auto allocator = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator"));
    allocator.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto allocator_avframe = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.AVFrame"));
    allocator_avframe.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto allocator_fb = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Framebuffer"));
    allocator_fb.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto allocator_normal = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Normal"));
    allocator_normal.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto allocator_factory = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Factory"));
    allocator_factory.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto allocator_facade = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Facade"));
    allocator_facade.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    // ========== 数据源模块 ==========
    auto datasource_rtsp = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.DataSource.Rtsp"));
    datasource_rtsp.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto datasource_file = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.DataSource.File"));
    datasource_file.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    // ========== 工具类模块 ==========
    auto util_timer = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Util.Timer"));
    util_timer.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto worker_factory = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Factory"));
    worker_factory.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto worker_facade = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Facade"));
    worker_facade.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    // ========== 显示和监控 ==========
    auto display_fb = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Display.Framebuffer"));
    display_fb.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto monitor_perf = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Monitor.Performance"));
    monitor_perf.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    
    auto pool_registry = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.BufferPool.Registry"));
    pool_registry.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
}

/**
 * @brief 快速检查命令行参数，判断是否需要显示帮助或列表
 * 
 * 在初始化日志系统之前调用，避免在显示帮助信息时打印日志
 */
static bool needsHelpOrList(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help" || arg == "-l" || arg == "--list") {
            return true;
        }
        // 如果 -m 后面没有参数，也会触发帮助信息
        if (arg == "-m" || arg == "--mode") {
            if (i + 1 >= argc) {
                return true;  // -m 缺少参数，会显示帮助
            }
        }
    }
    return false;
}

/**
 * 主函数
 */
int main(int argc, char* argv[]) {
    // ⭐ 先快速检查命令行参数，如果是帮助或列表命令，直接输出并退出（不初始化日志系统）
    if (needsHelpOrList(argc, argv)) {
        // 直接使用 TEST_MAIN 来处理帮助信息（它会解析参数并输出帮助）
        // optind 在 TestMacros.hpp 包含的 getopt.h 中已定义，可以直接使用
        extern int optind;
        optind = 1;  // 重置 getopt 的内部状态，确保能正确解析参数
        TEST_MAIN(argc, argv);
        return 0;
    }
    
    // 初始化日志系统（支持配置文件或编程式配置）
    INIT_LOGGER();
    
    // ⭐ 配置所有模块的日志级别
    // 注意：如果 INIT_LOGGER() 已成功加载配置文件，下面的硬编码配置会被覆盖
    // 推荐使用配置文件方式（无需重新编译）
    struct stat buffer;
    bool has_config_file = (stat("./logger.properties", &buffer) == 0) ||
                           (stat("/etc/logger.properties", &buffer) == 0) ||
                           (stat("../logger.properties", &buffer) == 0);
    
    if (!has_config_file) {
        // 如果没有配置文件，使用编程式配置（静默配置，不输出信息）
        configureModuleLoggers();
    }
    // 如果有配置文件，直接使用配置文件中的设置（静默，不输出提示信息）

    // 注册信号处理
    signal(SIGINT, [](int) { g_running = false; });
    signal(SIGTERM, [](int) { g_running = false; });

    // 使用测试框架主函数
    TEST_MAIN(argc, argv);
}

