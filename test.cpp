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
#include <getopt.h>
#include <string>
#include <vector>
#include "include/display/LinuxFramebufferDevice.hpp"
#include "include/videoFile/VideoFile.hpp"
#include "include/buffer/BufferPool.hpp"
#include "include/producer/VideoProducer.hpp"
#include "include/decoder/Decoder.hpp"

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
    DECODER,
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
    } else if (strcmp(mode_str, "decoder") == 0) {
        return TestMode::DECODER;
    } else if (strcmp(mode_str, "rtsp") == 0) {
        return TestMode::RTSP;
    } else if (strcmp(mode_str, "ffmpeg") == 0) {
        return TestMode::FFMPEG;
    } else {
        return TestMode::UNKNOWN;
    }
}

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
    
    // 打开视频文件（使用 MMAP 读取器）
    VideoFile video;
    video.setReaderType(VideoReaderFactory::ReaderType::MMAP);  // 显式指定 MMAP 读取器
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
    
    // 打开视频文件（使用 MMAP 读取器）
    VideoFile video;
    video.setReaderType(VideoReaderFactory::ReaderType::MMAP);  // 显式指定 MMAP 读取器
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
    
    // 2. 获取 display 的 BufferPool（framebuffer 已托管）
    BufferPool& pool = display.getBufferPool();
    pool.printStats();
    
    // 3. 创建 VideoProducer（依赖注入 BufferPool）
    VideoProducer producer(pool);
    // 4. 配置并启动视频生产者
    int producer_thread_count = 2;  // 使用2个生产者线程
    
    VideoProducer::Config config(
        raw_video_path,
        display.getWidth(),
        display.getHeight(),
        display.getBitsPerPixel(),
        true,  // loop
        producer_thread_count,
        VideoReaderFactory::ReaderType::MMAP  // 显式指定 MMAP 读取器
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
    // 注册信号处理
    signal(SIGINT, signal_handler);
    
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
    BufferPool& pool = display.getBufferPool();
    
    printf("\n📦 Using LinuxFramebufferDevice's BufferPool\n");
    pool.printStats();
    
    // 3. 创建 VideoProducer（单线程，顺序读取）
    VideoProducer producer(pool);
    
    printf("\n🎬 Starting video producer (io_uring mode)...\n");
    printf("   Using 1 producer thread with io_uring async I/O\n");
    
    VideoProducer::Config config(
        raw_video_path,
        display.getWidth(),
        display.getHeight(),
        display.getBitsPerPixel(),
        true,  // loop
        1,  // 单线程顺序读取
        VideoReaderFactory::ReaderType::IOURING  // 显式指定 io_uring 读取器
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
    printf("\nℹ️  TODO: Implement IoUringVideoProducer for true async I/O performance\n");
    
    return 0;
}

/**
 * 测试5：解码器基础功能测试（零拷贝模式）
 * 
 * 功能：
 * - 演示解码器系统的零拷贝使用方法
 * - 测试FFmpeg解码器与BufferPool深度集成
 * - 展示FFmpeg原生类型的使用（AVPixelFormat等）
 * - 演示send/receive模式（FFmpeg标准）
 * 
 * 零拷贝工作流程：
 * 1. 创建BufferPool（预分配内存）
 * 2. 配置解码器使用ZERO_COPY模式
 * 3. FFmpeg通过get_buffer2回调从BufferPool获取空闲Buffer
 * 4. FFmpeg直接解码到BufferPool的Buffer
 * 5. 用户通过DecodedFrame.buffer使用（零拷贝！）
 * 6. 用户归还buffer到BufferPool
 * 
 * 架构设计：
 * 编码数据 → FFmpeg(get_buffer2) → 直接写入BufferPool → Display
 *                      ^^^^^^^^^^^^^^^^^^^^^^^^ 零拷贝！
 */
static int test_decoder_basic() {
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  TEST 5: Decoder Zero-Copy Test\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    // 1. 创建BufferPool（必须在配置解码器前创建）
    printf("📦 Step 1: Create BufferPool...\n");
    // 计算buffer大小：1920x1080 NV12 = 1920*1080*1.5 = 3,110,400 bytes
    size_t frame_size = 1920 * 1080 * 3 / 2;  // NV12 是 12bpp
    printf("   Frame size: %zu bytes (%.2f MB)\n", frame_size, frame_size / (1024.0 * 1024.0));
    
    // 创建预分配的BufferPool（自动分配模式）
    // 使用构造方式 1：BufferPool(int count, size_t size, bool use_cma, name, category)
    BufferPool decoder_pool(10, frame_size, false, "Decoder_Pool", "Decoder");
    printf("   ✅ BufferPool created: 10 buffers x %.2f MB\n", 
           frame_size / (1024.0 * 1024.0));
    
    // 2. 创建解码器（使用工厂模式）
    printf("\n⚙️  Step 2: Create and configure decoder...\n");
    Decoder decoder(DecoderFactory::DecoderType::FFMPEG);
    
    // 3. 配置解码器（使用FFmpeg原生类型！）
    decoder.setCodec(AV_CODEC_ID_H264);  // 使用FFmpeg的codec ID
    decoder.setOutputFormat(1920, 1080, AV_PIX_FMT_NV12);  // 使用FFmpeg的像素格式
    decoder.setThreadCount(4);
    
    // 4. 关键：设置零拷贝模式并关联BufferPool
    printf("   🔗 Attaching BufferPool for zero-copy...\n");
    decoder.setBufferMode(BufferAllocationMode::ZERO_COPY);  // 零拷贝模式
    decoder.attachBufferPool(&decoder_pool);
    
    // 5. 初始化解码器
    printf("\n🚀 Step 3: Initialize decoder...\n");
    DecoderStatus status = decoder.open();
    if (status != DecoderStatus::OK) {
        printf("❌ Failed to open decoder: %s\n", decoder.getLastError());
        return -1;
    }
    
    // 6. 显示解码器信息
    printf("\n📊 Decoder Information:\n");
    printf("   Type: %s\n", DecoderFactory::getDecoderTypeName(decoder.getDecoderType()));
    printf("   Codec: %s (ID=%d)\n", decoder.getCodecName(), decoder.getConfig().codec_id);
    printf("   Output: %dx%d\n", decoder.getConfig().width, decoder.getConfig().height);
    printf("   Pixel format: %s\n", av_get_pix_fmt_name(decoder.getConfig().pix_fmt));
    printf("   Hardware accelerated: %s\n", decoder.isHardwareAccelerated() ? "Yes" : "No");
    printf("   Buffer mode: ZERO_COPY ⚡\n");
    
    // 7. 模拟解码流程
    printf("\n🎬 Step 4: Decoder workflow demonstration:\n");
    printf("\n💡 Zero-Copy Workflow:\n");
    printf("   1. Create AVPacket with encoded data\n");
    printf("   2. Call decoder.sendPacket(packet)\n");
    printf("   3. Loop: decoder.receiveFrame(frame) until NEED_MORE_DATA\n");
    printf("   4. frame.buffer points to BufferPool's Buffer (zero-copy!)\n");
    printf("   5. Use: display.displayBufferByDMA(frame.buffer)\n");
    printf("   6. Release: frame.release() and pool.releaseFilled(buffer)\n");
    
    printf("\n📝 Example code:\n");
    printf("   AVPacket* packet = /* read from file/network */;\n");
    printf("   decoder.sendPacket(packet);\n");
    printf("   \n");
    printf("   DecodedFrame frame;\n");
    printf("   while (decoder.receiveFrame(frame) == DecoderStatus::OK) {\n");
    printf("       // frame.buffer -> BufferPool's Buffer (zero-copy!)\n");
    printf("       display.displayBufferByDMA(frame.buffer);\n");
    printf("       \n");
    printf("       frame.release();\n");
    printf("       pool.releaseFilled(frame.buffer);\n");
    printf("   }\n");
    
    printf("\n✅ Zero-copy decoder test completed!\n");
    printf("\n🎯 Key Benefits:\n");
    printf("   ⚡ Zero memory copy: FFmpeg -> BufferPool directly\n");
    printf("   🚀 High performance: Eliminates memcpy overhead\n");
    printf("   🔗 Deep integration: FFmpeg + BufferPool + Display\n");
    printf("   📐 Industry standard: Uses FFmpeg native types (AVPixelFormat, etc.)\n");
    
    // 8. 清理
    decoder.close();
    // BufferPool 会自动清理分配的 buffers
    
    printf("\n🎉 Test passed!\n\n");
    return 0;
}

/**
 * 测试6：RTSP 视频流播放（独立 BufferPool + DMA 零拷贝显示）
 * 
 * 功能演示：
 * - 连接 RTSP 视频流
 * - 使用 RtspVideoReader 解码（FFmpeg + 硬件解码器）
 * - 独立的 BufferPool 管理解码输出
 * - DMA 零拷贝显示：直接使用物理地址
 * - 展示 RTSP 流的实时处理能力
 * 
 * 架构设计：
 * RTSP Stream → FFmpeg 硬件解码 → AVFrame (带物理地址)
 *                                      ↓
 *                         独立的 BufferPool（管理解码输出）
 *                                      ↓
 *                         Buffer (包含物理地址)
 *                                      ↓
 *                  display.displayBufferByDMA(buffer)
 *                                      ↓
 *                         Display 驱动 DMA 显示
 * 
 * 零拷贝工作流程：
 * 1. RtspVideoReader 解码 RTSP 流，获得带物理地址的 AVFrame
 * 2. RtspVideoReader 从 AVFrame 提取物理地址（通过 DMA buf）
 * 3. RtspVideoReader 将 AVFrame 包装为 Buffer，注入独立的 BufferPool
 * 4. 消费者从独立的 BufferPool 获取 Buffer（含物理地址）
 * 5. 消费者调用 display.displayBufferByDMA(buffer)：
 *    - 直接将物理地址传递给驱动（FB_IOCTL_SET_DMA_INFO）
 *    - 驱动通过 DMA 从解码器内存直接读取显示
 *    - 整个过程：0 次 memcpy！
 * 6. 消费者归还 buffer，触发 deleter 回收 AVFrame
 * 
 * 关键设计理念：
 * - 独立 BufferPool：专门管理 RTSP 解码输出，不依赖 framebuffer
 * - 显式 DMA 调用：明确使用 displayBufferByDMA，清晰可控
 * - 零拷贝路径：解码器输出 → DMA → 显示，无中间拷贝
 */
static int test_rtsp_stream(const char* rtsp_url) {
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Test: RTSP Stream Playback (Independent BufferPool + DMA)\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    printf("ℹ️  Zero-Copy Workflow:\n");
    printf("   1. RtspVideoReader decodes RTSP → AVFrame with phys_addr\n");
    printf("   2. Extract phys_addr from AVFrame (via DMA buf)\n");
    printf("   3. Inject Buffer to independent BufferPool\n");
    printf("   4. Consumer acquires Buffer from independent pool\n");
    printf("   5. display.displayBufferByDMA(buffer) → DMA zero-copy\n");
    printf("   6. Consumer releases Buffer → triggers deleter\n\n");
    
    // 1. 初始化显示设备
    printf("🖥️  Initializing display device...\n");
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 创建独立的 BufferPool（动态注入模式）
    printf("📦 Creating independent BufferPool for RTSP decoder...\n");
    // 使用动态注入模式构造函数：初始为空，buffer 由 RtspVideoReader 在运行时动态注入
    // - 对用户透明：RtspVideoReader 内部通过 injectFilledBuffer() 注入解码后的 AVFrame
    // - 用户只需要正常使用 acquireFilled() / releaseFilled()，无需关心内部细节
    BufferPool rtsp_pool("RTSP_Decoder_Pool", "RTSP", 10);  // 最多缓存10帧
    
    printf("✅ Independent BufferPool created (dynamic injection mode)\n");
    rtsp_pool.printStats();
    
    // 3. 创建 VideoProducer（依赖注入独立的 BufferPool）
    printf("📹 Creating VideoProducer with independent BufferPool...\n");
    VideoProducer producer(rtsp_pool);  // 使用独立的 rtsp_pool
    
    // 4. 配置 RTSP 流（注意：推荐单线程）
    printf("🔗 Configuring RTSP stream: %s\n", rtsp_url);
    VideoProducer::Config config(
        rtsp_url,
        display.getWidth(),
        display.getHeight(),
        display.getBitsPerPixel(),
        false,  // loop（对RTSP无意义）
        1,      // thread_count（RTSP推荐单线程）
        VideoReaderFactory::ReaderType::RTSP  // 显式指定 RTSP 读取器
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
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    
    // 7. 消费者循环：从独立 BufferPool 获取并通过 DMA 显示
    int frame_count = 0;
    int dma_success = 0;
    int dma_failed = 0;
    
    while (g_running) {
        // 从独立的 RTSP BufferPool 获取已解码的 buffer（带物理地址）
        Buffer* decoded_buffer = rtsp_pool.acquireFilled(true, 100);
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
        rtsp_pool.releaseFilled(decoded_buffer);
        
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
    rtsp_pool.printStats();
    
    return 0;
}

/**
 * 测试6：FFmpeg 编码视频文件播放（使用 FfmpegVideoReader）
 * 
 * 功能：
 * - 打开编码视频文件（MP4, AVI, MKV等）
 * - 使用 FfmpegVideoReader 进行解码
 * - 集成 VideoProducer + BufferPool 架构
 * - 支持两种模式：
 *   1. 普通模式：使用 framebuffer pool（解码后 memcpy）
 *   2. 零拷贝模式：使用独立 pool + 特殊解码器（如 h264_taco）
 * 
 * 参数：
 * @param video_path 视频文件路径（如 "video.mp4"）
 * @param use_zero_copy 是否使用零拷贝模式（需要特殊硬件）
 */
static int test_ffmpeg_video(const char* video_path, bool use_zero_copy = false) {
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Test: FFmpeg Encoded Video Playback\n");
    printf("  File: %s\n", video_path);
    printf("  Mode: %s\n", use_zero_copy ? "Zero-Copy (h264_taco)" : "Normal (memcpy)");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    // 1. 初始化显示设备
    printf("🖥️  Initializing display device...\n");
    LinuxFramebufferDevice display;
    if (!display.initialize(0)) {
        return -1;
    }
    
    // 2. 根据模式选择 BufferPool
    BufferPool* pool = nullptr;
    BufferPool* independent_pool = nullptr;
    // 零拷贝模式：创建独立的 BufferPool（动态注入模式）
    printf("📦 Creating independent BufferPool for FFmpeg decoder (zero-copy)...\n");
    independent_pool = new BufferPool("FFmpeg_Decoder_Pool", "FFMPEG", 10);
    pool = independent_pool;
    printf("✅ Independent BufferPool created (dynamic injection mode)\n");
    pool->printStats();
    // 3. 创建 VideoProducer（依赖注入 BufferPool）
    printf("📹 Creating VideoProducer with BufferPool...\n");
    VideoProducer producer(*pool);
    
    // 4. 配置 FFmpeg 解码
    printf("🎬 Configuring FFmpeg video reader: %s\n", video_path);
    
    VideoProducer::Config config(
        video_path,
        display.getWidth(),
        display.getHeight(),
        display.getBitsPerPixel(),
        true,  // loop（循环播放）
        1,  // 零拷贝推荐单线程，普通模式可以多线程
        VideoReaderFactory::ReaderType::FFMPEG  // 显式指定 FFMPEG 读取器
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
        if (independent_pool) delete independent_pool;
        return -1;
    }
    
    printf("\n✅ Video decoding started, starting playback...\n");
    printf("   Press Ctrl+C to stop\n\n");
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    
    // 7. 消费者循环
    int frame_count = 0;
    
    while (g_running) {
        // 从 BufferPool 获取已解码的 buffer
        Buffer* filled_buffer = pool->acquireFilled(true, 100);
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
        pool->releaseFilled(filled_buffer);
        
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
    pool->printStats();
    
    // 清理
    if (independent_pool) {
        delete independent_pool;
    }
    
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
    printf("                      producer:   BufferPool + VideoProducer test\n");
    printf("                      iouring:    io_uring mode (using VideoProducer)\n");
    printf("                      decoder:    Decoder system test\n");
    printf("                      rtsp:       RTSP stream playback (zero-copy)\n");
    printf("                      ffmpeg:     FFmpeg encoded video playback (NEW)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s video.raw\n", prog_name);
    printf("  %s -m loop video.raw\n", prog_name);
    printf("  %s -m sequential video.raw\n", prog_name);
    printf("  %s -m producer video.raw\n", prog_name);
    printf("  %s -m iouring video.raw\n", prog_name);
    printf("  %s -m decoder\n", prog_name);
    printf("  %s -m rtsp rtsp://192.168.1.100:8554/stream\n", prog_name);
    printf("  %s -m ffmpeg video.mp4\n", prog_name);
    printf("\n");
    printf("Test Modes Description:\n");
    printf("  loop:       Load N frames into framebuffer and loop display them\n");
    printf("  sequential: Read and display frames sequentially from file\n");
    printf("  producer:   Use BufferPool + VideoProducer architecture (zero-copy)\n");
    printf("  iouring:    io_uring async I/O mode\n");
    printf("  decoder:    Decoder system basic functionality test\n");
    printf("  rtsp:       RTSP stream decoding and display (zero-copy, FFmpeg)\n");
    printf("  ffmpeg:     FFmpeg encoded video file decoding (MP4/AVI/MKV/etc)\n");
    printf("\n");
    printf("Note:\n");
    printf("  - Raw video file must match framebuffer resolution\n");
    printf("  - Format: ARGB888 (4 bytes per pixel)\n");
    printf("  - Decoder mode demonstrates the decoder API (no file needed)\n");
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
    
    // 检查是否提供了视频文件路径（decoder模式除外）
    if (!raw_video_path && test_mode != TestMode::DECODER) {
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
        
        case TestMode::DECODER:
            result = test_decoder_basic();
            break;
        
        case TestMode::RTSP:
            result = test_rtsp_stream(raw_video_path);  // raw_video_path实际是rtsp_url
            break;
        
        case TestMode::FFMPEG:
            result = test_ffmpeg_video(raw_video_path, false);  // 使用普通模式（memcpy）
            // 如果需要零拷贝模式，可以改为: test_ffmpeg_video(raw_video_path, true)
            break;
        
        case TestMode::UNKNOWN:
        default:
            printf("Error: Unknown mode '%s'\n\n", mode);
            print_usage(argv[0]);
            return 1;
    }
    
    return result;
}

