/**
 * Display Framework Test Program
 * 
 * 测试 LinuxFramebufferDevice, BufferFillingWorkerFacade, PerformanceMonitor, BufferManager 四个类的功能
 * 
 * 编译命令：
 *   g++ -o test test.cpp \
 *       source/LinuxFramebufferDevice.cpp \
 *       source/productionline/worker/facade/BufferFillingWorkerFacade.cpp \
 *       source/PerformanceMonitor.cpp \
 *       source/BufferManager.cpp \
 *       -I./include -std=c++17 -pthread
 * 
 * 运行命令：
 *   ./test <raw_video_file>
 * 
 * 示例：
 *   ./test /usr/testdata/ids/test_video_argb888.raw
 *   ./test -m producer /usr/testdata/ids/test_video_argb888.raw
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <getopt.h>
#include <string>
#include <vector>
#include <memory>
#include "display/LinuxFramebufferDevice.hpp"
#include "productionline/worker/facade/BufferFillingWorkerFacade.hpp"
#include "buffer/BufferPool.hpp"
#include "productionline/VideoProductionLine.hpp"

// FFmpeg头文件（解码器测试使用）
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

// 全局标志，用于处理 Ctrl+C 退出
static volatile bool g_running = true;

// 测试模式枚举
enum class TestMode {
    LOOP,
    SEQUENTIAL,
    PRODUCER,
    IOURING,
    RTSP,
    FFMPEG,
    UNKNOWN
};

// 将字符串转换为测试模式枚举
static TestMode parse_test_mode(const char* mode_str) {
    if (strcmp(mode_str, "loop") == 0) {
        return TestMode::LOOP;
    } else if (strcmp(mode_str, "sequential") == 0) {
        return TestMode::SEQUENTIAL;
    } else if (strcmp(mode_str, "producer") == 0) {
        return TestMode::PRODUCER;
    } else if (strcmp(mode_str, "iouring") == 0) {
        return TestMode::IOURING;
    } else if (strcmp(mode_str, "rtsp") == 0) {
        return TestMode::RTSP;
    } else if (strcmp(mode_str, "ffmpeg") == 0) {
        return TestMode::FFMPEG;
    } else {
        return TestMode::UNKNOWN;
    }
}

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
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Test: Multi-Buffer Loop Display (Using VideoProductionLine)\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    // 1. 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    int buffer_count = display.getBufferCount();
    
    // 2. 获取 display 的 BufferPool（framebuffer 已托管）
    BufferPool& pool = *display.getBufferPool();
    
    // 3. 创建 VideoProductionLine（Worker会在open()时自动创建BufferPool）
    VideoProductionLine producer;
    
    // 4. 配置并启动视频生产者
    VideoProductionLine::Config config(
        raw_video_path,
        display.getWidth(),
        display.getHeight(),
        display.getBitsPerPixel(),
        true,  // loop
        1,     // 单线程，顺序加载帧
        BufferFillingWorkerFactory::WorkerType::MMAP_RAW  // 显式指定 MMAP Worker
    );
    
    // 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        printf("\n❌ Producer Error: %s\n", error.c_str());
        g_running = false;
    });
    
    if (!producer.start(config)) {
        printf("❌ Failed to start video producer\n");
        return -1;
    }
    
    // 5. 加载帧到 framebuffer（从Worker的BufferPool获取）
    printf("\n📥 Loading %d frames into framebuffer...\n", buffer_count);
    BufferPool* worker_pool = producer.getWorkingBufferPool();
    if (!worker_pool) {
        printf("❌ ERROR: Worker failed to create BufferPool\n");
        producer.stop();
        return -1;
    }
    
    // 等待生产者填充buffer（生产者线程会自动填充）
    // 这里我们等待足够多的帧被填充
    for (int i = 0; i < buffer_count; i++) {
        Buffer* filled_buffer = worker_pool->acquireFilled(true, 5000);
        if (!filled_buffer || !filled_buffer->isValid()) {
            printf("❌ ERROR: Failed to acquire filled buffer %d\n", i);
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
            display.displayBuffer(&pool, buf_idx);
        }
        
        loop_count++;
    }
    
    // 7. 停止生产者
    producer.stop();
    
    printf("\n🛑 Playback stopped\n\n");
    printf("\n✅ Test completed successfully\n");
    
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
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Test: Sequential Playback (Using VideoProductionLine)\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    // 1. 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 获取 display 的 BufferPool（framebuffer 已托管）
    BufferPool& pool = *display.getBufferPool();
    (void)pool;  // 消除未使用警告
    
    // 3. 创建 VideoProductionLine（Worker会在open()时自动创建BufferPool）
    VideoProductionLine producer;
    
    // 4. 配置并启动视频生产者
    VideoProductionLine::Config config(
        raw_video_path,
        display.getWidth(),
        display.getHeight(),
        display.getBitsPerPixel(),
        true,  // loop
        1,     // 单线程，顺序读取
        BufferFillingWorkerFactory::WorkerType::MMAP_RAW  // 显式指定 MMAP Worker
    );
    
    // 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        printf("\n❌ Producer Error: %s\n", error.c_str());
        g_running = false;
    });
    
    if (!producer.start(config)) {
        printf("❌ Failed to start video producer\n");
        return -1;
    }
    
    // 5. 开始播放
    printf("\n🎬 Starting sequential playback (Ctrl+C to stop)...\n\n");
    
    // 6. 消费者循环：从 BufferPool 获取 buffer 并显示
    int frame_count = 0;
    BufferPool* worker_pool = producer.getWorkingBufferPool();
    if (!worker_pool) {
        printf("❌ ERROR: Worker failed to create BufferPool\n");
        producer.stop();
        return -1;
    }
    
    while (g_running) {
        // 获取一个已填充的 buffer（阻塞，100ms超时）
        Buffer* filled_buffer = worker_pool->acquireFilled(true, 100);
        if (filled_buffer == nullptr) {
            // 超时，继续等待
            continue;
        }
        
        // 直接显示（零拷贝）
        display.waitVerticalSync();
        if (!display.displayFilledFramebuffer(filled_buffer)) {
            printf("⚠️  Warning: Failed to display buffer\n");
        }
        
        // 归还 buffer 到空闲队列
        worker_pool->releaseFilled(filled_buffer);
        frame_count++;
        
        // 每100帧打印一次进度
        if (frame_count % 100 == 0) {
            printf("   Frames displayed: %d (%.1f fps)\n", 
                   frame_count, producer.getAverageFPS());
        }
    }
    
    // 7. 停止生产者
    producer.stop();
    
    printf("\n🛑 Playback stopped\n\n");
    printf("   Total frames played: %d\n", frame_count);
    printf("\n✅ Test completed successfully\n");
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
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Test: BufferPool + VideoProductionLine (New Architecture)\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    // 1. 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 获取 display 的 BufferPool（framebuffer 已托管）
    BufferPool& pool = *display.getBufferPool();
    pool.printStats();
    
    // 3. 创建 VideoProductionLine（Worker会自动创建BufferPool）
    VideoProductionLine producer;
    // 4. 配置并启动视频生产者
    int producer_thread_count = 2;  // 使用2个生产者线程
    
    VideoProductionLine::Config config(
        raw_video_path,
        display.getWidth(),
        display.getHeight(),
        display.getBitsPerPixel(),
        true,  // loop
        producer_thread_count,
        BufferFillingWorkerFactory::WorkerType::MMAP_RAW  // 显式指定 MMAP Worker
    );
    
    // 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        printf("\n❌ Producer Error: %s\n", error.c_str());
        g_running = false;
    });
    
    if (!producer.start(config)) {
        printf("❌ Failed to start video producer\n");
        return -1;
    }
    
    // 5. 消费者循环：从 BufferPool 获取 buffer 并显示（零拷贝）
    int frame_count = 0;
    
    while (g_running) {
        // 获取一个已填充的 buffer（阻塞，100ms超时）
        Buffer* filled_buffer = pool.acquireFilled(true, 100);
        if (filled_buffer == nullptr) {
            // 超时，继续等待
            continue;
        }
        // 直接显示（无需拷贝，buffer 本身就是 framebuffer）
        display.waitVerticalSync();
        if (!display.displayFilledFramebuffer(filled_buffer)) {
            printf("⚠️  Warning: Failed to display buffer\n");
        }
        // 归还 buffer 到空闲队列
        pool.releaseFilled(filled_buffer);
        frame_count++;
        // 每100帧打印一次进度
        if (frame_count % 100 == 0) {
            printf("   Frames displayed: %d (%.1f fps)\n", 
                   frame_count, producer.getAverageFPS());
        }
    }
    
    // 6. 停止生产者
    producer.stop();
    pool.printStats();
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
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Test: io_uring Mode (using VideoProductionLine temporarily)\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    printf("ℹ️  Note: IoUringVideoProductionLine not yet implemented in new architecture\n");
    printf("   Using standard VideoProductionLine as fallback\n\n");
    
    // 1. 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    printf("📺 Display initialized:\n");
    printf("   Resolution: %dx%d\n", display.getWidth(), display.getHeight());
    printf("   Bits per pixel: %d\n", display.getBitsPerPixel());
    printf("   Buffer count: %d\n", display.getBufferCount());
    
    // 2. 获取 display 的 BufferPool
    BufferPool& pool = *display.getBufferPool();
    
    printf("\n📦 Using LinuxFramebufferDevice's BufferPool\n");
    pool.printStats();
    
    // 3. 创建 VideoProductionLine（Worker会自动创建BufferPool）
    VideoProductionLine producer;
    
    printf("\n🎬 Starting video producer (io_uring mode)...\n");
    printf("   Using 1 producer thread with io_uring async I/O\n");
    
    VideoProductionLine::Config config(
        raw_video_path,
        display.getWidth(),
        display.getHeight(),
        display.getBitsPerPixel(),
        true,  // loop
        1,  // 单线程顺序读取
        BufferFillingWorkerFactory::WorkerType::IOURING_RAW  // 显式指定 IoUring Worker
    );
    
    producer.setErrorCallback([](const std::string& error) {
        printf("\n❌ Producer Error: %s\n", error.c_str());
        g_running = false;
    });
    
    if (!producer.start(config)) {
        printf("❌ Failed to start video producer\n");
        return -1;
    }
    
    printf("✅ Video producer started\n");
    printf("\n🎥 Starting display loop (Ctrl+C to stop)...\n\n");
    
    // 4. 消费者循环
    int frame_count = 0;
    
    while (g_running) {
        Buffer* filled_buffer = pool.acquireFilled(true, 100);
        if (filled_buffer == nullptr) {
            continue;
        }
        
        display.waitVerticalSync();
        if (!display.displayFilledFramebuffer(filled_buffer)) {
            printf("⚠️  Warning: Failed to display buffer\n");
        }
        
        pool.releaseFilled(filled_buffer);
        frame_count++;
        
        if (frame_count % 100 == 0) {
            printf("   Frames displayed: %d (%.1f fps)\n", 
                   frame_count, producer.getAverageFPS());
        }
    }
    
    // 5. 停止生产者
    printf("\n\n🛑 Stopping video producer...\n");
    producer.stop();
    
    printf("🛑 Playback stopped\n\n");
    
    // 6. 打印统计
    printf("📊 Final Statistics:\n");
    printf("   Frames displayed: %d\n", frame_count);
    printf("   Frames produced: %d\n", producer.getProducedFrames());
    printf("   Frames skipped: %d\n", producer.getSkippedFrames());
    printf("   Average FPS: %.2f\n", producer.getAverageFPS());
    
    pool.printStats();
    
    printf("\n✅ Test completed successfully\n");
    printf("\nℹ️  TODO: Implement IoUringVideoProductionLine for true async I/O performance\n");
    
    return 0;
}


/**
 * 测试6：RTSP 视频流播放（Worker自动创建BufferPool + DMA 零拷贝显示）
 * 
 * 功能演示：
 * - 连接 RTSP 视频流
 * - Worker在open()时自动创建BufferPool和Allocator
 * - Worker自动管理BufferPool的创建和Buffer注入
 * - DMA 零拷贝显示：直接使用物理地址
 * - 展示 RTSP 流的实时处理能力
 * 
 * 架构设计（最新）：
 * RTSP Stream → Worker::open() → 自动创建BufferPool和Allocator
 *                                      ↓
 *                          Worker内部解码循环
 *                                      ↓
 *                         FFmpeg解码 → AVFrame (带物理地址)
 *                                      ↓
 *                         Worker自动注入Buffer到BufferPool
 *                                      ↓
 *                         Buffer (包含物理地址)
 *                                      ↓
 *                  display.displayBufferByDMA(buffer)
 *                                      ↓
 *                         Display 驱动 DMA 显示
 * 
 * 零拷贝工作流程（最新架构）：
 * 1. Worker在open()时自动创建BufferPool和Allocator（如果需要）
 * 2. Worker解码RTSP流，获得带物理地址的AVFrame
 * 3. Worker自动将AVFrame包装为Buffer，注入到Worker的BufferPool
 * 4. ProductionLine从Worker获取BufferPool（通过getOutputBufferPool()）
 * 5. 消费者从Worker的BufferPool获取Buffer（含物理地址）
 * 6. 消费者调用display.displayBufferByDMA(buffer)：
 *    - 直接将物理地址传递给驱动（FB_IOCTL_SET_DMA_INFO）
 *    - 驱动通过DMA从解码器内存直接读取显示
 *    - 整个过程：0次memcpy！
 * 7. 消费者归还buffer，Worker自动管理AVFrame的释放
 * 
 * 关键设计理念（最新架构）：
 * - Worker自动创建BufferPool：Worker在open()时自动创建，应用层无需关心
 * - Worker必须创建BufferPool：Worker在open()时自动调用Allocator创建BufferPool
 * - 显式DMA调用：明确使用displayBufferByDMA，清晰可控
 * - 零拷贝路径：解码器输出 → DMA → 显示，无中间拷贝
 */
static int test_rtsp_stream(const char* rtsp_url) {
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Test: RTSP Stream Playback (Independent BufferPool + DMA)\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    printf("ℹ️  Zero-Copy Workflow:\n");
    printf("   1. Worker opens RTSP stream and automatically creates BufferPool (if needed)\n");
    printf("   2. Worker decodes RTSP → AVFrame with phys_addr\n");
    printf("   3. Worker injects Buffer to its BufferPool\n");
    printf("   4. Consumer acquires Buffer from Worker's BufferPool\n");
    printf("   5. display.displayBufferByDMA(buffer) → DMA zero-copy\n");
    printf("   6. Consumer releases Buffer → triggers deleter\n\n");
    
    // 1. 初始化显示设备
    printf("🖥️  Initializing display device...\n");
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 创建 VideoProductionLine（Worker会在open()时自动调用Allocator创建BufferPool）
    printf("📹 Creating VideoProductionLine...\n");
    VideoProductionLine producer;  // Worker会在open()时自动创建BufferPool
    
    // 4. 配置 RTSP 流（注意：推荐单线程）
    printf("🔗 Configuring RTSP stream: %s\n", rtsp_url);
    VideoProductionLine::Config config(
        rtsp_url,
        display.getWidth(),
        display.getHeight(),
        display.getBitsPerPixel(),
        false,  // loop（对RTSP无意义）
        1,      // thread_count（RTSP推荐单线程）
        BufferFillingWorkerFactory::WorkerType::FFMPEG_RTSP  // 显式指定 FFmpeg RTSP Worker
    );
    
    // 5. 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        printf("\n❌ RTSP Error: %s\n", error.c_str());
        g_running = false;
    });
    
    // 6. 启动生产者（内部会创建RTSP Reader并启用零拷贝）
    printf("🚀 Starting RTSP producer...\n");
    if (!producer.start(config)) {
        printf("❌ Failed to start RTSP producer\n");
        return -1;
    }
    
    printf("\n✅ RTSP stream connected, starting playback...\n");
    printf("   Press Ctrl+C to stop\n");
    printf("   Watch for '[DMA Display]' messages below\n\n");
    
    // 7. 获取工作BufferPool（Worker创建的或fallback的）
    BufferPool* working_pool = producer.getWorkingBufferPool();
    if (!working_pool) {
        printf("❌ ERROR: No working BufferPool available\n");
        return -1;
    }
    
    printf("✅ Using BufferPool: '%s' (created by Worker via Allocator)\n", 
           working_pool->getName().c_str());
    working_pool->printStats();
    
    // 8. 消费者循环：从工作BufferPool获取并通过DMA显示
    int frame_count = 0;
    int dma_success = 0;
    int dma_failed = 0;
    
    while (g_running) {
        // 从工作BufferPool获取已解码的buffer（带物理地址）
        Buffer* decoded_buffer = working_pool->acquireFilled(true, 100);
        if (decoded_buffer == nullptr) {
            continue;  // 超时，继续等待
        }
        
        // ✨ 关键调用：display.displayBufferByDMA(buffer)
        // - 直接使用 buffer 的物理地址
        // - 通过 FB_IOCTL_SET_DMA_INFO 将物理地址传递给驱动
        // - 驱动通过 DMA 从解码器内存直接读取显示
        // - 零拷贝：解码器输出 → DMA → 显示
        display.waitVerticalSync();
        if (display.displayBufferByDMA(decoded_buffer)) {
            dma_success++;
        } else {
            dma_failed++;
            printf("⚠️  Warning: DMA display failed for buffer (phys_addr=0x%llx)\n",
                   (unsigned long long)decoded_buffer->getPhysicalAddress());
        }
        
        // 归还 buffer（会触发 RtspVideoReader 的 deleter 回收 AVFrame）
        working_pool->releaseFilled(decoded_buffer);
        
        frame_count++;
        
        // 每100帧打印一次统计
        if (frame_count % 100 == 0) {
            printf("📊 Progress: %d frames displayed (%.1f fps, DMA success: %d, failed: %d)\n", 
                   frame_count, producer.getAverageFPS(), dma_success, dma_failed);
        }
    }
    
    // 8. 停止生产者
    printf("\n\n🛑 Stopping RTSP producer...\n");
    producer.stop();
    
    printf("\n✅ RTSP test completed\n");
    printf("   Total frames displayed: %d\n", frame_count);
    printf("   DMA display success: %d\n", dma_success);
    printf("   DMA display failed: %d\n", dma_failed);
    printf("   Success rate: %.1f%%\n", 
           frame_count > 0 ? (100.0 * dma_success / frame_count) : 0.0);
    
    printf("\n📦 Final BufferPool statistics:\n");
    working_pool->printStats();
    
    return 0;
}

/**
 * 测试6：FFmpeg 编码视频文件播放（使用Worker自动创建BufferPool）
 * 
 * 功能：
 * - 打开编码视频文件（MP4, AVI, MKV等）
 * - Worker在open()时自动创建BufferPool和Allocator
 * - Worker自动管理BufferPool的创建和Buffer注入
 * - 集成 VideoProductionLine + BufferPool 架构
 * - 支持 DMA 零拷贝显示
 * 
 * 架构说明（最新）：
 * - Worker在open()时自动创建BufferPool（如果需要）
 * - ProductionLine从Worker获取BufferPool（通过getOutputBufferPool()）
 * - Worker必须创建BufferPool：Worker在open()时自动调用Allocator创建BufferPool
 * 
 * 参数：
 * @param video_path 视频文件路径（如 "video.mp4"）
 */
static int test_h264_taco_video(const char* video_path) {
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Test: FFmpeg Encoded Video Playback\n");
    printf("  File: %s\n", video_path);
    printf("═══════════════════════════════════════════════════════\n\n");
    
    // 1. 初始化显示设备
    printf("🖥️  Initializing display device...\n");
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 创建 VideoProductionLine（Worker会在open()时自动调用Allocator创建BufferPool）
    printf("📹 Creating VideoProductionLine...\n");
    VideoProductionLine producer;  // Worker会在open()时自动创建BufferPool
    
    // 4. 配置 FFmpeg 解码
    printf("🎬 Configuring FFmpeg video reader: %s\n", video_path);
    
    VideoProductionLine::Config config(
        video_path,
        display.getWidth(),
        display.getHeight(),
        display.getBitsPerPixel(),
        true,  // loop（循环播放）
        1,  // 零拷贝推荐单线程，普通模式可以多线程
        BufferFillingWorkerFactory::WorkerType::FFMPEG_VIDEO_FILE  // 显式指定 FFmpeg Video File Worker
    );
    
    // 5. 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        printf("\n❌ FFmpeg Error: %s\n", error.c_str());
        g_running = false;
    });
    
    // 6. 启动生产者
    printf("🚀 Starting FFmpeg video producer...\n");
    if (!producer.start(config)) {
        printf("❌ Failed to start FFmpeg producer\n");
        return -1;
    }
    
    printf("\n✅ Video decoding started, starting playback...\n");
    printf("   Press Ctrl+C to stop\n\n");
    
    // 7. 获取工作BufferPool（Worker创建的或fallback的）
    BufferPool* working_pool = producer.getWorkingBufferPool();
    if (!working_pool) {
        printf("❌ ERROR: No working BufferPool available\n");
        return -1;
    }
    
    printf("✅ Using BufferPool: '%s' (created by Worker via Allocator)\n", 
           working_pool->getName().c_str());
    working_pool->printStats();
    
    // 8. 消费者循环
    int frame_count = 0;
    
    while (g_running) {
        // 从工作BufferPool获取已解码的buffer
        Buffer* filled_buffer = working_pool->acquireFilled(true, 100);
        if (filled_buffer == nullptr) {
            continue;  // 超时，继续等待
        }
        // 显示
        display.waitVerticalSync();
        // 零拷贝模式：使用 DMA 显示
        if (!display.displayBufferByDMA(filled_buffer)) {
            printf("⚠️  Warning: DMA display failed, falling back to normal\n");
            display.displayFilledFramebuffer(filled_buffer);
        }
        // 归还 buffer
        working_pool->releaseFilled(filled_buffer);
        
        frame_count++;
        
        // 每100帧打印一次统计
        if (frame_count % 100 == 0) {
            printf("📊 Frames displayed: %d (%.1f fps)\n", 
                   frame_count, producer.getAverageFPS());
        }
    }
    
    // 8. 停止生产者
    printf("\n\n🛑 Stopping FFmpeg producer...\n");
    producer.stop();
    
    printf("\n✅ FFmpeg video test completed\n");
    printf("   Total frames displayed: %d\n", frame_count);
    printf("   Frames produced: %d\n", producer.getProducedFrames());
    printf("   Frames skipped: %d\n", producer.getSkippedFrames());
    printf("   Average FPS: %.2f\n", producer.getAverageFPS());
    
    printf("\n📦 Final BufferPool statistics:\n");
    working_pool->printStats();
    
    return 0;
}

/**
 * 打印使用说明
 */
static void print_usage(const char* prog_name) {
    printf("Usage: %s [options] <raw_video_file|rtsp_url>\n\n", prog_name);
    printf("Options:\n");
    printf("  -h, --help          Show this help message\n");
    printf("  -m, --mode <mode>   Test mode (default: loop)\n");
    printf("                      loop:       4-frame loop display\n");
    printf("                      sequential: Sequential playback (play once)\n");
    printf("                      producer:   BufferPool + VideoProductionLine test\n");
    printf("                      iouring:    io_uring mode (using VideoProductionLine)\n");
    printf("                      rtsp:       RTSP stream playback (zero-copy)\n");
    printf("                      ffmpeg:     FFmpeg encoded video playback (NEW)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s video.raw\n", prog_name);
    printf("  %s -m loop video.raw\n", prog_name);
    printf("  %s -m sequential video.raw\n", prog_name);
    printf("  %s -m producer video.raw\n", prog_name);
    printf("  %s -m iouring video.raw\n", prog_name);
    printf("  %s -m rtsp rtsp://192.168.1.100:8554/stream\n", prog_name);
    printf("  %s -m ffmpeg video.mp4\n", prog_name);
    printf("\n");
    printf("Test Modes Description:\n");
    printf("  loop:       Load N frames into framebuffer and loop display them\n");
    printf("  sequential: Read and display frames sequentially from file\n");
    printf("  producer:   Use BufferPool + VideoProductionLine architecture (zero-copy)\n");
    printf("  iouring:    io_uring async I/O mode\n");
    printf("  rtsp:       RTSP stream decoding and display (zero-copy, FFmpeg)\n");
    printf("  ffmpeg:     FFmpeg encoded video file decoding (MP4/AVI/MKV/etc)\n");
    printf("\n");
    printf("Note:\n");
    printf("  - Raw video file must match framebuffer resolution\n");
    printf("  - Format: ARGB888 (4 bytes per pixel)\n");
    printf("  - RTSP/FFmpeg modes require FFmpeg libraries\n");
    printf("  - Press Ctrl+C to stop playback\n");
}

/**
 * 主函数
 */
int main(int argc, char* argv[]) {
    const char* raw_video_path = NULL;
    const char* mode = "loop";  // 默认模式：循环播放
    
    // 定义长选项
    static struct option long_options[] = {
        {"help",    no_argument,       0, 'h'},
        {"mode",    required_argument, 0, 'm'},
        {0,         0,                 0,  0 }
    };
    
    // 解析命令行参数
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "hm:", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return 0;
            
            case 'm':
                mode = optarg;
                break;
            
            case '?':
                // getopt_long 已经打印了错误信息
                printf("\n");
                print_usage(argv[0]);
                return 1;
            
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    // 获取非选项参数（视频文件路径或RTSP URL）
    if (optind < argc) {
        raw_video_path = argv[optind];
    }
    
    // 解析测试模式
    TestMode test_mode = parse_test_mode(mode);
    
    // 检查是否提供了视频文件路径
    if (!raw_video_path) {
        printf("Error: Missing raw video file path\n\n");
        print_usage(argv[0]);
        return 1;
    }
    
    // 根据模式运行测试
    int result = 0;
    switch (test_mode) {
        case TestMode::LOOP:
            result = test_4frame_loop(raw_video_path);
            break;
        
        case TestMode::SEQUENTIAL:
            result = test_sequential_playback(raw_video_path);
            break;
        
        case TestMode::PRODUCER:
            result = test_buffermanager_producer(raw_video_path);
            break;
        
        case TestMode::IOURING:
            result = test_buffermanager_iouring(raw_video_path);
            break;
        
        case TestMode::RTSP:
            result = test_rtsp_stream(raw_video_path);  // raw_video_path实际是rtsp_url
            break;
        
        case TestMode::FFMPEG:
            result = test_h264_taco_video(raw_video_path);
            break;
        
        case TestMode::UNKNOWN:
        default:
            printf("Error: Unknown mode '%s'\n\n", mode);
            print_usage(argv[0]);
            return 1;
    }
    
    return result;
}

