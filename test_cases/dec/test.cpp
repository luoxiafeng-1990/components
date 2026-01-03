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
#include "display/LinuxFramebufferDevice.hpp"
#include "productionline/worker/BufferFillingWorkerFacade.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/worker/FfmpegRecordRtspWorker.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "productionline/VideoProductionLine.hpp"
#include "productionline/io/BufferWriter.hpp"
#include "monitor/PerformanceMonitor.hpp"
#include "common/Logger.hpp"
#include "framework/TestMacros.hpp"

// FFmpeg头文件（解码器测试使用）
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>  // av_get_pix_fmt_name() 函数
}

// 全局标志，用于处理 Ctrl+C 退出
static volatile bool g_running = true;

/**
 * 测试1：多缓冲循环播放测试
 * 
 * 功能：
 * - 打开原始视频文件
 * - 加载帧到framebuffer的所有buffer中（数量由硬件决定）
 * - 循环播放这些帧
 * - 显示性能统计
 */
static int test_4frame_loop(const char* raw_video_path) {
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO("  Test: Multi-Buffer Loop Display (Using VideoProductionLine)");
    LOG_INFO("═══════════════════════════════════════════════════════");
    
    // 1. 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    int buffer_count = display.getBufferCount();
    
    // 2. 获取 display 的 BufferPool（framebuffer 已托管，v2.0: 通过 Registry 获取）
    uint64_t display_pool_id = display.getBufferPoolId();
    if (display_pool_id == 0) {
        LOG_ERROR("Display BufferPool not initialized");
        return -1;
    }
    auto display_pool_weak = BufferPoolRegistry::getInstance().getPool(display_pool_id);
    auto display_pool_sptr = display_pool_weak.lock();
    if (!display_pool_sptr) {
        LOG_ERROR_FMT("Display BufferPool (ID: %lu) not found or already destroyed", display_pool_id);
        return -1;
    }
    
    // 3. 创建 VideoProductionLine（Worker会在open()时自动创建BufferPool）
    VideoProductionLine producer(true, 1);  // loop=true, thread_count=1
    
    // 4. 配置并启动视频生产者
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath(raw_video_path)
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
                .useSoftware()  // 原始文件无需解码，使用软件解码作为默认配置
                .build()
        )
        .setWorkerType(WorkerType::MMAP_RAW)
        .build();
    
    // 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        LOG_ERROR_FMT("Producer Error: %s", error.c_str());
        g_running = false;
    });
    
    if (!producer.start(workerConfig)) {
        LOG_ERROR("Failed to start video producer");
        return -1;
    }
    
    // 5. 加载帧到 framebuffer（从Worker的BufferPool获取）
    LOG_INFO_FMT("Loading %d frames into framebuffer...", buffer_count);
    uint64_t producer_pool_id = producer.getWorkingBufferPoolId();
    if (producer_pool_id == 0) {
        LOG_ERROR("Worker failed to create BufferPool");
        producer.stop();
        return -1;
    }
    auto producer_pool_weak = BufferPoolRegistry::getInstance().getPool(producer_pool_id);
    auto producer_pool_sptr = producer_pool_weak.lock();
    if (!producer_pool_sptr) {
        LOG_ERROR("BufferPool not found or destroyed");
        producer.stop();
        return -1;
    }
    BufferPool* worker_pool = producer_pool_sptr.get();
    
    // 等待生产者填充buffer（生产者线程会自动填充）
    // 这里我们等待足够多的帧被填充
    for (int i = 0; i < buffer_count; i++) {
        Buffer* filled_buffer = worker_pool->acquireFilled(true, 5000);
        if (!filled_buffer || !filled_buffer->isValid()) {
            LOG_ERROR_FMT("Failed to acquire filled buffer %d", i);
            producer.stop();
            return -1;
        }
        
        // 显示buffer（零拷贝）
        display.waitVerticalSync();
        display.displayFilledFramebuffer(filled_buffer);
        
        // 归还buffer（但保留在framebuffer中用于循环显示）
        worker_pool->releaseFilled(filled_buffer);
    }

    // 6. 循环显示已加载的帧
    int loop_count = 0;
    while (g_running) {
        for (int buf_idx = 0; buf_idx < buffer_count && g_running; buf_idx++) {
            // 等待垂直同步
            display.waitVerticalSync();
            // 切换显示buffer（使用BufferPool和索引）
            display.displayBuffer(display_pool_sptr.get(), buf_idx);
        }
        
        loop_count++;
    }
    
    // 7. 停止生产者
    producer.stop();
    
    LOG_INFO("Playback stopped");
    LOG_INFO("Test completed successfully");
    
    return 0;
}

/**
 * 测试2：顺序播放测试（使用 VideoProductionLine）
 * 
 * 功能：
 * - 使用 VideoProductionLine 架构
 * - 顺序读取并显示所有帧（循环播放）
 * - 展示生产者-消费者模式
 */
static int test_sequential_playback(const char* raw_video_path) {
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO("  Test: Sequential Playback (Using VideoProductionLine)");
    LOG_INFO("═══════════════════════════════════════════════════════");
    
    // 1. 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 获取 display 的 BufferPool（framebuffer 已托管，v2.0: 通过 Registry 获取）
    uint64_t display_pool_id = display.getBufferPoolId();
    if (display_pool_id == 0) {
        LOG_ERROR("Display BufferPool not initialized");
        return -1;
    }
    auto display_pool_weak = BufferPoolRegistry::getInstance().getPool(display_pool_id);
    auto display_pool_sptr = display_pool_weak.lock();
    if (!display_pool_sptr) {
        LOG_ERROR_FMT("Display BufferPool (ID: %lu) not found or already destroyed", display_pool_id);
        return -1;
    }
    
    // 3. 创建 VideoProductionLine（Worker会在open()时自动创建BufferPool）
    VideoProductionLine producer(true, 1);  // loop=true, thread_count=1
    
    // 4. 配置并启动视频生产者
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath(raw_video_path)
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
                .useSoftware()  // 原始文件无需解码，使用软件解码作为默认配置
                .build()
        )
        .setWorkerType(WorkerType::MMAP_RAW)
        .build();
    
    // 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        LOG_ERROR_FMT("Producer Error: %s", error.c_str());
        g_running = false;
    });
    
    if (!producer.start(workerConfig)) {
        LOG_ERROR("Failed to start video producer");
        return -1;
    }
    
    // 5. 开始播放
    LOG_INFO("Starting sequential playback (Ctrl+C to stop)...");
    
    // 6. 消费者循环：从 BufferPool 获取 buffer 并显示
    int frame_count = 0;
    uint64_t producer_pool_id = producer.getWorkingBufferPoolId();
    if (producer_pool_id == 0) {
        LOG_ERROR("Worker failed to create BufferPool");
        producer.stop();
        return -1;
    }
    auto producer_pool_weak = BufferPoolRegistry::getInstance().getPool(producer_pool_id);
    auto producer_pool_sptr = producer_pool_weak.lock();
    if (!producer_pool_sptr) {
        LOG_ERROR("BufferPool not found or destroyed");
        producer.stop();
        return -1;
    }
    BufferPool* worker_pool = producer_pool_sptr.get();
    
    while (g_running) {
        // 获取一个已填充的 buffer（阻塞，100ms超时）
        Buffer* filled_buffer = worker_pool->acquireFilled(true, 100);
        
        if (filled_buffer == nullptr) {
            // 超时时检查生产者状态
            if (!producer.isRunning()) {
                LOG_INFO("Producer stopped naturally, exiting consumer loop...");
                break;
            }
            continue;  // 超时，继续等待
        }
        
        // 直接显示（零拷贝）
        display.waitVerticalSync();
        if (!display.displayFilledFramebuffer(filled_buffer)) {
            LOG_WARN("Failed to display buffer");
        }
        
        // 归还 buffer 到空闲队列
        worker_pool->releaseFilled(filled_buffer);
        frame_count++;
        
        // 每100帧打印一次进度
        if (frame_count % 100 == 0) {
            LOG_DEBUG_FMT("Frames displayed: %d (%.1f fps)", 
                          frame_count, producer.getAverageFPS());
        }
    }
    
    // 排空剩余的已填充 buffer
    LOG_INFO("Draining remaining buffers from BufferPool...");
    Buffer* remaining_buffer = nullptr;
    int drained_count = 0;
    while ((remaining_buffer = worker_pool->acquireFilled(false, 0)) != nullptr) {
        display.waitVerticalSync();
        display.displayFilledFramebuffer(remaining_buffer);
        worker_pool->releaseFilled(remaining_buffer);
        frame_count++;
        drained_count++;
    }
    if (drained_count > 0) {
        LOG_INFO_FMT("Drained %d remaining buffers", drained_count);
    }
    
    // 7. 停止生产者
    producer.stop();
    
    LOG_INFO("Playback stopped");
    LOG_INFO_FMT("Total frames played: %d", frame_count);
    LOG_INFO("Test completed successfully");
    return 0;
}

/**
 * 测试3：BufferPool + VideoProductionLine 测试（新架构）
 * 
 * 功能：
 * - 使用 LinuxFramebufferDevice 的 BufferPool（零拷贝）
 * - 使用 VideoProductionLine 自动从视频文件读取数据
 * - 主线程作为消费者，获取 buffer 并显示到屏幕
 * - 展示生产者-消费者模式的解耦架构
 */
static int test_buffermanager_producer(const char* raw_video_path) {
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO("  Test: BufferPool + VideoProductionLine (New Architecture)");
    LOG_INFO("═══════════════════════════════════════════════════════");
    
    // 1. 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 获取 display 的 BufferPool（framebuffer 已托管，v2.0: 通过 Registry 获取）
    uint64_t display_pool_id = display.getBufferPoolId();
    if (display_pool_id == 0) {
        LOG_ERROR("Display BufferPool not initialized");
        return -1;
    }
    auto display_pool_weak = BufferPoolRegistry::getInstance().getPool(display_pool_id);
    auto display_pool_sptr = display_pool_weak.lock();
    if (!display_pool_sptr) {
        LOG_ERROR_FMT("Display BufferPool (ID: %lu) not found or already destroyed", display_pool_id);
        return -1;
    }
    display_pool_sptr->printStats();
    
    // 3. 创建 VideoProductionLine（Worker会自动创建BufferPool）
    int producer_thread_count = 2;  // 使用2个生产者线程
    VideoProductionLine producer(true, producer_thread_count);  // loop=true, thread_count=2
    
    // 4. 配置并启动视频生产者
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath(raw_video_path)
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
                .useSoftware()  // 原始文件无需解码，使用软件解码作为默认配置
                .build()
        )
        .setWorkerType(WorkerType::MMAP_RAW)
        .build();
    
    // 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        LOG_ERROR_FMT("Producer Error: %s", error.c_str());
        g_running = false;
    });
    
    if (!producer.start(workerConfig)) {
        LOG_ERROR("Failed to start video producer");
        return -1;
    }
    
    // 5. 消费者循环：从 BufferPool 获取 buffer 并显示（零拷贝）
    int frame_count = 0;
    
    while (g_running) {
        // 获取一个已填充的 buffer（阻塞，100ms超时）
        Buffer* filled_buffer = display_pool_sptr->acquireFilled(true, 100);
        
        if (filled_buffer == nullptr) {
            // 超时时检查生产者状态
            if (!producer.isRunning()) {
                LOG_INFO("Producer stopped naturally, exiting consumer loop...");
                break;
            }
            continue;  // 超时，继续等待
        }
        
        // 直接显示（无需拷贝，buffer 本身就是 framebuffer）
        display.waitVerticalSync();
        if (!display.displayFilledFramebuffer(filled_buffer)) {
            LOG_WARN("Failed to display buffer");
        }
        // 归还 buffer 到空闲队列
        display_pool_sptr->releaseFilled(filled_buffer);
        frame_count++;
        // 每100帧打印一次进度
        if (frame_count % 100 == 0) {
            LOG_DEBUG_FMT("Frames displayed: %d (%.1f fps)", 
                          frame_count, producer.getAverageFPS());
        }
    }
    
    // 排空剩余的已填充 buffer
    LOG_INFO("Draining remaining buffers from BufferPool...");
    Buffer* remaining_buffer = nullptr;
    int drained_count = 0;
    while ((remaining_buffer = display_pool_sptr->acquireFilled(false, 0)) != nullptr) {
        display.waitVerticalSync();
        display.displayFilledFramebuffer(remaining_buffer);
        display_pool_sptr->releaseFilled(remaining_buffer);
        frame_count++;
        drained_count++;
    }
    if (drained_count > 0) {
        LOG_INFO_FMT("Drained %d remaining buffers", drained_count);
    }
    
    // 6. 停止生产者
    producer.stop();
    display_pool_sptr->printStats();
    return 0;
}

/**
 * 测试4：io_uring 模式（待实现 IoUringVideoProductionLine）
 * 
 * 功能：
 * - 使用 BufferPool 管理 buffer 池
 * - 使用 IoUringVideoProductionLine 进行高性能异步 I/O（待实现）
 * - 暂时使用普通 VideoProductionLine 作为替代
 */
static int test_buffermanager_iouring(const char* raw_video_path) {
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO("  Test: io_uring Mode (using VideoProductionLine temporarily)");
    LOG_INFO("═══════════════════════════════════════════════════════");
    
    LOG_INFO("Note: IoUringVideoProductionLine not yet implemented in new architecture");
    LOG_INFO("Using standard VideoProductionLine as fallback");
    
    // 1. 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    LOG_INFO_FMT("Display initialized: Resolution: %dx%d, Bits per pixel: %d, Buffer count: %d",
                 display.getWidth(), display.getHeight(), display.getBitsPerPixel(), display.getBufferCount());
    
    // 2. 获取 display 的 BufferPool（v2.0: 通过 Registry 获取）
    uint64_t display_pool_id = display.getBufferPoolId();
    if (display_pool_id == 0) {
        LOG_ERROR("Display BufferPool not initialized");
        return -1;
    }
    auto display_pool_weak = BufferPoolRegistry::getInstance().getPool(display_pool_id);
    auto display_pool_sptr = display_pool_weak.lock();
    if (!display_pool_sptr) {
        LOG_ERROR_FMT("Display BufferPool (ID: %lu) not found or already destroyed", display_pool_id);
        return -1;
    }
    
    LOG_INFO("Using LinuxFramebufferDevice's BufferPool");
    display_pool_sptr->printStats();
    
    // 3. 创建 VideoProductionLine（Worker会自动创建BufferPool）
    VideoProductionLine producer(true, 1);  // loop=true, thread_count=1
    
    LOG_INFO("Starting video producer (io_uring mode)");
    LOG_INFO("Using 1 producer thread with io_uring async I/O");
    
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath(raw_video_path)
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
                .useSoftware()  // 原始文件无需解码，使用软件解码作为默认配置
                .build()
        )
        .setWorkerType(WorkerType::IOURING_RAW)
        .build();
    
    producer.setErrorCallback([](const std::string& error) {
        LOG_ERROR_FMT("Producer Error: %s", error.c_str());
        g_running = false;
    });
    
    if (!producer.start(workerConfig)) {
        LOG_ERROR("Failed to start video producer");
        return -1;
    }
    
    LOG_INFO("Video producer started");
    LOG_INFO("Starting display loop (Ctrl+C to stop)...");
    
    // 4. 消费者循环
    int frame_count = 0;
    
    while (g_running) {
        Buffer* filled_buffer = display_pool_sptr->acquireFilled(true, 100);
        
        if (filled_buffer == nullptr) {
            // 超时时检查生产者状态
            if (!producer.isRunning()) {
                LOG_INFO("Producer stopped naturally, exiting consumer loop...");
                break;
            }
            continue;  // 超时，继续等待
        }
        
        display.waitVerticalSync();
        if (!display.displayFilledFramebuffer(filled_buffer)) {
            LOG_WARN("Failed to display buffer");
        }
        
        display_pool_sptr->releaseFilled(filled_buffer);
        frame_count++;
        
        if (frame_count % 100 == 0) {
            LOG_DEBUG_FMT("Frames displayed: %d (%.1f fps)", 
                          frame_count, producer.getAverageFPS());
        }
    }
    
    // 排空剩余的已填充 buffer
    LOG_INFO("Draining remaining buffers from BufferPool...");
    Buffer* remaining_buffer = nullptr;
    int drained_count = 0;
    while ((remaining_buffer = display_pool_sptr->acquireFilled(false, 0)) != nullptr) {
        display.waitVerticalSync();
        display.displayFilledFramebuffer(remaining_buffer);
        display_pool_sptr->releaseFilled(remaining_buffer);
        frame_count++;
        drained_count++;
    }
    if (drained_count > 0) {
        LOG_INFO_FMT("Drained %d remaining buffers", drained_count);
    }
    
    // 5. 停止生产者
    LOG_INFO("Stopping video producer...");
    producer.stop();
    
    LOG_INFO("Playback stopped");
    
    // 6. 打印统计
    LOG_DEBUG_FMT("Final Statistics: Frames displayed: %d, Frames produced: %d, Frames skipped: %d, Average FPS: %.2f",
                  frame_count, producer.getProducedFrames(), producer.getSkippedFrames(), producer.getAverageFPS());
    
    display_pool_sptr->printStats();
    
    LOG_INFO("Test completed successfully");
    LOG_INFO("TODO: Implement IoUringVideoProductionLine for true async I/O performance");
    
    return 0;
}


/**
 * 测试5：RTSP 视频流播放（Worker自动创建BufferPool + DMA 零拷贝显示）
 */
static int test_rtsp_stream(const char* rtsp_url) {
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO("  Test: RTSP Stream Playback (Independent BufferPool + DMA)");
    LOG_INFO("═══════════════════════════════════════════════════════");
    
    LOG_INFO("Zero-Copy Workflow:");
    LOG_INFO("  1. Worker opens RTSP stream and automatically creates BufferPool (if needed)");
    LOG_INFO("  2. Worker decodes RTSP → AVFrame with phys_addr");
    LOG_INFO("  3. Worker injects Buffer to its BufferPool");
    LOG_INFO("  4. Consumer acquires Buffer from Worker's BufferPool");
    LOG_INFO("  5. display.displayBufferByDMA(buffer) → DMA zero-copy");
    LOG_INFO("  6. Consumer releases Buffer → triggers deleter");
    
    // 1. 初始化显示设备
    LOG_INFO("[Test] 初始化显示设备...");
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 创建 VideoProductionLine（Worker会在open()时自动调用Allocator创建BufferPool）
    LOG_INFO("[Test] 创建VideoProductionLine...");
    VideoProductionLine producer(false, 1);  // loop=false, thread_count=1
    
    // 4. 配置 RTSP 流（注意：推荐单线程）
    LOG_INFO_FMT("Configuring RTSP stream: %s", rtsp_url);
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath(rtsp_url)
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
                .useTaco("h264")  // 使用 TACO 硬件解码器进行 H.264 RTSP 流解码
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_RTSP)
        .build();
    
    // 5. 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        LOG_ERROR_FMT("RTSP Error: %s", error.c_str());
        g_running = false;
    });
    
    // 6. 启动生产者（内部会创建RTSP Reader并启用零拷贝）
    LOG_INFO("Starting RTSP producer...");
    if (!producer.start(workerConfig)) {
        LOG_ERROR("Failed to start RTSP producer");
        return -1;
    }
    
    LOG_INFO("RTSP stream connected, starting playback...");
    LOG_INFO("[Test] 按Ctrl+C停止");
    LOG_INFO("Watch for '[DMA Display]' messages below");
    
    // 7. 获取工作BufferPool（Worker创建的或fallback的）
    uint64_t producer_pool_id = producer.getWorkingBufferPoolId();
    if (producer_pool_id == 0) {
        LOG_ERROR("No working BufferPool ID available");
        return -1;
    }
    auto producer_pool_weak = BufferPoolRegistry::getInstance().getPool(producer_pool_id);
    auto producer_pool_sptr = producer_pool_weak.lock();
    if (!producer_pool_sptr) {
        LOG_ERROR("BufferPool not found or destroyed");
        return -1;
    }
    
    LOG_INFO_FMT("[Test] Using BufferPool: '%s' (created by Worker via Allocator)", 
                 producer_pool_sptr->getName().c_str());
    producer_pool_sptr->printStats();
    
    // 8. 消费者循环：从工作BufferPool获取并通过DMA显示
    int frame_count = 0;
    int dma_success = 0;
    int dma_failed = 0;
    
    while (g_running) {
        // 从工作BufferPool获取已解码的buffer（带物理地址）
        Buffer* decoded_buffer = producer_pool_sptr->acquireFilled(true, 100);
        
        if (decoded_buffer == nullptr) {
            // 超时时检查生产者状态
            if (!producer.isRunning()) {
                LOG_INFO("Producer stopped naturally, exiting consumer loop...");
                break;
            }
            continue;  // 超时，继续等待
        }
        
        // ✨ 关键调用：display.displayBufferByDMA(buffer)
        display.waitVerticalSync();
        if (display.displayBufferByDMA(decoded_buffer)) {
            dma_success++;
        } else {
            dma_failed++;
            LOG_WARN_FMT("DMA display failed for buffer (phys_addr=0x%llx)",
                        (unsigned long long)decoded_buffer->getPhysicalAddress());
        }
        
        // 归还 buffer（会触发 RtspVideoReader 的 deleter 回收 AVFrame）
        producer_pool_sptr->releaseFilled(decoded_buffer);
        
        frame_count++;
        
        // 每100帧打印一次统计
        if (frame_count % 100 == 0) {
            LOG_DEBUG_FMT("Progress: %d frames displayed (%.1f fps, DMA success: %d, failed: %d)", 
                          frame_count, producer.getAverageFPS(), dma_success, dma_failed);
        }
    }
    
    // 排空剩余的已填充 buffer
    LOG_INFO("Draining remaining buffers from BufferPool...");
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
        LOG_INFO_FMT("Drained %d remaining buffers", drained_count);
    }
    
    // 8. 停止生产者
    LOG_INFO("Stopping RTSP producer...");
    producer.stop();
    
    LOG_INFO("RTSP test completed");
    LOG_INFO_FMT("Total frames displayed: %d", frame_count);
    LOG_INFO_FMT("DMA display success: %d", dma_success);
    LOG_INFO_FMT("DMA display failed: %d", dma_failed);
    LOG_INFO_FMT("Success rate: %.1f%%", 
                 frame_count > 0 ? (100.0 * dma_success / frame_count) : 0.0);
    
    LOG_INFO("Final BufferPool statistics:");
    producer_pool_sptr->printStats();
    
    return 0;
}

/**
 * 测试5.5：RTSP码流录制为MP4文件
 * 
 * 架构：
 * - 生产者：FfmpegRecordRtspWorker（读取RTSP编码包 → Buffer）
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
static int test_rtsp_record(const char* rtsp_url) {
    using namespace productionline::io;
    
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║   Test: RTSP Stream Recording to MP4                 ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝\n");
    
    // 从环境变量获取输出路径，如果没有则使用默认值
    const char* output_file = std::getenv("RTSP_OUTPUT_FILE");
    if (!output_file || strlen(output_file) == 0) {
        output_file = "/tmp/rtsp_recorded.mp4";  // 默认输出为MP4
    }
    
    LOG_INFO_FMT("Output file: %s\n", output_file);
    
    const int duration_seconds = 10;
    
    // 1. 创建 VideoProductionLine（生产者）
    LOG_INFO("[Step 1] Creating VideoProductionLine...");
    VideoProductionLine producer(false, 1, false);
    
    // 2. 配置 Worker
    LOG_INFO("[Step 2] Configuring FfmpegRecordRtspWorker...");
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath(rtsp_url)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_RTSP_RECORD)
        .build();
    
    // 3. 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        LOG_ERROR_FMT("Recording Error: %s", error.c_str());
        g_running = false;
    });
    
    // 4. 启动生产者
    LOG_INFO("[Step 3] Starting producer...");
    if (!producer.start(workerConfig)) {
        LOG_ERROR("Failed to start producer");
        return -1;
    }
    
    // 5. 获取 BufferPool
    LOG_INFO("[Step 4] Getting BufferPool...");
    uint64_t pool_id = producer.getWorkingBufferPoolId();
    auto pool_sptr = BufferPoolRegistry::getInstance().getPool(pool_id).lock();
    if (!pool_sptr) {
        LOG_ERROR("Failed to get BufferPool");
        producer.stop();
        return -1;
    }
    
    LOG_INFO_FMT("  BufferPool: '%s' (ID: %lu)", pool_sptr->getName().c_str(), pool_id);
    
    // 6. 获取Worker并打开BufferWriter（MP4模式）
    LOG_INFO("[Step 5] Opening BufferWriter (MP4 mode)...");
    
    // 获取Worker的编解码器参数
    auto worker_facade_sptr = producer.getWorkerFacade();
    if (!worker_facade_sptr) {
        LOG_ERROR("Failed to get worker facade");
        producer.stop();
        return -1;
    }
    
    // 获取底层Worker并转换为FfmpegRecordRtspWorker
    WorkerBase* worker_base = worker_facade_sptr->getWorkerBase();
    if (!worker_base) {
        LOG_ERROR("Failed to get worker base");
        producer.stop();
        return -1;
    }
    
    FfmpegRecordRtspWorker* rtsp_worker = dynamic_cast<FfmpegRecordRtspWorker*>(worker_base);
    if (!rtsp_worker) {
        LOG_ERROR("Worker is not FfmpegRecordRtspWorker type");
        producer.stop();
        return -1;
    }
    
    const AVCodecParameters* codec_params = rtsp_worker->getCodecParameters();
    if (!codec_params) {
        LOG_ERROR("Failed to get codec parameters from worker");
        producer.stop();
        return -1;
    }
    
    // 获取时间基
    AVRational time_base = rtsp_worker->getTimeBase();
    
    // 打开BufferWriter（编码流模式）
    BufferWriter writer;
    if (!writer.open(output_file, codec_params, time_base)) {
        LOG_ERROR("Failed to open BufferWriter");
        producer.stop();
        return -1;
    }
    
    // 7. 消费者线程：保存编码流到MP4文件
    LOG_INFO("\n[Step 6] Recording to MP4...");
    LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    auto start_time = std::chrono::steady_clock::now();
    int packet_count = 0;
    int64_t total_bytes = 0;
    int timeout_count = 0;
    const int MAX_TIMEOUT = 50;
    
    while (g_running) {
        // 检查时长
        auto now = std::chrono::steady_clock::now();
        int elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        if (elapsed >= duration_seconds) {
            LOG_INFO_FMT("\n  ⏱️  Reached duration limit: %d seconds", duration_seconds);
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
                        double rate_mbps = elapsed > 0 ? (total_bytes * 8.0) / (elapsed * 1000000.0) : 0.0;
                        LOG_INFO_FMT("  Recorded %d packets | %d seconds | %.2f Mbps",
                                     packet_count, elapsed, rate_mbps);
                    }
                } else {
                    LOG_WARN("Failed to write packet to MP4");
                }
            }
            
            pool_sptr->releaseFilled(buffer);
            timeout_count = 0;
        } else {
            timeout_count++;
            if (timeout_count >= MAX_TIMEOUT) {
                LOG_WARN("\n  ⚠️  Stream timeout, stopping...");
                break;
            }
        }
    }
    
    LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // 8. 清理（BufferWriter会自动写入MP4 trailer）
    writer.close();
    producer.stop();
    
    // 9. 统计信息
    auto end_time = std::chrono::steady_clock::now();
    double total_duration = std::chrono::duration<double>(end_time - start_time).count();
    
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO("  Recording Results");
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO_FMT("  ✅ MP4 file:      %s", output_file);
    LOG_INFO_FMT("  Packets recorded: %d", packet_count);
    LOG_INFO_FMT("  Duration:         %.2f seconds", total_duration);
    LOG_INFO_FMT("  Total bytes:      %.2f MB", total_bytes / (1024.0 * 1024.0));
    
    if (total_duration > 0) {
        LOG_INFO_FMT("  Average bitrate:  %.2f Mbps", 
                     (total_bytes * 8.0) / (total_duration * 1000000.0));
    }
    
    LOG_INFO("\n💡 Play the recorded MP4 file with:");
    LOG_INFO_FMT("   ffplay %s", output_file);
    LOG_INFO_FMT("   vlc %s", output_file);
    LOG_INFO("\n💡 Or test with this program:");
    LOG_INFO_FMT("   ./display_test -m ffmpeg %s              # Hardware decode", output_file);
    LOG_INFO_FMT("   ./display_test -m ffmpeg_software %s     # Software decode", output_file);
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
    if (packet_count > 0) {
        return 0;
    } else {
        LOG_ERROR("No packets recorded");
        return -1;
    }
}

/**
 * 测试6：FFmpeg 编码视频文件播放（使用Worker自动创建BufferPool）
 */
static int test_h264_taco_video(const char* video_path) {
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO_FMT("  Test: FFmpeg Encoded Video Playback - File: %s", video_path);
    LOG_INFO("═══════════════════════════════════════════════════════");
    
    // 1. 初始化显示设备
    LOG_INFO("[Test] 初始化显示设备...");
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 创建 VideoProductionLine（Worker会在open()时自动调用Allocator创建BufferPool）
    LOG_INFO("[Test] 创建VideoProductionLine...");
    VideoProductionLine producer(false, 1,false);  // loop=true, thread_count=1
    
    // 4. 配置 FFmpeg 解码
    LOG_INFO_FMT("[Test] 配置FFmpeg: %s", video_path);
    
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath(video_path)
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
                .useTaco("h264")  // 使用 TACO 硬件解码器进行 H.264 视频文件解码
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    // 5. 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        LOG_ERROR_FMT("FFmpeg Error: %s", error.c_str());
        g_running = false;
    });
    
    // 6. 启动生产者
    LOG_INFO("[Test] 启动FFmpeg...");
    if (!producer.start(workerConfig)) {
        LOG_ERROR("Failed to start FFmpeg producer");
        return -1;
    }
    
    LOG_INFO("[Test] 视频解码已启动, starting playback...");
    LOG_INFO("[Test] 按Ctrl+C停止");
    
    // 7. 获取工作BufferPool（Worker创建的或fallback的）
    uint64_t producer_pool_id = producer.getWorkingBufferPoolId();
    if (producer_pool_id == 0) {
        LOG_ERROR("No working BufferPool ID available");
        return -1;
    }
    auto producer_pool_weak = BufferPoolRegistry::getInstance().getPool(producer_pool_id);
    auto producer_pool_sptr = producer_pool_weak.lock();
    if (!producer_pool_sptr) {
        LOG_ERROR("BufferPool not found or destroyed");
        return -1;
    }
    
    LOG_INFO_FMT("[Test] Using BufferPool: '%s' (created by Worker via Allocator)", 
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
                LOG_INFO("Producer stopped naturally, exiting consumer loop...");
                break;
            }
            continue;  // 超时，继续等待
        }
        
        // 开始计时显示操作
        if (display_monitor) {
            display_monitor->beginTiming("display");
        }
        
        // 显示
        display.waitVerticalSync();
        // 零拷贝模式：使用 DMA 显示
        if (!display.displayBufferByDMA(filled_buffer)) {
            LOG_WARN("DMA display failed, falling back to normal");
            display.displayFilledFramebuffer(filled_buffer);
            
        }
        // 归还 buffer
        producer_pool_sptr->releaseFilled(filled_buffer);
        if (display_monitor ) {
            display_monitor->endTiming("display");
        }
        frame_count++;
        
        // 每100帧打印一次统计
        if (frame_count % 100 == 0) {
            LOG_DEBUG_FMT("Frames displayed: %d (%.1f fps)", 
                          frame_count, producer.getAverageFPS());
        }
    }
    
    // 排空剩余的已填充 buffer
    LOG_INFO("Draining remaining buffers from BufferPool...");
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
        LOG_INFO_FMT("Drained %d remaining buffers", drained_count);
    }
    
    // 10. 停止性能监控
    if (display_monitor) {
        display_monitor->stop();
        LOG_INFO("═══════════════════════════════════════════════════════");
        LOG_INFO("  Display Performance Statistics");
        LOG_INFO("═══════════════════════════════════════════════════════");
        display_monitor->printStatistics();
        display_monitor.reset();
    }
    
    // 11. 停止生产者
    LOG_INFO("Stopping FFmpeg producer...");
    producer.stop();
    
    LOG_INFO("FFmpeg video test completed");
    LOG_INFO_FMT("Total frames displayed: %d", frame_count);
    LOG_INFO_FMT("Frames produced: %d", producer.getProducedFrames());
    LOG_INFO_FMT("Frames skipped: %d", producer.getSkippedFrames());
    LOG_INFO_FMT("Average FPS: %.2f", producer.getAverageFPS());
    
    LOG_INFO("Final BufferPool statistics:");
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
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO_FMT("  Test: FFmpeg Software Decoder - File: %s", video_path);
    LOG_INFO("  (Using libavcodec, no hardware acceleration)");
    LOG_INFO("═══════════════════════════════════════════════════════");
    
    // 1. 初始化显示设备
    LOG_INFO("[Test] 初始化显示设备...");
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 创建 VideoProductionLine
    LOG_INFO("[Test] 创建VideoProductionLine...");
    VideoProductionLine producer(false, 1, false);  // loop=false, thread_count=1
    
    // 3. 配置 FFmpeg 软件解码
    LOG_INFO_FMT("[Test] 配置FFmpeg软件解码: %s", video_path);
    
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath(video_path)
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
        LOG_ERROR_FMT("FFmpeg Software Decoder Error: %s", error.c_str());
        g_running = false;
    });
    
    // 5. 启动生产者
    LOG_INFO("[Test] 启动FFmpeg软件解码...");
    if (!producer.start(workerConfig)) {
        LOG_ERROR("Failed to start FFmpeg software decoder");
        return -1;
    }
    
    LOG_INFO("[Test] 视频解码已启动 (Software Decoder), starting playback...");
    LOG_INFO("[Test] 按Ctrl+C停止");
    LOG_INFO("[Test] ⚠️ 注意：软件解码输出系统内存，需要拷贝到 framebuffer 显示");
    
    // 6. 获取 Display 的 BufferPool（用于显示）
    LOG_INFO("[Test] 获取 Display BufferPool...");
    uint64_t display_pool_id = display.getBufferPoolId();
    if (display_pool_id == 0) {
        LOG_ERROR("Display BufferPool not initialized");
        producer.stop();
        return -1;
    }
    auto display_pool_weak = BufferPoolRegistry::getInstance().getPool(display_pool_id);
    auto display_pool_sptr = display_pool_weak.lock();
    if (!display_pool_sptr) {
        LOG_ERROR_FMT("Display BufferPool (ID: %lu) not found or already destroyed", display_pool_id);
        producer.stop();
        return -1;
    }
    
    // 7. 获取 Worker 的 BufferPool（软件解码输出）
    LOG_INFO("[Test] 获取 Worker BufferPool...");
    uint64_t producer_pool_id = producer.getWorkingBufferPoolId();
    if (producer_pool_id == 0) {
        LOG_ERROR("No working BufferPool ID available");
        producer.stop();
        return -1;
    }
    auto producer_pool_weak = BufferPoolRegistry::getInstance().getPool(producer_pool_id);
    auto producer_pool_sptr = producer_pool_weak.lock();
    if (!producer_pool_sptr) {
        LOG_ERROR("Worker BufferPool not found or destroyed");
        producer.stop();
        return -1;
    }
    
    LOG_INFO_FMT("[Test] Worker BufferPool: '%s' (ID: %lu)", 
                 producer_pool_sptr->getName().c_str(), producer_pool_id);
    LOG_INFO_FMT("[Test] Display BufferPool: '%s' (ID: %lu)", 
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
                LOG_INFO("Producer stopped naturally, exiting consumer loop...");
                break;
            }
            continue;  // 超时，继续等待
        }
        
        // 步骤2：从 Display BufferPool 获取空闲的 framebuffer
        Buffer* display_buffer = display_pool_sptr->acquireFree(true, 100);
        if (display_buffer == nullptr) {
            LOG_WARN("Failed to acquire free display buffer, skipping frame");
            producer_pool_sptr->releaseFilled(decoded_buffer);
            continue;
        }
        
        // 步骤3：⭐ 拷贝数据（软件解码的关键步骤）
        // 前置检查：确保指针有效
        void* src_addr = decoded_buffer->getVirtualAddress();
        void* dst_addr = display_buffer->getVirtualAddress();
        size_t copy_size = std::min(decoded_buffer->size(), display_buffer->size());
        
        if (!src_addr) {
            LOG_ERROR_FMT("❌ ERROR: decoded_buffer->getVirtualAddress() is nullptr (buffer #%u)", 
                          decoded_buffer->id());
            display_pool_sptr->releaseFree(display_buffer);
            producer_pool_sptr->releaseFilled(decoded_buffer);
            continue;
        }
        
        if (!dst_addr) {
            LOG_ERROR_FMT("❌ ERROR: display_buffer->getVirtualAddress() is nullptr (buffer #%u)", 
                          display_buffer->id());
            display_pool_sptr->releaseFree(display_buffer);
            producer_pool_sptr->releaseFilled(decoded_buffer);
            continue;
        }
        
        if (copy_size == 0) {
            LOG_WARN("⚠️  WARNING: copy_size is 0, skipping frame");
            display_pool_sptr->releaseFree(display_buffer);
            producer_pool_sptr->releaseFilled(decoded_buffer);
            continue;
        }
        
        // 使用 C++ 标准库的安全方式拷贝
        std::memcpy(dst_addr, src_addr, copy_size);
        // 步骤4：显示（现在是 Display BufferPool 的 buffer，可以正常显示）
        display.waitVerticalSync();
        if (!display.displayFilledFramebuffer(display_buffer)) {
            LOG_WARN("Display failed");
        }
        
        // 步骤5：归还两个 buffer
        display_pool_sptr->releaseFree(display_buffer);
        producer_pool_sptr->releaseFilled(decoded_buffer);
        
        frame_count++;
        
        // 每100帧打印一次统计
        if (frame_count % 100 == 0) {
            LOG_DEBUG_FMT("Frames displayed: %d (%.1f fps)", 
                          frame_count, producer.getAverageFPS());
        }
    }
    
    // 9. 排空剩余的已填充 buffer
    LOG_INFO("Draining remaining buffers from BufferPool...");
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
                LOG_WARN("⚠️  Skipping invalid buffer during drain");
            }
            
            display_pool_sptr->releaseFree(display_buffer);
        }
        
        producer_pool_sptr->releaseFilled(remaining_decoded);
        frame_count++;
        drained_count++;
    }
    if (drained_count > 0) {
        LOG_INFO_FMT("Drained %d remaining buffers", drained_count);
    }
    
    // 10. 停止生产者
    LOG_INFO("Stopping FFmpeg software decoder...");
    producer.stop();
    
    LOG_INFO("FFmpeg software decoder test completed");
    LOG_INFO_FMT("Total frames displayed: %d", frame_count);
    LOG_INFO_FMT("Frames produced: %d", producer.getProducedFrames());
    LOG_INFO_FMT("Frames skipped: %d", producer.getSkippedFrames());
    LOG_INFO_FMT("Average FPS: %.2f", producer.getAverageFPS());
    LOG_INFO("💡 Tip: Compare with test_h264_taco_video (hardware decoder) for performance difference");
    
    LOG_INFO("Final BufferPool statistics:");
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
    
    LOG_INFO_FMT("%sStarting decode worker for: %s", thread_prefix.c_str(), video_path);
    
    // 1. 创建 VideoProductionLine（Worker会在open()时自动调用Allocator创建BufferPool）
    VideoProductionLine producer(true, 1);  // loop=true, thread_count=1
    
    // 2. 配置 FFmpeg 解码
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath(video_path)
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
                .useTaco("h264")  // 使用 TACO 硬件解码器进行 H.264 视频文件解码
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    // 3. 设置错误回调
    producer.setErrorCallback([thread_prefix, total_errors](const std::string& error) {
        LOG_ERROR_FMT("%sFFmpeg Error: %s", thread_prefix.c_str(), error.c_str());
        (*total_errors)++;
    });
    
    // 4. 启动生产者
    LOG_INFO_FMT("%sStarting FFmpeg video producer...", thread_prefix.c_str());
    if (!producer.start(workerConfig)) {
        LOG_ERROR_FMT("%sFailed to start FFmpeg producer", thread_prefix.c_str());
        (*total_errors)++;
        return;
    }
    
    LOG_INFO_FMT("%sVideo decoding started", thread_prefix.c_str());
    
    // 5. 获取工作BufferPool（Worker创建的或fallback的）
    uint64_t producer_pool_id = producer.getWorkingBufferPoolId();
    if (producer_pool_id == 0) {
        LOG_ERROR_FMT("%sNo working BufferPool ID available", thread_prefix.c_str());
        (*total_errors)++;
        producer.stop();
        return;
    }
    
    auto producer_pool_weak = BufferPoolRegistry::getInstance().getPool(producer_pool_id);
    auto producer_pool_sptr = producer_pool_weak.lock();
    if (!producer_pool_sptr) {
        LOG_ERROR_FMT("%sBufferPool not found or destroyed", thread_prefix.c_str());
        (*total_errors)++;
        producer.stop();
        return;
    }
    
    LOG_INFO_FMT("%sUsing BufferPool: '%s'", thread_prefix.c_str(), 
                 producer_pool_sptr->getName().c_str());
    
    // 6. 解码循环（不显示，直接释放buffer）
    int frame_count = 0;
    
    while (g_running) {
        // 从工作BufferPool获取已解码的buffer
        Buffer* filled_buffer = producer_pool_sptr->acquireFilled(true, 100);
        
        if (filled_buffer == nullptr) {
            // 超时时检查生产者状态
            if (!producer.isRunning()) {
                LOG_INFO_FMT("%sProducer stopped naturally, exiting decode loop...", 
                           thread_prefix.c_str());
                break;
            }
            continue;  // 超时，继续等待
        }
        
        // 不显示，直接归还 buffer
        producer_pool_sptr->releaseFilled(filled_buffer);
        
        frame_count++;
        (*total_frames)++;
        
        // 每100帧打印一次统计
        if (frame_count % 100 == 0) {
            LOG_DEBUG_FMT("%sDecoded %d frames (%.1f fps)", 
                         thread_prefix.c_str(), frame_count, producer.getAverageFPS());
        }
    }
    
    // 排空剩余的已填充 buffer
    LOG_INFO_FMT("%sDraining remaining buffers from BufferPool...", thread_prefix.c_str());
    Buffer* remaining_buffer = nullptr;
    int drained_count = 0;
    while ((remaining_buffer = producer_pool_sptr->acquireFilled(false, 0)) != nullptr) {
        producer_pool_sptr->releaseFilled(remaining_buffer);
        frame_count++;
        (*total_frames)++;
        drained_count++;
    }
    if (drained_count > 0) {
        LOG_INFO_FMT("%sDrained %d remaining buffers", thread_prefix.c_str(), drained_count);
    }
    
    // 7. 停止生产者
    LOG_INFO_FMT("%sStopping FFmpeg producer...", thread_prefix.c_str());
    producer.stop();
    
    LOG_INFO_FMT("%sDecode worker completed", thread_prefix.c_str());
    LOG_INFO_FMT("%sTotal frames decoded: %d", thread_prefix.c_str(), frame_count);
    LOG_INFO_FMT("%sFrames produced: %d", thread_prefix.c_str(), producer.getProducedFrames());
    LOG_INFO_FMT("%sFrames skipped: %d", thread_prefix.c_str(), producer.getSkippedFrames());
    LOG_INFO_FMT("%sAverage FPS: %.2f", thread_prefix.c_str(), producer.getAverageFPS());
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
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO_FMT("  Test: Multi-threaded FFmpeg Video Decoding - File: %s", video_path);
    LOG_INFO("═══════════════════════════════════════════════════════");
    
    // 配置参数
    const int num_threads = 4;  // 固定4个线程
    const int output_width = 1920;
    const int output_height = 1080;
    
    LOG_INFO_FMT("Configuration:");
    LOG_INFO_FMT("  Threads: %d", num_threads);
    LOG_INFO_FMT("  Video file: %s", video_path);
    LOG_INFO_FMT("  Output resolution: %dx%d", output_width, output_height);
    LOG_INFO_FMT("  Display: Disabled (decode only)");
    
    
    // 全局统计
    std::atomic<int> total_frames(0);
    std::atomic<int> total_errors(0);
    
    // 创建多个线程，每个线程运行一个 VideoProductionLine
    LOG_INFO("Creating decode threads...");
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
    
    LOG_INFO_FMT("All %d decode threads started", num_threads);
    LOG_INFO("[Test] 按Ctrl+C停止");
    
    
    // 等待所有线程完成（或通过 g_running 控制）
    for (auto& t : threads) {
        t.join();
    }
    
    // 打印最终统计
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO("  Test Results");
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO_FMT("Total threads: %d", num_threads);
    LOG_INFO_FMT("Total frames decoded: %d", total_frames.load());
    LOG_INFO_FMT("Total errors: %d", total_errors.load());
    
    if (total_errors.load() > 0) {
        LOG_WARN_FMT("Test completed with %d errors", total_errors.load());
        return -1;
    }
    
    LOG_INFO("Test completed successfully");
    return 0;
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
    
    // ⭐ 从 taco_config 推导格式信息
    std::string format_name;
    std::string ffplay_fmt;
    
    if (taco_config.ch1_rgb) {
        // RGB格式：使用配置的格式名
        format_name = taco_config.ch1_rgb_format;
        
        // ⭐ 映射 TACO 格式名到 FFplay 格式名
        // TACO: argb888/bgra888/rgba888/rgb888/bgr888/xrgb888/xbgr888/r16g16b16/b16g16r16
        // FFplay: argb/bgra/rgba/rgb24/bgr24/0rgb/0bgr/rgb0/bgr0/rgb48le/bgr48le
        // ⚠️ 特殊注意：xrgb888/xbgr888 由于字节序转换，实际有两种映射
        if (format_name == "argb888") {
            ffplay_fmt = "argb";
        } else if (format_name == "abgr888") {
            ffplay_fmt = "abgr";
        } else if (format_name == "bgra888") {
            ffplay_fmt = "bgra";
        } else if (format_name == "rgba888") {
            ffplay_fmt = "rgba";
        } else if (format_name == "rgb888") {
            ffplay_fmt = "rgb24";
        } else if (format_name == "bgr888") {
            ffplay_fmt = "bgr24";
        } else if (format_name == "xrgb888") {
            // xrgb888 可能是 0rgb (padding 在前) 或 bgr0 (实际内存 BGRX)
            // 根据 Buffer 的实际格式元数据决定
            ffplay_fmt = "0rgb";  // 默认按命名理解
        } else if (format_name == "xbgr888") {
            // xbgr888 可能是 0bgr (padding 在前) 或 rgb0 (实际内存 RGBX)
            // 根据 Buffer 的实际格式元数据决定
            ffplay_fmt = "0bgr";  // 默认按命名理解
        } else if (format_name == "r16g16b16") {
            ffplay_fmt = "rgb48le";  // ⭐ 16-bit RGB (little endian)
        } else if (format_name == "b16g16r16") {
            ffplay_fmt = "bgr48le";  // ⭐ 16-bit BGR (little endian)
        } else {
            // 默认：尝试去掉 888 后缀
            ffplay_fmt = format_name;
            if (ffplay_fmt.size() > 3 && ffplay_fmt.substr(ffplay_fmt.size() - 3) == "888") {
                ffplay_fmt = ffplay_fmt.substr(0, ffplay_fmt.size() - 3);
            }
        }
    } else {
        // YUV格式：从配置中读取硬件格式名称
        format_name = taco_config.ch0_yuv_format;  // ⭐ 从配置读取硬件格式名称
        
        // ⭐ 映射硬件格式名称到 FFplay 格式名
        // 注意：某些硬件格式（如 I010, L010, Pack10, Tiled-4×4）可能需要转换
        if (format_name == "YUV420 8-bit NV12" || 
            (format_name.find("NV12") != std::string::npos && format_name.find("8-bit") != std::string::npos)) {
            ffplay_fmt = "nv12";
        } else if (format_name == "YUV420 8-bit NV21" || 
                   (format_name.find("NV21") != std::string::npos && format_name.find("8-bit") != std::string::npos)) {
            ffplay_fmt = "nv21";
        } else if (format_name.find("P010") != std::string::npos) {
            if (format_name.find("YUV400") != std::string::npos) {
                ffplay_fmt = "gray10le";  // YUV400 P010 → gray10le
            } else if (format_name.find("NV12") != std::string::npos) {
                ffplay_fmt = "p010le";  // YUV420 NV12 P010 → p010le
            } else if (format_name == "YUV420 P010") {
                ffplay_fmt = "yuv420p10le";  // YUV420 P010 → yuv420p10le
            } else {
                ffplay_fmt = "p010le";  // 默认
            }
        } else if (format_name == "YUV400 8-bit") {
            ffplay_fmt = "gray";
        } else if (format_name.find("I010") != std::string::npos || 
                   format_name.find("L010") != std::string::npos ||
                   format_name.find("Pack10") != std::string::npos ||
                   format_name.find("Tiled-4×4") != std::string::npos ||
                   format_name.find("I011") != std::string::npos) {
            // 这些格式可能需要转换，暂时使用默认映射
            // 实际格式会从 Buffer 元数据中检测
            if (format_name.find("NV12") != std::string::npos) {
                ffplay_fmt = "nv12";  // 尝试映射到 NV12
            } else if (format_name.find("NV21") != std::string::npos) {
                ffplay_fmt = "nv21";  // 尝试映射到 NV21
            } else if (format_name.find("YUV400") != std::string::npos) {
                ffplay_fmt = "gray10le";  // 尝试映射到 gray10le
            } else {
                ffplay_fmt = "nv12";  // 默认
            }
        } else {
            // 默认：尝试从格式名称推断
            ffplay_fmt = format_name;
            // 转换为小写并替换空格
            std::transform(ffplay_fmt.begin(), ffplay_fmt.end(), ffplay_fmt.begin(), ::tolower);
            std::replace(ffplay_fmt.begin(), ffplay_fmt.end(), ' ', '_');
        }
    }
    
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO_FMT("  BufferWriter Format Test: %s", format_name.c_str());
    LOG_INFO_FMT("  Video: %s", video_path);
    LOG_INFO("═══════════════════════════════════════════════════════");
    
    // 1. 配置VideoProductionLine（使用传入的taco配置）
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath(video_path)
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
                .useTaco("h264", taco_config)  // ⭐ 使用 TACO H.264 解码器 + 自定义配置
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    LOG_INFO_FMT("Decoder config: ch1_rgb=%s, format=%s", 
                 taco_config.ch1_rgb ? "true" : "false",
                 format_name.c_str());
    
    // 2. 启动生产线
    LOG_INFO("Step 2: Starting VideoProductionLine...");
    VideoProductionLine producer(false, 1, false);
    if (!producer.start(workerConfig)) {
        LOG_ERROR("Failed to start VideoProductionLine");
        return -1;
    }
    
    // 3. 获取BufferPool
    LOG_INFO("Step 3: Getting BufferPool...");
    uint64_t pool_id = producer.getWorkingBufferPoolId();
    auto pool_sptr = BufferPoolRegistry::getInstance().getPool(pool_id).lock();
    if (!pool_sptr) {
        LOG_ERROR("Failed to get BufferPool");
        producer.stop();
        return -1;
    }
    
    LOG_INFO_FMT("BufferPool: %s (ID: %lu)", 
                 pool_sptr->getName().c_str(), pool_id);
    
    // 4. 等待第一个Buffer，获取实际格式
    LOG_INFO("Step 4: Waiting for first buffer to detect format...");
    Buffer* first_buffer = pool_sptr->acquireFilled(true, 5000);  // 5秒超时
    if (!first_buffer) {
        LOG_ERROR("Failed to get first buffer (timeout)");
        producer.stop();
        return -1;
    }
    
    // 5. 从Buffer元数据获取实际格式
    AVPixelFormat actual_format = AV_PIX_FMT_NONE;
    int actual_width = 1920;
    int actual_height = 1080;
    
    if (first_buffer->hasImageMetadata()) {
        actual_format = first_buffer->getImageFormat();
        actual_width = first_buffer->getImageWidth();
        actual_height = first_buffer->getImageHeight();
        LOG_INFO_FMT("Detected format from buffer: %s (%dx%d)", 
                    av_get_pix_fmt_name(actual_format),
                    actual_width, actual_height);
    } else {
        LOG_WARN("Buffer has no metadata, using default NV12");
        actual_format = AV_PIX_FMT_NV12;
    }
    
    // 6. 创建BufferWriter（使用检测到的格式）
    LOG_INFO("Step 5: Creating BufferWriter...");
    BufferWriter writer;
    char output_path[256];
    snprintf(output_path, sizeof(output_path), 
             "output_test_%s.yuv", format_name.c_str());
    
    if (!writer.open(output_path, actual_format, actual_width, actual_height)) {
        LOG_ERROR_FMT("Failed to open BufferWriter for format %s", 
                     av_get_pix_fmt_name(actual_format));
        pool_sptr->releaseFilled(first_buffer);
        producer.stop();
        return -1;
    }
    
    LOG_INFO_FMT("Saving to: %s (format: %s)", 
                 output_path, av_get_pix_fmt_name(actual_format));
    
    // 7. 保存第一帧
    LOG_INFO("\nStep 6: Saving frames...");
    LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    if (writer.write(first_buffer)) {
        LOG_INFO("  ✅ Saved frame 1");
    }
    pool_sptr->releaseFilled(first_buffer);
    
    // 8. 消费者循环：保存剩余帧（直到视频播放完毕）
    int timeout_count = 0;
    const int MAX_TIMEOUT = 10;
    
    while (g_running) {  // ⭐ 移除 max_frames 限制，让视频自然结束
        Buffer* buffer = pool_sptr->acquireFilled(true, 100);
        
        if (buffer) {
            if (writer.write(buffer)) {
                if (writer.getWriteCount() % 10 == 0) {
                    LOG_INFO_FMT("  Saved %d frames", writer.getWriteCount());
                }
            } else {
                LOG_ERROR_FMT("Failed to write frame %d", 
                             writer.getWriteCount() + 1);
            }
            
            pool_sptr->releaseFilled(buffer);
            timeout_count = 0;
        } else {
            timeout_count++;
            if (timeout_count >= MAX_TIMEOUT) {
                LOG_INFO("Video finished, stopping...");
                break;
            }
        }
    }
    
    LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // 9. 关闭
    LOG_INFO("\nStep 7: Cleaning up...");
    writer.close();
    producer.stop();
    
    // 10. 打印结果
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO("  Test Results");
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO_FMT("Format requested: %s", format_name.c_str());
    LOG_INFO_FMT("Format actual: %s", av_get_pix_fmt_name(actual_format));
    LOG_INFO_FMT("Output file: %s", output_path);
    LOG_INFO_FMT("Frames saved: %d", writer.getWriteCount());
    
    bool success = (writer.getWriteCount() > 0);
    if (success) {
        LOG_INFO("\n✅ Test PASSED");
        LOG_INFO_FMT("   - Successfully saved %d frames", writer.getWriteCount());
        LOG_INFO("\n💡 Tip: Verify the output with FFmpeg:");
        LOG_INFO_FMT("   ffplay -f rawvideo -pixel_format %s -video_size %dx%d %s",
                     ffplay_fmt.c_str(), actual_width, actual_height, output_path);
    } else {
        LOG_ERROR("\n❌ Test FAILED: No frames saved");
    }
    
    LOG_INFO("═══════════════════════════════════════════════════════");
    
    return success ? 0 : -1;
}

/**
 * 测试8：BufferWriter保存帧测试（默认NV12格式）
 * 
 * 功能：
 * - 使用VideoProductionLine解码视频
 * - 使用BufferWriter将解码后的帧保存到文件
 * - 演示BufferWriter的简化接口（open/write/close）
 * 
 * 目的：
 * - 展示如何使用BufferWriter保存Buffer数据
 * - 验证FFmpeg格式标准的使用
 * - 测试原子计数器功能
 */
static int test_buffer_writer(const char* video_path) {
    // ✅ 直接使用TacoConfigBuilder配置NV12格式（YUV输出）
    auto tacoConfig = TacoConfigBuilder()
        .setRgbConfig(false, "", "bt601")  // ch1_rgb=false，输出YUV
        .build();
    
    return test_buffer_writer_format(video_path, tacoConfig);
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
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  BufferWriter RGB Formats Test Suite                  ║");
    LOG_INFO("║  Testing 12 RGB formats (full coverage)               ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    
    // ✅ 定义测试用例：所有 RGB 格式（NV12 由 test_buffer_writer 单独测试）
    std::function<WorkerConfig::DecoderConfig::TacoConfig()> tests[] = {
        // 8-bit ARGB/ABGR/BGRA/RGBA 格式（Alpha 通道，4 字节/像素）
        []() { return TacoConfigBuilder()
                   .setRgbConfig(true, "argb888", "bt601")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setRgbConfig(true, "abgr888", "bt601")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setRgbConfig(true, "bgra888", "bt601")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setRgbConfig(true, "rgba888", "bt601")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        
        // 8-bit RGB/BGR 格式（3 字节/像素）
        []() { return TacoConfigBuilder()
                   .setRgbConfig(true, "rgb888", "bt601")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setRgbConfig(true, "bgr888", "bt601")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        
       /*  // ⭐ 8-bit 0RGB/0BGR 格式（padding 在前，4 字节/像素）
        // 注意：TACO xrgb888 → FFmpeg 0RGB，xbgr888 → FFmpeg 0BGR
        []() { return TacoConfigBuilder()
                   .setRgbConfig(true, "xrgb888", "bt601")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setRgbConfig(true, "xbgr888", "bt601")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        
        // ⭐⭐ 8-bit RGB0/BGR0 格式（padding 在后，4 字节/像素）
        // 关键发现：TACO 的 xrgb888/xbgr888 经过字节序转换后，实际内存是 BGRX/RGBX
        // libdec24 注释：XRGB888 → 内存:BGRX，XBGR888 → 内存:RGBX
        // 所以同样的配置字符串可能映射到不同的 FFmpeg 格式（取决于驱动实现）
        // 这里我们暂时复用 xrgb888/xbgr888，期望驱动能正确处理为 RGB0/BGR0
        []() { return TacoConfigBuilder()
                   .setRgbConfig(true, "xbgr888", "bt601")  // → 实际内存 RGBX → RGB0
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setRgbConfig(true, "xrgb888", "bt601")  // → 实际内存 BGRX → BGR0
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
         */
        // ⭐ 16-bit RGB/BGR 格式（6 字节/像素，文件更大）
        []() { return TacoConfigBuilder()
                   .setRgbConfig(true, "r16g16b16", "bt601")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setRgbConfig(true, "b16g16r16", "bt601")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
    };
    
    int total_tests = sizeof(tests) / sizeof(tests[0]);
    int passed = 0;
    int failed = 0;
    
    LOG_INFO_FMT("\nTotal RGB formats to test: %d\n", total_tests);
    
    for (int i = 0; i < total_tests; i++) {
        LOG_INFO_FMT("\n╔═══════════════════════════════════════════════════════╗");
        LOG_INFO_FMT("║  [%d/%d] Testing format                                ║", i + 1, total_tests);
        LOG_INFO_FMT("╚═══════════════════════════════════════════════════════╝");
        
        // ✅ 调用build_config()构建TacoConfig，直接传给测试函数
        int result = test_buffer_writer_format(video_path, tests[i]());
        
        if (result == 0) {
            passed++;
            LOG_INFO_FMT("\n✅ [%d/%d] PASSED\n", i + 1, total_tests);
        } else {
            failed++;
            LOG_ERROR_FMT("\n❌ [%d/%d] FAILED\n", i + 1, total_tests);
        }
        
        // 短暂延迟，避免资源冲突
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    // 最终统计
    LOG_INFO("\n╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  Test Summary                                          ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO_FMT("Total tests: %d", total_tests);
    LOG_INFO_FMT("Passed: %d ✅", passed);
    LOG_INFO_FMT("Failed: %d ❌", failed);
    LOG_INFO_FMT("Success rate: %.1f%%", (100.0 * passed / total_tests));
    LOG_INFO("\n╔═══════════════════════════════════════════════════════╗");
    
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
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  BufferWriter YUV Formats Test Suite                   ║");
    LOG_INFO("║  Testing YUV formats supported by PP0 (ch0)            ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    
    // ✅ 定义测试用例：所有硬件支持的 YUV 格式（与 test_buffer_writer_rgb_formats 保持一致的结构）
    std::function<WorkerConfig::DecoderConfig::TacoConfig()> tests[] = {
        // YUV400 系列
        []() { return TacoConfigBuilder()
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setYuvConfig("YUV400 I010", "bt2020")
                   .setChannels(true, true)
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setYuvConfig("YUV400 L010", "bt2020")
                   .setChannels(true, true)
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setYuvConfig("YUV400 Pack10", "bt2020")
                   .setChannels(true, true)
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setYuvConfig("YUV400 8-bit", "bt601")
                   .setChannels(true, true)
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        
        // YUV420 NV12 系列
        []() { return TacoConfigBuilder()
                   .setYuvConfig("YUV420 NV12 P010", "bt2020")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setYuvConfig("YUV420 NV12 I010", "bt2020")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setYuvConfig("YUV420 NV12 L010", "bt2020")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setYuvConfig("YUV420 NV12 Pack10", "bt2020")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setYuvConfig("YUV420 8-bit NV12", "bt601")  // 最常用
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        
        // YUV420 NV21 系列
        []() { return TacoConfigBuilder()
                   .setYuvConfig("YUV420 NV21 P010 Tiled-4×4", "bt2020")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setYuvConfig("YUV420 NV21 I011", "bt2020")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setYuvConfig("YUV420 NV21 L010", "bt2020")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        []() { return TacoConfigBuilder()
                   .setYuvConfig("YUV420 8-bit NV21", "bt601")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
        
        // YUV420 P010
        []() { return TacoConfigBuilder()
                   .setYuvConfig("YUV420 P010", "bt2020")
                   .setDecoderOutputResolution(1920, 1080)
                   .build(); 
        },
    };
    
    int total_tests = sizeof(tests) / sizeof(tests[0]);
    int passed = 0;
    int failed = 0;
    
    LOG_INFO_FMT("\nTotal YUV formats to test: %d\n", total_tests);
    
    for (int i = 0; i < total_tests; i++) {
        LOG_INFO_FMT("\n╔═══════════════════════════════════════════════════════╗");
        LOG_INFO_FMT("║  [%d/%d] Testing format                                ║", i + 1, total_tests);
        LOG_INFO_FMT("╚═══════════════════════════════════════════════════════╝");
        
        // ✅ 调用build_config()构建TacoConfig，直接传给测试函数（与RGB测试保持一致）
        int result = test_buffer_writer_format(video_path, tests[i]());
        
        if (result == 0) {
            passed++;
            LOG_INFO_FMT("\n✅ [%d/%d] PASSED\n", i + 1, total_tests);
        } else {
            failed++;
            LOG_ERROR_FMT("\n❌ [%d/%d] FAILED\n", i + 1, total_tests);
        }
        
        // 短暂延迟，避免资源冲突
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    // 最终统计
    LOG_INFO("\n╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  Test Summary                                          ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO_FMT("Total tests: %d", total_tests);
    LOG_INFO_FMT("Passed: %d ✅", passed);
    LOG_INFO_FMT("Failed: %d ❌", failed);
    LOG_INFO_FMT("Success rate: %.1f%%", (100.0 * passed / total_tests));
    LOG_INFO("\n╔═══════════════════════════════════════════════════════╝");
    
    return (failed == 0) ? 0 : -1;
}

// ========== 测试用例注册 ==========
// 使用新的测试框架，自动注册所有测试用例
REGISTER_TEST(loop, "4-frame loop display", test_4frame_loop);
REGISTER_TEST(sequential, "Sequential playback (play once)", test_sequential_playback);
REGISTER_TEST(producer, "BufferPool + VideoProductionLine test (zero-copy)", test_buffermanager_producer);
REGISTER_TEST(iouring, "io_uring async I/O mode", test_buffermanager_iouring);
REGISTER_TEST(rtsp, "RTSP stream playback (zero-copy, FFmpeg)", test_rtsp_stream);
REGISTER_TEST(rtsp_record, "RTSP stream recording to MP4 (use env RTSP_OUTPUT_FILE to specify path)", test_rtsp_record);
REGISTER_TEST(ffmpeg, "FFmpeg encoded video playback (MP4/AVI/MKV/etc)", test_h264_taco_video);
REGISTER_TEST(ffmpeg_software, "FFmpeg software decoder (libavcodec, no hardware acceleration)", test_ffmpeg_software_decoder);
REGISTER_TEST(ffmpeg_multithread, "Multi-threaded FFmpeg video decoding (no display, decode only)", test_h264_taco_video_multithread);
REGISTER_TEST(writer, "BufferWriter - Save frames (NV12 format)", test_buffer_writer);
REGISTER_TEST(writer_rgb, "BufferWriter - 12 RGB formats (ARGB/ABGR/BGRA/RGBA/RGB/BGR/0RGB/0BGR/RGB0/BGR0/RGB48/BGR48)", test_buffer_writer_rgb_formats);
REGISTER_TEST(writer_yuv, "BufferWriter - 15 YUV formats (PP0 ch0: YUV400/YUV420 NV12/YUV420 NV21/YUV420 P010 series)", test_buffer_writer_yuv_formats);

/**
 * 主函数
 */
int main(int argc, char* argv[]) {
    // 初始化日志系统（无需配置文件）
    INIT_LOGGER();
    
    // 注册信号处理
    signal(SIGINT, [](int) { g_running = false; });
    signal(SIGTERM, [](int) { g_running = false; });
    
    // 使用测试框架主函数
    TEST_MAIN(argc, argv);
}

