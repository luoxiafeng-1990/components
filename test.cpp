/**
 * Display Framework Test Program
 * 
 * 测试 LinuxFramebufferDevice, VideoFile, PerformanceMonitor, BufferManager 四个类的功能
 * 
 * 编译命令：
 *   g++ -o test test.cpp \
 *       source/LinuxFramebufferDevice.cpp \
 *       source/VideoFile.cpp \
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
#include "include/display/LinuxFramebufferDevice.hpp"
#include "include/videoFile/VideoFile.hpp"
#include "include/buffer/BufferPool.hpp"
#include "include/producer/VideoProducer.hpp"

// 全局标志，用于处理 Ctrl+C 退出
static volatile bool g_running = true;

// 信号处理函数
static void signal_handler(int signum) {
    if (signum == SIGINT) {
        printf("\n\n🛑 Received Ctrl+C, stopping playback...\n");
        g_running = false;
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
    printf("  Test: Multi-Buffer Loop Display\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    // 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    int buffer_count = display.getBufferCount();
    
    // 打开视频文件
    VideoFile video;
    if (!video.openRaw(raw_video_path, 
                       display.getWidth(), 
                       display.getHeight(), 
                       display.getBitsPerPixel())) {
        return -1;
    }
    
    // 检查文件是否有足够的帧
    if (video.getTotalFrames() < buffer_count) {
        printf("❌ ERROR: File contains only %d frames, need at least %d frames\n",
               video.getTotalFrames(), buffer_count);
        return -1;
    }
    
    // 加载帧到 framebuffer
    printf("\n📥 Loading %d frames into framebuffer...\n", buffer_count);
    for (int i = 0; i < buffer_count; i++) {
        // 获取buffer引用
        Buffer& buffer = display.getBuffer(i);
        if (!buffer.isValid()) {
            printf("❌ ERROR: Invalid buffer %d\n", i);
            return -1;
        }
        
        // 直接读取视频帧到framebuffer的buffer中
        if (!video.readFrameTo(buffer)) {
            printf("❌ ERROR: Failed to load frame %d\n", i);
            return -1;
        }
    }

    // 注册信号处理
    signal(SIGINT, signal_handler);
    
    int loop_count = 0;
    while (g_running) {
        for (int buf_idx = 0; buf_idx < buffer_count && g_running; buf_idx++) {
            // 等待垂直同步
            display.waitVerticalSync();
            // 切换显示buffer
            display.displayBuffer(buf_idx);
        }
        
        loop_count++;
    }
    
    printf("\n🛑 Playback stopped\n\n");
    
    printf("\n✅ Test completed successfully\n");
    
    return 0;
}

/**
 * 测试2：顺序播放测试
 * 
 * 功能：
 * - 打开原始视频文件
 * - 顺序读取并显示所有帧（只播放一次）
 */
static int test_sequential_playback(const char* raw_video_path) {
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Test: Sequential Playback\n");
    printf("═══════════════════════════════════════════════════════\n\n");  
    
    // 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 打开视频文件
    VideoFile video;
    if (!video.openRaw(raw_video_path, 
                       display.getWidth(), 
                       display.getHeight(), 
                       display.getBitsPerPixel())) {
        return -1;
    }
    // 开始播放
    printf("\n🎬 Starting sequential playback (Ctrl+C to stop)...\n\n");
    
    signal(SIGINT, signal_handler);
    
    int current_buffer = 0;
    int frame_index = 0;
    
    while (g_running) {
        // 检查视频是否播放完毕，如果是则回到开头继续循环
        if (!video.hasMoreFrames()) {
            video.seekToBegin();
            printf("🔄 Video reached end, looping back to start...\n");
        }
        
        Buffer& buffer = display.getBuffer(current_buffer);
        if (!video.readFrameTo(buffer)) {
            printf("❌ ERROR: Failed to read frame %d\n", frame_index);
            break;
        }
        display.waitVerticalSync();
        display.displayBuffer(current_buffer);
        // 切换到下一个buffer
        current_buffer = (current_buffer + 1) % display.getBufferCount();
        frame_index++;
    }
    printf("\n🛑 Playback stopped\n\n");
    // 打印最终统计
    printf("   Total frames played: %d / %d\n", frame_index, video.getTotalFrames());
    printf("\n✅ Test completed successfully\n");
    return 0;
}

/**
 * 测试3：BufferPool + VideoProducer 测试（新架构）
 * 
 * 功能：
 * - 使用 LinuxFramebufferDevice 的 BufferPool（零拷贝）
 * - 使用 VideoProducer 自动从视频文件读取数据
 * - 主线程作为消费者，获取 buffer 并显示到屏幕
 * - 展示生产者-消费者模式的解耦架构
 */
static int test_buffermanager_producer(const char* raw_video_path) {
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Test: BufferPool + VideoProducer (New Architecture)\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    // 1. 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    printf("📺 Display initialized:\n");
    printf("   Resolution: %dx%d\n", display.getWidth(), display.getHeight());
    printf("   Bits per pixel: %d\n", display.getBitsPerPixel());
    printf("   Buffer count: %d\n", display.getBufferCount());
    
    // 2. 获取 display 的 BufferPool（framebuffer 已托管）
    BufferPool* pool = display.getBufferPool();
    if (!pool) {
        printf("❌ Failed to get BufferPool from display\n");
        return -1;
    }
    
    printf("\n📦 Using LinuxFramebufferDevice's BufferPool (zero-copy)\n");
    pool->printStats();
    
    // 3. 创建 VideoProducer（依赖注入 BufferPool）
    VideoProducer producer(*pool);
    
    // 4. 配置并启动视频生产者
    printf("\n🎬 Starting video producer...\n");
    
    int producer_thread_count = 2;  // 使用2个生产者线程
    printf("   Using %d producer threads for parallel reading\n", producer_thread_count);
    
    VideoProducer::Config config(
        raw_video_path,
        display.getWidth(),
        display.getHeight(),
        display.getBitsPerPixel(),
        true,  // loop
        producer_thread_count
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
    
    printf("✅ Video producer started\n");
    printf("\n🎥 Starting display loop (Ctrl+C to stop)...\n\n");
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    
    // 5. 消费者循环：从 BufferPool 获取 buffer 并显示（零拷贝）
    int frame_count = 0;
    
    while (g_running) {
        // 获取一个已填充的 buffer（阻塞，100ms超时）
        Buffer* filled_buffer = pool->acquireFilled(true, 100);
        if (filled_buffer == nullptr) {
            // 超时，继续等待
            continue;
        }
        
        // 直接显示（无需拷贝，buffer 本身就是 framebuffer）
        display.waitVerticalSync();
        if (!display.displayBuffer(filled_buffer)) {
            printf("⚠️  Warning: Failed to display buffer\n");
        }
        
        // 归还 buffer 到空闲队列
        pool->releaseFilled(filled_buffer);
        
        frame_count++;
        
        // 每100帧打印一次进度
        if (frame_count % 100 == 0) {
            printf("   Frames displayed: %d (%.1f fps)\n", 
                   frame_count, producer.getAverageFPS());
        }
    }
    
    // 6. 停止生产者
    printf("\n\n🛑 Stopping video producer...\n");
    producer.stop();
    
    printf("🛑 Playback stopped\n\n");
    
    // 7. 打印最终统计
    printf("📊 Final Statistics:\n");
    printf("   Frames displayed: %d\n", frame_count);
    printf("   Frames produced: %d\n", producer.getProducedFrames());
    printf("   Frames skipped: %d\n", producer.getSkippedFrames());
    printf("   Average FPS: %.2f\n", producer.getAverageFPS());
    
    pool->printStats();
    
    printf("\n✅ Test completed successfully\n");
    
    return 0;
}

/**
 * 测试4：io_uring 模式（待实现 IoUringVideoProducer）
 * 
 * 功能：
 * - 使用 BufferPool 管理 buffer 池
 * - 使用 IoUringVideoProducer 进行高性能异步 I/O（待实现）
 * - 暂时使用普通 VideoProducer 作为替代
 */
static int test_buffermanager_iouring(const char* raw_video_path) {
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Test: io_uring Mode (using VideoProducer temporarily)\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    printf("ℹ️  Note: IoUringVideoProducer not yet implemented in new architecture\n");
    printf("   Using standard VideoProducer as fallback\n\n");
    
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
    BufferPool* pool = display.getBufferPool();
    if (!pool) {
        printf("❌ Failed to get BufferPool from display\n");
        return -1;
    }
    
    printf("\n📦 Using LinuxFramebufferDevice's BufferPool\n");
    pool->printStats();
    
    // 3. 创建 VideoProducer（单线程，顺序读取）
    VideoProducer producer(*pool);
    
    printf("\n🎬 Starting video producer (sequential mode)...\n");
    printf("   Using 1 producer thread for sequential reading\n");
    
    VideoProducer::Config config(
        raw_video_path,
        display.getWidth(),
        display.getHeight(),
        display.getBitsPerPixel(),
        true,  // loop
        1  // 单线程顺序读取
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
    
    signal(SIGINT, signal_handler);
    
    // 4. 消费者循环
    int frame_count = 0;
    
    while (g_running) {
        Buffer* filled_buffer = pool->acquireFilled(true, 100);
        if (filled_buffer == nullptr) {
            continue;
        }
        
        display.waitVerticalSync();
        if (!display.displayBuffer(filled_buffer)) {
            printf("⚠️  Warning: Failed to display buffer\n");
        }
        
        pool->releaseFilled(filled_buffer);
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
    
    pool->printStats();
    
    printf("\n✅ Test completed successfully\n");
    printf("\nℹ️  TODO: Implement IoUringVideoProducer for true async I/O performance\n");
    
    return 0;
}

/**
 * 打印使用说明
 */
static void print_usage(const char* prog_name) {
    printf("Usage: %s [options] <raw_video_file>\n\n", prog_name);
    printf("Options:\n");
    printf("  -h, --help          Show this help message\n");
    printf("  -m, --mode <mode>   Test mode (default: loop)\n");
    printf("                      loop:       4-frame loop display\n");
    printf("                      sequential: Sequential playback (play once)\n");
    printf("                      producer:   BufferPool + VideoProducer test (NEW ARCHITECTURE)\n");
    printf("                      iouring:    io_uring mode (using VideoProducer temporarily)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s video.raw\n", prog_name);
    printf("  %s -m loop video.raw\n", prog_name);
    printf("  %s -m sequential video.raw\n", prog_name);
    printf("  %s -m producer video.raw\n", prog_name);
    printf("  %s -m iouring video.raw\n", prog_name);
    printf("\n");
    printf("Test Modes Description:\n");
    printf("  loop:       Load N frames into framebuffer and loop display them\n");
    printf("  sequential: Read and display frames sequentially from file\n");
    printf("  producer:   Use NEW BufferPool + VideoProducer architecture (zero-copy, decoupled)\n");
    printf("  iouring:    io_uring async I/O mode (TODO: implement IoUringVideoProducer)\n");
    printf("\n");
    printf("Note:\n");
    printf("  - Raw video file must match framebuffer resolution\n");
    printf("  - Format: ARGB888 (4 bytes per pixel)\n");
    printf("  - Press Ctrl+C to stop playback\n");
    printf("  - NEW: 'producer' mode now uses decoupled architecture with zero-copy display\n");
}

/**
 * 主函数
 */
int main(int argc, char* argv[]) {
    const char* raw_video_path = NULL;
    const char* mode = "loop";  // 默认模式：循环播放
    
    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--mode") == 0) {
            if (i + 1 < argc) {
                mode = argv[++i];
            } else {
                printf("Error: -m/--mode requires an argument\n\n");
                print_usage(argv[0]);
                return 1;
            }
        } else {
            raw_video_path = argv[i];
        }
    }
    
    // 检查是否提供了视频文件路径
    if (!raw_video_path) {
        printf("Error: Missing raw video file path\n\n");
        print_usage(argv[0]);
        return 1;
    }
    
    // 根据模式运行测试
    int result = 0;
    if (strcmp(mode, "loop") == 0) {
        result = test_4frame_loop(raw_video_path);
    } else if (strcmp(mode, "sequential") == 0) {
        result = test_sequential_playback(raw_video_path);
    } else if (strcmp(mode, "producer") == 0) {
        result = test_buffermanager_producer(raw_video_path);
    } else if (strcmp(mode, "iouring") == 0) {
        result = test_buffermanager_iouring(raw_video_path);
    } else {
        printf("Error: Unknown mode '%s'\n\n", mode);
        print_usage(argv[0]);
        return 1;
    }
    
    return result;
}

