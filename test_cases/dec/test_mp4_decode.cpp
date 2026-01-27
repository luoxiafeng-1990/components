/**
 * Multi-Codec Video Decode Test
 *
 * 通用视频解码测试程序，支持多种编码格式和灵活配置
 *
 * 功能：
 * - 支持多种编码格式：H264, H265(HEVC), MJPEG
 * - 使用硬件解码器（h264_taco, hevc_taco, mjpeg_taco）
 * - 可配置分辨率、帧率、线程数
 * - 多线程并行解码，提升性能
 * - 实时显示到 Framebuffer（可选）
 * - 保存解码后的数据用于验证
 * - 性能监控和详细统计报告
 * - 自动验证解码 FPS 是否达标
 *
 * 编译：
 *   通过 Buildroot 构建系统：
 *     cd /home/zyko/workshop-debian
 *     make components-rebuild
 *
 *   或手动编译（不推荐）：
 *     g++ -o test_mp4_decode test_mp4_decode.cpp \
 *         -I../../include -L../../build/lib \
 *         -lcomponents -lavformat -lavcodec -lavutil -lpthread -std=c++17
 *
 * 使用方法：
 *   # H264 解码
 *   ./test_mp4_decode video.mp4
 *   ./test_mp4_decode video.mp4 --codec h264
 *
 *   # H265 解码
 *   ./test_mp4_decode video.mp4 --codec h265
 *
 *   # MJPEG 解码
 *   ./test_mp4_decode video.mjpeg --codec mjpeg
 *
 *   # 自定义参数
 *   ./test_mp4_decode video.mp4 --codec h264 --resolution 1280x720 --fps 30
 *   ./test_mp4_decode video.mp4 --save-frames 100 --max-frames 500
 *   ./test_mp4_decode video.mp4 --no-display --threads 4
 *   ./test_mp4_decode video.mp4 --output /tmp/my.rgb --save-frames -1
 *
 *   # PSNR 验证（需要FFmpeg）
 *   ./test_mp4_decode video.mp4 --enable-psnr --save-frames -1
 *   ./test_mp4_decode video.mp4 --enable-psnr --min-psnr 35.0 --save-frames -1
 *
 * 验证解码结果：
 *   ffplay -f rawvideo -pix_fmt argb -s 1920x1080 /tmp/decoded_*.rgb
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include <cmath>
#include <chrono>
#include <thread>
#include <memory>
#include <algorithm>
#include <numeric>
#include <cctype>
#include <vector>
#include <atomic>

// Components 头文件
#include "productionline/VideoProductionLine.hpp"
#include "productionline/MultiWorkerProductionLine.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/worker/WorkerBase.hpp"
#include "productionline/worker/RtspPacketSource.hpp"
#include "productionline/io/BufferWriter.hpp"
#include "productionline/io/BufferComparator.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "display/LinuxFramebufferDevice.hpp"
#include "monitor/PerformanceMonitor.hpp"
#include "common/Logger.hpp"
#include "framework/TestMacros.hpp"

// FFmpeg 头文件
extern "C" {
#include <libavutil/pixdesc.h>
#include <libavcodec/avcodec.h>
}

// 全局变量
static volatile bool g_running = true;
static std::atomic<bool> g_rtsp_interrupted(false);

/**
 * @brief 检测输入是否为RTSP流
 */
static bool is_rtsp_stream(const char *path) {
  if (!path)
    return false;
  return (strncmp(path, "rtsp://", 7) == 0) ||
         (strncmp(path, "rtsps://", 8) == 0);
}

/**
 * @brief 使用两个独立的 VideoProductionLine + BufferComparator 进行PSNR验证
 * @param source_video 原始视频文件
 * @param width 视频宽度
 * @param height 视频高度
 * @param decoder_name 硬件解码器名称（h264_taco, hevc_taco, mjpeg_taco等）
 * @param max_frames 最大对比帧数
 * @param min_psnr 最小PSNR要求
 * @return true PSNR达标，false 不达标
 *
 * @note 使用两个独立的 VideoProductionLine
 * 同时运行硬件和软件解码器（都直接从文件读取）
 * @note 使用 BufferComparator 组件进行对比，自动计算PSNR
 * @note 格式自适应，支持YUV和RGB格式
 */
bool validate_psnr_streaming(const char *source_video, int width, int height,
                             const char *decoder_name, int max_frames,
                             double min_psnr) {
  using namespace productionline::io;

  // ⭐ 重置运行标志（PSNR验证是独立的流程，不受主解码流程影响）
  g_running = true;

  // ⭐ 检测是否为RTSP流
  bool is_rtsp = is_rtsp_stream(source_video);
  if (is_rtsp) {
    LOG_INFO("📡 Detected RTSP stream for PSNR validation");
    g_rtsp_interrupted = false;
    RtspPacketSource::clearInterrupt();
  }

  LOG_INFO("");
  LOG_INFO("╔═══════════════════════════════════════════════════════╗");
  LOG_INFO(
      "║  PSNR Validation (Dual VideoProductionLine + BufferComparator)     ║");
  LOG_INFO("╚═══════════════════════════════════════════════════════╝");
  LOG_INFO_FMT("Source: %s", source_video);
  LOG_INFO_FMT("Type: %s", is_rtsp ? "RTSP Stream" : "Video File");
  LOG_INFO_FMT("Resolution: %dx%d", width, height);
  LOG_INFO_FMT("Hardware decoder: %s", decoder_name ? decoder_name : "unknown");
  LOG_INFO_FMT("Max frames: %d", max_frames);
  LOG_INFO_FMT("Minimum PSNR requirement: %.2f dB", min_psnr);
  LOG_INFO("");

  // ========================================================================
  // 步骤1：同时启动硬件和软件解码器（确保帧序列对齐）
  // ========================================================================
  LOG_INFO(
      "[Step 1] Starting hardware and software decoders simultaneously...");
  if (is_rtsp) {
    LOG_INFO("  (RTSP stream: both decoders will read from the same stream)");
  } else {
    LOG_INFO(
        "  (Both decoders will start from frame 0 for sequence alignment)");
  }

  // 1.1 配置硬件解码器
  VideoProductionLine hw_producer(false, 1);
  DecoderConfigBuilder hw_decoderConfigBuilder;
  // ⭐ 参考 test.cpp 的 test_buffer_writer_format：保存YUV文件时需要传入 TacoConfig
  // 构建 TacoConfig，确保 YUV 格式正确配置（与 test_buffer_writer_yuv_formats 一致）
  auto tacoConfig =
      TacoConfigBuilder()
          .setYuvConfig("YUV420 8-bit NV12", "bt601")  // ⭐ 明确设置 YUV 格式（与 test.cpp 一致）
          .setChannels(true, false)                     // ⭐ 只启用 ch0，禁用 ch1（确保 PP0 输出 YUV）
          .setDecoderOutputResolution(width, height)     // ⭐ 设置输出分辨率（与 test.cpp 一致，可能影响UV平面访问）
          .build();
  
  if (decoder_name && decoder_name[0] != '\0') {
    std::string dname(decoder_name);
    if (dname == "h264_taco") {
      hw_decoderConfigBuilder.useTaco("h264", tacoConfig);  // ⭐ 传入 TacoConfig 以确保 YUV 格式正确
    } else if (dname == "hevc_taco") {
      hw_decoderConfigBuilder.useTaco("hevc", tacoConfig);  // ⭐ 传入 TacoConfig 以确保 YUV 格式正确
    } else if (dname == "jpeg_taco") {
      hw_decoderConfigBuilder.useTaco("jpeg", tacoConfig);  // ⭐ 传入 TacoConfig 以确保 YUV 格式正确
    } else {
      if (dname.length() > 5 && dname.substr(dname.length() - 5) == "_taco") {
        std::string codec = dname.substr(0, dname.length() - 5);
        hw_decoderConfigBuilder.useTaco(codec, tacoConfig);  // ⭐ 传入 TacoConfig 以确保 YUV 格式正确
      } else {
        hw_decoderConfigBuilder.setDecoderName(decoder_name);
      }
    }
  } else {
    hw_decoderConfigBuilder.useSoftware();
  }

  // ⭐ 根据输入类型选择WorkerType
  WorkerType worker_type =
      is_rtsp ? WorkerType::FFMPEG_DECODE : WorkerType::FFMPEG_DECODE;

  // ⭐ RTSP流：使用更大的BufferPool（32个Buffer）以支持PSNR对比
  DataSourceConfigBuilder hw_dataSourceBuilder;
  hw_dataSourceBuilder.setPath(source_video);
  if (is_rtsp) {
    hw_dataSourceBuilder.setBufferCount(32); // PSNR对比需要更大的BufferPool
  }

  auto hw_workerConfig =
      WorkerConfigBuilder()
          .setDataSourceConfig(hw_dataSourceBuilder.build())
          .setDisplayConfig(DisplayConfigBuilder()
                                .setDisplayResolution(width, height)
                                .setBitsPerPixel(32)
                                .build())
          .setDecoderConfig(hw_decoderConfigBuilder.build())
          .setWorkerType(worker_type)
          .build();

  hw_producer.setErrorCallback([](const std::string &error) {
    LOG_ERROR_FMT("Hardware Decoder Error: %s", error.c_str());
    g_running = false;
  });

  // 1.2 配置软件解码器
  VideoProductionLine sw_producer(false, 1);

  // ⭐ RTSP流：使用更大的BufferPool（32个Buffer）以支持PSNR对比
  DataSourceConfigBuilder sw_dataSourceBuilder;
  sw_dataSourceBuilder.setPath(source_video);
  if (is_rtsp) {
    sw_dataSourceBuilder.setBufferCount(32); // PSNR对比需要更大的BufferPool
  }

  auto sw_workerConfig =
      WorkerConfigBuilder()
          .setDataSourceConfig(sw_dataSourceBuilder.build())
          .setDisplayConfig(DisplayConfigBuilder()
                                .setDisplayResolution(width, height)
                                .setBitsPerPixel(32)
                                .build())
          .setDecoderConfig(DecoderConfigBuilder().useSoftware().build())
          .setWorkerType(worker_type)
          .build();

  sw_producer.setErrorCallback([](const std::string &error) {
    LOG_ERROR_FMT("Software Decoder Error: %s", error.c_str());
    g_running = false;
  });

  // ⭐ 关键：同时启动两个解码器（都从视频文件的第0帧开始）
  LOG_INFO("  Starting hardware decoder...");
  if (!hw_producer.start(hw_workerConfig)) {
    LOG_ERROR("Failed to start hardware decoder");
    return false;
  }

  LOG_INFO("  Starting software decoder...");
  if (!sw_producer.start(sw_workerConfig)) {
    LOG_ERROR("Failed to start software decoder");
    hw_producer.stop();
    return false;
  }

  LOG_INFO("  ✅ Both decoders started (frame sequence aligned from frame 0)");

  // ========================================================================
  // 步骤2：同时获取两个解码器的第一个Buffer（确保都是第0帧）
  // ========================================================================
  LOG_INFO("\n[Step 2] Getting BufferPools and acquiring first buffers (frame "
           "0)...");

  uint64_t hw_pool_id = hw_producer.getWorkingBufferPoolId();
  uint64_t sw_pool_id = sw_producer.getWorkingBufferPoolId();

  if (hw_pool_id == 0 || sw_pool_id == 0) {
    LOG_ERROR("No working BufferPool ID available");
    if (hw_pool_id == 0)
      hw_producer.stop();
    if (sw_pool_id == 0)
      sw_producer.stop();
    return false;
  }

  auto hw_pool = BufferPoolRegistry::getInstance().getPool(hw_pool_id).lock();
  auto sw_pool = BufferPoolRegistry::getInstance().getPool(sw_pool_id).lock();

  if (!hw_pool || !sw_pool) {
    LOG_ERROR("BufferPool not found");
    hw_producer.stop();
    sw_producer.stop();
    return false;
  }

  LOG_INFO_FMT("  Hardware BufferPool: '%s' (ID: %lu)",
               hw_pool->getName().c_str(), hw_pool_id);
  LOG_INFO_FMT("  Software BufferPool: '%s' (ID: %lu)",
               sw_pool->getName().c_str(), sw_pool_id);

  // ⭐ RTSP流：等待两个解码器都生产足够的帧（至少5帧）再开始对比
  if (is_rtsp) {
    LOG_INFO(
        "  ⏳ Waiting for decoders to produce initial frames (RTSP stream)...");
    int wait_count = 0;
    while (wait_count < 30 && g_running) { // 最多等待30次（30秒）
      int hw_filled = hw_pool->getFilledCount();
      int sw_filled = sw_pool->getFilledCount();
      if (hw_filled >= 5 && sw_filled >= 5) {
        LOG_INFO_FMT("  ✅ Decoders ready: HW=%d frames, SW=%d frames",
                     hw_filled, sw_filled);
        break;
      }
      if (wait_count % 5 == 0) {
        LOG_INFO_FMT("  ⏳ Waiting... HW=%d frames, SW=%d frames", hw_filled,
                     sw_filled);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      wait_count++;
    }
    if (wait_count >= 30) {
      LOG_WARN("  ⚠️  Timeout waiting for decoders to produce initial frames");
    }
  }

  // ⭐ 关键：同时等待两个解码器的第0帧
  LOG_INFO("  Acquiring first buffers (frame 0) from both decoders...");
  Buffer *first_hw_buf = hw_pool->acquireFilled(true, 5000);
  Buffer *first_sw_buf = sw_pool->acquireFilled(true, 5000);

  if (!first_hw_buf || !first_sw_buf) {
    LOG_ERROR("Failed to get first buffers (frame 0) from both decoders");
    if (first_hw_buf)
      hw_pool->releaseFilled(first_hw_buf);
    if (first_sw_buf)
      sw_pool->releaseFilled(first_sw_buf);
    hw_producer.stop();
    sw_producer.stop();
    return false;
  }

  LOG_INFO("  ✅ Got frame 0 from both decoders (sequence aligned)");

  // ========================================================================
  // 步骤3：创建 BufferComparator
  // ========================================================================
  LOG_INFO("\n[Step 3] Creating BufferComparator...");

  CompareConfig compare_config;
  compare_config.strategy        = CompareConfig::AUTO_LAYERED; // 自动分层验证
  compare_config.format_strategy = CompareConfig::AUTO;         // 格式自适应
  compare_config.quick_psnr_threshold = min_psnr; // 使用传入的阈值
  compare_config.quick_warn_threshold =
      min_psnr - 5.0;                             // 警告阈值比通过阈值低5dB
  compare_config.use_perceptual_weighting = true; // 感知加权
  compare_config.verbose                  = true; // 详细日志
  compare_config.save_report              = true; // 保存报告
  char report_path[256];
  snprintf(report_path, sizeof(report_path), "./psnr_validation_report_%d.txt",
           getpid());
  compare_config.report_path = report_path;

  BufferComparator comparator;
  if (!comparator.open(compare_config)) {
    LOG_ERROR("Failed to open BufferComparator");
    hw_producer.stop();
    sw_producer.stop();
    return false;
  }

  LOG_INFO("  ✅ BufferComparator initialized");
  LOG_INFO("  Strategy: AUTO_LAYERED (fast → deep)");
  LOG_INFO("  Format: AUTO (YUV/RGB adaptive)");
  LOG_INFO_FMT("  PSNR threshold: %.1f dB (pass), %.1f dB (warn)",
               compare_config.quick_psnr_threshold,
               compare_config.quick_warn_threshold);

  // ========================================================================
  // 步骤4：检测格式并创建BufferWriter（可选）
  // ========================================================================
  LOG_INFO("\n[Step 4] Detecting format...");

  // 从软件Buffer获取格式（用于保存）
  AVPixelFormat sw_output_format = AV_PIX_FMT_NV12; // 默认格式
  int sw_actual_width            = width;
  int sw_actual_height           = height;
  std::string sw_format_name     = "NV12";
  std::string sw_ffplay_format   = "nv12";

  if (first_sw_buf->hasImageMetadata()) {
    sw_output_format     = first_sw_buf->getImageFormat();
    sw_actual_width      = first_sw_buf->getImageWidth();
    sw_actual_height     = first_sw_buf->getImageHeight();
    const char *fmt_name = av_get_pix_fmt_name(sw_output_format);
    sw_format_name       = fmt_name ? fmt_name : "NV12";

    // 设置ffplay格式
    if (sw_output_format == AV_PIX_FMT_NV12) {
      sw_ffplay_format = "nv12";
    } else if (sw_output_format == AV_PIX_FMT_YUV420P) {
      sw_ffplay_format = "yuv420p";
    } else {
      sw_ffplay_format = sw_format_name;
    }

    LOG_INFO_FMT("   Detected software decoder format: %s (%dx%d)",
                 sw_format_name.c_str(), sw_actual_width, sw_actual_height);
  } else {
    LOG_WARN("   Software decoder buffer has no metadata, using default NV12");
  }

  // ⭐ 对比第一帧（使用 BufferComparator 组件）
  LOG_INFO("\n[Step 5] Comparing first frame (frame 0)...");
  LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

  // Reduced debug output for PSNR calculation

  FrameCompareResult first_result =
      comparator.compare(first_sw_buf, first_hw_buf);

  // ⭐ 简化第一帧输出：只在失败时输出
  if (!first_result.passed) {
    LOG_WARN_FMT("  Frame 0: PSNR-Y=%.2f dB %s",
                 first_result.psnr_y,
                 first_result.level == FrameCompareResult::FAIL ? "❌" : "⚠️");
  }

  // 释放第一帧
  sw_pool->releaseFilled(first_sw_buf);
  hw_pool->releaseFilled(first_hw_buf);

  // ========================================================================
  // 步骤6：帧序列对齐的对比循环（第1帧、第2帧...）
  // ========================================================================
  LOG_INFO("\n[Step 6] Comparing decoder outputs frame by frame (sequence "
           "aligned)...");
  LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

  int frame_count      = 1; // 第0帧已对比，从第1帧开始
  const int MAX_FRAMES = max_frames > 0 ? max_frames : 300; // 最多对比指定帧数
  int timeout_count    = 0;
  const int MAX_TIMEOUT =
      200; // ⭐ RTSP流需要更长的超时时间（200次 × 5秒 = 1000秒）

  // ⭐ PSNR统计：记录每帧的PSNR值（参考 run_psnr_compare_test）
  std::vector<double> psnr_y_values;
  std::vector<double> psnr_u_values;
  std::vector<double> psnr_v_values;
  std::vector<double> psnr_avg_values;

  // ⭐ 将第一帧的PSNR值也记录到统计中
  if (first_result.psnr_y > 0.0) {
    psnr_y_values.push_back(first_result.psnr_y);
    psnr_u_values.push_back(first_result.psnr_u);
    psnr_v_values.push_back(first_result.psnr_v);
    psnr_avg_values.push_back(first_result.psnr_avg);
  }

  LOG_INFO_FMT(
      "  Starting comparison loop: frame_count=%d, MAX_FRAMES=%d, g_running=%d",
      frame_count, MAX_FRAMES, g_running ? 1 : 0);

  while (g_running && frame_count < MAX_FRAMES) {
    // ⭐ RTSP流：检查中断标志
    if (is_rtsp && g_rtsp_interrupted.load()) {
      LOG_INFO("\n⚠️  检测到RTSP中断请求，停止PSNR验证...");
      break;
    }

    // ⭐ RTSP流：使用更长的超时时间（5秒），文件流使用1秒
    int timeout_ms = is_rtsp ? 5000 : 1000;

    // 同时获取硬件和软件解码的 Buffer
    Buffer *sw_buffer = sw_pool->acquireFilled(true, timeout_ms);
    Buffer *hw_buffer = hw_pool->acquireFilled(true, timeout_ms);

    if (!sw_buffer || !hw_buffer) {
      // ⭐ RTSP流：检查中断标志
      if (is_rtsp && g_rtsp_interrupted.load()) {
        LOG_INFO("\n⚠️  检测到RTSP中断请求，停止PSNR验证...");
        if (sw_buffer)
          sw_pool->releaseFilled(sw_buffer);
        if (hw_buffer)
          hw_pool->releaseFilled(hw_buffer);
        break;
      }

      // ⭐ 添加更详细的日志（使用INFO级别，确保能看到）
      int hw_filled   = hw_pool->getFilledCount();
      int sw_filled   = sw_pool->getFilledCount();
      bool hw_running = hw_producer.isRunning();
      bool sw_running = sw_producer.isRunning();

      if (!sw_buffer) {
        LOG_INFO_FMT("⚠️  Software decoder timeout (waiting for frame %d, %d "
                     "frames available, running=%d)",
                     frame_count, sw_filled, sw_running ? 1 : 0);
      }
      if (!hw_buffer) {
        LOG_INFO_FMT("⚠️  Hardware decoder timeout (waiting for frame %d, %d "
                     "frames available, running=%d)",
                     frame_count, hw_filled, hw_running ? 1 : 0);
      }

      if (sw_buffer)
        sw_pool->releaseFilled(sw_buffer);
      if (hw_buffer)
        hw_pool->releaseFilled(hw_buffer);

      timeout_count++;

      // ⭐ RTSP流：每5次超时打印一次进度（更频繁）
      if (is_rtsp && timeout_count % 5 == 0) {
        LOG_INFO_FMT("  ⏳ Waiting for frame %d... (HW: %d frames available, "
                     "running=%d; SW: %d frames available, running=%d)",
                     frame_count, hw_filled, hw_running ? 1 : 0, sw_filled,
                     sw_running ? 1 : 0);
      }

      if (timeout_count >= MAX_TIMEOUT) {
        LOG_INFO_FMT("\n  ⚠️  Decoders finished or timeout after %d attempts "
                     "(compared %d frames)",
                     timeout_count, frame_count);
        break;
      }

      // ⭐ RTSP流：不要因为解码器停止就立即退出，继续等待一段时间
      // 只有在连续多次超时且两个解码器都停止时才退出
      if (!hw_running && !sw_running) {
        // 如果两个解码器都停止了，再等待几次看看是否有新帧
        if (timeout_count >= 10) {
          LOG_INFO_FMT("\n  Decoders finished naturally after %d timeouts "
                       "(compared %d frames)",
                       timeout_count, frame_count);
          break;
        }
      }

      continue;
    }

    timeout_count = 0;

    // ⭐ 关键调用：使用 BufferComparator 对比两个 Buffer
    // 注意：对于RTSP流，两个解码器都从同一个流读取，帧序列应该是对齐的
    FrameCompareResult result = comparator.compare(sw_buffer, hw_buffer);

    // ⭐ 记录PSNR值（用于统计）
    if (result.psnr_y > 0.0) {
      psnr_y_values.push_back(result.psnr_y);
      psnr_u_values.push_back(result.psnr_u);
      psnr_v_values.push_back(result.psnr_v);
      psnr_avg_values.push_back(result.psnr_avg);
    }

    // 释放Buffer
    sw_pool->releaseFilled(sw_buffer);
    hw_pool->releaseFilled(hw_buffer);

    frame_count++;

    // ⭐ 简化输出：只在失败时输出，或每100帧输出一次进度
    if (!result.passed && result.psnr_y > 0.0) {
      // 失败帧：输出详细信息
      LOG_WARN_FMT("  Frame %3d: PSNR-Y=%.2f dB %s",
                   frame_count, result.psnr_y,
                   result.level == FrameCompareResult::FAIL ? "❌ FAIL" : "⚠️ WARN");
    } else if (frame_count % 100 == 0) {
      // 每100帧：输出进度统计
      double avg_psnr_y = psnr_y_values.empty()
                              ? 0.0
                              : std::accumulate(psnr_y_values.begin(),
                                                psnr_y_values.end(), 0.0) /
                                    psnr_y_values.size();

      LOG_INFO_FMT("  Progress: %d/%d frames | Avg PSNR-Y=%.2f dB | "
                   "Passed: %d, Failed: %d",
                   frame_count, MAX_FRAMES, avg_psnr_y,
                   comparator.getPassedCount(), comparator.getFailedCount());
    }
  }

  LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

  // ⭐ 排空剩余 Buffer（不对比，直接释放）
  LOG_INFO("Draining remaining buffers...");
  Buffer *remaining = nullptr;
  int hw_drained    = 0;
  while ((remaining = hw_pool->acquireFilled(false, 0)) != nullptr) {
    hw_drained++;
    hw_pool->releaseFilled(remaining);
  }
  if (hw_drained > 0) {
    LOG_INFO_FMT("Drained %d remaining hardware buffers", hw_drained);
  }

  int sw_drained = 0;
  while ((remaining = sw_pool->acquireFilled(false, 0)) != nullptr) {
    sw_drained++;
    sw_pool->releaseFilled(remaining);
  }
  if (sw_drained > 0) {
    LOG_INFO_FMT("Drained %d remaining software buffers", sw_drained);
  }

  // ========================================================================
  // 步骤7：关闭资源并打印结果
  // ========================================================================
  LOG_INFO("\n[Step 7] Cleaning up...");

  comparator.close();
  hw_producer.stop();
  sw_producer.stop();

  // ========================================================================
  // 步骤8：计算详细的PSNR统计信息
  // ========================================================================
  LOG_INFO("\n[Step 8] Calculating PSNR statistics...");

  // ⭐ 使用 BufferComparator 打印结果
  LOG_INFO("\n═══════════════════════════════════════════════════════");
  LOG_INFO("  Decoder Comparison Results");
  LOG_INFO("═══════════════════════════════════════════════════════");
  comparator.printSummary();
  LOG_INFO("═══════════════════════════════════════════════════════\n");

  // ⭐ 打印简化的PSNR统计
  if (!psnr_y_values.empty()) {
    // 计算平均值
    double avg_psnr_y =
        std::accumulate(psnr_y_values.begin(), psnr_y_values.end(), 0.0) /
        psnr_y_values.size();
    double avg_psnr_avg =
        std::accumulate(psnr_avg_values.begin(), psnr_avg_values.end(), 0.0) /
        psnr_avg_values.size();

    // 计算最小值和最大值
    auto minmax_y =
        std::minmax_element(psnr_y_values.begin(), psnr_y_values.end());
    auto minmax_avg =
        std::minmax_element(psnr_avg_values.begin(), psnr_avg_values.end());

    double min_psnr_y   = *minmax_y.first;
    double max_psnr_y   = *minmax_y.second;
    double min_psnr_avg = *minmax_avg.first;
    double max_psnr_avg = *minmax_avg.second;

    LOG_INFO("  PSNR Statistics (Hardware vs Software):");
    LOG_INFO_FMT("    Total frames analyzed: %zu", psnr_y_values.size());
    LOG_INFO_FMT("    Average: Y=%.2f dB (avg=%.2f dB)", avg_psnr_y, avg_psnr_avg);
    LOG_INFO_FMT("    Range Y:  [%.2f, %.2f] dB", min_psnr_y, max_psnr_y);
    LOG_INFO_FMT("    Range Avg: [%.2f, %.2f] dB", min_psnr_avg, max_psnr_avg);
    LOG_INFO("");
    LOG_INFO("  Quality Assessment (by Comparator):");
    LOG_INFO_FMT("    Passed: %d ✅ (%.1f%%)", comparator.getPassedCount(),
                 100.0 * comparator.getPassedCount() / frame_count);
    LOG_INFO_FMT("    Failed: %d ❌ (%.1f%%)", comparator.getFailedCount(),
                 100.0 * comparator.getFailedCount() / frame_count);
    LOG_INFO("");

    // 质量评级
    if (avg_psnr_avg >= 38.0) {
      LOG_INFO("  ✅ Overall Quality: EXCELLENT (visually lossless)");
    } else if (avg_psnr_avg >= 35.0) {
      LOG_INFO("  ⚠️  Overall Quality: GOOD (minor differences)");
    } else {
      LOG_INFO("  ❌ Overall Quality: POOR (visible artifacts)");
    }
    LOG_INFO("");
  } else {
    LOG_WARN("  No PSNR values recorded for statistics");
  }

  LOG_INFO("  Decoding Statistics");
  LOG_INFO("═══════════════════════════════════════════════════════");
  LOG_INFO_FMT("Hardware decoder:");
  LOG_INFO_FMT("  Frames produced: %d", hw_producer.getProducedFrames());
  LOG_INFO_FMT("  Frames skipped: %d", hw_producer.getSkippedFrames());
  LOG_INFO_FMT("  Average FPS: %.2f", hw_producer.getAverageFPS());
  LOG_INFO_FMT("Software decoder:");
  LOG_INFO_FMT("  Frames produced: %d", sw_producer.getProducedFrames());
  LOG_INFO_FMT("  Frames skipped: %d", sw_producer.getSkippedFrames());
  LOG_INFO_FMT("  Average FPS: %.2f", sw_producer.getAverageFPS());
  LOG_INFO("═══════════════════════════════════════════════════════");

  if (compare_config.save_report) {
    LOG_INFO_FMT("💡 Detailed comparison report saved to: %s",
                 compare_config.report_path.c_str());
    LOG_INFO("   View the report for frame-by-frame analysis");
  }

  LOG_INFO("\n💡 PSNR Interpretation:");
  LOG_INFO("   >= 38 dB: Excellent quality (visually lossless)");
  LOG_INFO("   35-38 dB: Good quality (minor differences)");
  LOG_INFO("   < 35 dB:  Poor quality (visible artifacts)");

  // 判断是否通过
  bool passed = comparator.isPassed();
  if (passed) {
    LOG_INFO_FMT("\n✅ PASS - 解码精度优秀（完全一致）");
    return true;
  } else {
    LOG_INFO_FMT("\n❌ FAIL - 解码精度差（存在明显错误）");
    LOG_INFO("说明: 部分帧 PSNR < 35dB，硬件解码存在严重问题");
    return false;
  }
}

// 编码格式枚举
enum class CodecType {
  H264,
  H265,
  MJPEG,
  CUSTOM // 自定义解码器
};

// 测试配置结构体
struct TestConfig {
  const char *video_path;
  CodecType codec_type;
  const char *decoder_name;
  const char *output_path;
  const char *profile; // H264 Profile: baseline, main, high
  bool enable_display;
  bool enable_psnr; // 启用PSNR验证
  double min_psnr;  // 最小PSNR要求（dB）
  int save_frames;  // 保存多少帧，0=不保存，-1=全部保存
  int max_frames;   // 最大处理帧数，-1=全部处理
  int threads;      // 生产线线程数
  int width;        // 视频宽度
  int height;       // 视频高度
  int fps;          // 目标帧率（用于性能验证）
  bool auto_test;   // 自动测试模式

  TestConfig()
      : video_path(nullptr), codec_type(CodecType::H264), decoder_name(nullptr),
        output_path(nullptr), profile("high"), enable_display(true),
        enable_psnr(false), min_psnr(30.0), save_frames(-1), max_frames(-1),
        threads(2), width(1920), height(1080), fps(60), auto_test(false) {}
};

/**
 * @brief 从环境变量或默认值创建测试配置
 */
TestConfig create_test_config_from_env(CodecType codec_type = CodecType::H264) {
  TestConfig config;
  config.codec_type = codec_type;

  // 从环境变量读取配置（如果存在）
  const char *env_val;

  if ((env_val = getenv("MP4_DECODE_DISPLAY")) != nullptr) {
    config.enable_display =
        (strcmp(env_val, "1") == 0 || strcasecmp(env_val, "true") == 0);
  }

  if ((env_val = getenv("MP4_DECODE_PSNR")) != nullptr) {
    config.enable_psnr =
        (strcmp(env_val, "1") == 0 || strcasecmp(env_val, "true") == 0);
  }

  if ((env_val = getenv("MP4_DECODE_MIN_PSNR")) != nullptr) {
    config.min_psnr = atof(env_val);
  }

  if ((env_val = getenv("MP4_DECODE_SAVE_FRAMES")) != nullptr) {
    config.save_frames = atoi(env_val);
  }

  if ((env_val = getenv("MP4_DECODE_MAX_FRAMES")) != nullptr) {
    config.max_frames = atoi(env_val);
  }

  if ((env_val = getenv("MP4_DECODE_THREADS")) != nullptr) {
    config.threads = atoi(env_val);
    if (config.threads < 1)
      config.threads = 1;
  }

  if ((env_val = getenv("MP4_DECODE_RESOLUTION")) != nullptr) {
    if (sscanf(env_val, "%dx%d", &config.width, &config.height) != 2) {
      // 解析失败，使用默认值
    }
  }

  if ((env_val = getenv("MP4_DECODE_FPS")) != nullptr) {
    config.fps = atoi(env_val);
  }

  if ((env_val = getenv("MP4_DECODE_OUTPUT")) != nullptr) {
    config.output_path = env_val;
  }

  return config;
}

/**
 * @brief 获取解码器名称（内联辅助函数）
 */
static const char *getDecoderName(const TestConfig &config) {
  if (config.decoder_name != nullptr) {
    return config.decoder_name;
  }
  switch (config.codec_type) {
  case CodecType::H264:
    return "h264_taco";
  case CodecType::H265:
    return "hevc_taco";
  case CodecType::MJPEG:
    return "mjpeg_taco";
  default:
    return "h264_taco";
  }
}

/**
 * @brief 获取编解码器名称（内联辅助函数）
 * @note WorkerBase::getExpectedCodecIdFromDecoderName 和 getCodecFriendlyName
 * 是 protected， 因此使用简单的字符串映射
 */
static std::string getCodecName(const TestConfig &config) {
  if (config.decoder_name != nullptr && strlen(config.decoder_name) > 0) {
    std::string dname(config.decoder_name);
    // 转换为小写进行比较
    std::transform(dname.begin(), dname.end(), dname.begin(), ::tolower);

    if (dname.find("h264") != std::string::npos ||
        dname.find("avc") != std::string::npos) {
      return "H.264/AVC";
    } else if (dname.find("h265") != std::string::npos ||
               dname.find("hevc") != std::string::npos) {
      return "H.265/HEVC";
    } else if (dname.find("mjpeg") != std::string::npos ||
               dname.find("jpeg") != std::string::npos) {
      return "MJPEG";
    } else if (dname.find("vp8") != std::string::npos) {
      return "VP8";
    } else if (dname.find("vp9") != std::string::npos) {
      return "VP9";
    } else if (dname.find("av1") != std::string::npos) {
      return "AV1";
    }
    return "CUSTOM";
  }

  // 根据 codec_type 返回名称
  switch (config.codec_type) {
  case CodecType::H264:
    return "H.264/AVC";
  case CodecType::H265:
    return "H.265/HEVC";
  case CodecType::MJPEG:
    return "MJPEG";
  default:
    return "UNKNOWN";
  }
}

// 前向声明
static int test_mp4_decode_single_impl(const char *video_path,
                                       const TestConfig &config);
static int test_rtsp_decode_single_impl(const char *rtsp_url,
                                        const TestConfig &config);

/**
 * MP4视频解码测试主函数（支持H264/H265/MJPEG等多种编码格式）
 * 模仿 test_mp4_azh.cpp 的格式
 */
static int run_decode_test_with_params(const char *video_path, int width,
                                       int height, const char *decoder_name,
                                       int decode_threads, double frame_rate,
                                       const char *profile,
                                       const char *test_tag) {
  LOG_INFO("\n═══════════════════════════════════════════════════════");
  LOG_INFO_FMT("  Decode Test: %s", test_tag ? test_tag : "unknown");
  LOG_INFO("═══════════════════════════════════════════════════════\n");

  if (!video_path || video_path[0] == '\0') {
    LOG_ERROR("No video file path specified");
    return -1;
  }

  // ⭐ 检查是否为RTSP流，如果是则跳过文件存在性检查
  bool is_rtsp = is_rtsp_stream(video_path);
  if (!is_rtsp) {
    // 检查视频文件是否存在（仅对文件路径检查）
    if (access(video_path, F_OK) != 0) {
      LOG_ERROR_FMT("❌ Video file not found: %s", video_path);
      return -1;
    }
  } else {
    LOG_INFO_FMT("📡 RTSP stream detected: %s", video_path);
  }

  // 从环境变量创建配置
  TestConfig test_config = create_test_config_from_env();
  test_config.video_path = video_path;
  test_config.width      = width;
  test_config.height     = height;
  test_config.fps        = frame_rate;
  test_config.threads    = decode_threads;
  if (profile) {
    test_config.profile = profile;
  }
  if (decoder_name && decoder_name[0] != '\0') {
    test_config.decoder_name = decoder_name;
    // 根据解码器名称设置 codec_type
    if (strcmp(decoder_name, "h264_taco") == 0) {
      test_config.codec_type = CodecType::H264;
    } else if (strcmp(decoder_name, "hevc_taco") == 0 ||
               strcmp(decoder_name, "h265_taco") == 0) {
      test_config.codec_type = CodecType::H265;
    } else if (strcmp(decoder_name, "mjpeg_taco") == 0 ||
               strcmp(decoder_name, "jpeg_taco") == 0) {
      test_config.codec_type = CodecType::MJPEG;
    } else if (strcmp(decoder_name, "software") == 0 ||
               strcmp(decoder_name, "libavcodec") == 0 ||
               strcmp(decoder_name, "sw") == 0) {
      // 软件解码器：codec_type 需要根据视频文件自动检测或从参数推断
      // TestConfig 默认构造函数已经将 codec_type 初始化为
      // H264，所以不需要额外设置
    }
  }

  LOG_INFO_FMT("Codec: %s", getCodecName(test_config).c_str());
  if (test_config.codec_type == CodecType::H264) {
    LOG_INFO_FMT("Profile: %s", test_config.profile);
  }
  LOG_INFO_FMT("Decoder: %s", getDecoderName(test_config));
  LOG_INFO_FMT("Resolution: %dx%d @ %dfps", test_config.width,
               test_config.height, test_config.fps);
  LOG_INFO_FMT("Threads: %d", test_config.threads);
  LOG_INFO_FMT("Display output: %s",
               test_config.enable_display ? "enabled" : "disabled");
  if (test_config.save_frames == -1) {
    LOG_INFO("Save frames: all");
  } else if (test_config.save_frames == 0) {
    LOG_INFO("Save frames: none");
  } else {
    LOG_INFO_FMT("Save frames: first %d", test_config.save_frames);
  }
  if (test_config.max_frames == -1) {
    LOG_INFO("Max frames: unlimited");
  } else {
    LOG_INFO_FMT("Max frames: %d", test_config.max_frames);
  }
  if (test_config.output_path) {
    LOG_INFO_FMT("Output file: %s", test_config.output_path);
  }
  LOG_INFO("");

  // ⭐ 根据输入类型调用不同的实现函数
  if (is_rtsp) {
    return test_rtsp_decode_single_impl(video_path, test_config);
  } else {
    return test_mp4_decode_single_impl(video_path, test_config);
  }
}

/**
 * RTSP流解码测试主函数内部实现（专门处理RTSP流）
 */
static int test_rtsp_decode_single_impl(const char *rtsp_url,
                                        const TestConfig &config) {
  // ========== RTSP流初始化 ==========
  LOG_INFO("📡 Detected RTSP stream source");
  g_rtsp_interrupted = false;
  RtspPacketSource::clearInterrupt();

  // ========== 第1步：初始化显示设备（必须在配置WorkerConfig之前）==========
  // ⭐ 参考 test.cpp：先初始化显示设备，然后使用实际分辨率配置WorkerConfig
  LOG_INFO("[Step 1/8] Initializing display device...");

  std::unique_ptr<LinuxFramebufferDevice> display;
  bool has_display = false;
  int display_width = config.width;
  int display_height = config.height;
  int display_bpp = 32;

  if (config.enable_display) {
    display     = std::make_unique<LinuxFramebufferDevice>();
    has_display = display->initialize(0);
    if (has_display) {
      // ⭐ 使用显示设备的实际分辨率（与 test.cpp 一致）
      display_width = display->getWidth();
      display_height = display->getHeight();
      display_bpp = display->getBitsPerPixel();
      LOG_INFO_FMT("✅ Display initialized: %dx%d @ %d bpp",
                   display_width, display_height, display_bpp);
    } else {
      LOG_WARN("⚠️  Display not available, continuing without display");
    }
  } else {
    LOG_INFO("ℹ️  Display disabled by user");
  }

  // ========== 第2步：配置解码器 ==========
  LOG_INFO("[Step 2/8] Configuring decoder...");

  // ⭐ 参考 test.cpp：使用简化的 TacoConfig（只设置 channels）
  DecoderConfigBuilder decoderBuilder;
  const char *decoder_name = getDecoderName(config);

  // ⭐ 检查是否使用软件解码器
  bool use_software = false;
  if (decoder_name && (strcmp(decoder_name, "software") == 0 ||
                       strcmp(decoder_name, "libavcodec") == 0 ||
                       strcmp(decoder_name, "sw") == 0)) {
    use_software = true;
    decoderBuilder.useSoftware();
    LOG_INFO("  Decoder: Software decoder (libavcodec)");
  } else {
    // ⭐ 参考 test.cpp：使用简化的 TacoConfig（只设置 channels）
    auto tacoConfig = TacoConfigBuilder()
        .setChannels(true, false)  // ⭐ 与 test.cpp 一致
        .build();
    
    if (strcmp(decoder_name, "h264_taco") == 0) {
      decoderBuilder.useTaco("h264", tacoConfig);
      LOG_INFO("  Decoder: TACO H.264 hardware decoder");
    } else if (strcmp(decoder_name, "hevc_taco") == 0) {
      decoderBuilder.useTaco("hevc", tacoConfig);
      LOG_INFO("  Decoder: TACO H.265/HEVC hardware decoder");
    } else if (strcmp(decoder_name, "mjpeg_taco") == 0 ||
               strcmp(decoder_name, "jpeg_taco") == 0) {
      decoderBuilder.useTaco("jpeg", tacoConfig);
      LOG_INFO("  Decoder: TACO MJPEG hardware decoder");
    } else {
      decoderBuilder.setDecoderName(decoder_name);
      LOG_INFO_FMT("  Decoder: %s", decoder_name);
    }
  }

  // ⭐ 参考 test.cpp：配置 RTSP 流（完全按照 test.cpp 的方式）
  LOG_INFO_FMT("Configuring RTSP stream: %s", rtsp_url);

  auto workerConfig = WorkerConfigBuilder()
      .setDataSourceConfig(
          DataSourceConfigBuilder()
              .setPath(rtsp_url)
              .setBufferCount(32)  // ⭐ 与 test.cpp 一致：使用32个Buffer
              .build()
      )
      .setDisplayConfig(
          DisplayConfigBuilder()
              .setDisplayResolution(display_width, display_height)  // ⭐ 使用显示设备的实际分辨率
              .setBitsPerPixel(display_bpp)  // ⭐ 使用显示设备的实际 bpp
              .build()
      )
      .setDecoderConfig(decoderBuilder.build())
      .setWorkerType(WorkerType::FFMPEG_DECODE)  // ⭐ RTSP专用WorkerType
      .build();

  // ⭐ RTSP流特殊处理：设置buffer_mode=true
  // ⭐ v2.22 重构：数据源配置从 decoder 移至 datasource
  // 让BufferFillingWorkerFacade调用open(nullptr)，从而触发无参open()
  // 无参open()会从WorkerConfig读取所有参数（RTSP URL、分辨率等）
  workerConfig.data_source.buffer_mode = true;
  LOG_DEBUG_FMT("  RTSP stream: Setting buffer_mode=true (workerConfig.data_source.buffer_mode = %d)", 
                workerConfig.data_source.buffer_mode ? 1 : 0);

  if (use_software) {
    LOG_INFO_FMT("✅ Decoder configured: Software decoder (libavcodec), %dx%d",
                 display_width, display_height);
  } else {
    LOG_INFO_FMT("✅ Decoder configured: %s, %dx%d, hardware acceleration",
                 getDecoderName(config), display_width, display_height);
  }

  // ========== 第3步：创建生产线 ==========
  // ⭐ 参考 test.cpp：使用单线程（推荐用于RTSP流）
  LOG_INFO("[Step 3/8] Creating VideoProductionLine...");

  VideoProductionLine producer(false, 1, false);  // ⭐ 与 test.cpp 一致：单线程

  // 设置错误回调
  producer.setErrorCallback([](const std::string &error) {
    LOG_ERROR_FMT("Decode Error: %s", error.c_str());
    g_running = false;
  });

  LOG_INFO("✅ VideoProductionLine created (1 producer thread, recommended for RTSP)");

  // ========== 第4步：启动生产线 ==========
  // ⭐ 参考 test.cpp：直接调用 start()，不设置 buffer_mode
  LOG_INFO("[Step 4/8] Starting decode...");

  if (!producer.start(workerConfig)) {
    LOG_ERROR("❌ Failed to start VideoProductionLine");
    return -1;
  }

  LOG_INFO("✅ Decoding started");

  // ========== 第5步：获取 BufferPool ==========
  LOG_INFO("[Step 5/8] Getting BufferPool...");

  uint64_t pool_id = producer.getWorkingBufferPoolId();
  if (pool_id == 0) {
    LOG_ERROR("❌ No working BufferPool ID available");
    producer.stop();
    return -1;
  }

  auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
  auto pool_sptr = pool_weak.lock();
  if (!pool_sptr) {
    LOG_ERROR("❌ BufferPool not found or destroyed");
    producer.stop();
    return -1;
  }

  LOG_INFO_FMT("✅ BufferPool: '%s' (ID: %lu)", pool_sptr->getName().c_str(),
               pool_id);
  pool_sptr->printStats();

  // ⚠️ RTSP流：禁用MultiWorkerProductionLine对比（Buffer模式在RTSP下有问题）
  if (!use_software && config.save_frames != 0) {
    LOG_INFO("[Step 5.5/8] RTSP stream: skipping MultiWorkerProductionLine "
             "comparison");
    LOG_INFO("  (RTSP streams use direct decoding, comparison disabled for "
             "stability)");
  }

  // ========== 第6步：创建 BufferWriter ==========
  LOG_INFO("[Step 6/8] Creating BufferWriter...");

  using productionline::io::BufferWriter;
  std::unique_ptr<BufferWriter> writer;
  char output_yuv[256];
  AVPixelFormat output_format = AV_PIX_FMT_NONE;
  int actual_width            = config.width;
  int actual_height           = config.height;
  std::string format_name     = "NV12";
  const char *file_ext        = "yuv";
  std::string ffplay_format   = "nv12";
  int save_count              = 0; // 保存的帧数计数器

  if (config.save_frames != 0) {
    // 等待第一个Buffer以检测实际格式
    LOG_INFO("   Waiting for first buffer to detect format...");
    Buffer *first_buffer = pool_sptr->acquireFilled(true, 5000); // 5秒超时
    if (!first_buffer) {
      LOG_ERROR("❌ Failed to get first buffer (timeout)");
      producer.stop();
      return -1;
    }

    // 从Buffer元数据获取实际格式
    if (first_buffer->hasImageMetadata()) {
      output_format        = first_buffer->getImageFormat();
      actual_width         = first_buffer->getImageWidth();
      actual_height        = first_buffer->getImageHeight();
      const char *fmt_name = av_get_pix_fmt_name(output_format);
      format_name          = fmt_name ? fmt_name : "NV12";

      // 设置ffplay格式（NV12对应nv12）
      if (output_format == AV_PIX_FMT_NV12) {
        ffplay_format = "nv12";
      } else if (output_format == AV_PIX_FMT_YUV420P) {
        ffplay_format = "yuv420p";
      } else {
        ffplay_format = format_name;
      }

      LOG_INFO_FMT("   Detected format: %s (%dx%d)", format_name.c_str(),
                   actual_width, actual_height);
    } else {
      // 默认使用NV12
      output_format = AV_PIX_FMT_NV12;
      format_name   = "NV12";
      ffplay_format = "nv12";
      LOG_WARN("   Buffer has no metadata, using default NV12");
    }

    // 创建BufferWriter
    writer = std::make_unique<BufferWriter>();
    if (config.output_path) {
      snprintf(output_yuv, sizeof(output_yuv), "%s", config.output_path);
    } else {
      snprintf(output_yuv, sizeof(output_yuv), "/tmp/decoded_%dx%d_%ld.%s",
               actual_width, actual_height, time(nullptr), file_ext);
    }

    if (!writer->openRaw(output_yuv, output_format, actual_width, actual_height)) {
      LOG_ERROR_FMT("❌ Failed to open BufferWriter: %s", output_yuv);
      pool_sptr->releaseFilled(first_buffer);
      producer.stop();
      return -1;
    }

    LOG_INFO_FMT("✅ BufferWriter opened: %s (format: %s, %dx%d)", output_yuv,
                 format_name.c_str(), actual_width, actual_height);

    // 保存第一帧
    if (writer->write(first_buffer)) {
      LOG_INFO("   ✅ Saved first frame");
      save_count = 1; // 第一帧已保存，初始化计数器
    }
    pool_sptr->releaseFilled(first_buffer);
  } else {
    LOG_INFO("ℹ️  Output file disabled (save_frames = 0)");
  }

  // ========== 第7步：消费者循环（解码+显示+保存） ==========
  LOG_INFO("[Step 7/8] Consuming decoded frames...");
  LOG_INFO("Press Ctrl+C to stop early");
  LOG_INFO("");

  int frame_count   = 0;
  int display_count = 0;
  // save_count 已在上面声明，如果 writer 已打开且第一帧已保存，则保持为
  // 1，否则为 0
  if (!(writer && writer->isOpen())) {
    save_count = 0; // writer 未打开，重置为 0
  }
  int save_limit = (config.save_frames == -1) ? INT32_MAX : config.save_frames;
  int max_frames_limit =
      (config.max_frames == -1) ? INT32_MAX : config.max_frames;

  // ⭐ 参考 test.cpp 的消费者循环逻辑：使用超时计数，让视频自然结束
  int timeout_count = 0;
  const int MAX_TIMEOUT = 10; // ⭐ 与 test.cpp 一致：超时10次后退出

  auto start_time       = std::chrono::steady_clock::now();
  auto last_report_time = start_time;

  while (g_running && frame_count < max_frames_limit) {
    // 从 BufferPool 获取已解码的 Buffer
    Buffer *buffer = pool_sptr->acquireFilled(true, 100); // ⭐ 与 test.cpp 一致：100ms 超时

    if (!buffer) {
      // ⭐ RTSP流：检查中断标志
      if (g_rtsp_interrupted.load()) {
        LOG_INFO("⚠️  检测到RTSP中断请求，停止解码...");
        break;
      }

      // ⭐ 超时处理：使用超时计数，与 test.cpp 一致
      timeout_count++;
      if (timeout_count >= MAX_TIMEOUT) {
        LOG_INFO("Video finished, stopping...");
        break;
      }
      continue; // 超时但未达到最大次数，继续等待
    }
    
    // ⭐ 成功获取 buffer，重置超时计数
    timeout_count = 0;

    // 显示到屏幕（如果启用）
    if (has_display) {
      display->waitVerticalSync();
      if (display->displayBufferByDMA(buffer)) {
        display_count++;
      } else {
        // DMA 失败，回退到普通显示
        display->displayFilledFramebuffer(buffer);
        display_count++;
      }
    }

    // 保存到文件（使用BufferWriter）
    if (writer && save_count < save_limit) {
      if (writer->write(buffer)) {
        save_count++;
      } else {
        LOG_WARN_FMT("Failed to write frame %d to file", frame_count);
      }
    }

    // 归还 Buffer
    pool_sptr->releaseFilled(buffer);
    frame_count++;

    // 每 60 帧（约1秒）打印一次进度
    auto now     = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - last_report_time)
                       .count();

    if (elapsed >= 1000) { // 每秒报告一次
      double current_fps = producer.getAverageFPS();
      LOG_INFO_FMT("Progress: %d frames | FPS: %.1f | Display: %d | Saved: %d",
                   frame_count, current_fps, display_count, save_count);
      last_report_time = now;
    }
  }

  // 排空剩余的 Buffer
  LOG_INFO("Draining remaining buffers...");
  Buffer *remaining = nullptr;
  int drained       = 0;
  while ((remaining = pool_sptr->acquireFilled(false, 0)) != nullptr) {
    if (has_display) {
      display->waitVerticalSync();
      display->displayBufferByDMA(remaining);
      display_count++;
    }

    if (writer && save_count < save_limit) {
      if (writer->write(remaining)) {
        save_count++;
      } else {
        LOG_WARN_FMT("Failed to write remaining frame %d to file", frame_count);
      }
    }
    pool_sptr->releaseFilled(remaining);
    frame_count++;
    drained++;
  }
  if (drained > 0) {
    LOG_INFO_FMT("Drained %d remaining buffers", drained);
  }

  producer.stop();

  if (writer) {
    writer->close();
    LOG_INFO_FMT("✅ BufferWriter closed: %d frames written",
                 writer->getWriteCount());
  }

  auto end_time       = std::chrono::steady_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            end_time - start_time)
                            .count();

  LOG_INFO("✅ Consuming completed");

  // ========== 第8步：停止生产线并输出统计 ==========
  LOG_INFO("[Step 8/8] Stopping and generating report...");

  producer.stop();

  // 计算性能指标
  double decode_fps          = producer.getAverageFPS();
  double realtime_fps        = (frame_count * 1000.0) / total_duration;
  double target_fps          = config.fps;
  bool fps_meets_requirement = decode_fps >= target_fps;

  // ========== 输出测试报告 ==========
  LOG_INFO("");
  LOG_INFO("╔═══════════════════════════════════════════════════════╗");
  LOG_INFO("║  Test Report: RTSP Stream Decode                    ║");
  LOG_INFO("╚═══════════════════════════════════════════════════════╝");
  LOG_INFO_FMT("RTSP URL: %s", rtsp_url);
  LOG_INFO_FMT("Codec: %s", getCodecName(config).c_str());
  LOG_INFO_FMT("Decoder: %s (hardware)", getDecoderName(config));
  LOG_INFO_FMT("Resolution: %dx%d", config.width, config.height);
  LOG_INFO_FMT("Target FPS: %.0f", target_fps);
  LOG_INFO("");
  LOG_INFO("--- Performance Metrics ---");
  LOG_INFO_FMT("Total frames decoded: %d", frame_count);
  LOG_INFO_FMT("Frames displayed: %d", display_count);
  LOG_INFO_FMT("Frames saved: %d", save_count);
  LOG_INFO_FMT("Total time: %.2f seconds", total_duration / 1000.0);
  LOG_INFO_FMT("Decode FPS (producer): %.2f", decode_fps);
  LOG_INFO_FMT("Realtime FPS (overall): %.2f", realtime_fps);
  LOG_INFO_FMT("Frames produced: %d", producer.getProducedFrames());
  LOG_INFO_FMT("Frames skipped: %d", producer.getSkippedFrames());
  LOG_INFO("");
  LOG_INFO("--- Result ---");
  if (fps_meets_requirement) {
    LOG_INFO_FMT("✅ PASS: Decode FPS (%.2f) >= Target FPS (%.0f)", decode_fps,
                 target_fps);
  } else {
    LOG_WARN_FMT("⚠️  WARN: Decode FPS (%.2f) < Target FPS (%.0f)", decode_fps,
                 target_fps);
  }

  if (writer && save_count > 0) {
    int actual_written =
        writer->getWriteCount(); // 使用 BufferWriter 的实际写入计数
    LOG_INFO_FMT("📁 Saved data: %s", output_yuv);
    LOG_INFO_FMT("   Format: %s, Resolution: %dx%d, Frames: %d (BufferWriter "
                 "reports: %d)",
                 format_name.c_str(), actual_width, actual_height, save_count,
                 actual_written);

    // 计算预期文件大小
    size_t expected_size = actual_written * actual_width * actual_height * 3 /
                           2; // NV12: 1.5 bytes per pixel
    LOG_INFO_FMT("   Expected file size: %.2f MB (%zu bytes)",
                 expected_size / (1024.0 * 1024.0), expected_size);

    LOG_INFO("   You can verify with FFmpeg:");
    LOG_INFO_FMT("   ffplay -f rawvideo -pix_fmt %s -s %dx%d %s",
                 ffplay_format.c_str(), actual_width, actual_height,
                 output_yuv);
  }

  LOG_INFO("");
  LOG_INFO("--- BufferPool Final Stats ---");
  pool_sptr->printStats();
  LOG_INFO("╚═══════════════════════════════════════════════════════╝");

  // ========== PSNR 验证（如果启用） ==========
  // ⭐ 使用两个独立的 VideoProductionLine + BufferComparator 进行PSNR验证
  bool psnr_pass = true;
  if (config.enable_psnr) {
    // 使用 validate_psnr_streaming 同时运行硬件和软件解码器进行对比
    int max_frames = (save_count > 0) ? save_count : config.max_frames;
    if (max_frames == -1) {
      max_frames = 300; // 默认对比300帧
    }

    psnr_pass =
        validate_psnr_streaming(rtsp_url,      // source_video
                                actual_width,  // width（使用实际检测到的宽度）
                                actual_height, // height（使用实际检测到的高度）
                                getDecoderName(config), // decoder_name
                                max_frames,             // max_frames
                                config.min_psnr         // min_psnr
        );
  }

  // 最终判断：
  // - 如果启用PSNR：只看PSNR结果
  // - 如果未启用PSNR：看FPS是否达标
  bool final_result;
  if (config.enable_psnr) {
    final_result = psnr_pass; // PSNR模式下只看质量，不看FPS
  } else {
    final_result = fps_meets_requirement; // 性能模式下看FPS
  }

  return final_result ? 0 : -1;
}

/**
 * MP4视频解码测试主函数内部实现（支持H264/H265/MJPEG等多种编码格式）
 */
static int test_mp4_decode_single_impl(const char *video_path,
                                       const TestConfig &config) {
  // ========== 第1步：配置解码器 ==========
  LOG_INFO("[Step 1/8] Configuring decoder...");

  // 配置解码器输出格式 - 统一使用 NV12（CPU 可访问）
  DecoderConfigBuilder decoderBuilder;
  const char *decoder_name = getDecoderName(config);

  // ⭐ 检查是否使用软件解码器
  bool use_software = false;
  if (decoder_name && (strcmp(decoder_name, "software") == 0 ||
                       strcmp(decoder_name, "libavcodec") == 0 ||
                       strcmp(decoder_name, "sw") == 0)) {
    use_software = true;
    decoderBuilder.useSoftware();
    LOG_INFO("  Decoder: Software decoder (libavcodec)");
  } else {
    // ⭐ 参考 test.cpp 的 test_buffer_writer_format：保存YUV文件时需要传入 TacoConfig
    // 构建 TacoConfig，确保 YUV 格式正确配置（与 test_buffer_writer_yuv_formats 一致）
    auto tacoConfig =
        TacoConfigBuilder()
            .setYuvConfig("YUV420 8-bit NV12", "bt601")  // ⭐ 明确设置 YUV 格式（与 test.cpp 一致）
            .setChannels(true, false)                     // ⭐ 只启用 ch0，禁用 ch1（确保 PP0 输出 YUV）
            .setDecoderOutputResolution(config.width, config.height)  // ⭐ 设置输出分辨率（与 test.cpp 一致，可能影响UV平面访问）
            .build();
    
    if (strcmp(decoder_name, "h264_taco") == 0) {
      decoderBuilder.useTaco("h264", tacoConfig);  // ⭐ 传入 TacoConfig 以确保 YUV 格式正确
      LOG_INFO("  Decoder: TACO H.264 hardware decoder");
    } else if (strcmp(decoder_name, "hevc_taco") == 0) {
      decoderBuilder.useTaco("hevc", tacoConfig);  // ⭐ 传入 TacoConfig 以确保 YUV 格式正确
      LOG_INFO("  Decoder: TACO H.265/HEVC hardware decoder");
    } else if (strcmp(decoder_name, "mjpeg_taco") == 0 ||
               strcmp(decoder_name, "jpeg_taco") == 0) {
      decoderBuilder.useTaco("jpeg", tacoConfig);  // ⭐ 传入 TacoConfig 以确保 YUV 格式正确
      LOG_INFO("  Decoder: TACO MJPEG hardware decoder");
    } else {
      decoderBuilder.setDecoderName(decoder_name);
      LOG_INFO_FMT("  Decoder: %s", decoder_name);
    }
  }

  // ⭐ MP4文件：使用FFMPEG_DECODE WorkerType
  DataSourceConfigBuilder dataSourceBuilder;
  dataSourceBuilder.setPath(video_path);

  auto workerConfig = WorkerConfigBuilder()
                          .setDataSourceConfig(dataSourceBuilder.build())
                          .setDisplayConfig(DisplayConfigBuilder()
                                                .setDisplayResolution(
                                                    config.width, config.height)
                                                .setBitsPerPixel(32)
                                                .build())
                          .setDecoderConfig(decoderBuilder.build())
                          .setWorkerType(WorkerType::FFMPEG_DECODE)
                          .build();

  if (use_software) {
    LOG_INFO_FMT("✅ Decoder configured: Software decoder (libavcodec), %dx%d",
                 config.width, config.height);
  } else {
    LOG_INFO_FMT("✅ Decoder configured: %s, %dx%d, hardware acceleration",
                 getDecoderName(config), config.width, config.height);
  }

  // ========== 第2步：初始化显示设备（可选） ==========
  LOG_INFO("[Step 2/8] Initializing display device...");

  std::unique_ptr<LinuxFramebufferDevice> display;
  bool has_display = false;

  if (config.enable_display) {
    display     = std::make_unique<LinuxFramebufferDevice>();
    has_display = display->initialize(0);
    if (has_display) {
      LOG_INFO_FMT("✅ Display initialized: %dx%d @ %d bpp",
                   display->getWidth(), display->getHeight(),
                   display->getBitsPerPixel());
    } else {
      LOG_WARN("⚠️  Display not available, continuing without display");
    }
  } else {
    LOG_INFO("ℹ️  Display disabled by user");
  }

  // ========== 第3步：创建生产线 ==========
  LOG_INFO("[Step 3/8] Creating VideoProductionLine...");

  VideoProductionLine producer(false, // loop = false（不循环，解码一次）
                               config.threads, // thread_count
                               false           // enable_monitor = false
  );

  // 设置错误回调
  producer.setErrorCallback([](const std::string &error) {
    LOG_ERROR_FMT("Decode Error: %s", error.c_str());
    g_running = false;
  });

  LOG_INFO_FMT("✅ VideoProductionLine created (%d producer threads)",
               config.threads);

  // ========== 第4步：启动生产线 ==========
  LOG_INFO("[Step 4/8] Starting decode...");

  if (!producer.start(workerConfig)) {
    LOG_ERROR("❌ Failed to start VideoProductionLine");
    return -1;
  }

  LOG_INFO("✅ Decoding started");

  // ========== 第5步：获取 BufferPool ==========
  LOG_INFO("[Step 5/8] Getting BufferPool...");

  uint64_t pool_id = producer.getWorkingBufferPoolId();
  if (pool_id == 0) {
    LOG_ERROR("❌ No working BufferPool ID available");
    producer.stop();
    return -1;
  }

  auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
  auto pool_sptr = pool_weak.lock();
  if (!pool_sptr) {
    LOG_ERROR("❌ BufferPool not found or destroyed");
    producer.stop();
    return -1;
  }

  LOG_INFO_FMT("✅ BufferPool: '%s' (ID: %lu)", pool_sptr->getName().c_str(),
               pool_id);
  pool_sptr->printStats();

  // ========== 第5.5步：如果使用硬件解码器，同时启动软件解码器进行对比
  // ==========
  using productionline::io::BufferComparator;
  using productionline::io::CompareConfig;
  using productionline::io::FrameCompareResult;

  std::unique_ptr<MultiWorkerProductionLine> comparison_worker;
  std::shared_ptr<BufferPool> sw_pool_sptr;
  std::shared_ptr<BufferPool> hw_pool_sptr;
  std::unique_ptr<BufferComparator> comparator;
  bool enable_comparison = false;

  // ⭐ 如果当前使用硬件解码器且需要保存文件，同时启动软件解码器进行对比
  if (!use_software && config.save_frames != 0) {
    LOG_INFO(
        "[Step 5.5/8] Setting up hardware vs software decoder comparison...");

    // 创建 MultiWorkerProductionLine 配置
    MultiWorkerProductionLine::MultiWorkerConfig multi_config;
    MultiWorkerProductionLine::WorkerGroup group("hw_sw_comparison");

    // 生产者：Packet Recorder（录制 packet）
    MultiWorkerProductionLine::ProducerConfig producer_config;
    producer_config.producer_name = "packet_recorder";
    producer_config.worker_config =
        WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder().setPath(video_path).build())
            .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
            .build();
    group.producer_configs.push_back(producer_config);

    // 消费者1：硬件解码器（当前使用的解码器）
    MultiWorkerProductionLine::ConsumerConfig hw_consumer_config;
    hw_consumer_config.consumer_name = "hw_decoder";
    hw_consumer_config.worker_config =
        WorkerConfigBuilder()
            .setDecoderConfig(decoderBuilder.build())
            .setWorkerType(WorkerType::FFMPEG_DECODE)
            .build();
    group.consumer_configs.push_back(hw_consumer_config);

    // 消费者2：软件解码器
    DecoderConfigBuilder sw_decoder_builder;
    sw_decoder_builder.useSoftware();
    MultiWorkerProductionLine::ConsumerConfig sw_consumer_config;
    sw_consumer_config.consumer_name = "sw_decoder";
    sw_consumer_config.worker_config =
        WorkerConfigBuilder()
            .setDecoderConfig(sw_decoder_builder.build())
            .setWorkerType(WorkerType::FFMPEG_DECODE)
            .build();
    group.consumer_configs.push_back(sw_consumer_config);

    multi_config.groups.push_back(group);
    multi_config.thread_pool_size = 4;

    // 创建并启动 MultiWorkerProductionLine
    comparison_worker = std::make_unique<MultiWorkerProductionLine>(
        multi_config, false, 1, false);

    if (comparison_worker->start()) {
      // 获取两个解码器的 BufferPool
      uint64_t hw_pool_id =
          comparison_worker->getGroupConsumerBufferPoolId(0, 0);
      uint64_t sw_pool_id =
          comparison_worker->getGroupConsumerBufferPoolId(0, 1);

      if (hw_pool_id != 0 && sw_pool_id != 0) {
        auto hw_pool_weak =
            BufferPoolRegistry::getInstance().getPool(hw_pool_id);
        auto sw_pool_weak =
            BufferPoolRegistry::getInstance().getPool(sw_pool_id);
        hw_pool_sptr = hw_pool_weak.lock();
        sw_pool_sptr = sw_pool_weak.lock();

        if (hw_pool_sptr && sw_pool_sptr) {
          // 创建 BufferComparator
          comparator = std::make_unique<BufferComparator>();
          CompareConfig compare_config;
          compare_config.strategy                 = CompareConfig::AUTO_LAYERED;
          compare_config.format_strategy          = CompareConfig::AUTO;
          compare_config.quick_psnr_threshold     = 38.0;
          compare_config.quick_warn_threshold     = 35.0;
          compare_config.use_perceptual_weighting = true;
          compare_config.verbose                  = false; // 减少日志输出
          compare_config.save_report = false; // 不保存报告（主流程中）

          if (comparator->open(compare_config)) {
            enable_comparison = true;
            LOG_INFO("  ✅ Hardware vs Software decoder comparison enabled");
            LOG_INFO_FMT("  Hardware BufferPool: '%s' (ID: %lu)",
                         hw_pool_sptr->getName().c_str(), hw_pool_id);
            LOG_INFO_FMT("  Software BufferPool: '%s' (ID: %lu)",
                         sw_pool_sptr->getName().c_str(), sw_pool_id);

            // ⭐ 如果启用了对比，使用 comparison_worker 的硬件解码器 BufferPool
            // 替代主 producer 这样可以从同一个数据源获取 buffer 进行显示和保存
            pool_sptr = hw_pool_sptr; // 使用硬件解码器的 BufferPool
            LOG_INFO("  ℹ️  Using hardware decoder BufferPool from comparison "
                     "worker for display/save");
          } else {
            LOG_WARN(
                "  ⚠️  Failed to open BufferComparator, comparison disabled");
            comparator.reset();
            comparison_worker->stop();
            comparison_worker.reset();
          }
        } else {
          LOG_WARN("  ⚠️  Failed to get BufferPools, comparison disabled");
          comparison_worker->stop();
          comparison_worker.reset();
        }
      } else {
        LOG_WARN("  ⚠️  Failed to get BufferPool IDs, comparison disabled");
        comparison_worker->stop();
        comparison_worker.reset();
      }
    } else {
      LOG_WARN("  ⚠️  Failed to start MultiWorkerProductionLine, comparison "
               "disabled");
      comparison_worker.reset();
    }
  }

  // ========== 第6步：创建 BufferWriter ==========
  LOG_INFO("[Step 6/8] Creating BufferWriter...");

  using productionline::io::BufferWriter;
  std::unique_ptr<BufferWriter> writer;
  char output_yuv[256];
  AVPixelFormat output_format = AV_PIX_FMT_NONE;
  int actual_width            = config.width;
  int actual_height           = config.height;
  std::string format_name     = "NV12";
  const char *file_ext        = "yuv";
  std::string ffplay_format   = "nv12";
  int save_count              = 0; // 保存的帧数计数器
  int comparison_count        = 0; // 对比的帧数计数器

  if (config.save_frames != 0) {
    // 等待第一个Buffer以检测实际格式
    LOG_INFO("   Waiting for first buffer to detect format...");
    Buffer *first_buffer = pool_sptr->acquireFilled(true, 5000); // 5秒超时
    if (!first_buffer) {
      LOG_ERROR("❌ Failed to get first buffer (timeout)");
      producer.stop();
      return -1;
    }

    // 从Buffer元数据获取实际格式
    if (first_buffer->hasImageMetadata()) {
      output_format        = first_buffer->getImageFormat();
      actual_width         = first_buffer->getImageWidth();
      actual_height        = first_buffer->getImageHeight();
      const char *fmt_name = av_get_pix_fmt_name(output_format);
      format_name          = fmt_name ? fmt_name : "NV12";

      // 设置ffplay格式（NV12对应nv12）
      if (output_format == AV_PIX_FMT_NV12) {
        ffplay_format = "nv12";
      } else if (output_format == AV_PIX_FMT_YUV420P) {
        ffplay_format = "yuv420p";
      } else {
        ffplay_format = format_name;
      }

      LOG_INFO_FMT("   Detected format: %s (%dx%d)", format_name.c_str(),
                   actual_width, actual_height);
    } else {
      // 默认使用NV12
      output_format = AV_PIX_FMT_NV12;
      format_name   = "NV12";
      ffplay_format = "nv12";
      LOG_WARN("   Buffer has no metadata, using default NV12");
    }

    // 创建BufferWriter
    writer = std::make_unique<BufferWriter>();
    if (config.output_path) {
      snprintf(output_yuv, sizeof(output_yuv), "%s", config.output_path);
    } else {
      snprintf(output_yuv, sizeof(output_yuv), "/tmp/decoded_%dx%d_%ld.%s",
               actual_width, actual_height, time(nullptr), file_ext);
    }

    if (!writer->openRaw(output_yuv, output_format, actual_width, actual_height)) {
      LOG_ERROR_FMT("❌ Failed to open BufferWriter: %s", output_yuv);
      pool_sptr->releaseFilled(first_buffer);
      producer.stop();
      return -1;
    }

    LOG_INFO_FMT("✅ BufferWriter opened: %s (format: %s, %dx%d)", output_yuv,
                 format_name.c_str(), actual_width, actual_height);

    // 保存第一帧
    if (writer->write(first_buffer)) {
      LOG_INFO("   ✅ Saved first frame");
      save_count = 1; // 第一帧已保存，初始化计数器
    }
    pool_sptr->releaseFilled(first_buffer);
  } else {
    LOG_INFO("ℹ️  Output file disabled (save_frames = 0)");
  }

  // ========== 第7步：消费者循环（解码+显示+保存） ==========
  LOG_INFO("[Step 7/8] Consuming decoded frames...");
  LOG_INFO("Press Ctrl+C to stop early");
  LOG_INFO("");

  int frame_count   = 0;
  int display_count = 0;
  // save_count 已在上面声明，如果 writer 已打开且第一帧已保存，则保持为
  // 1，否则为 0
  if (!(writer && writer->isOpen())) {
    save_count = 0; // writer 未打开，重置为 0
  }
  int save_limit = (config.save_frames == -1) ? INT32_MAX : config.save_frames;
  int max_frames_limit =
      (config.max_frames == -1) ? INT32_MAX : config.max_frames;

  // ⭐ PSNR统计：记录每帧的PSNR值（参考附件文件的方法）
  std::vector<double> psnr_y_values;
  std::vector<double> psnr_u_values;
  std::vector<double> psnr_v_values;
  std::vector<double> psnr_avg_values;

  auto start_time       = std::chrono::steady_clock::now();
  auto last_report_time = start_time;

  // ⭐ 参考 test.cpp 的消费者循环逻辑：使用超时计数，让视频自然结束
  int timeout_count = 0;
  const int MAX_TIMEOUT = 10; // ⭐ 与 test.cpp 一致：超时10次后退出
  
  while (g_running && frame_count < max_frames_limit) {
    // 从 BufferPool 获取已解码的 Buffer
    Buffer *buffer = pool_sptr->acquireFilled(true, 100); // ⭐ 与 test.cpp 一致：100ms 超时

    if (!buffer) {
      // ⭐ 超时处理：使用超时计数，与 test.cpp 一致
      timeout_count++;
      if (timeout_count >= MAX_TIMEOUT) {
        LOG_INFO("Video finished, stopping...");
        break;
      }
      continue; // 超时但未达到最大次数，继续等待
    }
    
    // ⭐ 成功获取 buffer，重置超时计数
    timeout_count = 0;

    // 显示到屏幕（如果启用）
    if (has_display) {
      display->waitVerticalSync();
      if (display->displayBufferByDMA(buffer)) {
        display_count++;
      } else {
        // DMA 失败，回退到普通显示
        display->displayFilledFramebuffer(buffer);
        display_count++;
      }
    }

    // ⭐ 在保存之前进行软硬解码对比（如果启用）
    // 注意：buffer 来自硬件解码器（如果启用了对比，pool_sptr == hw_pool_sptr）
    if (enable_comparison && comparator && sw_pool_sptr) {
      // 从软件解码器的 BufferPool 获取 Buffer（硬件解码器的 buffer
      // 已经在主循环中获取）
      // ⭐ 增加超时时间，确保能获取到软件解码器的buffer进行对比
      Buffer *sw_buffer = sw_pool_sptr->acquireFilled(true, 1000); // 1秒超时

      if (sw_buffer) {
        // 使用 BufferComparator
        // 进行对比（软件解码器作为参考，硬件解码器作为测试）
        FrameCompareResult result = comparator->compare(sw_buffer, buffer);
        comparison_count++;

        // ⭐ 记录PSNR值（用于统计）
        if (result.psnr_y > 0.0) {
          psnr_y_values.push_back(result.psnr_y);
          psnr_u_values.push_back(result.psnr_u);
          psnr_v_values.push_back(result.psnr_v);
          psnr_avg_values.push_back(result.psnr_avg);
        }

        // ⭐ 简化输出：只在失败时输出（减少输出频率）
        if (!result.passed && result.psnr_y > 0.0 && frame_count % 10 == 0) {
          LOG_WARN_FMT("Frame %3d: PSNR-Y=%.2f dB %s",
                       frame_count, result.psnr_y,
                       result.level == FrameCompareResult::FAIL ? "❌ FAIL" : "⚠️ WARN");
        }

        // 释放软件解码器的 Buffer
        sw_pool_sptr->releaseFilled(sw_buffer);
      }
      // 如果无法获取软件解码器的 buffer，跳过对比（硬件解码器的 buffer
      // 继续用于显示和保存）
    }

    // 保存到文件（使用BufferWriter）- 在对比之后保存
    // ⭐ 参考 test.cpp 的 test_buffer_writer_format 函数：直接调用 writer->write(buffer)
    if (writer && save_count < save_limit) {
      if (writer->write(buffer)) {
        save_count++;
      } else {
        LOG_WARN_FMT("Failed to write frame %d to file", frame_count);
      }
    }

    // 归还 Buffer
    pool_sptr->releaseFilled(buffer);
    frame_count++;

    // 每 60 帧（约1秒）打印一次进度
    auto now     = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - last_report_time)
                       .count();

    if (elapsed >= 1000) { // 每秒报告一次
      double current_fps =
          enable_comparison ? 0.0
                            : producer.getAverageFPS(); // 对比模式下不显示FPS
      if (enable_comparison) {
        // ⭐ 简化PSNR输出：只显示关键统计信息（减少输出频率）
        if (frame_count % 60 == 0) {
          double avg_psnr_y = psnr_y_values.empty()
                                  ? 0.0
                                  : std::accumulate(psnr_y_values.begin(),
                                                    psnr_y_values.end(), 0.0) /
                                        psnr_y_values.size();

          LOG_INFO_FMT("Progress: %d frames | Saved: %d | Compared: %d | "
                       "Avg PSNR-Y=%.2f dB | Passed: %d, Failed: %d",
                       frame_count, save_count, comparison_count,
                       avg_psnr_y, comparator->getPassedCount(),
                       comparator->getFailedCount());
        }
      } else {
        LOG_INFO_FMT(
            "Progress: %d frames | FPS: %.1f | Display: %d | Saved: %d",
            frame_count, current_fps, display_count, save_count);
      }
      last_report_time = now;
    }
  }

  // 排空剩余的 Buffer
  LOG_INFO("Draining remaining buffers...");
  Buffer *remaining = nullptr;
  int drained       = 0;
  while ((remaining = pool_sptr->acquireFilled(false, 0)) != nullptr) {
    if (has_display) {
      display->waitVerticalSync();
      display->displayBufferByDMA(remaining);
      display_count++;
    }

    // ⭐ 在保存之前进行软硬解码对比（如果启用）
    if (enable_comparison && comparator && sw_pool_sptr) {
      // ⭐ 增加超时时间，确保能获取到软件解码器的buffer进行对比
      Buffer *sw_buffer = sw_pool_sptr->acquireFilled(true, 1000); // 1秒超时
      if (sw_buffer) {
        FrameCompareResult result = comparator->compare(sw_buffer, remaining);
        comparison_count++;

        // ⭐ 记录PSNR值（用于统计）
        if (result.psnr_y > 0.0) {
          psnr_y_values.push_back(result.psnr_y);
          psnr_u_values.push_back(result.psnr_u);
          psnr_v_values.push_back(result.psnr_v);
          psnr_avg_values.push_back(result.psnr_avg);
        }

        // Reduced output for remaining frames
        if (!result.passed && result.psnr_y > 0.0 && frame_count % 10 == 0) {
          LOG_WARN_FMT("Remaining frame %d comparison: PSNR-Y=%.2f dB, %s",
                       frame_count, result.psnr_y,
                       result.level == FrameCompareResult::FAIL ? "FAIL"
                                                                : "WARN");
        }
        sw_pool_sptr->releaseFilled(sw_buffer);
      }
    }

    // ⭐ 参考 test.cpp 的 test_buffer_writer_format 函数：直接调用 writer->write(buffer)
    if (writer && save_count < save_limit) {
      if (writer->write(remaining)) {
        save_count++;
      } else {
        LOG_WARN_FMT("Failed to write remaining frame %d to file", frame_count);
      }
    }
    pool_sptr->releaseFilled(remaining);
    frame_count++;
    drained++;
  }
  if (drained > 0) {
    LOG_INFO_FMT("Drained %d remaining buffers", drained);
  }

  // 关闭对比器（如果启用）
  if (comparator) {
    comparator->close();
    if (comparison_count > 0) {
      LOG_INFO_FMT("✅ BufferComparator closed: %d frames compared",
                   comparison_count);

      // ⭐ 打印详细的PSNR统计（参考附件文件的方法）
      LOG_INFO("\n═══════════════════════════════════════════════════════");
      LOG_INFO("  Decoder Comparison Results");
      LOG_INFO("═══════════════════════════════════════════════════════");
      comparator->printSummary();
      LOG_INFO("═══════════════════════════════════════════════════════\n");

      // ⭐ 打印简化的PSNR统计
      if (!psnr_y_values.empty()) {
        // 计算平均值
        double avg_psnr_y =
            std::accumulate(psnr_y_values.begin(), psnr_y_values.end(), 0.0) /
            psnr_y_values.size();
        double avg_psnr_avg = std::accumulate(psnr_avg_values.begin(),
                                              psnr_avg_values.end(), 0.0) /
                              psnr_avg_values.size();

        // 计算最小值和最大值
        auto minmax_y =
            std::minmax_element(psnr_y_values.begin(), psnr_y_values.end());
        auto minmax_avg =
            std::minmax_element(psnr_avg_values.begin(), psnr_avg_values.end());

        double min_psnr_y   = *minmax_y.first;
        double max_psnr_y   = *minmax_y.second;
        double min_psnr_avg = *minmax_avg.first;
        double max_psnr_avg = *minmax_avg.second;

        LOG_INFO("  PSNR Statistics (Hardware vs Software):");
        LOG_INFO_FMT("    Average: Y=%.2f dB (avg=%.2f dB)", avg_psnr_y, avg_psnr_avg);
        LOG_INFO_FMT("    Range Y:  [%.2f, %.2f] dB", min_psnr_y, max_psnr_y);
        LOG_INFO_FMT("    Range Avg: [%.2f, %.2f] dB", min_psnr_avg, max_psnr_avg);
        LOG_INFO("");
        LOG_INFO("  Quality Assessment:");
        LOG_INFO_FMT("    Passed: %d ✅ (%.1f%%)", comparator->getPassedCount(),
                     100.0 * comparator->getPassedCount() / comparison_count);
        LOG_INFO_FMT("    Failed: %d ❌ (%.1f%%)", comparator->getFailedCount(),
                     100.0 * comparator->getFailedCount() / comparison_count);
        LOG_INFO("");

        // 质量评级
        if (avg_psnr_avg >= 38.0) {
          LOG_INFO("  ✅ Overall Quality: EXCELLENT (visually lossless)");
        } else if (avg_psnr_avg >= 35.0) {
          LOG_INFO("  ⚠️  Overall Quality: GOOD (minor differences)");
        } else {
          LOG_INFO("  ❌ Overall Quality: POOR (visible artifacts)");
        }
      }
    }
  }

  if (comparison_worker) {
    comparison_worker->stop();
  }

  // 停止主 producer（如果未使用 comparison_worker）
  if (!enable_comparison) {
    producer.stop();
  }

  if (writer) {
    writer->close();
    LOG_INFO_FMT("✅ BufferWriter closed: %d frames written",
                 writer->getWriteCount());
  }

  auto end_time       = std::chrono::steady_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            end_time - start_time)
                            .count();

  LOG_INFO("✅ Consuming completed");

  // ========== 第8步：停止生产线并输出统计 ==========
  LOG_INFO("[Step 8/8] Stopping and generating report...");

  producer.stop();

  // 计算性能指标
  double decode_fps          = producer.getAverageFPS();
  double realtime_fps        = (frame_count * 1000.0) / total_duration;
  double target_fps          = config.fps;
  bool fps_meets_requirement = decode_fps >= target_fps;
  if (config.enable_psnr) {
    fps_meets_requirement = true; // PSNR模式下不检查FPS
  }

  // ========== 输出测试报告 ==========
  LOG_INFO("");
  LOG_INFO("╔═══════════════════════════════════════════════════════╗");
  LOG_INFO("║  Test Report: Video Decode                            ║");
  LOG_INFO("╚═══════════════════════════════════════════════════════╝");
  LOG_INFO_FMT("Video file: %s", video_path);
  LOG_INFO_FMT("Codec: %s", getCodecName(config).c_str());
  LOG_INFO_FMT("Decoder: %s (hardware)", getDecoderName(config));
  LOG_INFO_FMT("Resolution: %dx%d", config.width, config.height);
  LOG_INFO_FMT("Target FPS: %.0f", target_fps);
  LOG_INFO("");
  LOG_INFO("--- Performance Metrics ---");
  LOG_INFO_FMT("Total frames decoded: %d", frame_count);
  LOG_INFO_FMT("Frames displayed: %d", display_count);
  LOG_INFO_FMT("Frames saved: %d", save_count);
  LOG_INFO_FMT("Total time: %.2f seconds", total_duration / 1000.0);
  LOG_INFO_FMT("Decode FPS (producer): %.2f", decode_fps);
  LOG_INFO_FMT("Realtime FPS (overall): %.2f", realtime_fps);
  LOG_INFO_FMT("Frames produced: %d", producer.getProducedFrames());
  LOG_INFO_FMT("Frames skipped: %d", producer.getSkippedFrames());
  LOG_INFO("");
  LOG_INFO("--- Result ---");
  if (config.enable_psnr) {
    LOG_INFO(
        "ℹ️  FPS check skipped (PSNR mode prioritizes accuracy over speed)");
  } else if (fps_meets_requirement) {
    LOG_INFO_FMT("✅ PASS: Decode FPS (%.2f) >= Target FPS (%.0f)", decode_fps,
                 target_fps);
  } else {
    LOG_WARN_FMT("⚠️  WARN: Decode FPS (%.2f) < Target FPS (%.0f)", decode_fps,
                 target_fps);
  }

  if (writer && save_count > 0) {
    int actual_written =
        writer->getWriteCount(); // 使用 BufferWriter 的实际写入计数
    LOG_INFO_FMT("📁 Saved data: %s", output_yuv);
    LOG_INFO_FMT("   Format: %s, Resolution: %dx%d, Frames: %d (BufferWriter "
                 "reports: %d)",
                 format_name.c_str(), actual_width, actual_height, save_count,
                 actual_written);

    // 计算预期文件大小
    size_t expected_size = actual_written * actual_width * actual_height * 3 /
                           2; // NV12: 1.5 bytes per pixel
    LOG_INFO_FMT("   Expected file size: %.2f MB (%zu bytes)",
                 expected_size / (1024.0 * 1024.0), expected_size);

    LOG_INFO("   You can verify with FFmpeg:");
    LOG_INFO_FMT("   ffplay -f rawvideo -pix_fmt %s -s %dx%d %s",
                 ffplay_format.c_str(), actual_width, actual_height,
                 output_yuv);
  }

  LOG_INFO("");
  LOG_INFO("--- BufferPool Final Stats ---");
  pool_sptr->printStats();
  LOG_INFO("╚═══════════════════════════════════════════════════════╝");

  // ========== PSNR 验证（如果启用） ==========
  // ⭐ 使用 MultiWorkerProductionLine + BufferComparator 进行PSNR验证
  bool psnr_pass = true;
  if (config.enable_psnr) {
    // 使用 MultiWorkerProductionLine 同时运行硬件和软件解码器进行对比
    // 不再需要先保存硬件解码输出到文件
    int max_frames = (save_count > 0) ? save_count : config.max_frames;
    if (max_frames == -1) {
      max_frames = 300; // 默认对比300帧
    }

    psnr_pass =
        validate_psnr_streaming(video_path,    // source_video
                                actual_width,  // width（使用实际检测到的宽度）
                                actual_height, // height（使用实际检测到的高度）
                                getDecoderName(config), // decoder_name
                                max_frames,             // max_frames
                                config.min_psnr         // min_psnr
        );
  }

  // 最终判断：
  // - 如果启用PSNR：只看PSNR结果
  // - 如果未启用PSNR：看FPS是否达标
  bool final_result;
  if (config.enable_psnr) {
    final_result = psnr_pass; // PSNR模式下只看质量，不看FPS
  } else {
    final_result = fps_meets_requirement; // 性能模式下看FPS
  }

  return final_result ? 0 : -1;
}

// ========== 不同参数的视频文件解码测试用例 ==========
// 说明：
//   - 针对不同编码格式（H.264 / H.265 / MJPEG），按分辨率 / 帧率 / profile
//   拆分为独立用例
//   - 建议命令行使用方式：
//       ./test_mp4_decode -m <test_name> /path/to/<对应视频文件>.mp4
//   - 例如：
//       ./test_mp4_decode -m mp4_decode_h264_1920x1080_60_high
//       test_h264_1920x1080_60fps_high.mp4
//       ./test_mp4_decode -m mp4_decode_h265_3840x2160_30_main
//       test_h265_3840x2160_30fps_main.mp4
//       ./test_mp4_decode -m mp4_decode_mjpeg_640x480_60
//       test_mjpeg_640x480_60fps_none.mp4

// ---------------- H.264 系列 ----------------

static int test_mp4_decode_h264_128x128_30_main(const char *video_path) {
  return run_decode_test_with_params(video_path, 128, 128, "h264_taco", 0, 30.0,
                                     "main", "h264_128x128_30_main");
}

static int test_mp4_decode_h264_320x240_30_high(const char *video_path) {
  return run_decode_test_with_params(video_path, 320, 240, "h264_taco", 0, 30.0,
                                     "high", "h264_320x240_30_high");
}

static int test_mp4_decode_h264_640x480_30_main(const char *video_path) {
  return run_decode_test_with_params(video_path, 640, 480, "h264_taco", 0, 30.0,
                                     "main", "h264_640x480_30_main");
}

static int test_mp4_decode_h264_640x480_60_high(const char *video_path) {
  return run_decode_test_with_params(video_path, 640, 480, "h264_taco", 4, 60.0,
                                     "high", "h264_640x480_60_high");
}

static int test_mp4_decode_h264_1280x720_30_high(const char *video_path) {
  return run_decode_test_with_params(video_path, 1280, 720, "h264_taco", 0,
                                     30.0, "high", "h264_1280x720_30_high");
}

static int test_mp4_decode_h264_1920x1080_30_high(const char *video_path) {
  return run_decode_test_with_params(video_path, 1920, 1080, "h264_taco", 0,
                                     30.0, "high", "h264_1920x1080_30_high");
}

static int test_mp4_decode_h264_1920x1080_60_high(const char *video_path) {
  return run_decode_test_with_params(video_path, 1920, 1080, "h264_taco", 4,
                                     60.0, "high", "h264_1920x1080_60_high");
}

static int test_mp4_decode_h264_2560x1440_30_high(const char *video_path) {
  return run_decode_test_with_params(video_path, 2560, 1440, "h264_taco", 4,
                                     30.0, "high", "h264_2560x1440_30_high");
}

static int test_mp4_decode_h264_3840x2160_30_high(const char *video_path) {
  return run_decode_test_with_params(video_path, 3840, 2160, "h264_taco", 4,
                                     30.0, "high", "h264_3840x2160_30_high");
}

// ---------------- H.265 系列 ----------------

static int test_mp4_decode_h265_128x128_30_main(const char *video_path) {
  return run_decode_test_with_params(video_path, 128, 128, "hevc_taco", 0, 30.0,
                                     "main", "h265_128x128_30_main");
}

static int test_mp4_decode_h265_320x240_30_main(const char *video_path) {
  return run_decode_test_with_params(video_path, 320, 240, "hevc_taco", 0, 30.0,
                                     "main", "h265_320x240_30_main");
}

static int test_mp4_decode_h265_640x480_30_main(const char *video_path) {
  return run_decode_test_with_params(video_path, 640, 480, "hevc_taco", 0, 30.0,
                                     "main", "h265_640x480_30_main");
}

static int test_mp4_decode_h265_640x480_60_main(const char *video_path) {
  return run_decode_test_with_params(video_path, 640, 480, "hevc_taco", 4, 60.0,
                                     "main", "h265_640x480_60_main");
}

static int test_mp4_decode_h265_1280x720_30_main(const char *video_path) {
  return run_decode_test_with_params(video_path, 1280, 720, "hevc_taco", 0,
                                     30.0, "main", "h265_1280x720_30_main");
}

static int test_mp4_decode_h265_1920x1080_30_main(const char *video_path) {
  return run_decode_test_with_params(video_path, 1920, 1080, "hevc_taco", 0,
                                     30.0, "main", "h265_1920x1080_30_main");
}

static int test_mp4_decode_h265_1920x1080_60_main(const char *video_path) {
  return run_decode_test_with_params(video_path, 1920, 1080, "hevc_taco", 4,
                                     60.0, "main", "h265_1920x1080_60_main");
}

static int test_mp4_decode_h265_2560x1440_30_main(const char *video_path) {
  return run_decode_test_with_params(video_path, 2560, 1440, "hevc_taco", 4,
                                     30.0, "main", "h265_2560x1440_30_main");
}

static int test_mp4_decode_h265_3840x2160_30_main(const char *video_path) {
  return run_decode_test_with_params(video_path, 3840, 2160, "hevc_taco", 4,
                                     30.0, "main", "h265_3840x2160_30_main");
}

// ---------------- MJPEG 系列 ----------------

static int test_mp4_decode_mjpeg_128x128_30(const char *video_path) {
  return run_decode_test_with_params(video_path, 128, 128, "mjpeg_taco", 0,
                                     30.0, "none", "mjpeg_128x128_30");
}

static int test_mp4_decode_mjpeg_320x240_30(const char *video_path) {
  return run_decode_test_with_params(video_path, 320, 240, "mjpeg_taco", 0,
                                     30.0, "none", "mjpeg_320x240_30");
}

static int test_mp4_decode_mjpeg_640x480_30(const char *video_path) {
  return run_decode_test_with_params(video_path, 640, 480, "mjpeg_taco", 0,
                                     30.0, "none", "mjpeg_640x480_30");
}

static int test_mp4_decode_mjpeg_640x480_60(const char *video_path) {
  return run_decode_test_with_params(video_path, 640, 480, "mjpeg_taco", 4,
                                     60.0, "none", "mjpeg_640x480_60");
}

static int test_mp4_decode_mjpeg_1280x720_30(const char *video_path) {
  return run_decode_test_with_params(video_path, 1280, 720, "mjpeg_taco", 0,
                                     30.0, "none", "mjpeg_1280x720_30");
}

static int test_mp4_decode_mjpeg_1920x1080_30(const char *video_path) {
  return run_decode_test_with_params(video_path, 1920, 1080, "mjpeg_taco", 0,
                                     30.0, "none", "mjpeg_1920x1080_30");
}

static int test_mp4_decode_mjpeg_1920x1080_60(const char *video_path) {
  return run_decode_test_with_params(video_path, 1920, 1080, "mjpeg_taco", 4,
                                     60.0, "none", "mjpeg_1920x1080_60");
}

static int test_mp4_decode_mjpeg_2560x1440_30(const char *video_path) {
  return run_decode_test_with_params(video_path, 2560, 1440, "mjpeg_taco", 4,
                                     30.0, "none", "mjpeg_2560x1440_30");
}

static int test_mp4_decode_mjpeg_3840x2160_30(const char *video_path) {
  return run_decode_test_with_params(video_path, 3840, 2160, "mjpeg_taco", 4,
                                     30.0, "none", "mjpeg_3840x2160_30");
}

// ---------------- 通用测试用例（保持向后兼容） ----------------

/**
 * @brief MP4视频解码测试（H264，通用）
 */
static int test_mp4_decode_h264(const char *video_path) {
  TestConfig config = create_test_config_from_env(CodecType::H264);
  return run_decode_test_with_params(
      video_path, config.width, config.height, getDecoderName(config),
      config.threads, config.fps, config.profile, "h264_generic");
}

/**
 * @brief MP4视频解码测试（H265，通用）
 */
static int test_mp4_decode_h265(const char *video_path) {
  TestConfig config = create_test_config_from_env(CodecType::H265);
  return run_decode_test_with_params(
      video_path, config.width, config.height, getDecoderName(config),
      config.threads, config.fps, config.profile, "h265_generic");
}

/**
 * @brief MP4视频解码测试（MJPEG，通用）
 */
static int test_mp4_decode_mjpeg(const char *video_path) {
  TestConfig config = create_test_config_from_env(CodecType::MJPEG);
  return run_decode_test_with_params(video_path, config.width, config.height,
                                     getDecoderName(config), config.threads,
                                     config.fps, "none", "mjpeg_generic");
}

/**
 * @brief MP4视频解码测试（通用，自动检测codec）
 */
static int test_mp4_decode(const char *video_path) {
  TestConfig config = create_test_config_from_env(CodecType::H264);
  return run_decode_test_with_params(video_path, config.width, config.height,
                                     getDecoderName(config), config.threads,
                                     config.fps, config.profile, "auto_detect");
}

/**
 * @brief MP4视频解码测试（软件解码器，H.264）
 */
static int test_mp4_decode_sw_h264_1920x1080_30(const char *video_path) {
  return run_decode_test_with_params(video_path, 1920, 1080, "software", 0,
                                     30.0, "high", "sw_h264_1920x1080_30");
}

/**
 * @brief MP4视频解码测试（软件解码器，H.265）
 */
static int test_mp4_decode_sw_h265_1920x1080_30(const char *video_path) {
  return run_decode_test_with_params(video_path, 1920, 1080, "software", 0,
                                     30.0, "main", "sw_h265_1920x1080_30");
}

// ========== 测试用例注册（仿照 test_mp4_azh.cpp 结构） ==========

// H.264
REGISTER_TEST(mp4_decode_h264_128x128_30_main,
              "H.264 128x128 30fps main profile",
              test_mp4_decode_h264_128x128_30_main);
REGISTER_TEST(mp4_decode_h264_320x240_30_high,
              "H.264 320x240 30fps high profile",
              test_mp4_decode_h264_320x240_30_high);
REGISTER_TEST(mp4_decode_h264_640x480_30_main,
              "H.264 640x480 30fps main profile",
              test_mp4_decode_h264_640x480_30_main);
REGISTER_TEST(mp4_decode_h264_640x480_60_high,
              "H.264 640x480 60fps high profile",
              test_mp4_decode_h264_640x480_60_high);
REGISTER_TEST(mp4_decode_h264_1280x720_30_high,
              "H.264 1280x720 30fps high profile",
              test_mp4_decode_h264_1280x720_30_high);
REGISTER_TEST(mp4_decode_h264_1920x1080_30_high,
              "H.264 1920x1080 30fps high profile",
              test_mp4_decode_h264_1920x1080_30_high);
REGISTER_TEST(mp4_decode_h264_1920x1080_60_high,
              "H.264 1920x1080 60fps high profile",
              test_mp4_decode_h264_1920x1080_60_high);
REGISTER_TEST(mp4_decode_h264_2560x1440_30_high,
              "H.264 2560x1440 30fps high profile",
              test_mp4_decode_h264_2560x1440_30_high);
REGISTER_TEST(mp4_decode_h264_3840x2160_30_high,
              "H.264 3840x2160 30fps high profile",
              test_mp4_decode_h264_3840x2160_30_high);

// H.265
REGISTER_TEST(mp4_decode_h265_128x128_30_main,
              "H.265 128x128 30fps main profile",
              test_mp4_decode_h265_128x128_30_main);
REGISTER_TEST(mp4_decode_h265_320x240_30_main,
              "H.265 320x240 30fps main profile",
              test_mp4_decode_h265_320x240_30_main);
REGISTER_TEST(mp4_decode_h265_640x480_30_main,
              "H.265 640x480 30fps main profile",
              test_mp4_decode_h265_640x480_30_main);
REGISTER_TEST(mp4_decode_h265_640x480_60_main,
              "H.265 640x480 60fps main profile",
              test_mp4_decode_h265_640x480_60_main);
REGISTER_TEST(mp4_decode_h265_1280x720_30_main,
              "H.265 1280x720 30fps main profile",
              test_mp4_decode_h265_1280x720_30_main);
REGISTER_TEST(mp4_decode_h265_1920x1080_30_main,
              "H.265 1920x1080 30fps main profile",
              test_mp4_decode_h265_1920x1080_30_main);
REGISTER_TEST(mp4_decode_h265_1920x1080_60_main,
              "H.265 1920x1080 60fps main profile",
              test_mp4_decode_h265_1920x1080_60_main);
REGISTER_TEST(mp4_decode_h265_2560x1440_30_main,
              "H.265 2560x1440 30fps main profile",
              test_mp4_decode_h265_2560x1440_30_main);
REGISTER_TEST(mp4_decode_h265_3840x2160_30_main,
              "H.265 3840x2160 30fps main profile",
              test_mp4_decode_h265_3840x2160_30_main);

// MJPEG
REGISTER_TEST(mp4_decode_mjpeg_128x128_30, "MJPEG 128x128 30fps",
              test_mp4_decode_mjpeg_128x128_30);
REGISTER_TEST(mp4_decode_mjpeg_320x240_30, "MJPEG 320x240 30fps",
              test_mp4_decode_mjpeg_320x240_30);
REGISTER_TEST(mp4_decode_mjpeg_640x480_30, "MJPEG 640x480 30fps",
              test_mp4_decode_mjpeg_640x480_30);
REGISTER_TEST(mp4_decode_mjpeg_640x480_60, "MJPEG 640x480 60fps",
              test_mp4_decode_mjpeg_640x480_60);
REGISTER_TEST(mp4_decode_mjpeg_1280x720_30, "MJPEG 1280x720 30fps",
              test_mp4_decode_mjpeg_1280x720_30);
REGISTER_TEST(mp4_decode_mjpeg_1920x1080_30, "MJPEG 1920x1080 30fps",
              test_mp4_decode_mjpeg_1920x1080_30);
REGISTER_TEST(mp4_decode_mjpeg_1920x1080_60, "MJPEG 1920x1080 60fps",
              test_mp4_decode_mjpeg_1920x1080_60);
REGISTER_TEST(mp4_decode_mjpeg_2560x1440_30, "MJPEG 2560x1440 30fps",
              test_mp4_decode_mjpeg_2560x1440_30);
REGISTER_TEST(mp4_decode_mjpeg_3840x2160_30, "MJPEG 3840x2160 30fps",
              test_mp4_decode_mjpeg_3840x2160_30);

// 软件解码器测试用例
REGISTER_TEST(mp4_decode_sw_h264_1920x1080_30,
              "Software decoder H.264 1920x1080 30fps",
              test_mp4_decode_sw_h264_1920x1080_30);
REGISTER_TEST(mp4_decode_sw_h265_1920x1080_30,
              "Software decoder H.265 1920x1080 30fps",
              test_mp4_decode_sw_h265_1920x1080_30);

// 通用测试用例（保持向后兼容）
REGISTER_TEST(mp4_decode, "MP4 video decode test (auto-detect codec)",
              test_mp4_decode);
REGISTER_TEST(mp4_decode_h264, "MP4 video decode test (H264)",
              test_mp4_decode_h264);
REGISTER_TEST(mp4_decode_h265, "MP4 video decode test (H265/HEVC)",
              test_mp4_decode_h265);
REGISTER_TEST(mp4_decode_mjpeg, "MP4 video decode test (MJPEG)",
              test_mp4_decode_mjpeg);

/**
 * @brief 信号处理函数（支持RTSP中断）
 */
static void signal_handler(int sig) {
  g_running = false;
  if (sig == SIGINT || sig == SIGTERM) {
    g_rtsp_interrupted = true;
    RtspPacketSource::requestInterrupt();
    LOG_INFO("\n⚠️  收到中断信号，正在停止...");
  }
}

/**
 * 主函数
 */
int main(int argc, char *argv[]) {
  // 初始化日志系统
  INIT_LOGGER();

  // 注册信号处理（支持RTSP中断）
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  g_rtsp_interrupted = false;
  RtspPacketSource::clearInterrupt();

  // 使用测试框架主函数
  TEST_MAIN(argc, argv);
}
