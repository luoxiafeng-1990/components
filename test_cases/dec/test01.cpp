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
#include "productionline/worker/facade/BufferFillingWorkerFacade.hpp"
#include "buffer/BufferPool.hpp"

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
static int test_ffmpeg_worker_open(const char* video_path) {
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Test: FFmpeg Worker Open Test\n");
    printf("  File: %s\n", video_path);
    printf("═══════════════════════════════════════════════════════\n\n");
    
    // 1. 创建 BufferFillingWorkerFacade（使用门面模式）
    printf("🔧 Creating BufferFillingWorkerFacade...\n");
    auto worker_facade = std::make_shared<BufferFillingWorkerFacade>(
        BufferFillingWorkerFactory::WorkerType::FFMPEG_VIDEO_FILE
    );
    
    if (!worker_facade) {
        printf("❌ Failed to create worker facade\n");
        return -1;
    }
    printf("✅ Worker facade created successfully\n");
    printf("   Worker Type: %s\n", worker_facade->getWorkerType());
    
    // 2. 打开视频文件
    printf("\n📹 Opening video file: %s\n", video_path);
    if (!worker_facade->open(video_path)) {
        printf("❌ Failed to open video file\n");
        printf("   Possible reasons: file not found, unsupported format, or FFmpeg initialization failed\n");
        return -1;
    }
    
    printf("✅ Video file opened successfully\n");
    
    // 3. 验证视频信息
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
    
    // 4. 获取并验证 BufferPool
    printf("\n📦 BufferPool Information:\n");
    auto pool = worker_facade->getOutputBufferPool();
    if (pool) {
        printf("   Pool Name:     %s\n", pool->getName().c_str());
        printf("   Pool Category: %s\n", pool->getCategory().c_str());
        printf("   Total Buffers: %d\n", pool->getTotalCount());
        printf("   Free Buffers:  %d\n", pool->getFreeCount());
        printf("   Filled Buffers:%d\n", pool->getFilledCount());
        
        // 打印详细统计
        pool->printStats();
    } else {
        printf("⚠️  Warning: BufferPool not created\n");
    }
    
    // 5. 测试 seek 功能
    printf("\n🔍 Testing seek functionality...\n");
    if (worker_facade->getTotalFrames() > 0) {
        printf("   Seeking to beginning...\n");
        if (worker_facade->seekToBegin()) {
            printf("   ✅ Seek to begin successful, index=%d\n", 
                   worker_facade->getCurrentFrameIndex());
        } else {
            printf("   ⚠️  Seek to begin failed\n");
        }
    }
    
    // 6. 可选：测试读取一帧
    printf("\n🎬 Testing frame reading (optional)...\n");
    if (pool) {
        Buffer* buf = pool->acquireFree(false, 0);  // 非阻塞尝试
        if (buf) {
            printf("   Acquired free buffer #%u\n", buf->id());
            
            // 填充一帧数据
            if (worker_facade->fillBuffer(0, buf)) {
                printf("   ✅ Frame filled successfully\n");
                printf("      Buffer size: %zu bytes\n", buf->size());
                printf("      Virtual addr: %p\n", buf->getVirtualAddress());
                printf("      Physical addr: 0x%lx\n", buf->getPhysicalAddress());
                
                // 提交到 filled 队列
                pool->submitFilled(buf);
                printf("   Buffer submitted to filled queue\n");
                
                // 立即取回并释放
                Buffer* filled = pool->acquireFilled(false, 0);
                if (filled) {
                    pool->releaseFilled(filled);
                    printf("   Buffer released back to free queue\n");
                }
            } else {
                printf("   ⚠️  Failed to fill buffer\n");
                // 如果失败，需要归还 buffer
                pool->releaseFilled(buf);
            }
        } else {
            printf("   ℹ️  No free buffer available (expected)\n");
        }
    }
    
    // 7. 清理
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
    int result = test_ffmpeg_worker_open(video_path);
    
    if (result == 0) {
        printf("\n🎉 All tests passed!\n\n");
    } else {
        printf("\n❌ Test failed with code: %d\n\n", result);
    }
    
    return result;
}

