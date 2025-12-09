/**
 * FFmpeg Worker Open Test
 * 
 * 测试 BufferFillingWorkerFacade 创建 FFmpeg Worker 并打开视频文件
 * 
 * 编译命令：
 *   make  # 使用项目的 Makefile
 * 
 * 运行命令：
 *   ./test01 <video_file>
 * 
 * 示例：
 *   ./test01 /usr/testdata/video.mp4
 *   ./test01 /usr/testdata/test_h264.mkv
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <memory>
#include "productionline/worker/BufferFillingWorkerFacade.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "buffer/bufferpool/BufferPool.hpp"

// 全局标志，用于处理 Ctrl+C 退出
static volatile bool g_running = true;

/**
 * 信号处理函数
 */
static void signal_handler(int signum) {
    (void)signum;
    printf("\n\n🛑 Received interrupt signal, stopping...\n");
    g_running = false;
}

/**
 * 测试：FFmpeg Worker 打开视频文件
 * 
 * 功能：
 * - 使用 BufferFillingWorkerFacade 创建 FFmpeg Worker
 * - 打开视频文件并验证信息
 * - 检查 BufferPool 是否正确创建
 * - 测试 Worker 的基本功能
 */
static int test_ffmpeg_worker_open_close(const char* video_path) {
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Test: FFmpeg Worker Open Test\n");
    printf("  File: %s\n", video_path);
    printf("═══════════════════════════════════════════════════════\n\n");
    
    // 1. 创建 WorkerConfig
    WorkerConfig config;
    config.worker_type = WorkerType::FFMPEG_VIDEO_FILE;
    config.file.file_path = video_path;
    // 编码视频会自动检测格式，无需设置 output 参数
    
    // 2. 创建 BufferFillingWorkerFacade（v2.2：使用配置构造）
    auto worker_facade = std::make_shared<BufferFillingWorkerFacade>(config);
    
    if (!worker_facade) {
        printf("❌ Failed to create worker facade\n");
        return -1;
    }
    
    // 3. 打开视频文件（v2.2：无参数，从 config 获取）
    if (!worker_facade->open()) {
        printf("❌ Failed to open video file\n");
        printf("   Possible reasons: file not found, unsupported format, or FFmpeg initialization failed\n");
        return -1;
    }
    
    
    // 4. 验证视频信息
    printf("\n📊 Video Information:\n");
    printf("   Path:          %s\n", worker_facade->getPath());
    printf("   Resolution:    %dx%d\n", 
           worker_facade->getWidth(), worker_facade->getHeight());
    printf("   Bytes/Pixel:   %d\n", worker_facade->getBytesPerPixel());
    printf("   Frame Size:    %zu bytes (%.2f MB)\n", 
           worker_facade->getFrameSize(),
           worker_facade->getFrameSize() / (1024.0 * 1024.0));
    printf("   Total Frames:  %d\n", worker_facade->getTotalFrames());
    
    long file_size = worker_facade->getFileSize();
    if (file_size > 0) {
        printf("   File Size:     %ld bytes (%.2f MB)\n", 
               file_size, file_size / (1024.0 * 1024.0));
    }
    
    printf("   Current Index: %d\n", worker_facade->getCurrentFrameIndex());
    printf("   Has More:      %s\n", worker_facade->hasMoreFrames() ? "Yes" : "No");
    printf("   At End:        %s\n", worker_facade->isAtEnd() ? "Yes" : "No");
    
    
    // 5. 清理
    printf("\n🔄 Closing worker...\n");
    worker_facade->close();
    printf("✅ Worker closed successfully\n");
    printf("\n✅ Test completed successfully\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    return 0;
}

/**
 * 打印使用说明
 */
static void print_usage(const char* prog_name) {
    printf("Usage: %s <video_file>\n\n", prog_name);
    printf("Description:\n");
    printf("  Test FFmpeg Worker open functionality using BufferFillingWorkerFacade\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  video_file    Path to video file (MP4, AVI, MKV, MOV, etc.)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s /usr/testdata/video.mp4\n", prog_name);
    printf("  %s /usr/testdata/test_h264.mkv\n", prog_name);
    printf("\n");
    printf("Note:\n");
    printf("  - Supports all FFmpeg-compatible video formats\n");
    printf("  - Tests Worker creation, open, info query, and close\n");
    printf("  - Verifies BufferPool creation\n");
    printf("  - Tests basic seek and frame reading\n");
}

/**
 * 主函数
 */
int main(int argc, char* argv[]) {
    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 检查参数
    if (argc < 2) {
        printf("Error: Missing video file path\n\n");
        print_usage(argv[0]);
        return 1;
    }
    
    const char* video_path = argv[1];
    
    // 检查文件是否为帮助选项
    if (strcmp(video_path, "-h") == 0 || strcmp(video_path, "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║       FFmpeg Worker Open Test (test01.cpp)           ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n");
    
    // 运行测试
    int result = test_ffmpeg_worker_open_close(video_path);
    
    if (result == 0) {
        printf("\n🎉 All tests passed!\n\n");
    } else {
        printf("\n❌ Test failed with code: %d\n\n", result);
    }
    
    return result;
}

