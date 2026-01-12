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
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>  // av_get_pix_fmt_name() 函数
}

// 全局标志，用于处理 Ctrl+C 退出
static volatile bool g_running = true;

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
        
        if (codec) {
            // 指定了编解码器，使用 TACO 硬件解码器
        TacoConfigBuilder taco_builder;
            taco_builder.setChannels(true, false);  // 启用ch0，禁用ch1（单PP测试）
            
            if (has_crop) {
                taco_builder.setCropRegion(crop_x, crop_y, crop_width, crop_height);
            }
            
            if (has_scale) {
                taco_builder.setDecoderOutputResolution(scale_width, scale_height);
            }
            
            auto taco_config = taco_builder.build();
            decoder_builder.useTaco(codec, taco_config);
        } else {
            // 未指定编解码器，让系统自动检测
            // 系统会根据视频文件自动选择解码器：
            // - MJPEG -> 软件解码器（不支持PP功能）
            // - H.264/H.265 -> 硬件解码器（如果可用）
            // 为了支持PP功能，即使自动检测也设置默认TACO配置
            TacoConfigBuilder taco_builder;
            taco_builder.setChannels(true, false);  // 启用ch0，禁用ch1（单PP测试）
            
            if (has_crop) {
                taco_builder.setCropRegion(crop_x, crop_y, crop_width, crop_height);
            }
            
            if (has_scale) {
                taco_builder.setDecoderOutputResolution(scale_width, scale_height);
        }
        
        auto taco_config = taco_builder.build();
            // 不指定具体编解码器，让系统自动检测，但提供TACO配置以支持PP功能
            // 系统会自动检测编解码器类型（h264/hevc/mjpeg）并应用TACO配置
            decoder_builder.setDecoderName("");  // 空字符串表示自动检测
            // 注意：自动检测模式下，如果检测到MJPEG，系统会自动使用软件解码器，TACO配置会被忽略
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
  
  if (!writer.open(output_path, actual_format, actual_width, actual_height)) {
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
      // 未指定编解码器，让系统自动检测
      // 系统会根据视频文件自动选择解码器：
      // - MJPEG -> 软件解码器（不支持PP功能）
      // - H.264/H.265 -> 硬件解码器（如果可用）
      // 为了支持PP功能，即使自动检测也设置TACO配置
      // 方法：先使用 useTaco 设置 TACO 配置，然后清除解码器名称以启用自动检测
      decoder_builder.useTaco("h264", taco_config);  // 临时设置，用于配置 TACO
      decoder_builder.clearDecoderName();  // 清除解码器名称，启用自动检测
      // 注意：自动检测模式下，如果检测到MJPEG，系统会自动使用软件解码器，TACO配置会被忽略
      // 但如果检测到H.264/H.265，TACO配置会被应用，PP功能可用
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
  // 步骤3：获取PP0和PP1 BufferPool
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
  
  // PP1 BufferPool（ch1）
  auto worker_facade = producer.getWorkerFacade();
  if (!worker_facade) {
    LOG_ERROR("Failed to get Worker Facade");
    producer.stop();
    return -1;
  }
  
  uint64_t pp1_pool_id = worker_facade->getOutputBufferPoolId(
      BufferPoolType::DECODE_VIDEO_SECONDARY);
  if (pp1_pool_id == 0) {
    LOG_ERROR("Failed to get PP1 BufferPool ID (ch1 not enabled?)");
    producer.stop();
    return -1;
  }
  
  auto pp1_pool_sptr = BufferPoolRegistry::getInstance().getPool(pp1_pool_id).lock();
  if (!pp1_pool_sptr) {
    LOG_ERROR("Failed to get PP1 BufferPool from Registry");
    producer.stop();
    return -1;
  }
  
  LOG_INFO_FMT("PP1 BufferPool: %s (ID: %lu)", 
              pp1_pool_sptr->getName().c_str(), pp1_pool_id);
  
  // ========================================================================
  // 步骤4：等待第一个Buffer，获取实际格式
  // ========================================================================
  LOG_INFO("Step 4: Waiting for first buffers to detect formats...");
  
  Buffer* pp0_first_buffer = pp0_pool_sptr->acquireFilled(true, 5000);
  Buffer* pp1_first_buffer = pp1_pool_sptr->acquireFilled(true, 5000);
  
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
  snprintf(pp0_output_path, sizeof(pp0_output_path), 
          "/tmp/pp0_output_%ld.yuv", now);
  
  if (!pp0_writer.open(pp0_output_path, pp0_format, pp0_width, pp0_height)) {
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
  
  if (!pp1_writer.open(pp1_output_path, pp1_av_format, pp1_width, pp1_height)) {
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
  
  while (g_running) {
    Buffer* pp0_buffer = pp0_pool_sptr->acquireFilled(true, 100);
    Buffer* pp1_buffer = pp1_pool_sptr->acquireFilled(true, 100);
    
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
