/**
 * Display Framework Test Program
 * 
 * 测试 LinuxFramebufferDevice, VideoFile, PerformanceMonitor 三个类的功能
 * 
 * 编译命令：
 *   g++ -o test test.cpp \
 *       source/LinuxFramebufferDevice.cpp \
 *       source/VideoFile.cpp \
 *       source/PerformanceMonitor.cpp \
 *       -I./include -std=c++11
 * 
 * 运行命令：
 *   ./test <raw_video_file>
 * 
 * 示例：
 *   ./test /usr/testdata/ids/test_video_argb888.raw
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "include/LinuxFramebufferDevice.hpp"
#include "include/VideoFile.hpp"
#include "include/PerformanceMonitor.hpp"

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

/**
 * 测试1：4帧循环播放测试
 * 
 * 功能：
 * - 打开原始视频文件
 * - 加载前4帧到framebuffer的4个buffer
 * - 循环播放这4帧
 * - 显示性能统计
 */
static int test_4frame_loop(const char* raw_video_path) {
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Display Framework Test - 4-Frame Loop Display\n");
    printf("  File: %s\n", raw_video_path);
    printf("  Mode: Load 4 frames, loop display\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    // 1. 初始化显示设备
    printf("📺 Step 1: Initialize display device...\n");
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        printf("❌ ERROR: Failed to initialize display device\n");
        return -1;
    }
 
    // 2. 打开视频文件
    printf("📂 Step 2: Open video file...\n");
    VideoFile video;
    if (!video.openRaw(raw_video_path, 
                       display.getWidth(), 
                       display.getHeight(), 
                       display.getBytesPerPixel())) {
        printf("❌ ERROR: Failed to open video file\n");
        return -1;
    }
    printf("✅ Video file opened\n");
    printf("   Total frames: %d\n", video.getTotalFrames());
    printf("   Frame size: %zu bytes\n\n", video.getFrameSize());
    
    // 检查文件是否有足够的帧
    if (video.getTotalFrames() < 4) {
        printf("❌ ERROR: File contains only %d frames, need at least 4 frames\n",
               video.getTotalFrames());
        return -1;
    }
    
    // 3. 创建性能监控器
    printf("📊 Step 3: Initialize performance monitor...\n");
    PerformanceMonitor monitor;
    monitor.start();
    printf("✅ Performance monitor started\n\n");
    
    // 4. 加载前4帧到framebuffer的4个buffer中
    printf("📥 Step 4: Loading 4 frames into framebuffer...\n");
    for (int i = 0; i < 4; i++) {
        // 开始计时
        monitor.beginLoadFrame();
        
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
        monitor.endLoadFrame();
    }
    
    printf("✅ All 4 frames loaded\n");
    printf("   Loaded frames: %d\n", monitor.getLoadedFrames());
    printf("   Average load FPS: %.2f fps\n", monitor.getAverageLoadFPS());
    printf("   Total time: %.2f seconds\n\n", monitor.getTotalTime());
    
    // 5. 循环播放这4帧
    printf("🎬 Step 5: Starting 4-frame loop display...\n");
    printf("   Press Ctrl+C to stop\n\n");
    
    // ========== 定时器配置 ==========
    // 
    // 方式1：周期性统计（默认）
    monitor.setTimerTask(TASK_PRINT_FULL_STATS);  // 选择任务类型
    monitor.setTimerInterval(1.0,5.0);  // 每1秒统计一次
    monitor.startTimer();

    PerformanceMonitor auto_stop_timer;
    auto_stop_timer.setOneShotTimer(60.0);  // 60秒后触发
    auto_stop_timer.setTimerCallback(auto_stop_callback, (void*)&g_running);
    auto_stop_timer.startTimer();
    printf("   ⏰ Auto-stop enabled: will stop after 60 seconds\n\n");
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    
    int loop_count = 0;
    while (g_running) {
        for (int buf_idx = 0; buf_idx < 4 && g_running; buf_idx++) {
            // 开始显示计时
            monitor.beginDisplayFrame();
            // 等待垂直同步
            display.waitVerticalSync();
            // 切换显示buffer
            display.displayBuffer(buf_idx);
            // 结束显示计时并记录
            monitor.endDisplayFrame();
        }
        
        loop_count++;
    }
    
    // 停止定时器
    monitor.stopTimer();
    
    printf("\n🛑 Playback stopped by user\n\n");
    
    // 6. 打印最终统计
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Final Statistics\n");
    printf("═══════════════════════════════════════════════════════\n");
    monitor.printStatistics();
    
    printf("\n✅ Test completed successfully\n");
    printf("   Total loops: %d\n", loop_count);
    printf("   Total frames displayed: %d\n", loop_count * 4);
    
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
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Display Framework Test - Sequential Playback\n");
    printf("  File: %s\n", raw_video_path);
    printf("  Mode: Play once, display all frames\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    // 1. 初始化显示设备
    printf("📺 Initializing display device...\n");
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        printf("❌ ERROR: Failed to initialize display device\n");
        return -1;
    }
    printf("✅ Display initialized: %dx%d, %d buffers\n\n",
           display.getWidth(), display.getHeight(), display.getBufferCount());
    
    // 2. 打开视频文件
    printf("📂 Opening video file...\n");
    VideoFile video;
    if (!video.openRaw(raw_video_path, 
                       display.getWidth(), 
                       display.getHeight(), 
                       display.getBytesPerPixel())) {
        printf("❌ ERROR: Failed to open video file\n");
        return -1;
    }
    printf("✅ Video opened: %d frames\n\n", video.getTotalFrames());
    
    // 3. 创建性能监控器
    PerformanceMonitor monitor;
    monitor.start();
    
    // 4. 顺序播放所有帧
    printf("🎬 Starting sequential playback...\n");
    printf("   Press Ctrl+C to stop\n\n");
    
    signal(SIGINT, signal_handler);
    
    int current_buffer = 0;
    int frame_index = 0;
    
    while (g_running && video.hasMoreFrames()) {
        // 加载帧
        monitor.beginLoadFrame();
        Buffer& buffer = display.getBuffer(current_buffer);
        if (!video.readFrameTo(buffer)) {
            printf("❌ ERROR: Failed to read frame %d\n", frame_index);
            break;
        }
        monitor.endLoadFrame();
        
        // 显示帧
        monitor.beginDisplayFrame();
        display.waitVerticalSync();
        display.displayBuffer(current_buffer);
        monitor.endDisplayFrame();
        
        // 切换到下一个buffer
        current_buffer = (current_buffer + 1) % display.getBufferCount();
        frame_index++;
        
        // 每100帧打印一次进度
        if (frame_index % 100 == 0) {
            monitor.printRealTimeStats();
        }
    }
    
    printf("\n🎬 Playback finished\n\n");
    
    // 5. 打印最终统计
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Final Statistics\n");
    printf("═══════════════════════════════════════════════════════\n");
    monitor.printStatistics();
    printf("   Total frames played: %d / %d\n", frame_index, video.getTotalFrames());
    
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
    printf("\n");
    printf("Examples:\n");
    printf("  %s video.raw\n", prog_name);
    printf("  %s -m loop video.raw\n", prog_name);
    printf("  %s -m sequential video.raw\n", prog_name);
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
    } else {
        printf("Error: Unknown mode '%s'\n\n", mode);
        print_usage(argv[0]);
        return 1;
    }
    
    return result;
}

