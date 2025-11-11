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
#include <chrono>
#include "include/LinuxFramebufferDevice.hpp"
#include "include/VideoFile.hpp"
#include "include/PerformanceMonitor.hpp"
#include "include/BufferManager.hpp"
#include "include/Timer.hpp"

// 全局标志，用于处理 Ctrl+C 退出
static volatile bool g_running = true;

// 信号处理函数
static void signal_handler(int signum) {
    if (signum == SIGINT) {
        printf("\n\n🛑 Received Ctrl+C, stopping playback...\n");
        g_running = false;
    }
}

// 定时器回调函数：自动停止播放
static void auto_stop_callback(void* user_data) {
    bool* running_flag = (bool*)user_data;
    *running_flag = false;
    printf("\n⏰ Auto-stop timer triggered: stopping playback...\n");
}

// 定时器回调函数：打印性能统计
static void print_stats_callback(void* user_data) {
    PerformanceMonitor* monitor = (PerformanceMonitor*)user_data;
    
    int loaded = monitor->getLoadedFrames();
    int displayed = monitor->getDisplayedFrames();
    double elapsed = monitor->getElapsedTime();
    double load_fps = monitor->getAverageLoadFPS();
    double display_fps = monitor->getAverageDisplayFPS();
    
    printf("┌─────────────────────────────────────────────────────┐\n");
    printf("│      ⏱️  性能统计 (%.1fs)                        │\n", elapsed);
    printf("└─────────────────────────────────────────────────────┘\n");
    
    if (loaded > 0) {
        printf("📥 加载帧: %d 帧 (%.1f fps)\n", loaded, load_fps);
    }
    
    if (displayed > 0) {
        printf("📺 显示操作: %d 次 (%.1f fps)\n", displayed, display_fps);
    }
    
    printf("\n");
}

// BufferManager状态数据结构
struct BufferManagerStats {
    PerformanceMonitor* monitor;
    std::shared_ptr<BufferManager>* manager;
};

// 定时器回调函数：打印性能统计和BufferManager状态
static void print_buffermanager_stats_callback(void* user_data) {
    BufferManagerStats* stats = (BufferManagerStats*)user_data;
    PerformanceMonitor* monitor = stats->monitor;
    auto manager = stats->manager->get();
    
    int loaded = monitor->getLoadedFrames();
    int displayed = monitor->getDisplayedFrames();
    double elapsed = monitor->getElapsedTime();
    double load_fps = monitor->getAverageLoadFPS();
    double display_fps = monitor->getAverageDisplayFPS();
    
    printf("┌─────────────────────────────────────────────────────┐\n");
    printf("│      ⏱️  性能统计 (%.1fs)                        │\n", elapsed);
    printf("└─────────────────────────────────────────────────────┘\n");
    
    if (loaded > 0) {
        printf("📥 加载帧: %d 帧 (%.1f fps)\n", loaded, load_fps);
    }
    
    if (displayed > 0) {
        printf("📺 显示操作: %d 次 (%.1f fps)\n", displayed, display_fps);
    }
    
    // BufferManager 状态
    printf("┌─────────────────────────────────────────────────────┐\n");
    printf("│      📦 BufferManager 状态                      │\n");
    printf("└─────────────────────────────────────────────────────┘\n");
    
    auto state = manager->getProducerState();
    const char* state_str = "";
    switch (state) {
        case BufferManager::ProducerState::STOPPED:
            state_str = "🛑 STOPPED";
            break;
        case BufferManager::ProducerState::RUNNING:
            state_str = "✅ RUNNING";
            break;
        case BufferManager::ProducerState::ERROR:
            state_str = "❌ ERROR";
            break;
    }
    
    printf("🎬 生产者状态: %s\n", state_str);
    printf("📊 已填充buffer: %d 个\n", manager->getFilledBufferCount());
    printf("📦 空闲buffer: %d 个\n", manager->getFreeBufferCount());
    printf("📈 总buffer数: %d 个\n", manager->getTotalBufferCount());
    
    printf("\n");
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
    
    // 创建并启动性能监控器
    PerformanceMonitor monitor;
    monitor.start();
    
    // 加载帧到 framebuffer
    printf("\n📥 Loading %d frames into framebuffer...\n", buffer_count);
    for (int i = 0; i < buffer_count; i++) {
        // 开始计时
        monitor.beginLoadFrameTiming();
        
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
        // 结束计时并记录
        monitor.endLoadFrameTiming();
    }
    
    // 创建并启动定时器（每1秒打印统计，延迟10秒开始）
    Timer stats_timer(1.0, print_stats_callback, &monitor, 10.0, 0.0);
    stats_timer.start();
    
    // 创建自动停止定时器（40秒后停止：10秒延迟 + 30秒统计）
    Timer stop_timer(40.0, auto_stop_callback, (void*)&g_running, 0.0, 0.0);
    stop_timer.start();
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    
    int loop_count = 0;
    while (g_running) {
        for (int buf_idx = 0; buf_idx < buffer_count && g_running; buf_idx++) {
            // 开始显示计时
            monitor.beginDisplayFrameTiming();
            // 等待垂直同步
            display.waitVerticalSync();
            // 切换显示buffer
            display.displayBuffer(buf_idx);
            // 结束显示计时并记录
            monitor.endDisplayFrameTiming();
        }
        
        loop_count++;
    }
    
    // 停止定时器
    stats_timer.stop();
    stop_timer.stop();
    
    printf("\n🛑 Playback stopped\n\n");
    
    // 打印最终统计
    monitor.printStatistics();
    
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
    
    // 创建并启动性能监控器
    PerformanceMonitor monitor;
    monitor.start();
    
    // 创建并启动定时器（每1秒打印统计，延迟20秒开始）
    Timer stats_timer(1.0, print_stats_callback, &monitor, 20.0, 0.0);
    stats_timer.start();
    
    // 创建自动停止定时器（50秒后停止：20秒延迟 + 30秒统计）
    Timer stop_timer(50.0, auto_stop_callback, (void*)&g_running, 0.0, 0.0);
    stop_timer.start();
    
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
        
        // 加载帧
        monitor.beginLoadFrameTiming();
        Buffer& buffer = display.getBuffer(current_buffer);
        if (!video.readFrameTo(buffer)) {
            printf("❌ ERROR: Failed to read frame %d\n", frame_index);
            break;
        }
        monitor.endLoadFrameTiming();
        
        // 显示帧
        monitor.beginDisplayFrameTiming();
        display.waitVerticalSync();
        display.displayBuffer(current_buffer);
        monitor.endDisplayFrameTiming();
        
        // 切换到下一个buffer
        current_buffer = (current_buffer + 1) % display.getBufferCount();
        frame_index++;
    }
    
    // 停止定时器
    stats_timer.stop();
    stop_timer.stop();
    
    printf("\n🛑 Playback stopped\n\n");
    
    // 打印最终统计
    monitor.printStatistics();
    printf("   Total frames played: %d / %d\n", frame_index, video.getTotalFrames());
    
    printf("\n✅ Test completed successfully\n");
    
    return 0;
}

/**
 * 测试3：BufferManager 生产者线程测试
 * 
 * 功能：
 * - 使用 BufferManager 管理 buffer 池
 * - 自动启动生产者线程从视频文件读取数据
 * - 主线程作为消费者，获取 buffer 并显示到屏幕
 * - 展示生产者-消费者模式的多线程架构
 */
static int test_buffermanager_producer(const char* raw_video_path) {
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Test: BufferManager Producer Thread\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    // 1. 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 计算帧大小
    size_t frame_size = (size_t)display.getWidth() * display.getHeight() * 
                        (display.getBitsPerPixel() / 8);
    
    printf("📺 Display initialized:\n");
    printf("   Resolution: %dx%d\n", display.getWidth(), display.getHeight());
    printf("   Bits per pixel: %d\n", display.getBitsPerPixel());
    printf("   Frame size: %zu bytes (%.2f MB)\n", frame_size, frame_size / (1024.0 * 1024.0));
    printf("   Buffer count: %d\n", display.getBufferCount());
    
    // 2. 创建 BufferManager（使用 shared_ptr 管理）
    auto manager = std::make_shared<BufferManager>(30, frame_size, true);
    
    printf("\n📦 BufferManager created with 40 buffers\n");
    
    // 3. 创建性能监控器
    PerformanceMonitor monitor;
    monitor.start();
    
    // 配置定时器 - 打印性能统计和 BufferManager 状态
    BufferManagerStats bm_stats = { &monitor, &manager };
    Timer stats_timer(1.0, print_buffermanager_stats_callback, &bm_stats, 10.0, 0.0);
    stats_timer.start();
    
    // 创建自动停止定时器（40秒后停止：10秒延迟 + 30秒统计）
    Timer stop_timer(40.0, auto_stop_callback, (void*)&g_running, 0.0, 0.0);
    stop_timer.start();
    
    // 4. 启动视频生产者线程（使用多线程模式）
    printf("\n🎬 Starting video producer threads...\n");
    
    int producer_thread_count = 3;  // 使用3个生产者线程
    printf("   Using %d producer threads for parallel reading\n", producer_thread_count);
    
    bool started = manager->startMultipleVideoProducers(
        producer_thread_count,  // 线程数量
        raw_video_path,
        display.getWidth(),
        display.getHeight(),
        display.getBitsPerPixel(),
        true,  // 循环播放
        [](const std::string& error) {
            // 错误回调
            printf("\n❌ Producer Error: %s\n", error.c_str());
            g_running = false;
        }
    );
    
    if (!started) {
        printf("❌ Failed to start video producer threads\n");
        return -1;
    }
    
    printf("✅ Video producer threads started\n");
    printf("\n🎥 Starting display loop (Ctrl+C to stop)...\n\n");
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    
    // 5. 消费者循环：从 BufferManager 获取 buffer 并显示
    int current_display_buffer = 0;
    int frame_count = 0;
    
    while (g_running) {
        // 检查生产者状态
        auto state = manager->getProducerState();
        if (state == BufferManager::ProducerState::ERROR) {
            printf("❌ Producer encountered an error: %s\n", 
                   manager->getLastProducerError().c_str());
            break;
        }
        
        // 获取一个已填充的 buffer（阻塞，100ms超时）
        Buffer* filled_buffer = manager->acquireFilledBuffer(true, 100);
        if (filled_buffer == nullptr) {
            // 超时，继续等待
            printf("🔄 Consumer got no buffer, waiting for 100ms...\n");
            continue;
        }
        
        // 开始加载帧计时（从buffer拷贝到display）
        monitor.beginLoadFrameTiming();
        
        // 获取 display 的 buffer
        Buffer& display_buffer = display.getBuffer(current_display_buffer);
        
        // 将数据从 BufferManager 的 buffer 拷贝到 display 的 buffer
        if (!display_buffer.copyFrom(filled_buffer->data(), filled_buffer->size())) {
            printf("⚠️  Warning: Failed to copy buffer data\n");
        }
        
        monitor.endLoadFrameTiming();
        
        // 显示帧
        monitor.beginDisplayFrameTiming();
        // 性能分析：测量VSync等待时间
        display.waitVerticalSync();
        display.displayBuffer(current_display_buffer);
        monitor.endDisplayFrameTiming();
        
        // 回收 buffer 到空闲队列
        manager->recycleBuffer(filled_buffer);
        
        // 切换到下一个 display buffer
        current_display_buffer = (current_display_buffer + 1) % display.getBufferCount();
        frame_count++;
    }
    
    // 6. 停止生产者线程
    printf("\n\n🛑 Stopping video producer thread...\n");
    manager->stopVideoProducer();
    
    // 停止定时器
    stats_timer.stop();
    stop_timer.stop();
    
    printf("🛑 Playback stopped\n\n");
    
    // 7. 打印最终统计
    monitor.printStatistics();
    printf("   Total frames displayed: %d\n", frame_count);
    printf("   Final buffer states:\n");
    printf("     - Free buffers: %d\n", manager->getFreeBufferCount());
    printf("     - Filled buffers: %d\n", manager->getFilledBufferCount());
    
    printf("\n✅ Test completed successfully\n");
    
    return 0;
}

/**
 * 测试4：BufferManager io_uring 生产者线程测试
 * 
 * 功能：
 * - 使用 BufferManager 管理 buffer 池
 * - 使用 io_uring 进行高性能异步 I/O
 * - 自动启动多个生产者线程，使用零拷贝技术从视频文件读取数据
 * - 主线程作为消费者，获取 buffer 并显示到屏幕
 * - 展示 io_uring 异步 I/O 的性能优势
 */
static int test_buffermanager_iouring(const char* raw_video_path) {
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Test: BufferManager io_uring Producer Thread\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    // 1. 初始化显示设备
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 计算帧大小
    size_t frame_size = (size_t)display.getWidth() * display.getHeight() * 
                        (display.getBitsPerPixel() / 8);
    
    printf("📺 Display initialized:\n");
    printf("   Resolution: %dx%d\n", display.getWidth(), display.getHeight());
    printf("   Bits per pixel: %d\n", display.getBitsPerPixel());
    printf("   Frame size: %zu bytes (%.2f MB)\n", frame_size, frame_size / (1024.0 * 1024.0));
    printf("   Buffer count: %d\n", display.getBufferCount());
    
    // 2. 创建 BufferManager（使用 shared_ptr 管理）
    auto manager = std::make_shared<BufferManager>(40, frame_size, true);
    
    printf("\n📦 BufferManager created with 40 buffers\n");
    
    // 3. 创建性能监控器
    PerformanceMonitor monitor;
    monitor.start();
    
    // 配置定时器 - 打印性能统计和 BufferManager 状态（延迟10秒开始）
    BufferManagerStats bm_stats = { &monitor, &manager };
    Timer stats_timer(1.0, print_buffermanager_stats_callback, &bm_stats, 10.0, 0.0);
    stats_timer.start();
    
    // 不设置自动停止，让用户用 Ctrl+C 手动停止（io_uring模式性能测试需要更长时间）
    
    // 4. 启动 io_uring 视频生产者线程
    printf("\n🎬 Starting io_uring video producer threads...\n");
    
    // io_uring的优势在于异步I/O，不需要多线程！
    // 多线程反而会造成随机跳跃读取，降低性能
    int producer_thread_count = 1;  // 使用1个生产者线程（顺序读取）
    printf("   Using %d io_uring producer thread for sequential async reading\n", producer_thread_count);
    
    bool started = manager->startMultipleVideoProducersIoUring(
        producer_thread_count,  // 线程数量
        raw_video_path,
        display.getWidth(),
        display.getHeight(),
        display.getBitsPerPixel(),
        true,  // 循环播放
        [](const std::string& error) {
            // 错误回调
            printf("\n❌ Producer Error: %s\n", error.c_str());
            g_running = false;
        }
    );
    
    if (!started) {
        printf("❌ Failed to start io_uring video producer threads\n");
        return -1;
    }
    
    printf("✅ io_uring video producer threads started\n");
    printf("\n🎥 Starting display loop (Ctrl+C to stop)...\n\n");
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    
    // 5. 消费者循环：从 BufferManager 获取 buffer 并显示
    int current_display_buffer = 0;
    int frame_count = 0;
    
    while (g_running) {
        // 检查生产者状态
        auto state = manager->getProducerState();
        if (state == BufferManager::ProducerState::ERROR) {
            printf("❌ Producer encountered an error: %s\n", 
                   manager->getLastProducerError().c_str());
            break;
        }
        
        // 获取一个已填充的 buffer（阻塞，100ms超时）
        Buffer* filled_buffer = manager->acquireFilledBuffer(true, 100);
        if (filled_buffer == nullptr) {
            // 超时，继续等待
            continue;
        }
        
        // 开始加载帧计时（从buffer拷贝到display）
        monitor.beginLoadFrameTiming();
        
        // 获取 display 的 buffer
        Buffer& display_buffer = display.getBuffer(current_display_buffer);
        
        // 将数据从 BufferManager 的 buffer 拷贝到 display 的 buffer
        if (!display_buffer.copyFrom(filled_buffer->data(), filled_buffer->size())) {
            printf("⚠️  Warning: Failed to copy buffer data\n");
        }
        
        monitor.endLoadFrameTiming();
        
        // 显示帧
        monitor.beginDisplayFrameTiming();
        // 等待垂直同步
        display.waitVerticalSync();
        display.displayBuffer(current_display_buffer);
        monitor.endDisplayFrameTiming();
        
        // 回收 buffer 到空闲队列
        manager->recycleBuffer(filled_buffer);
        
        // 切换到下一个 display buffer
        current_display_buffer = (current_display_buffer + 1) % display.getBufferCount();
        frame_count++;
    }
    
    // 6. 停止生产者线程
    printf("\n\n🛑 Stopping io_uring video producer threads...\n");
    manager->stopVideoProducer();
    
    // 停止定时器
    stats_timer.stop();
    
    printf("🛑 Playback stopped\n\n");
    
    // 7. 打印最终统计
    monitor.printStatistics();
    printf("   Total frames displayed: %d\n", frame_count);
    printf("   Final buffer states:\n");
    printf("     - Free buffers: %d\n", manager->getFreeBufferCount());
    printf("     - Filled buffers: %d\n", manager->getFilledBufferCount());
    
    printf("\n✅ Test completed successfully\n");
    
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
    printf("                      producer:   BufferManager producer thread test\n");
    printf("                      iouring:    BufferManager io_uring producer test (high-performance)\n");
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
    printf("  producer:   Use BufferManager with producer thread (multi-threaded)\n");
    printf("  iouring:    Use BufferManager with io_uring async I/O (zero-copy, high-performance)\n");
    printf("\n");
    printf("Note:\n");
    printf("  - Raw video file must match framebuffer resolution\n");
    printf("  - Format: ARGB888 (4 bytes per pixel)\n");
    printf("  - Press Ctrl+C to stop playback\n");
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

