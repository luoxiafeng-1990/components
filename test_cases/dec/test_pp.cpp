/**
 * PP Post-Processing Test Program
 * 
 * 测试 TACO 硬件解码器的后处理功能（PP0和PP1）
 * 
 * 功能：
 * - 单PP测试：测试单个后处理功能（裁剪或缩放）
 * - 多PP测试：测试多个后处理功能组合（裁剪+缩放）
 * - 裁剪测试：测试视频裁剪功能
 * - 使用组件接口完全实现，不添加额外逻辑
 * 
 * 运行命令：
 *   ./test_pp -l  # 列出所有测试用例
 *   ./test_pp -m single_pp_crop video.mp4 --crop 100,100,800,600  # 单PP裁剪测试
 *   ./test_pp -m single_pp_scale video.mp4 --scale 1280x720  # 单PP缩放测试
 *   ./test_pp -m multi_pp video.mp4 --crop 100,100,800,600 --scale 1280x720  # 多PP测试
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
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <functional>

// Components 头文件
#include "productionline/VideoProductionLine.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/worker/WorkerBase.hpp"  // BufferPoolType
#include "productionline/io/BufferWriter.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "display/LinuxFramebufferDevice.hpp"
#include "monitor/PerformanceMonitor.hpp"
#include "common/Logger.hpp"
#include "framework/TestMacros.hpp"

// FFmpeg头文件
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>  // av_get_pix_fmt_name() 函数
}

// 全局标志，用于处理 Ctrl+C 退出
static volatile bool g_running = true;

/**
 * @brief 快速检测视频文件的编解码器类型
 * @param video_path 视频文件路径
 * @return 编解码器名称（"h264", "h265", "mjpeg", nullptr=未知）
 */
static const char* detect_video_codec(const char* video_path) {
    if (!video_path) {
        return nullptr;
    }
    
    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, video_path, nullptr, nullptr);
    if (ret < 0) {
        LOG_WARN_FMT("Failed to open video file for codec detection: %s", video_path);
        return nullptr;
    }
    
    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        avformat_close_input(&fmt_ctx);
        return nullptr;
    }
    
    // 查找视频流
    int video_stream_index = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }
    
    const char* codec_name = nullptr;
    if (video_stream_index >= 0) {
        AVCodecParameters* codecpar = fmt_ctx->streams[video_stream_index]->codecpar;
        AVCodecID codec_id = codecpar->codec_id;
        
        if (codec_id == AV_CODEC_ID_H264) {
            codec_name = "h264";
        } else if (codec_id == AV_CODEC_ID_HEVC) {
            codec_name = "h265";
        } else if (codec_id == AV_CODEC_ID_MJPEG) {
            codec_name = "mjpeg";
        }
    }
    
    avformat_close_input(&fmt_ctx);
    return codec_name;
}

/**
 * @brief 解析裁剪参数 "x,y,width,height"
 */
__attribute__((unused))
static bool parse_crop(const char* crop_str, int& x, int& y, int& width, int& height) {
    if (!crop_str) {
        return false;
    }
    
    char* str = strdup(crop_str);
    char* token = strtok(str, ",");
    if (!token) {
        free(str);
        return false;
    }
    x = atoi(token);
    
    token = strtok(nullptr, ",");
    if (!token) {
        free(str);
        return false;
    }
    y = atoi(token);
    
    token = strtok(nullptr, ",");
    if (!token) {
        free(str);
        return false;
    }
    width = atoi(token);
    
    token = strtok(nullptr, ",");
    if (!token) {
        free(str);
        return false;
    }
    height = atoi(token);
    
    free(str);
    return (x >= 0 && y >= 0 && width > 0 && height > 0);
}

/**
 * @brief 解析分辨率参数 "widthxheight"
 */
__attribute__((unused))
static bool parse_resolution(const char* res_str, int& width, int& height) {
    if (!res_str) {
        return false;
    }
    
    char* str = strdup(res_str);
    char* token = strtok(str, "x");
    if (!token) {
        free(str);
        return false;
    }
    width = atoi(token);
    
    token = strtok(nullptr, "x");
    if (!token) {
        free(str);
        return false;
    }
    height = atoi(token);
    
    free(str);
    return (width > 0 && height > 0);
}

/**
 * @brief 单PP测试函数（裁剪或缩放）
 * 
 * @param video_path 视频文件路径
 * @param crop_x 裁剪X坐标（如果为-1则不裁剪）
 * @param crop_y 裁剪Y坐标（如果为-1则不裁剪）
 * @param crop_width 裁剪宽度（如果为-1则不裁剪）
 * @param crop_height 裁剪高度（如果为-1则不裁剪）
 * @param scale_width 缩放宽度（如果为-1则不缩放）
 * @param scale_height 缩放高度（如果为-1则不缩放）
 * @param codec 编解码器名称（h264, h265, mjpeg）
 */
static int test_single_pp(
    const char* video_path,
    int crop_x, int crop_y, int crop_width, int crop_height,
    int scale_width, int scale_height,
    const char* codec = nullptr  // nullptr = auto-detect from video file
) {
  using namespace productionline::io;
  
  LOG_INFO("═══════════════════════════════════════════════════════");
  LOG_INFO("  Single PP Test");
  LOG_INFO_FMT("  Video: %s", video_path);
  LOG_INFO_FMT("  Codec: %s", codec ? codec : "auto-detect");
  LOG_INFO("═══════════════════════════════════════════════════════");
  
  // ========================================================================
  // 步骤1：构建Worker配置
  // ========================================================================
  bool has_crop = (crop_x >= 0 && crop_y >= 0 && crop_width > 0 && crop_height > 0);
  bool has_scale = (scale_width > 0 && scale_height > 0);
  
  if (has_crop) {
    LOG_INFO_FMT("  Crop: (%d, %d) %dx%d", crop_x, crop_y, crop_width, crop_height);
  }
  if (has_scale) {
    LOG_INFO_FMT("  Scale: %dx%d", scale_width, scale_height);
  }
  if (!has_crop && !has_scale) {
            LOG_INFO("  No PP operations (basic decode test)");
  }
  
    WorkerConfig worker_config;
  // 自动检测：如果 codec 为 nullptr，让系统自动检测；如果指定了 mjpeg，使用软件解码器
  bool use_software_decoder = (codec && strcmp(codec, "mjpeg") == 0);
    
    if (use_software_decoder) {
        // MJPEG使用软件解码器（不支持PP功能）
        LOG_INFO("Using software decoder for MJPEG (PP features not available)");
        // 计算输出分辨率（用于DisplayConfig）
        int output_width = has_scale ? scale_width : (has_crop ? crop_width : 1920);
        int output_height = has_scale ? scale_height : (has_crop ? crop_height : 1080);
        worker_config = WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                .setPath(video_path)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useSoftware()
                    .build()
            )
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(output_width, output_height)
                    .setBitsPerPixel(32)  // 默认32位（ARGB），Worker会根据实际格式调整
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
            .build();
        
    if (has_crop || has_scale) {
            LOG_WARN("⚠️  Warning: Software decoder does not support PP features (crop/scale)");
            LOG_WARN("   PP parameters will be ignored");
        }
    } else {
        // H264/H265使用TACO硬件解码器（支持PP功能）
        // 如果 codec 为 nullptr，让系统自动检测编解码器
        DecoderConfigBuilder decoder_builder;
        
        // 构建 TACO 配置
        TacoConfigBuilder taco_builder;
        taco_builder.setChannels(true, false);  // 启用ch0，禁用ch1（单PP测试）
        
        if (has_crop) {
            taco_builder.setCropRegion(crop_x, crop_y, crop_width, crop_height);
        }
        
        if (has_scale) {
            taco_builder.setDecoderOutputResolution(scale_width, scale_height);
        }
        
        auto taco_config = taco_builder.build();
        
        if (codec) {
            // 指定了编解码器，使用 TACO 硬件解码器
            decoder_builder.useTaco(codec, taco_config);
            } else {
            // 未指定编解码器，需要自动检测
            // ⭐ 对于单PP测试，如果启用了PP功能（crop/scale），需要明确指定编解码器
            // 因为 Worker 只在 decoder_name_ == "h264_taco"/"hevc_taco" 时才配置 TACO
            if (has_crop || has_scale) {
                // 有PP功能，需要检测编解码器并明确指定
                const char* detected_codec = detect_video_codec(video_path);
                if (detected_codec && (strcmp(detected_codec, "h264") == 0 || strcmp(detected_codec, "h265") == 0)) {
                    LOG_INFO_FMT("Auto-detected codec: %s, using TACO decoder for PP support", detected_codec);
                    decoder_builder.useTaco(detected_codec, taco_config);
                } else if (detected_codec && strcmp(detected_codec, "mjpeg") == 0) {
                    LOG_WARN("Auto-detected MJPEG: Software decoder does not support PP features");
                    decoder_builder.useSoftware();
                } else {
                    LOG_WARN("Failed to detect codec, defaulting to h264_taco");
                    decoder_builder.useTaco("h264", taco_config);
                }
            } else {
                // 没有PP功能，可以使用自动检测（让系统选择解码器）
                decoder_builder.setDecoderName("");  // 空字符串表示自动检测
            }
        }
        
        // 计算输出分辨率（用于DisplayConfig）
        int output_width = has_scale ? scale_width : (has_crop ? crop_width : 1920);
        int output_height = has_scale ? scale_height : (has_crop ? crop_height : 1080);
        
        worker_config = WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                .setPath(video_path)
                    .build()
            )
            .setDecoderConfig(decoder_builder.build())
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(output_width, output_height)
                    .setBitsPerPixel(32)  // 默认32位（ARGB），Worker会根据实际格式调整
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
            .build();
    }
    
  // ========================================================================
  // 步骤2：启动VideoProductionLine
  // ========================================================================
  LOG_INFO("\nStep 2: Starting VideoProductionLine...");
  VideoProductionLine producer(false, 1, false);
    if (!producer.start(worker_config)) {
    LOG_ERROR("Failed to start VideoProductionLine");
    return -1;
    }
    LOG_INFO("✅ Producer started");
    
  // ========================================================================
  // 步骤3：获取BufferPool
  // ========================================================================
  LOG_INFO("Step 3: Getting BufferPool...");
    uint64_t pool_id = producer.getWorkingBufferPoolId();
    if (pool_id == 0) {
        LOG_ERROR("❌ No working BufferPool ID available");
        producer.stop();
    return -1;
    }
    
  auto pool_sptr = BufferPoolRegistry::getInstance().getPool(pool_id).lock();
    if (!pool_sptr) {
        LOG_ERROR("❌ BufferPool not found or destroyed");
        producer.stop();
    return -1;
    }
    
    LOG_INFO_FMT("✅ BufferPool: '%s' (ID: %lu)", 
                pool_sptr->getName().c_str(), pool_id);
    
  // ========================================================================
  // 步骤4：等待第一个Buffer，获取实际格式
  // ========================================================================
  LOG_INFO("Step 4: Waiting for first buffer to detect format...");
  Buffer* first_buffer = pool_sptr->acquireFilled(true, 5000);  // 5秒超时
  if (!first_buffer) {
    LOG_ERROR("Failed to get first buffer (timeout)");
    producer.stop();
    return -1;
  }
  
  AVPixelFormat actual_format = AV_PIX_FMT_NONE;
  int actual_width = has_scale ? scale_width : (has_crop ? crop_width : 1920);
  int actual_height = has_scale ? scale_height : (has_crop ? crop_height : 1080);
  
  if (first_buffer->hasImageMetadata()) {
    actual_format = first_buffer->getImageFormat();
    actual_width = first_buffer->getImageWidth();
    actual_height = first_buffer->getImageHeight();
    
    LOG_INFO_FMT("Detected format from buffer: %s (%dx%d)", 
                av_get_pix_fmt_name(actual_format),
                actual_width, actual_height);
        } else {
    LOG_WARN("Buffer has no metadata, using default NV12");
    actual_format = AV_PIX_FMT_NV12;
  }
  
  // ========================================================================
  // 步骤5：创建BufferWriter
  // ========================================================================
  LOG_INFO("Step 5: Creating BufferWriter...");
  BufferWriter writer;
  char output_path[256];
  time_t now = time(nullptr);
  snprintf(output_path, sizeof(output_path), 
          "/tmp/pp_output_%ld.yuv", now);
  
  if (!writer.openRaw(output_path, actual_format, actual_width, actual_height)) {
    LOG_ERROR_FMT("Failed to open BufferWriter for format %s", 
                 av_get_pix_fmt_name(actual_format));
    pool_sptr->releaseFilled(first_buffer);
            producer.stop();
    return -1;
  }
  
  LOG_INFO_FMT("Saving to: %s (format: %s)", 
              output_path, av_get_pix_fmt_name(actual_format));
  
  // ========================================================================
  // 步骤6：保存帧
  // ========================================================================
  LOG_INFO("\nStep 6: Saving frames...");
  LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  
  if (writer.write(first_buffer)) {
    LOG_INFO("  ✅ Saved frame 1");
  }
  pool_sptr->releaseFilled(first_buffer);
  
  // 消费者循环：保存剩余帧
  int timeout_count = 0;
  const int MAX_TIMEOUT = 10;
  
  while (g_running) {
    Buffer* buffer = pool_sptr->acquireFilled(true, 100);
    
    if (buffer) {
      if (writer.write(buffer)) {
        if (writer.getWriteCount() % 10 == 0) {
          LOG_INFO_FMT("  Saved %d frames", writer.getWriteCount());
        }
        } else {
        LOG_ERROR_FMT("Failed to write frame %d", 
                     writer.getWriteCount() + 1);
        }
      
      pool_sptr->releaseFilled(buffer);
      timeout_count = 0;
    } else {
      timeout_count++;
      if (timeout_count >= MAX_TIMEOUT) {
        LOG_INFO("Video finished, stopping...");
        break;
      }
    }
  }
  
  LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  
  // ========================================================================
  // 步骤7：清理和结果输出
  // ========================================================================
  LOG_INFO("\nStep 7: Cleaning up...");
  writer.close();
  producer.stop();
  
  LOG_INFO("═══════════════════════════════════════════════════════");
  LOG_INFO("  Test Results");
  LOG_INFO("═══════════════════════════════════════════════════════");
  LOG_INFO_FMT("Format actual: %s", av_get_pix_fmt_name(actual_format));
  LOG_INFO_FMT("Output file: %s", output_path);
  LOG_INFO_FMT("Frames saved: %d", writer.getWriteCount());
  
  bool success = (writer.getWriteCount() > 0);
  if (success) {
    LOG_INFO("\n✅ Test PASSED");
    LOG_INFO_FMT("   - Successfully saved %d frames", writer.getWriteCount());
  } else {
    LOG_ERROR("\n❌ Test FAILED: No frames saved");
  }
  
  LOG_INFO("═══════════════════════════════════════════════════════");
  
  return success ? 0 : -1;
}

/**
 * @brief 多PP测试函数（裁剪+缩放）
 * 
 * @param video_path 视频文件路径
 * @param crop_x 裁剪X坐标
 * @param crop_y 裁剪Y坐标
 * @param crop_width 裁剪宽度
 * @param crop_height 裁剪高度
 * @param scale_width 缩放宽度
 * @param scale_height 缩放高度
 * @param codec 编解码器名称（h264, h265）
 */
/**
 * @brief 多PP测试函数（统一函数，支持裁剪、缩放和格式配置）
 * 
 * @param video_path 视频文件路径
 * @param crop_x 裁剪X坐标（-1表示不裁剪）
 * @param crop_y 裁剪Y坐标（-1表示不裁剪）
 * @param crop_width 裁剪宽度（-1表示不裁剪）
 * @param crop_height 裁剪高度（-1表示不裁剪）
 * @param scale_width 缩放宽度（-1表示不缩放，使用默认1920）
 * @param scale_height 缩放高度（-1表示不缩放，使用默认1080）
 * @param pp1_format PP1输出格式（nullptr表示使用默认ARGB888）
 * @param pp1_rgb PP1是否输出RGB格式（true=RGB, false=YUV）
 * @param codec 编解码器名称（nullptr=自动检测，支持H.264/H.265/MJPEG）
 */
static int test_multi_pp(
    const char* video_path,
    int crop_x = -1, int crop_y = -1, int crop_width = -1, int crop_height = -1,
    int scale_width = -1, int scale_height = -1,
    const char* pp1_format = nullptr,
    bool pp1_rgb = true,
    const char* codec = nullptr  // nullptr = auto-detect from video file
) {
  using namespace productionline::io;
  
  bool has_crop = (crop_x >= 0 && crop_y >= 0 && crop_width > 0 && crop_height > 0);
  bool has_scale = (scale_width > 0 && scale_height > 0);
  int final_width = has_scale ? scale_width : (has_crop ? crop_width : 1920);
  int final_height = has_scale ? scale_height : (has_crop ? crop_height : 1080);
  
  LOG_INFO("═══════════════════════════════════════════════════════");
  LOG_INFO("  Multi-PP Test");
  LOG_INFO_FMT("  Video: %s", video_path);
  LOG_INFO_FMT("  Codec: %s", codec ? codec : "auto-detect");
  if (has_crop) {
    LOG_INFO_FMT("  Crop: (%d, %d) %dx%d", crop_x, crop_y, crop_width, crop_height);
  }
  if (has_scale) {
    LOG_INFO_FMT("  Scale: %dx%d", scale_width, scale_height);
  }
  if (pp1_format) {
    LOG_INFO_FMT("  PP1 Format: %s (%s)", pp1_format, pp1_rgb ? "RGB" : "YUV");
  } else {
    LOG_INFO_FMT("  PP1 Format: ARGB888 (default)");
  }
  LOG_INFO("═══════════════════════════════════════════════════════");
  
  // ========================================================================
  // 步骤1：构建Worker配置（同时启用ch0和ch1）
  // ========================================================================
  TacoConfigBuilder taco_builder;
  taco_builder.setChannels(true, true);  // 启用ch0和ch1
  
  // ⭐ 明确配置PP0输出YUV格式（默认NV12）
  taco_builder.setYuvConfig("YUV420 8-bit NV12", "bt601");  // PP0输出YUV420 NV12格式
  
  // 配置PP1格式
  if (pp1_format && pp1_rgb) {
    taco_builder.setRgbConfig(true, pp1_format, "bt601");  // PP1输出指定RGB格式
  } else if (pp1_rgb) {
    taco_builder.setRgbConfig(true, "argb888", "bt601");  // PP1输出默认ARGB
                                } else {
    taco_builder.setRgbConfig(false, "", "bt601");  // PP1输出YUV格式
  }
  
  // 配置裁剪
  if (has_crop) {
    taco_builder.setCropRegion(crop_x, crop_y, crop_width, crop_height);
  }
  
  // 配置缩放
  if (has_scale) {
    taco_builder.setDecoderOutputResolution(scale_width, scale_height);
  } else if (has_crop) {
    // 如果只有裁剪没有缩放，使用裁剪后的尺寸作为输出分辨率
    taco_builder.setDecoderOutputResolution(crop_width, crop_height);
                                } else {
    // 默认分辨率
    taco_builder.setDecoderOutputResolution(1920, 1080);
  }
  
  auto taco_config = taco_builder.build();
  
  // 计算输出分辨率（用于DisplayConfig）
  int output_width = has_scale ? scale_width : (has_crop ? crop_width : 1920);
  int output_height = has_scale ? scale_height : (has_crop ? crop_height : 1080);
  
  DecoderConfigBuilder decoder_builder;
  if (codec) {
      // 指定了编解码器，使用 TACO 硬件解码器
      decoder_builder.useTaco(codec, taco_config);
  } else {
      // 未指定编解码器，需要自动检测
      // ⭐ 对于多PP测试，需要明确指定编解码器名称才能启用PP1通道
      // 因为 Worker 只在 decoder_name_ == "h264_taco"/"hevc_taco" 时才配置 TACO
      // 所以我们需要先检测视频文件的编解码器类型，然后明确指定
      const char* detected_codec = detect_video_codec(video_path);
      if (detected_codec && (strcmp(detected_codec, "h264") == 0 || strcmp(detected_codec, "h265") == 0)) {
          // 检测到 H.264 或 H.265，明确指定解码器以启用 PP 功能
          LOG_INFO_FMT("Auto-detected codec: %s, using TACO decoder for PP support", detected_codec);
          decoder_builder.useTaco(detected_codec, taco_config);
      } else if (detected_codec && strcmp(detected_codec, "mjpeg") == 0) {
          // MJPEG 使用软件解码器（不支持PP功能）
          LOG_INFO("Auto-detected codec: MJPEG, using software decoder (PP features not available)");
          decoder_builder.useSoftware();
      } else {
          // 无法检测或未知编解码器，尝试使用 h264（默认）
          LOG_WARN("Failed to detect codec or unknown codec, defaulting to h264_taco");
          decoder_builder.useTaco("h264", taco_config);
      }
  }
  
  auto worker_config = WorkerConfigBuilder()
      .setDataSourceConfig(
          DataSourceConfigBuilder()
              .setPath(video_path)
              .build()
      )
      .setDecoderConfig(decoder_builder.build())
      .setDisplayConfig(
          DisplayConfigBuilder()
              .setDisplayResolution(output_width, output_height)
              .setBitsPerPixel(32)  // 默认32位（ARGB），Worker会根据实际格式调整
              .build()
      )
      .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
      .build();
  
  // ========================================================================
  // 步骤2：启动VideoProductionLine
  // ========================================================================
  LOG_INFO("\nStep 2: Starting VideoProductionLine...");
  VideoProductionLine producer(false, 1, false);
  if (!producer.start(worker_config)) {
    LOG_ERROR("Failed to start VideoProductionLine");
    return -1;
  }
  LOG_INFO("✅ Producer started");
  
  // ========================================================================
  // 步骤3：获取PP0 BufferPool
  // ========================================================================
  LOG_INFO("Step 3: Getting BufferPools...");
  
  // PP0 BufferPool（ch0）
  uint64_t pp0_pool_id = producer.getWorkingBufferPoolId();
  auto pp0_pool_sptr = BufferPoolRegistry::getInstance().getPool(pp0_pool_id).lock();
  if (!pp0_pool_sptr) {
    LOG_ERROR("Failed to get PP0 BufferPool");
    producer.stop();
    return -1;
  }
  
  LOG_INFO_FMT("PP0 BufferPool: %s (ID: %lu)", 
              pp0_pool_sptr->getName().c_str(), pp0_pool_id);
  
  // ========================================================================
  // 步骤4：等待第一个PP0 Buffer，然后获取PP1 BufferPool
  // ⭐ 注意：PP1 BufferPool 可能在解码开始后才创建，所以先等待一个帧
  // ========================================================================
  LOG_INFO("Step 4: Waiting for first PP0 buffer, then getting PP1 BufferPool...");
  
  // 获取第一个buffer，判断是PP0还是PP1
  Buffer* first_buffer = pp0_pool_sptr->acquireFilled(true, 5000);
  if (!first_buffer) {
    LOG_ERROR("Failed to get first buffer (timeout)");
    producer.stop();
    return -1;
  }
  
  // 判断第一个buffer是PP0还是PP1
  AVPixelFormat first_format = AV_PIX_FMT_NONE;
  bool first_is_rgb = false;
  bool first_is_yuv = false;
  
  if (first_buffer->hasImageMetadata()) {
    first_format = first_buffer->getImageFormat();
    first_is_rgb = (first_format == AV_PIX_FMT_RGB24 || 
                   first_format == AV_PIX_FMT_BGR24 ||
                   first_format == AV_PIX_FMT_ARGB ||
                   first_format == AV_PIX_FMT_ABGR ||
                   first_format == AV_PIX_FMT_RGBA ||
                   first_format == AV_PIX_FMT_BGRA ||
                   first_format == AV_PIX_FMT_RGB0 ||
                   first_format == AV_PIX_FMT_BGR0 ||
                   first_format == AV_PIX_FMT_0RGB ||
                   first_format == AV_PIX_FMT_0BGR ||
                   first_format == AV_PIX_FMT_RGB48LE ||
                   first_format == AV_PIX_FMT_BGR48LE ||
                   first_format == AV_PIX_FMT_GBRP);
    
    first_is_yuv = (first_format == AV_PIX_FMT_NV12 ||
                   first_format == AV_PIX_FMT_NV21 ||
                   first_format == AV_PIX_FMT_YUV420P ||
                   first_format == AV_PIX_FMT_YUV420P10LE ||
                   first_format == AV_PIX_FMT_P010LE ||
                   first_format == AV_PIX_FMT_GRAY8 ||
                   first_format == AV_PIX_FMT_GRAY10LE ||
                   first_format == AV_PIX_FMT_YUV422P ||
                   first_format == AV_PIX_FMT_YUV444P);
    
    LOG_INFO_FMT("First buffer format: %s (RGB=%d, YUV=%d)", 
                av_get_pix_fmt_name(first_format), first_is_rgb, first_is_yuv);
  }
  
  Buffer* pp0_first_buffer = nullptr;
  Buffer* pp1_first_buffer = nullptr;
  
  // 根据格式判断第一个buffer是PP0还是PP1
  if ((pp1_rgb && first_is_rgb) || (!pp1_rgb && first_is_yuv)) {
    // 第一个buffer是PP1
    pp1_first_buffer = first_buffer;
    LOG_INFO("First buffer is PP1, waiting for PP0...");
  } else if (first_is_yuv || (!pp1_rgb && first_is_rgb)) {
    // 第一个buffer是PP0
    pp0_first_buffer = first_buffer;
    LOG_INFO("First buffer is PP0, waiting for PP1...");
  } else {
    LOG_WARN("Cannot determine first buffer type, assuming it's PP0");
    pp0_first_buffer = first_buffer;
  }
  
  // 现在尝试获取 PP1 BufferPool（可能在解码开始后才可用）
  auto worker_facade = producer.getWorkerFacade();
  if (!worker_facade) {
    LOG_ERROR("Failed to get Worker Facade");
    pp0_pool_sptr->releaseFilled(pp0_first_buffer);
    producer.stop();
    return -1;
  }
  
  uint64_t pp1_pool_id = worker_facade->getOutputBufferPoolId(
      BufferPoolType::DECODE_VIDEO_SECONDARY);
  
  std::shared_ptr<BufferPool> pp1_pool_sptr;
  
  if (pp1_pool_id == 0) {
    // ⭐ 如果无法获取secondary BufferPool，可能PP0和PP1共享同一个BufferPool
    // 在这种情况下，我们需要从同一个BufferPool中根据格式区分PP0和PP1
    LOG_WARN("Secondary BufferPool not found, PP0 and PP1 may share the same BufferPool");
    LOG_WARN("Will filter buffers by format to distinguish PP0 and PP1");
    pp1_pool_sptr = pp0_pool_sptr;  // 使用同一个BufferPool
    pp1_pool_id = pp0_pool_id;
  } else {
    pp1_pool_sptr = BufferPoolRegistry::getInstance().getPool(pp1_pool_id).lock();
    if (!pp1_pool_sptr) {
      LOG_ERROR("Failed to get PP1 BufferPool from Registry");
      pp0_pool_sptr->releaseFilled(pp0_first_buffer);
      producer.stop();
      return -1;
    }
  }
  
  LOG_INFO_FMT("PP1 BufferPool: %s (ID: %lu)", 
              pp1_pool_sptr->getName().c_str(), pp1_pool_id);
  
  // ========================================================================
  // 步骤5：等待PP0和PP1的第一个Buffer，获取实际格式
  // ⭐ 如果PP0和PP1共享BufferPool，需要根据格式过滤
  // ========================================================================
  LOG_INFO("Step 5: Waiting for PP0 and PP1 buffers to detect formats...");
  
  if (pp1_pool_id == pp0_pool_id) {
    // PP0和PP1共享BufferPool，需要根据格式过滤
    int max_attempts = 30;
    int attempts = 0;
    
    while ((!pp0_first_buffer || !pp1_first_buffer) && attempts < max_attempts && g_running) {
      Buffer* candidate = pp0_pool_sptr->acquireFilled(true, 500);
      if (candidate) {
        if (candidate->hasImageMetadata()) {
          AVPixelFormat candidate_format = candidate->getImageFormat();
          
          // 检查是否是RGB格式
          bool is_rgb = (candidate_format == AV_PIX_FMT_RGB24 || 
                        candidate_format == AV_PIX_FMT_BGR24 ||
                        candidate_format == AV_PIX_FMT_ARGB ||
                        candidate_format == AV_PIX_FMT_ABGR ||
                        candidate_format == AV_PIX_FMT_RGBA ||
                        candidate_format == AV_PIX_FMT_BGRA ||
                        candidate_format == AV_PIX_FMT_RGB0 ||
                        candidate_format == AV_PIX_FMT_BGR0 ||
                        candidate_format == AV_PIX_FMT_0RGB ||
                        candidate_format == AV_PIX_FMT_0BGR ||
                        candidate_format == AV_PIX_FMT_RGB48LE ||
                        candidate_format == AV_PIX_FMT_BGR48LE ||
                        candidate_format == AV_PIX_FMT_GBRP);
          
          // 检查是否是YUV格式
          bool is_yuv = (candidate_format == AV_PIX_FMT_NV12 ||
                        candidate_format == AV_PIX_FMT_NV21 ||
                        candidate_format == AV_PIX_FMT_YUV420P ||
                        candidate_format == AV_PIX_FMT_YUV420P10LE ||
                        candidate_format == AV_PIX_FMT_P010LE ||
                        candidate_format == AV_PIX_FMT_GRAY8 ||
                        candidate_format == AV_PIX_FMT_GRAY10LE ||
                        candidate_format == AV_PIX_FMT_YUV422P ||
                        candidate_format == AV_PIX_FMT_YUV444P);
          
          // 判断是PP0还是PP1
          if (!pp0_first_buffer && is_yuv) {
            // PP0应该是YUV格式
            pp0_first_buffer = candidate;
            LOG_INFO_FMT("Found PP0 buffer: %s", av_get_pix_fmt_name(candidate_format));
          } else if (!pp1_first_buffer && ((pp1_rgb && is_rgb) || (!pp1_rgb && is_yuv))) {
            // PP1格式（根据pp1_rgb判断）
            pp1_first_buffer = candidate;
            LOG_INFO_FMT("Found PP1 buffer: %s", av_get_pix_fmt_name(candidate_format));
          } else {
            // 不是我们需要的格式，释放
            pp0_pool_sptr->releaseFilled(candidate);
          }
        } else {
          // 没有元数据，释放
          pp0_pool_sptr->releaseFilled(candidate);
        }
      }
      attempts++;
    }
    
    // ⭐ 如果找不到YUV格式的PP0 buffer，可能PP0也输出了RGB格式
    // 在这种情况下，我们假设PP0和PP1交替输出
    if (!pp0_first_buffer && pp1_first_buffer) {
      LOG_WARN("No YUV format PP0 buffer found, PP0 may also output RGB format");
      LOG_WARN("Will use alternating buffer strategy: first=PP0, second=PP1");
      // 第一个buffer（已经是PP1）作为PP0，再获取一个作为PP1
      pp0_first_buffer = pp1_first_buffer;
      pp1_first_buffer = nullptr;
      // 获取第二个buffer作为PP1
      pp1_first_buffer = pp0_pool_sptr->acquireFilled(true, 5000);
    }
    
    if (!pp0_first_buffer || !pp1_first_buffer) {
      LOG_ERROR_FMT("Failed to find both buffers (PP0=%d, PP1=%d)", 
                   pp0_first_buffer != nullptr, pp1_first_buffer != nullptr);
      if (pp0_first_buffer) pp0_pool_sptr->releaseFilled(pp0_first_buffer);
      if (pp1_first_buffer) pp1_pool_sptr->releaseFilled(pp1_first_buffer);
      producer.stop();
      return -1;
    }
  } else {
    // PP0和PP1有独立的BufferPool
    if (!pp0_first_buffer) {
      pp0_first_buffer = pp0_pool_sptr->acquireFilled(true, 5000);
    }
    if (!pp1_first_buffer) {
      pp1_first_buffer = pp1_pool_sptr->acquireFilled(true, 5000);
    }
  }
  
  if (!pp0_first_buffer || !pp1_first_buffer) {
    if (pp0_first_buffer) pp0_pool_sptr->releaseFilled(pp0_first_buffer);
    if (pp1_first_buffer) pp1_pool_sptr->releaseFilled(pp1_first_buffer);
    LOG_ERROR("Failed to get first buffers (timeout)");
    producer.stop();
    return -1;
  }
  
  AVPixelFormat pp0_format = AV_PIX_FMT_NONE;
  int pp0_width = final_width;
  int pp0_height = final_height;
  
  AVPixelFormat pp1_av_format = AV_PIX_FMT_NONE;
  int pp1_width = final_width;
  int pp1_height = final_height;
  
  if (pp0_first_buffer->hasImageMetadata()) {
    pp0_format = pp0_first_buffer->getImageFormat();
    pp0_width = pp0_first_buffer->getImageWidth();
    pp0_height = pp0_first_buffer->getImageHeight();
    
    LOG_INFO_FMT("PP0 detected format: %s (%dx%d)", 
                av_get_pix_fmt_name(pp0_format),
                pp0_width, pp0_height);
        } else {
    LOG_WARN("PP0 buffer has no metadata, using default NV12");
    pp0_format = AV_PIX_FMT_NV12;
  }
  
  if (pp1_first_buffer->hasImageMetadata()) {
    pp1_av_format = pp1_first_buffer->getImageFormat();
    pp1_width = pp1_first_buffer->getImageWidth();
    pp1_height = pp1_first_buffer->getImageHeight();
    
    LOG_INFO_FMT("PP1 detected format: %s (%dx%d)", 
                av_get_pix_fmt_name(pp1_av_format),
                pp1_width, pp1_height);
  } else {
    LOG_WARN("PP1 buffer has no metadata, using default");
    pp1_av_format = pp1_rgb ? AV_PIX_FMT_ARGB : AV_PIX_FMT_NV12;
  }
  
  // ========================================================================
  // 步骤5：创建BufferWriter
  // ========================================================================
  LOG_INFO("Step 5: Creating BufferWriters...");
  
  BufferWriter pp0_writer;
  char pp0_output_path[256];
  time_t now = time(nullptr);
  
  // ⭐ 根据PP0实际格式动态决定文件扩展名
  const char* pp0_ext = "yuv";  // 默认YUV格式
  bool pp0_is_rgb = (pp0_format == AV_PIX_FMT_RGB24 || 
                     pp0_format == AV_PIX_FMT_BGR24 ||
                     pp0_format == AV_PIX_FMT_ARGB ||
                     pp0_format == AV_PIX_FMT_ABGR ||
                     pp0_format == AV_PIX_FMT_RGBA ||
                     pp0_format == AV_PIX_FMT_BGRA ||
                     pp0_format == AV_PIX_FMT_RGB0 ||
                     pp0_format == AV_PIX_FMT_BGR0 ||
                     pp0_format == AV_PIX_FMT_0RGB ||
                     pp0_format == AV_PIX_FMT_0BGR ||
                     pp0_format == AV_PIX_FMT_RGB48LE ||
                     pp0_format == AV_PIX_FMT_BGR48LE ||
                     pp0_format == AV_PIX_FMT_GBRP);
  if (pp0_is_rgb) {
    pp0_ext = "rgb";
  }
  
  snprintf(pp0_output_path, sizeof(pp0_output_path), 
          "/tmp/pp0_output_%ld.%s", now, pp0_ext);
  
  if (!pp0_writer.openRaw(pp0_output_path, pp0_format, pp0_width, pp0_height)) {
    LOG_ERROR_FMT("Failed to open PP0 BufferWriter for format %s", 
                 av_get_pix_fmt_name(pp0_format));
    pp0_pool_sptr->releaseFilled(pp0_first_buffer);
    pp1_pool_sptr->releaseFilled(pp1_first_buffer);
    producer.stop();
    return -1;
  }
  
  BufferWriter pp1_writer;
  char pp1_output_path[256];
  const char* pp1_ext = pp1_rgb ? "rgb" : "yuv";
  snprintf(pp1_output_path, sizeof(pp1_output_path), 
          "/tmp/pp1_output_%ld.%s", now, pp1_ext);
  
  if (!pp1_writer.openRaw(pp1_output_path, pp1_av_format, pp1_width, pp1_height)) {
    LOG_ERROR_FMT("Failed to open PP1 BufferWriter for format %s", 
                 av_get_pix_fmt_name(pp1_av_format));
    pp0_writer.close();
    pp0_pool_sptr->releaseFilled(pp0_first_buffer);
    pp1_pool_sptr->releaseFilled(pp1_first_buffer);
    producer.stop();
    return -1;
  }
  
  LOG_INFO_FMT("PP0 saving to: %s (format: %s)", 
              pp0_output_path, av_get_pix_fmt_name(pp0_format));
  LOG_INFO_FMT("PP1 saving to: %s (format: %s)", 
              pp1_output_path, av_get_pix_fmt_name(pp1_av_format));
  
  // ========================================================================
  // 步骤6：保存帧
  // ========================================================================
  LOG_INFO("\nStep 6: Saving frames...");
  LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  
  bool pp0_ok = pp0_writer.write(pp0_first_buffer);
  bool pp1_ok = pp1_writer.write(pp1_first_buffer);
  
  if (pp0_ok) {
    LOG_INFO("  ✅ Saved PP0 frame 1");
    } else {
    LOG_ERROR("  ❌ Failed to write PP0 frame 1");
                }
  
  if (pp1_ok) {
    LOG_INFO("  ✅ Saved PP1 frame 1");
            } else {
    LOG_ERROR("  ❌ Failed to write PP1 frame 1");
  }
  
  pp0_pool_sptr->releaseFilled(pp0_first_buffer);
  pp1_pool_sptr->releaseFilled(pp1_first_buffer);
  
  // 消费者循环：保存剩余帧
  int timeout_count = 0;
  const int MAX_TIMEOUT = 10;
  bool shared_pool = (pp0_pool_id == pp1_pool_id);
  
  // ⭐ 检查是否使用交替策略（PP0格式也是RGB）
  bool use_alternating = false;
  if (pp0_format != AV_PIX_FMT_NONE) {
    bool pp0_is_rgb = (pp0_format == AV_PIX_FMT_RGB24 || 
                      pp0_format == AV_PIX_FMT_BGR24 ||
                      pp0_format == AV_PIX_FMT_ARGB ||
                      pp0_format == AV_PIX_FMT_ABGR ||
                      pp0_format == AV_PIX_FMT_RGBA ||
                      pp0_format == AV_PIX_FMT_BGRA ||
                      pp0_format == AV_PIX_FMT_RGB0 ||
                      pp0_format == AV_PIX_FMT_BGR0 ||
                      pp0_format == AV_PIX_FMT_0RGB ||
                      pp0_format == AV_PIX_FMT_0BGR ||
                      pp0_format == AV_PIX_FMT_RGB48LE ||
                      pp0_format == AV_PIX_FMT_BGR48LE ||
                      pp0_format == AV_PIX_FMT_GBRP);
    use_alternating = pp0_is_rgb;
  }
  
  // 用于缓存buffer（在共享BufferPool的情况下）
  Buffer* cached_pp0_buffer = nullptr;
  Buffer* cached_pp1_buffer = nullptr;
  
  while (g_running) {
    Buffer* pp0_buffer = nullptr;
    Buffer* pp1_buffer = nullptr;
    
    if (shared_pool) {
      if (use_alternating) {
        // ⭐ 交替策略：第一个buffer是PP0，第二个buffer是PP1
        pp0_buffer = pp0_pool_sptr->acquireFilled(true, 100);
        if (pp0_buffer) {
          pp1_buffer = pp0_pool_sptr->acquireFilled(true, 100);
        }
        } else {
        // 格式过滤策略
        // 先使用缓存的buffer
        if (cached_pp0_buffer) {
          pp0_buffer = cached_pp0_buffer;
          cached_pp0_buffer = nullptr;
        }
        if (cached_pp1_buffer) {
          pp1_buffer = cached_pp1_buffer;
          cached_pp1_buffer = nullptr;
        }
        
        // 如果还没有两个buffer，继续获取
        while ((!pp0_buffer || !pp1_buffer) && timeout_count < MAX_TIMEOUT) {
          Buffer* candidate = pp0_pool_sptr->acquireFilled(true, 100);
          if (candidate) {
            if (candidate->hasImageMetadata()) {
              AVPixelFormat candidate_format = candidate->getImageFormat();
              
              // 检查是否是YUV格式（PP0）
              bool is_yuv = (candidate_format == AV_PIX_FMT_NV12 ||
                            candidate_format == AV_PIX_FMT_NV21 ||
                            candidate_format == AV_PIX_FMT_YUV420P ||
                            candidate_format == AV_PIX_FMT_YUV420P10LE ||
                            candidate_format == AV_PIX_FMT_P010LE ||
                            candidate_format == AV_PIX_FMT_GRAY8 ||
                            candidate_format == AV_PIX_FMT_GRAY10LE ||
                            candidate_format == AV_PIX_FMT_YUV422P ||
                            candidate_format == AV_PIX_FMT_YUV444P);
              
              // 检查是否是RGB格式（PP1，如果pp1_rgb=true）
              bool is_rgb = (candidate_format == AV_PIX_FMT_RGB24 || 
                            candidate_format == AV_PIX_FMT_BGR24 ||
                            candidate_format == AV_PIX_FMT_ARGB ||
                            candidate_format == AV_PIX_FMT_ABGR ||
                            candidate_format == AV_PIX_FMT_RGBA ||
                            candidate_format == AV_PIX_FMT_BGRA ||
                            candidate_format == AV_PIX_FMT_RGB0 ||
                            candidate_format == AV_PIX_FMT_BGR0 ||
                            candidate_format == AV_PIX_FMT_0RGB ||
                            candidate_format == AV_PIX_FMT_0BGR ||
                            candidate_format == AV_PIX_FMT_RGB48LE ||
                            candidate_format == AV_PIX_FMT_BGR48LE ||
                            candidate_format == AV_PIX_FMT_GBRP);
              
              // 判断是PP0还是PP1
              if (!pp0_buffer && is_yuv) {
                pp0_buffer = candidate;
              } else if (!pp1_buffer && ((pp1_rgb && is_rgb) || (!pp1_rgb && is_yuv))) {
                pp1_buffer = candidate;
              } else {
                // 不是我们需要的格式，或者已经有两个buffer了，缓存或释放
                if (is_yuv && !cached_pp0_buffer) {
                  cached_pp0_buffer = candidate;
                } else if (((pp1_rgb && is_rgb) || (!pp1_rgb && is_yuv)) && !cached_pp1_buffer) {
                  cached_pp1_buffer = candidate;
                } else {
                  pp0_pool_sptr->releaseFilled(candidate);
                }
              }
            } else {
              // 没有元数据，释放
              pp0_pool_sptr->releaseFilled(candidate);
            }
          } else {
            // 超时，退出内层循环
            break;
          }
        }
      }
    } else {
      // PP0和PP1有独立的BufferPool
      pp0_buffer = pp0_pool_sptr->acquireFilled(true, 100);
      pp1_buffer = pp1_pool_sptr->acquireFilled(true, 100);
    }
    
    if (pp0_buffer && pp1_buffer) {
      if (pp0_writer.write(pp0_buffer)) {
        if (pp0_writer.getWriteCount() % 10 == 0) {
          LOG_INFO_FMT("  Saved PP0: %d frames, PP1: %d frames", 
                      pp0_writer.getWriteCount(),
                      pp1_writer.getWriteCount());
        }
      }
      
      if (pp1_writer.write(pp1_buffer)) {
        // PP1计数已在上面的日志中显示
      }
      
      pp0_pool_sptr->releaseFilled(pp0_buffer);
      pp1_pool_sptr->releaseFilled(pp1_buffer);
      timeout_count = 0;
    } else {
      if (pp0_buffer) pp0_pool_sptr->releaseFilled(pp0_buffer);
      if (pp1_buffer) pp1_pool_sptr->releaseFilled(pp1_buffer);
      
      timeout_count++;
      if (timeout_count >= MAX_TIMEOUT) {
        LOG_INFO("Video finished, stopping...");
        break;
      }
    }
  }
  
  // 清理缓存的buffer
  if (cached_pp0_buffer) pp0_pool_sptr->releaseFilled(cached_pp0_buffer);
  if (cached_pp1_buffer) pp1_pool_sptr->releaseFilled(cached_pp1_buffer);
  
  LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  
  // ========================================================================
  // 步骤7：清理和结果输出
  // ========================================================================
  LOG_INFO("\nStep 7: Cleaning up...");
  pp0_writer.close();
  pp1_writer.close();
  producer.stop();
  
  LOG_INFO("═══════════════════════════════════════════════════════");
  LOG_INFO("  Test Results");
  LOG_INFO("═══════════════════════════════════════════════════════");
  LOG_INFO_FMT("PP0 format: %s", av_get_pix_fmt_name(pp0_format));
  LOG_INFO_FMT("PP0 output: %s", pp0_output_path);
  LOG_INFO_FMT("PP0 frames saved: %d", pp0_writer.getWriteCount());
  LOG_INFO_FMT("PP1 format: %s", av_get_pix_fmt_name(pp1_av_format));
  LOG_INFO_FMT("PP1 output: %s", pp1_output_path);
  LOG_INFO_FMT("PP1 frames saved: %d", pp1_writer.getWriteCount());
  
  bool success = (pp0_writer.getWriteCount() > 0 && pp1_writer.getWriteCount() > 0);
  if (success) {
    LOG_INFO("\n✅ Test PASSED");
    LOG_INFO_FMT("   - PP0: %d frames", pp0_writer.getWriteCount());
    LOG_INFO_FMT("   - PP1: %d frames", pp1_writer.getWriteCount());
            } else {
    LOG_ERROR("\n❌ Test FAILED");
    if (pp0_writer.getWriteCount() == 0) {
      LOG_ERROR("   - PP0: No frames saved");
    }
    if (pp1_writer.getWriteCount() == 0) {
      LOG_ERROR("   - PP1: No frames saved");
    }
  }
  
  LOG_INFO("═══════════════════════════════════════════════════════");
  
  return success ? 0 : -1;
}

// ========== PP1单独测试函数 ==========

/**
 * @brief 单独测试PP1（只启用ch1，禁用ch0）
 * 
 * @param video_path 视频文件路径
 * @param crop_x 裁剪X坐标（-1表示不裁剪）
 * @param crop_y 裁剪Y坐标（-1表示不裁剪）
 * @param crop_width 裁剪宽度（-1表示不裁剪）
 * @param crop_height 裁剪高度（-1表示不裁剪）
 * @param scale_width 缩放宽度（-1表示不缩放）
 * @param scale_height 缩放高度（-1表示不缩放）
 * @param pp1_format PP1格式（如 "rgb888", "argb888" 等，nullptr表示默认ARGB888）
 * @param pp1_rgb PP1是否RGB格式（true=RGB, false=YUV）
 * @param codec 编解码器名称（nullptr表示自动检测）
 * @return 0成功，-1失败
 */
static int test_pp1_only(
    const char* video_path,
    int crop_x = -1, int crop_y = -1, int crop_width = -1, int crop_height = -1,
    int scale_width = -1, int scale_height = -1,
    const char* pp1_format = nullptr,
    bool pp1_rgb = true,
    const char* codec = nullptr
) {
  using namespace productionline::io;
  
  bool has_crop = (crop_x >= 0 && crop_y >= 0 && crop_width > 0 && crop_height > 0);
  bool has_scale = (scale_width > 0 && scale_height > 0);
  int final_width = has_scale ? scale_width : (has_crop ? crop_width : 1920);
  int final_height = has_scale ? scale_height : (has_crop ? crop_height : 1080);
  
  LOG_INFO("═══════════════════════════════════════════════════════");
  LOG_INFO("  PP1 Only Test (ch0 disabled, ch1 enabled)");
  LOG_INFO_FMT("  Video: %s", video_path);
  LOG_INFO_FMT("  Codec: %s", codec ? codec : "auto-detect");
  if (has_crop) {
    LOG_INFO_FMT("  Crop: (%d, %d) %dx%d", crop_x, crop_y, crop_width, crop_height);
  }
  if (has_scale) {
    LOG_INFO_FMT("  Scale: %dx%d", scale_width, scale_height);
  }
  if (pp1_format) {
    LOG_INFO_FMT("  PP1 Format: %s (%s)", pp1_format, pp1_rgb ? "RGB" : "YUV");
  } else {
    LOG_INFO_FMT("  PP1 Format: ARGB888 (default)");
  }
  LOG_INFO("═══════════════════════════════════════════════════════");
  
  // ========================================================================
  // 步骤1：构建Worker配置（只启用ch1，禁用ch0）
  // ========================================================================
  TacoConfigBuilder taco_builder;
  taco_builder.setChannels(false, true);  // 禁用ch0，启用ch1（只测试PP1）
  
  // 配置PP1格式
  if (pp1_format && pp1_rgb) {
    taco_builder.setRgbConfig(true, pp1_format, "bt601");  // PP1输出指定RGB格式
  } else if (pp1_rgb) {
    taco_builder.setRgbConfig(true, "argb888", "bt601");  // PP1输出默认ARGB
  } else {
    taco_builder.setRgbConfig(false, "", "bt601");  // PP1输出YUV格式
  }
  
  // 配置裁剪
  if (has_crop) {
    taco_builder.setCropRegion(crop_x, crop_y, crop_width, crop_height);
  }
  
  // 配置缩放
  if (has_scale) {
    taco_builder.setDecoderOutputResolution(scale_width, scale_height);
  } else if (has_crop) {
    // 如果只有裁剪没有缩放，使用裁剪后的尺寸作为输出分辨率
    taco_builder.setDecoderOutputResolution(crop_width, crop_height);
  } else {
    // 默认分辨率
    taco_builder.setDecoderOutputResolution(1920, 1080);
        }
        
        auto taco_config = taco_builder.build();
        
  // 计算输出分辨率（用于DisplayConfig）
  int output_width = has_scale ? scale_width : (has_crop ? crop_width : 1920);
  int output_height = has_scale ? scale_height : (has_crop ? crop_height : 1080);
  
  DecoderConfigBuilder decoder_builder;
  
  // 检测编解码器
  const char* actual_codec = codec;
  if (!actual_codec) {
    actual_codec = detect_video_codec(video_path);
    if (!actual_codec) {
      LOG_ERROR_FMT("Failed to detect codec for video: %s", video_path);
      return -1;
    }
  }
  
  if (actual_codec && (strcmp(actual_codec, "h264") == 0 || strcmp(actual_codec, "h265") == 0)) {
    LOG_INFO_FMT("Using codec: %s, using TACO decoder for PP1 support", actual_codec);
    decoder_builder.useTaco(actual_codec, taco_config);
  } else if (actual_codec && strcmp(actual_codec, "mjpeg") == 0) {
    LOG_WARN("MJPEG does not support PP features, cannot test PP1");
    return -1;
  } else {
    LOG_WARN_FMT("Unknown codec: %s, defaulting to h264_taco", actual_codec ? actual_codec : "unknown");
    decoder_builder.useTaco("h264", taco_config);
  }
  
  auto worker_config = WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
              .setPath(video_path)
                    .build()
            )
      .setDecoderConfig(decoder_builder.build())
      .setDisplayConfig(
          DisplayConfigBuilder()
              .setDisplayResolution(output_width, output_height)
              .setBitsPerPixel(32)  // 默认32位（ARGB），Worker会根据实际格式调整
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
            .build();
  
  // ========================================================================
  // 步骤2：启动VideoProductionLine
  // ========================================================================
  LOG_INFO("\nStep 2: Starting VideoProductionLine...");
  VideoProductionLine producer(false, 1, false);
    if (!producer.start(worker_config)) {
    LOG_ERROR("Failed to start VideoProductionLine");
    return -1;
    }
    LOG_INFO("✅ Producer started");
    
  // ========================================================================
  // 步骤3：获取PP1 BufferPool
  // ⭐ 注意：当只启用ch1时，PP1可能作为PRIMARY BufferPool输出
  // ========================================================================
  LOG_INFO("Step 3: Getting PP1 BufferPool...");
  
  // 首先尝试获取PRIMARY BufferPool（当ch0禁用时，可能是PP1的输出）
  uint64_t pp1_pool_id = producer.getWorkingBufferPoolId();
  auto pp1_pool_sptr = BufferPoolRegistry::getInstance().getPool(pp1_pool_id).lock();
  
  // 如果PRIMARY BufferPool不存在，尝试获取SECONDARY BufferPool
  if (!pp1_pool_sptr) {
    LOG_INFO("PRIMARY BufferPool not found, trying SECONDARY BufferPool...");
    auto worker_facade = producer.getWorkerFacade();
    if (worker_facade) {
      pp1_pool_id = worker_facade->getOutputBufferPoolId(
          BufferPoolType::DECODE_VIDEO_SECONDARY);
      if (pp1_pool_id != 0) {
        pp1_pool_sptr = BufferPoolRegistry::getInstance().getPool(pp1_pool_id).lock();
      }
    }
  }
  
  if (!pp1_pool_sptr) {
    LOG_ERROR("Failed to get PP1 BufferPool");
        producer.stop();
    return -1;
  }
  
  LOG_INFO_FMT("PP1 BufferPool: %s (ID: %lu)", 
              pp1_pool_sptr->getName().c_str(), pp1_pool_id);
  
  // ========================================================================
  // 步骤4：等待第一个PP1 Buffer，获取实际格式
  // ========================================================================
  LOG_INFO("Step 4: Waiting for first PP1 buffer to detect format...");
  
  Buffer* pp1_first_buffer = pp1_pool_sptr->acquireFilled(true, 5000);
  if (!pp1_first_buffer) {
    LOG_ERROR("Failed to get first PP1 buffer (timeout)");
        producer.stop();
    return -1;
  }
  
  AVPixelFormat pp1_av_format = AV_PIX_FMT_NONE;
  int pp1_width = final_width;
  int pp1_height = final_height;
  
  if (pp1_first_buffer->hasImageMetadata()) {
    pp1_av_format = pp1_first_buffer->getImageFormat();
    pp1_width = pp1_first_buffer->getImageWidth();
    pp1_height = pp1_first_buffer->getImageHeight();
    
    LOG_INFO_FMT("PP1 detected format: %s (%dx%d)", 
                av_get_pix_fmt_name(pp1_av_format),
                pp1_width, pp1_height);
        } else {
    LOG_WARN("PP1 buffer has no metadata, using default");
    pp1_av_format = pp1_rgb ? AV_PIX_FMT_ARGB : AV_PIX_FMT_NV12;
  }
  
  // ========================================================================
  // 步骤5：创建BufferWriter
  // ========================================================================
  LOG_INFO("Step 5: Creating BufferWriter...");
  
  BufferWriter pp1_writer;
  char pp1_output_path[256];
  time_t now = time(nullptr);
  const char* pp1_ext = pp1_rgb ? "rgb" : "yuv";
  snprintf(pp1_output_path, sizeof(pp1_output_path), 
          "/tmp/pp1_only_output_%ld.%s", now, pp1_ext);
  
  if (!pp1_writer.openRaw(pp1_output_path, pp1_av_format, pp1_width, pp1_height)) {
    LOG_ERROR_FMT("Failed to open PP1 BufferWriter for format %s", 
                 av_get_pix_fmt_name(pp1_av_format));
    pp1_pool_sptr->releaseFilled(pp1_first_buffer);
            producer.stop();
    return -1;
  }
  
  LOG_INFO_FMT("PP1 saving to: %s (format: %s)", 
              pp1_output_path, av_get_pix_fmt_name(pp1_av_format));
  
  // ========================================================================
  // 步骤6：保存帧
  // ========================================================================
  LOG_INFO("\nStep 6: Saving frames...");
  LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  
  bool pp1_ok = pp1_writer.write(pp1_first_buffer);
  if (pp1_ok) {
    LOG_INFO("  ✅ Saved PP1 frame 1");
    } else {
    LOG_ERROR("  ❌ Failed to write PP1 frame 1");
  }
  
  pp1_pool_sptr->releaseFilled(pp1_first_buffer);
  
  // 消费者循环：保存剩余帧
  int timeout_count = 0;
  const int MAX_TIMEOUT = 10;
  
  while (g_running) {
    Buffer* pp1_buffer = pp1_pool_sptr->acquireFilled(true, 100);
    
    if (pp1_buffer) {
      if (pp1_writer.write(pp1_buffer)) {
        if (pp1_writer.getWriteCount() % 10 == 0) {
          LOG_INFO_FMT("  Saved PP1: %d frames", pp1_writer.getWriteCount());
        }
      }
      
      pp1_pool_sptr->releaseFilled(pp1_buffer);
      timeout_count = 0;
        } else {
      timeout_count++;
      if (timeout_count >= MAX_TIMEOUT) {
        LOG_INFO("Video finished, stopping...");
        break;
      }
    }
  }
  
  LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  
  // ========================================================================
  // 步骤7：清理和结果输出
  // ========================================================================
  LOG_INFO("\nStep 7: Cleaning up...");
  pp1_writer.close();
  producer.stop();
  
  LOG_INFO("═══════════════════════════════════════════════════════");
  LOG_INFO("  Test Results");
  LOG_INFO("═══════════════════════════════════════════════════════");
  LOG_INFO_FMT("PP1 format: %s", av_get_pix_fmt_name(pp1_av_format));
  LOG_INFO_FMT("PP1 output: %s", pp1_output_path);
  LOG_INFO_FMT("PP1 frames saved: %d", pp1_writer.getWriteCount());
  
  bool success = (pp1_writer.getWriteCount() > 0);
  if (success) {
    LOG_INFO("\n✅ Test PASSED");
    LOG_INFO_FMT("   - PP1: %d frames", pp1_writer.getWriteCount());
    } else {
    LOG_ERROR("\n❌ Test FAILED");
    LOG_ERROR("   - PP1: No frames saved");
  }
  
  LOG_INFO("═══════════════════════════════════════════════════════");
  
  return success ? 0 : -1;
}

// ========== PP1格式测试用例（33个） ==========

// RGB/ARGB/BGR/BGRA/BGRX 格式（18个）
static int test_pp1_argb2101010(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "x2rgb10le", true, nullptr);
}

static int test_pp1_abgr2101010(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "abgr2101010", true, nullptr);
}

static int test_pp1_abgr8888(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "abgr8888", true, nullptr);
}

static int test_pp1_argb8888(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "argb888", true, nullptr);
}

static int test_pp1_bgr161616(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "bgr161616", true, nullptr);
}

static int test_pp1_bgr888(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "bgr888", true, nullptr);
}

static int test_pp1_bgra2101010(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "bgra2101010", true, nullptr);
}

static int test_pp1_bgra8888(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "bgra8888", true, nullptr);
}

static int test_pp1_bgrx8888(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "bgrx8888", true, nullptr);
}

static int test_pp1_rgb161616_planar(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "gbrp16le", true, nullptr);
}

static int test_pp1_rgb161616(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "r16g16b16", true, nullptr);
}

static int test_pp1_rgb888_planar(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "gbrp", true, nullptr);
}

static int test_pp1_rgb888(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "rgb888", true, nullptr);
}

static int test_pp1_rgba2101010(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "rgba2101010", true, nullptr);
}

static int test_pp1_rgba8888(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "rgba8888", true, nullptr);
}

static int test_pp1_rgbx8888(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "rgbx8888", true, nullptr);
}

static int test_pp1_xrgb8888(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "xrgb8888", true, nullptr);
}

static int test_pp1_xbgr8888(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, "xbgr8888", true, nullptr);
}

// YUV 格式（15个）
static int test_pp1_yuv400_p010(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);
}

static int test_pp1_yuv400_i010(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);
}

static int test_pp1_yuv400_l010(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);
}

static int test_pp1_yuv400_pack10(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);
}

static int test_pp1_yuv400_8bit(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);
}

static int test_pp1_yuv420_nv12_p010(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);
}

static int test_pp1_yuv420_nv12_i010(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);
}

static int test_pp1_yuv420_nv12_l010(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);
}

static int test_pp1_yuv420_nv12_pack10(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);
}

static int test_pp1_yuv420_8bit_nv12(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);
}

static int test_pp1_yuv420_nv21_p010_tiled(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);
}

static int test_pp1_yuv420_nv21_i010(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);
}

static int test_pp1_yuv420_nv21_l010(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);
}

static int test_pp1_yuv420_p010(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);
}

static int test_pp1_yuv420_8bit_nv21(const char* video_path) {
  return test_pp1_only(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);
}

// ========== PP0格式测试用例（15个） ==========

/**
 * @brief PP0格式测试函数（测试PP0的不同YUV输出格式）
 * 
 * 注意：实际的格式输出由TACO解码器硬件决定，这里主要测试解码和保存功能
 * 编解码器会根据视频文件自动检测（H.264/H.265/MJPEG）
 */
static int test_pp0_format(const char* video_path, const char* format_name) {
  // PP0格式测试：只启用ch0，不启用ch1
  // 格式由硬件解码器自动选择，这里主要验证解码和保存功能
  // 传递 nullptr 让系统根据视频文件自动检测编解码器
  return test_single_pp(video_path, -1, -1, -1, -1, -1, -1, nullptr);
}

// PP0 YUV400系列（5个）
static int test_pp0_yuv400_p010(const char* video_path) {
  return test_pp0_format(video_path, "YUV400_P010");
}

static int test_pp0_yuv400_i010(const char* video_path) {
  return test_pp0_format(video_path, "YUV400_I010");
}

static int test_pp0_yuv400_l010(const char* video_path) {
  return test_pp0_format(video_path, "YUV400_L010");
}

static int test_pp0_yuv400_pack10(const char* video_path) {
  return test_pp0_format(video_path, "YUV400_Pack10");
}

static int test_pp0_yuv400_8bit(const char* video_path) {
  return test_pp0_format(video_path, "YUV400_8bit");
}

// PP0 YUV420 NV12系列（5个）
static int test_pp0_yuv420_nv12_p010(const char* video_path) {
  return test_pp0_format(video_path, "YUV420_NV12_P010");
}

static int test_pp0_yuv420_nv12_i010(const char* video_path) {
  return test_pp0_format(video_path, "YUV420_NV12_I010");
}

static int test_pp0_yuv420_nv12_l010(const char* video_path) {
  return test_pp0_format(video_path, "YUV420_NV12_L010");
}

static int test_pp0_yuv420_nv12_pack10(const char* video_path) {
  return test_pp0_format(video_path, "YUV420_NV12_Pack10");
}

static int test_pp0_yuv420_8bit_nv12(const char* video_path) {
  return test_pp0_format(video_path, "YUV420_8bit_NV12");
}

// PP0 YUV420 NV21系列（3个）
static int test_pp0_yuv420_nv21_p010_tiled(const char* video_path) {
  return test_pp0_format(video_path, "YUV420_NV21_P010_Tiled_4x4");
}

static int test_pp0_yuv420_nv21_i011(const char* video_path) {
  return test_pp0_format(video_path, "YUV420_NV21_I011");
}

static int test_pp0_yuv420_nv21_l010(const char* video_path) {
  return test_pp0_format(video_path, "YUV420_NV21_L010");
}

// PP0 YUV420 Planar系列（2个）
static int test_pp0_yuv420_p010(const char* video_path) {
  return test_pp0_format(video_path, "YUV420_P010");
}

static int test_pp0_yuv420_8bit_nv21(const char* video_path) {
  return test_pp0_format(video_path, "YUV420_8bit_NV21");
}

// ========== 多PP测试用例（10个） ==========

// T01-T11测试用例（使用统一的test_multi_pp函数）
// 注意：所有测试用例都使用自动检测编解码器（nullptr），支持 H.264/H.265/MJPEG
static int test_multi_pp_t01(const char* video_path) {
  return test_multi_pp(video_path, -1, -1, -1, -1, -1, -1, "rgb888", true, nullptr);
}

static int test_multi_pp_t02(const char* video_path) {
  return test_multi_pp(video_path, -1, -1, -1, -1, -1, -1, "argb888", true, nullptr);
}

static int test_multi_pp_t03(const char* video_path) {
  return test_multi_pp(video_path, -1, -1, -1, -1, -1, -1, "bgr888", true, nullptr);
}

static int test_multi_pp_t04(const char* video_path) {
  return test_multi_pp(video_path, -1, -1, -1, -1, -1, -1, "rgb888", true, nullptr);
}

static int test_multi_pp_t05(const char* video_path) {
  return test_multi_pp(video_path, -1, -1, -1, -1, -1, -1, "x2rgb10le", true, nullptr);  // ARGB2101010
}

static int test_multi_pp_t06(const char* video_path) {
  return test_multi_pp(video_path, -1, -1, -1, -1, -1, -1, "r16g16b16", true, nullptr);  // RGB161616
}

static int test_multi_pp_t07(const char* video_path) {
  return test_multi_pp(video_path, -1, -1, -1, -1, -1, -1, "rgb888", true, nullptr);
}

static int test_multi_pp_t09(const char* video_path) {
  return test_multi_pp(video_path, -1, -1, -1, -1, -1, -1, "gbrp", true, nullptr);  // RGB888 planar
}

static int test_multi_pp_t10(const char* video_path) {
  return test_multi_pp(video_path, -1, -1, -1, -1, -1, -1, "argb888", true, nullptr);
}

static int test_multi_pp_t11(const char* video_path) {
  return test_multi_pp(video_path, -1, -1, -1, -1, -1, -1, nullptr, false, nullptr);  // PP1输出YUV
}

// ========== 多PP+裁剪测试用例（6个） ==========
// 注意：所有测试用例都使用自动检测编解码器（nullptr），支持 H.264/H.265/MJPEG

static int test_multi_pp_crop1(const char* video_path) {
  // PP0 crop: 4096x2160 -> 1920x1080
  return test_multi_pp(video_path, 0, 0, 1920, 1080, 1920, 1080, nullptr, true, nullptr);
}

static int test_multi_pp_crop2(const char* video_path) {
  // PP0 down-scale: 32768x32768 -> 1280x720
  return test_multi_pp(video_path, 0, 0, 1280, 720, 1280, 720, nullptr, true, nullptr);
}

static int test_multi_pp_crop3(const char* video_path) {
  // PP1 crop: 4096x2160 -> 1920x1080
  return test_multi_pp(video_path, 0, 0, 1920, 1080, 1920, 1080, nullptr, true, nullptr);
}

static int test_multi_pp_crop4(const char* video_path) {
  // PP1 down-scale: 32768x32768 -> 1280x720
  return test_multi_pp(video_path, 0, 0, 1280, 720, 1280, 720, nullptr, true, nullptr);
}

static int test_multi_pp_crop5(const char* video_path) {
  // PP0 down-scale: 32768x32768 -> 256x256
  return test_multi_pp(video_path, 0, 0, 256, 256, 256, 256, nullptr, true, nullptr);
}

static int test_multi_pp_crop6(const char* video_path) {
  // PP1 down-scale: 4096x2160 -> 128x128
  return test_multi_pp(video_path, 0, 0, 128, 128, 128, 128, nullptr, true, nullptr);
}

// ========== 测试用例注册 ==========

// PP0格式测试用例（15个）
REGISTER_TEST(pp0_yuv400_p010, "PP0 YUV400 P010 format test", test_pp0_yuv400_p010);
REGISTER_TEST(pp0_yuv400_i010, "PP0 YUV400 I010 format test", test_pp0_yuv400_i010);
REGISTER_TEST(pp0_yuv400_l010, "PP0 YUV400 L010 format test", test_pp0_yuv400_l010);
REGISTER_TEST(pp0_yuv400_pack10, "PP0 YUV400 Pack10 format test", test_pp0_yuv400_pack10);
REGISTER_TEST(pp0_yuv400_8bit, "PP0 YUV400 8-bit format test", test_pp0_yuv400_8bit);
REGISTER_TEST(pp0_yuv420_nv12_p010, "PP0 YUV420 NV12 P010 format test", test_pp0_yuv420_nv12_p010);
REGISTER_TEST(pp0_yuv420_nv12_i010, "PP0 YUV420 NV12 I010 format test", test_pp0_yuv420_nv12_i010);
REGISTER_TEST(pp0_yuv420_nv12_l010, "PP0 YUV420 NV12 L010 format test", test_pp0_yuv420_nv12_l010);
REGISTER_TEST(pp0_yuv420_nv12_pack10, "PP0 YUV420 NV12 Pack10 format test", test_pp0_yuv420_nv12_pack10);
REGISTER_TEST(pp0_yuv420_8bit_nv12, "PP0 YUV420 8-bit NV12 format test", test_pp0_yuv420_8bit_nv12);
REGISTER_TEST(pp0_yuv420_nv21_p010_tiled, "PP0 YUV420 NV21 P010 Tiled-4x4 format test", test_pp0_yuv420_nv21_p010_tiled);
REGISTER_TEST(pp0_yuv420_nv21_i011, "PP0 YUV420 NV21 I011 format test", test_pp0_yuv420_nv21_i011);
REGISTER_TEST(pp0_yuv420_nv21_l010, "PP0 YUV420 NV21 L010 format test", test_pp0_yuv420_nv21_l010);
REGISTER_TEST(pp0_yuv420_p010, "PP0 YUV420 P010 format test", test_pp0_yuv420_p010);
REGISTER_TEST(pp0_yuv420_8bit_nv21, "PP0 YUV420 8-bit NV21 format test", test_pp0_yuv420_8bit_nv21);

// PP1格式测试用例 - RGB/ARGB/BGR/BGRA/BGRX 格式（18个）
REGISTER_TEST(pp1_argb2101010, "PP1: ARGB2101010 format test", test_pp1_argb2101010);
REGISTER_TEST(pp1_abgr2101010, "PP1: ABGR2101010 format test", test_pp1_abgr2101010);
REGISTER_TEST(pp1_abgr8888, "PP1: ABGR8888 format test", test_pp1_abgr8888);
REGISTER_TEST(pp1_argb8888, "PP1: ARGB8888 format test", test_pp1_argb8888);
REGISTER_TEST(pp1_bgr161616, "PP1: BGR161616 format test", test_pp1_bgr161616);
REGISTER_TEST(pp1_bgr888, "PP1: BGR888 format test", test_pp1_bgr888);
REGISTER_TEST(pp1_bgra2101010, "PP1: BGRA2101010 format test", test_pp1_bgra2101010);
REGISTER_TEST(pp1_bgra8888, "PP1: BGRA8888 format test", test_pp1_bgra8888);
REGISTER_TEST(pp1_bgrx8888, "PP1: BGRX8888 format test", test_pp1_bgrx8888);
REGISTER_TEST(pp1_rgb161616_planar, "PP1: RGB161616 planar format test", test_pp1_rgb161616_planar);
REGISTER_TEST(pp1_rgb161616, "PP1: RGB161616 format test", test_pp1_rgb161616);
REGISTER_TEST(pp1_rgb888_planar, "PP1: RGB888 planar format test", test_pp1_rgb888_planar);
REGISTER_TEST(pp1_rgb888, "PP1: RGB888 format test", test_pp1_rgb888);
REGISTER_TEST(pp1_rgba2101010, "PP1: RGBA2101010 format test", test_pp1_rgba2101010);
REGISTER_TEST(pp1_rgba8888, "PP1: RGBA8888 format test", test_pp1_rgba8888);
REGISTER_TEST(pp1_rgbx8888, "PP1: RGBX8888 format test", test_pp1_rgbx8888);
REGISTER_TEST(pp1_xrgb8888, "PP1: XRGB8888 format test", test_pp1_xrgb8888);
REGISTER_TEST(pp1_xbgr8888, "PP1: XBGR8888 format test", test_pp1_xbgr8888);

// PP1格式测试用例 - YUV 格式（15个）
REGISTER_TEST(pp1_yuv400_p010, "PP1: YUV400 P010 format test", test_pp1_yuv400_p010);
REGISTER_TEST(pp1_yuv400_i010, "PP1: YUV400 I010 format test", test_pp1_yuv400_i010);
REGISTER_TEST(pp1_yuv400_l010, "PP1: YUV400 L010 format test", test_pp1_yuv400_l010);
REGISTER_TEST(pp1_yuv400_pack10, "PP1: YUV400 Pack10 format test", test_pp1_yuv400_pack10);
REGISTER_TEST(pp1_yuv400_8bit, "PP1: YUV400 8-bit format test", test_pp1_yuv400_8bit);
REGISTER_TEST(pp1_yuv420_nv12_p010, "PP1: YUV420 NV12 P010 format test", test_pp1_yuv420_nv12_p010);
REGISTER_TEST(pp1_yuv420_nv12_i010, "PP1: YUV420 NV12 I010 format test", test_pp1_yuv420_nv12_i010);
REGISTER_TEST(pp1_yuv420_nv12_l010, "PP1: YUV420 NV12 L010 format test", test_pp1_yuv420_nv12_l010);
REGISTER_TEST(pp1_yuv420_nv12_pack10, "PP1: YUV420 NV12 Pack10 format test", test_pp1_yuv420_nv12_pack10);
REGISTER_TEST(pp1_yuv420_8bit_nv12, "PP1: YUV420 8-bit NV12 format test", test_pp1_yuv420_8bit_nv12);
REGISTER_TEST(pp1_yuv420_nv21_p010_tiled, "PP1: YUV420 NV21 P010 Tiled-4x4 format test", test_pp1_yuv420_nv21_p010_tiled);
REGISTER_TEST(pp1_yuv420_nv21_i010, "PP1: YUV420 NV21 I010 format test", test_pp1_yuv420_nv21_i010);
REGISTER_TEST(pp1_yuv420_nv21_l010, "PP1: YUV420 NV21 L010 format test", test_pp1_yuv420_nv21_l010);
REGISTER_TEST(pp1_yuv420_p010, "PP1: YUV420 P010 format test", test_pp1_yuv420_p010);
REGISTER_TEST(pp1_yuv420_8bit_nv21, "PP1: YUV420 8-bit NV21 format test", test_pp1_yuv420_8bit_nv21);

// 多PP测试用例（10个）
REGISTER_TEST(multi_pp_t01, "Multi-PP T01: PP0=YUV420 8-bit NV12, PP1=RGB888", test_multi_pp_t01);
REGISTER_TEST(multi_pp_t02, "Multi-PP T02: PP0=YUV420 8-bit NV12, PP1=ARGB8888", test_multi_pp_t02);
REGISTER_TEST(multi_pp_t03, "Multi-PP T03: PP0=YUV420 8-bit NV21, PP1=BGR888", test_multi_pp_t03);
REGISTER_TEST(multi_pp_t04, "Multi-PP T04: PP0=YUV420 8-bit NV12, PP1=RGB888 (8-bit)", test_multi_pp_t04);
REGISTER_TEST(multi_pp_t05, "Multi-PP T05: PP0=YUV420 P010 (10-bit), PP1=ARGB2101010 (10-bit)", test_multi_pp_t05);
REGISTER_TEST(multi_pp_t06, "Multi-PP T06: PP0=YUV420 I010 (10-bit), PP1=RGB161616 (16-bit)", test_multi_pp_t06);
REGISTER_TEST(multi_pp_t07, "Multi-PP T07: PP0=YUV400 8-bit, PP1=RGB888", test_multi_pp_t07);
REGISTER_TEST(multi_pp_t09, "Multi-PP T09: PP0=YUV420 8-bit NV12, PP1=RGB888 planar", test_multi_pp_t09);
REGISTER_TEST(multi_pp_t10, "Multi-PP T10: PP0=YUV420 NV21 P010 Tiled-4x4, PP1=ARGB8888", test_multi_pp_t10);
REGISTER_TEST(multi_pp_t11, "Multi-PP T11: PP0=YUV420 8-bit NV12, PP1=YUV420 8-bit NV21", test_multi_pp_t11);

// 多PP+裁剪测试用例（6个）
REGISTER_TEST(multi_pp_crop1, "Multi-PP+Crop1: PP0 crop 4096x2160 -> 1920x1080", test_multi_pp_crop1);
REGISTER_TEST(multi_pp_crop2, "Multi-PP+Crop2: PP0 down-scale 32768x32768 -> 1280x720", test_multi_pp_crop2);
REGISTER_TEST(multi_pp_crop3, "Multi-PP+Crop3: PP1 crop 4096x2160 -> 1920x1080", test_multi_pp_crop3);
REGISTER_TEST(multi_pp_crop4, "Multi-PP+Crop4: PP1 down-scale 32768x32768 -> 1280x720", test_multi_pp_crop4);
REGISTER_TEST(multi_pp_crop5, "Multi-PP+Crop5: PP0 down-scale 32768x32768 -> 256x256", test_multi_pp_crop5);
REGISTER_TEST(multi_pp_crop6, "Multi-PP+Crop6: PP1 down-scale 4096x2160 -> 128x128", test_multi_pp_crop6);

/**
 * @brief 主函数
 */
int main(int argc, char* argv[]) {
  // 初始化日志系统
  INIT_LOGGER();
  
  // 注册信号处理
  signal(SIGINT, [](int) { g_running = false; });
  signal(SIGTERM, [](int) { g_running = false; });
  
  // 使用测试框架主函数
  TEST_MAIN(argc, argv);
}
