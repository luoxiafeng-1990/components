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
#include <string.h>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <sstream>
#include <algorithm>
#include "display/LinuxFramebufferDevice.hpp"
#include "productionline/worker/BufferFillingWorkerFacade.hpp"
#include "productionline/worker/WorkerConfig.hpp"
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
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO("  Test: Multi-Buffer Loop Display (Using VideoProductionLine)");
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
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
        .setOutputConfig(
            OutputConfigBuilder()
                .setResolution(display.getWidth(), display.getHeight())
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
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO("  Test: Sequential Playback (Using VideoProductionLine)");
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
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
        .setOutputConfig(
            OutputConfigBuilder()
                .setResolution(display.getWidth(), display.getHeight())
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
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO("  Test: BufferPool + VideoProductionLine (New Architecture)");
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
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
        .setOutputConfig(
            OutputConfigBuilder()
                .setResolution(display.getWidth(), display.getHeight())
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
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO("  Test: io_uring Mode (using VideoProductionLine temporarily)");
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
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
        .setOutputConfig(
            OutputConfigBuilder()
                .setResolution(display.getWidth(), display.getHeight())
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
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO("  Test: RTSP Stream Playback (Independent BufferPool + DMA)");
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
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
        .setOutputConfig(
            OutputConfigBuilder()
                .setResolution(display.getWidth(), display.getHeight())
                .setBitsPerPixel(display.getBitsPerPixel())
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useH264Taco()  // 使用 h264_taco 硬件解码器进行 RTSP 流解码
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
    
    LOG_INFO_FMT("Using BufferPool: '%s' (created by Worker via Allocator)", 
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
 * 测试6：FFmpeg 编码视频文件播放（使用Worker自动创建BufferPool）
 */
static int test_h264_taco_video(const char* video_path) {
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO_FMT("  Test: FFmpeg Encoded Video Playback - File: %s", video_path);
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
    // 1. 初始化显示设备
    LOG_INFO("[Test] 初始化显示设备...");
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 创建 VideoProductionLine（Worker会在open()时自动调用Allocator创建BufferPool）
    LOG_INFO("Creating VideoProductionLine...");
    VideoProductionLine producer(false, 1,false);  // loop=true, thread_count=1
    
    // 4. 配置 FFmpeg 解码
    LOG_INFO_FMT("Configuring FFmpeg video reader: %s", video_path);
    
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath(video_path)
                .build()
        )
        .setOutputConfig(
            OutputConfigBuilder()
                .setResolution(display.getWidth(), display.getHeight())
                .setBitsPerPixel(display.getBitsPerPixel())
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useH264Taco()  // 使用 h264_taco 硬件解码器进行视频文件解码
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
    
    LOG_INFO_FMT("Using BufferPool: '%s' (created by Worker via Allocator)", 
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
        LOG_INFO("\n═══════════════════════════════════════════════════════");
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
        .setOutputConfig(
            OutputConfigBuilder()
                .setResolution(width, height)
                .setBitsPerPixel(32)  // 固定32位，因为不显示，这个值不影响解码
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useH264Taco()  // 使用 h264_taco 硬件解码器进行视频文件解码
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
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO_FMT("  Test: Multi-threaded FFmpeg Video Decoding - File: %s", video_path);
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
    // 配置参数
    const int num_threads = 4;  // 固定4个线程
    const int output_width = 1920;
    const int output_height = 1080;
    
    LOG_INFO_FMT("Configuration:");
    LOG_INFO_FMT("  Threads: %d", num_threads);
    LOG_INFO_FMT("  Video file: %s", video_path);
    LOG_INFO_FMT("  Output resolution: %dx%d", output_width, output_height);
    LOG_INFO_FMT("  Display: Disabled (decode only)");
    LOG_INFO("");
    
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
    LOG_INFO("");
    
    // 等待所有线程完成（或通过 g_running 控制）
    for (auto& t : threads) {
        t.join();
    }
    
    // 打印最终统计
    LOG_INFO("\n═══════════════════════════════════════════════════════");
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
 * 测试8：BufferWriter保存帧测试（简化版）
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
    using namespace productionline::io;
    
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO_FMT("  Test 8: BufferWriter - Save Frames (Simplified API)");
    LOG_INFO_FMT("  Video: %s", video_path);
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
    // 1. 配置VideoProductionLine
    LOG_INFO("Step 1: Configuring VideoProductionLine...");
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath(video_path)
                .build()
        )
        .setOutputConfig(
            OutputConfigBuilder()
                .setResolution(1920, 1080)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useH264Taco()
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    // 2. 启动生产线
    LOG_INFO("Step 2: Starting VideoProductionLine...");
    VideoProductionLine producer(false, 1, false);  // loop=false, thread_count=1, no_display=true
    if (!producer.start(workerConfig)) {
        LOG_ERROR("Failed to start VideoProductionLine");
        return -1;
    }
    
    // 3. 获取BufferPool
    LOG_INFO("Step 3: Getting BufferPool...");
    uint64_t pool_id = producer.getWorkingBufferPoolId();
    auto& registry = BufferPoolRegistry::getInstance();
    auto pool_weak = registry.getPool(pool_id);
    auto pool_sptr = pool_weak.lock();
    
    if (!pool_sptr) {
        LOG_ERROR("Failed to get BufferPool");
        producer.stop();
        return -1;
    }
    
    LOG_INFO_FMT("BufferPool: %s (ID: %lu)", 
                 pool_sptr->getName().c_str(), pool_id);
    
    // 4. 创建BufferWriter（使用FFmpeg标准格式）
    LOG_INFO("Step 4: Creating BufferWriter (using FFmpeg format)...");
    BufferWriter writer;
    
    const char* output_path = "output_test_argb.raw";
    
    // ⭐ 使用FFmpeg标准格式定义（AV_PIX_FMT_ARGB）
    if (!writer.open(output_path, AV_PIX_FMT_ARGB, 1920, 1080)) {
        LOG_ERROR("Failed to open BufferWriter");
        producer.stop();
        return -1;
    }
    
    // 5. 消费者循环：获取Buffer并保存
    LOG_INFO("\nStep 5: Consuming and saving frames...");
    LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    const int MAX_FRAMES = 100;  // 最多保存100帧
    int timeout_count = 0;
    const int MAX_TIMEOUT = 10;  // 最多10次超时
    
    while (writer.getWriteCount() < MAX_FRAMES && g_running) {
        // 从BufferPool获取填充好的Buffer
        Buffer* buffer = pool_sptr->acquireFilled(true, 100);  // 超时100ms
        
        if (buffer) {
            // ⭐ 保存Buffer到文件（自动累加计数器）
            if (writer.write(buffer)) {
                // 每10帧打印一次进度
                if (writer.getWriteCount() % 10 == 0) {
                    LOG_INFO_FMT("  ✅ Saved frame %d (buffer #%u, %zu bytes)", 
                                 writer.getWriteCount(), buffer->id(), buffer->size());
                }
            } else {
                LOG_ERROR_FMT("Failed to write frame %d", writer.getWriteCount() + 1);
            }
            
            // 归还Buffer到BufferPool
            pool_sptr->releaseFilled(buffer);
            
            // 重置超时计数
            timeout_count = 0;
        } else {
            // 超时或没有更多帧
            timeout_count++;
            if (timeout_count >= MAX_TIMEOUT) {
                LOG_INFO("No more frames available, stopping...");
                break;
            }
        }
    }
    
    LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // 6. 关闭Writer和生产线
    LOG_INFO("\nStep 6: Cleaning up...");
    writer.close();
    producer.stop();
    
    // 7. 打印统计信息
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO("  Test Results");
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO_FMT("Output file: %s", output_path);
    LOG_INFO_FMT("Pixel format: ARGB8888 (AV_PIX_FMT_ARGB)");
    LOG_INFO_FMT("Resolution: 1920x1080");
    LOG_INFO_FMT("Frames written: %d", writer.getWriteCount());
    
    // 8. 验证结果
    bool success = (writer.getWriteCount() > 0);
    if (success) {
        LOG_INFO("\n✅ Test PASSED");
        LOG_INFO_FMT("   - Successfully saved %d frames", writer.getWriteCount());
        LOG_INFO("\n💡 Tip: Verify the output with FFmpeg:");
        LOG_INFO_FMT("   ffplay -f rawvideo -pix_fmt argb -s 1920x1080 %s", output_path);
    } else {
        LOG_ERROR("\n❌ Test FAILED: No frames saved");
    }
    
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
    return success ? 0 : -1;
}

// ========== 测试用例注册 ==========
// 使用新的测试框架，自动注册所有测试用例
REGISTER_TEST(loop, "4-frame loop display", test_4frame_loop);
REGISTER_TEST(sequential, "Sequential playback (play once)", test_sequential_playback);
REGISTER_TEST(producer, "BufferPool + VideoProductionLine test (zero-copy)", test_buffermanager_producer);
REGISTER_TEST(iouring, "io_uring async I/O mode", test_buffermanager_iouring);
REGISTER_TEST(rtsp, "RTSP stream playback (zero-copy, FFmpeg)", test_rtsp_stream);
REGISTER_TEST(ffmpeg, "FFmpeg encoded video playback (MP4/AVI/MKV/etc)", test_h264_taco_video);
REGISTER_TEST(ffmpeg_multithread, "Multi-threaded FFmpeg video decoding (no display, decode only)", test_h264_taco_video_multithread);
REGISTER_TEST(writer, "BufferWriter - Save frames to file", test_buffer_writer);

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

