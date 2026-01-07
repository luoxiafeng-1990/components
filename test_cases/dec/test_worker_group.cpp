/**
 * WorkerGroup 架构测试示例
 * 
 * 本示例演示如何使用 MultiWorkerProductionLine 的 WorkerGroup 架构：
 * - 1个生产者 Worker（RTSP 录制）
 * - 2个消费者 Worker（硬件解码 + 软件解码）
 * - 消费者自动从生产者的 BufferPool 获取数据
 * 
 * 编译命令：
 *   g++ -o test_worker_group test_worker_group.cpp -I../../include \
 *       -L../../lib -lcomponents -lavcodec -lavformat -lavutil -lpthread
 * 
 * 运行命令：
 *   ./test_worker_group rtsp://192.168.1.100:8554/stream
 */

#include <stdio.h>
#include <signal.h>
#include <atomic>
#include <chrono>
#include "productionline/MultiWorkerProductionLine.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "productionline/io/BufferWriter.hpp"
#include "common/Logger.hpp"

// 全局中断标志
static std::atomic<bool> g_running(true);

// 信号处理器
static void signal_handler(int signum) {
    if (signum == SIGINT) {
        printf("\n🛑 收到中断信号 (Ctrl+C)，正在停止...\n");
        g_running = false;
    }
}

int main(int argc, char* argv[]) {
    // 初始化日志系统
    INIT_LOGGER();
    
    // 检查参数
    if (argc < 2) {
        printf("用法: %s <rtsp_url>\n", argv[0]);
        printf("示例: %s rtsp://192.168.1.100:8554/stream\n", argv[0]);
        return -1;
    }
    
    const char* rtsp_url = argv[1];
    
    // 注册信号处理器
    signal(SIGINT, signal_handler);
    
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║   WorkerGroup 架构测试示例                            ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO_FMT("RTSP URL: %s\n", rtsp_url);
    
    // ============================================================
    // 步骤1：配置 WorkerGroup
    // ============================================================
    LOG_INFO("[步骤1] 配置 WorkerGroup...");
    
    MultiWorkerProductionLine::MultiWorkerConfig config;
    
    // 创建 WorkerGroup（1个生产者 + 2个消费者）
    MultiWorkerProductionLine::WorkerGroup group("my_rtsp_group");
    
    // 1.1 配置生产者 Worker（RTSP 录制）
    LOG_INFO("  [1.1] 配置生产者 Worker (RTSP Record)");
    group.producer_config = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(rtsp_url)  // RTSP 流地址
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)  // Packet 录制 Worker（支持 RTSP/文件/HTTP 等）
        .build();
    
    // 1.2 配置消费者 Worker 1（硬件解码器）
    LOG_INFO("  [1.2] 配置消费者 Worker 1 (Hardware Decoder - h264_taco)");
    auto consumer1_config = WorkerConfigBuilder()
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(1920, 1080)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useTaco("h264")  // 使用 TACO 硬件解码器
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_RTSP)  // RTSP 解码 Worker
        .build();
    group.consumer_configs.push_back(consumer1_config);
    
    // 1.3 配置消费者 Worker 2（软件解码器）
    LOG_INFO("  [1.3] 配置消费者 Worker 2 (Software Decoder - libavcodec)");
    auto consumer2_config = WorkerConfigBuilder()
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(1920, 1080)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useSoftware()  // 使用软件解码器
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_RTSP)  // RTSP 解码 Worker
        .build();
    group.consumer_configs.push_back(consumer2_config);
    
    // 1.4 添加 Group 到配置
    config.groups.push_back(group);
    
    // 1.5 配置线程池和其他参数
    config.thread_pool_size = 4;                      // 线程池大小
    config.default_sync_timeout_ms = 5000;            // 同步超时时间
    config.default_max_consecutive_errors = 10;       // 最大连续错误数
    
    LOG_INFO_FMT("  ✅ WorkerGroup 配置完成: group_id='%s', 1 producer + %zu consumers",
                 group.group_id.c_str(), group.consumer_configs.size());
    
    // ============================================================
    // 步骤2：创建并启动 MultiWorkerProductionLine
    // ============================================================
    LOG_INFO("\n[步骤2] 创建并启动 MultiWorkerProductionLine...");
    
    MultiWorkerProductionLine multi_worker(config, false, 1, false);
    
    if (!multi_worker.start()) {
        LOG_ERROR("❌ 启动失败");
        return -1;
    }
    
    LOG_INFO("  ✅ MultiWorkerProductionLine 启动成功");
    LOG_INFO("  说明：");
    LOG_INFO("    - 生产者 Worker 正在读取 RTSP 流并产生编码包 (AVPacket)");
    LOG_INFO("    - 消费者 Worker 1 自动从生产者 BufferPool 获取数据并硬件解码");
    LOG_INFO("    - 消费者 Worker 2 自动从生产者 BufferPool 获取数据并软件解码");
    LOG_INFO("    - 所有处理都在后台线程中自动进行");
    
    // ============================================================
    // 步骤3：获取消费者的 BufferPool（用于获取解码后的帧）
    // ============================================================
    LOG_INFO("\n[步骤3] 获取消费者的 BufferPool...");
    
    // 注意：对于测试程序而言，Group 内的消费者是数据的"生产者"
    // 因为我们要从消费者的输出 BufferPool 中获取解码后的帧
    uint64_t hw_decoder_pool_id = multi_worker.getGroupConsumerBufferPoolId(0, 0);  // Group 0, Consumer 0
    uint64_t sw_decoder_pool_id = multi_worker.getGroupConsumerBufferPoolId(0, 1);  // Group 0, Consumer 1
    
    if (hw_decoder_pool_id == 0 || sw_decoder_pool_id == 0) {
        LOG_ERROR("❌ 获取 BufferPool ID 失败");
        multi_worker.stop();
        return -1;
    }
    
    auto hw_pool_sptr = BufferPoolRegistry::getInstance().getPool(hw_decoder_pool_id).lock();
    auto sw_pool_sptr = BufferPoolRegistry::getInstance().getPool(sw_decoder_pool_id).lock();
    
    if (!hw_pool_sptr || !sw_pool_sptr) {
        LOG_ERROR("❌ 获取 BufferPool 失败");
        multi_worker.stop();
        return -1;
    }
    
    LOG_INFO_FMT("  ✅ 硬件解码器 BufferPool: '%s' (ID: %lu)",
                 hw_pool_sptr->getName().c_str(), hw_decoder_pool_id);
    LOG_INFO_FMT("  ✅ 软件解码器 BufferPool: '%s' (ID: %lu)",
                 sw_pool_sptr->getName().c_str(), sw_decoder_pool_id);
    
    // ============================================================
    // 步骤4：创建 BufferWriter（保存解码后的帧到文件）
    // ============================================================
    LOG_INFO("\n[步骤4] 创建 BufferWriter（保存解码帧）...");
    
    using namespace productionline::io;
    
    // 等待第一帧以获取格式信息
    Buffer* first_hw_buffer = hw_pool_sptr->acquireFilled(true, 5000);
    Buffer* first_sw_buffer = sw_pool_sptr->acquireFilled(true, 5000);
    
    if (!first_hw_buffer || !first_sw_buffer) {
        LOG_ERROR("❌ 等待第一帧超时");
        if (first_hw_buffer) hw_pool_sptr->releaseFilled(first_hw_buffer);
        if (first_sw_buffer) sw_pool_sptr->releaseFilled(first_sw_buffer);
        multi_worker.stop();
        return -1;
    }
    
    // 获取格式信息
    AVPixelFormat hw_format = first_hw_buffer->getImageFormat();
    int hw_width = first_hw_buffer->getImageWidth();
    int hw_height = first_hw_buffer->getImageHeight();
    
    AVPixelFormat sw_format = first_sw_buffer->getImageFormat();
    int sw_width = first_sw_buffer->getImageWidth();
    int sw_height = first_sw_buffer->getImageHeight();
    
    LOG_INFO_FMT("  硬件解码器格式: %s (%dx%d)",
                 av_get_pix_fmt_name(hw_format), hw_width, hw_height);
    LOG_INFO_FMT("  软件解码器格式: %s (%dx%d)",
                 av_get_pix_fmt_name(sw_format), sw_width, sw_height);
    
    // 创建两个 BufferWriter
    BufferWriter hw_writer, sw_writer;
    
    if (!hw_writer.open("/tmp/hw_decoder_output.yuv", hw_format, hw_width, hw_height)) {
        LOG_ERROR("❌ 打开硬件解码器 BufferWriter 失败");
        hw_pool_sptr->releaseFilled(first_hw_buffer);
        sw_pool_sptr->releaseFilled(first_sw_buffer);
        multi_worker.stop();
        return -1;
    }
    
    if (!sw_writer.open("/tmp/sw_decoder_output.yuv", sw_format, sw_width, sw_height)) {
        LOG_ERROR("❌ 打开软件解码器 BufferWriter 失败");
        hw_writer.close();
        hw_pool_sptr->releaseFilled(first_hw_buffer);
        sw_pool_sptr->releaseFilled(first_sw_buffer);
        multi_worker.stop();
        return -1;
    }
    
    LOG_INFO("  ✅ BufferWriter 创建成功");
    LOG_INFO("    - 硬件解码输出: /tmp/hw_decoder_output.yuv");
    LOG_INFO("    - 软件解码输出: /tmp/sw_decoder_output.yuv");
    
    // 保存第一帧
    hw_writer.write(first_hw_buffer);
    sw_writer.write(first_sw_buffer);
    hw_pool_sptr->releaseFilled(first_hw_buffer);
    sw_pool_sptr->releaseFilled(first_sw_buffer);
    
    // ============================================================
    // 步骤5：消费者循环（从两个解码器获取帧并保存）
    // ============================================================
    LOG_INFO("\n[步骤5] 开始消费解码后的帧...");
    LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    LOG_INFO("  按 Ctrl+C 停止测试");
    
    auto start_time = std::chrono::steady_clock::now();
    int hw_frame_count = 1;  // 已保存第一帧
    int sw_frame_count = 1;
    const int MAX_DURATION_SECONDS = 10;  // 录制 10 秒
    
    while (g_running) {
        // 检查录制时长
        auto now = std::chrono::steady_clock::now();
        int elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        if (elapsed >= MAX_DURATION_SECONDS) {
            LOG_INFO_FMT("\n⏱️  达到录制时长限制: %d 秒", MAX_DURATION_SECONDS);
            break;
        }
        
        // 从硬件解码器获取帧
        Buffer* hw_buffer = hw_pool_sptr->acquireFilled(true, 100);
        if (hw_buffer) {
            if (hw_writer.write(hw_buffer)) {
                hw_frame_count++;
            }
            hw_pool_sptr->releaseFilled(hw_buffer);
        }
        
        // 从软件解码器获取帧
        Buffer* sw_buffer = sw_pool_sptr->acquireFilled(true, 100);
        if (sw_buffer) {
            if (sw_writer.write(sw_buffer)) {
                sw_frame_count++;
            }
            sw_pool_sptr->releaseFilled(sw_buffer);
        }
        
        // 每 50 帧打印一次进度
        if (hw_frame_count % 50 == 0 || sw_frame_count % 50 == 0) {
            LOG_INFO_FMT("  进度: HW=%d 帧, SW=%d 帧, 已运行 %d 秒",
                         hw_frame_count, sw_frame_count, elapsed);
        }
    }
    
    LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // ============================================================
    // 步骤6：清理资源
    // ============================================================
    LOG_INFO("\n[步骤6] 清理资源...");
    
    hw_writer.close();
    sw_writer.close();
    multi_worker.stop();
    
    auto end_time = std::chrono::steady_clock::now();
    double total_duration = std::chrono::duration<double>(end_time - start_time).count();
    
    // ============================================================
    // 步骤7：打印统计信息
    // ============================================================
    LOG_INFO("\n╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║   测试结果                                            ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO_FMT("  硬件解码器:");
    LOG_INFO_FMT("    保存帧数: %d", hw_frame_count);
    LOG_INFO_FMT("    输出文件: /tmp/hw_decoder_output.yuv");
    LOG_INFO_FMT("  软件解码器:");
    LOG_INFO_FMT("    保存帧数: %d", sw_frame_count);
    LOG_INFO_FMT("    输出文件: /tmp/sw_decoder_output.yuv");
    LOG_INFO_FMT("  总时长: %.2f 秒", total_duration);
    
    // 打印详细统计
    LOG_INFO("\n详细统计信息:");
    multi_worker.printDetailedStats();
    
    LOG_INFO("\n💡 验证输出文件:");
    LOG_INFO_FMT("   ffplay -f rawvideo -pixel_format %s -video_size %dx%d /tmp/hw_decoder_output.yuv",
                 av_get_pix_fmt_name(hw_format), hw_width, hw_height);
    LOG_INFO_FMT("   ffplay -f rawvideo -pixel_format %s -video_size %dx%d /tmp/sw_decoder_output.yuv",
                 av_get_pix_fmt_name(sw_format), sw_width, sw_height);
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    
    return 0;
}
