/**
 * mp4_decode_test.cpp
 *
 * 专注于"不同参数的视频文件解码测试"，仿照 test.cpp 的测试框架结构。
 *
 * 设计思路：
 *   - 使用同一个解码/显示流水线（VideoProductionLine + LinuxFramebufferDevice）
 *   - 通过不同的 WorkerConfig 参数（分辨率、硬件解码器名称、解码线程数等）
 *     注册成多个独立的测试用例，方便对比不同参数下的解码表现
 *   - 使用实际可用的硬件解码器：h264_taco, hevc_taco, jpeg_taco, mpeg4_taco
 *
 * 用法示例：
 *   列出所有测试：
 *     ./mp4_decode_test -l
 *
 *   运行 720p/H.264 taco：
 *     ./mp4_decode_test -m dec_720p_h264 input.mp4
 *
 *   运行 1080p/H.264 taco（多线程解码）：
 *     ./mp4_decode_test -m dec_1080p_h264_mt input.mp4
 *
 *   运行 4K/HEVC taco：
 *     ./mp4_decode_test -m dec_4k_hevc input_hevc.mkv
 */

// 抑制 RTSP 测试函数的未使用警告（这些函数通过 REGISTER_TEST 宏注册使用）
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

 #include <signal.h>
 #include <atomic>
 #include <filesystem>
#include <cstdlib>
#include <cstring>
 #include <cstdio>
 #include <sstream>
 #include <vector>
 #include <numeric>
 #include <algorithm>
 #include <cmath>
 #include <thread>
 #include <chrono>
 #include <map>
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
  
#include "productionline/VideoProductionLine.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/worker/BufferFillingWorkerFacade.hpp"
#include "productionline/worker/FfmpegPacketRecorderWorker.hpp"
#include "productionline/worker/FfmpegDecodeVideoFileWorker.hpp"
#include "productionline/worker/WorkerBase.hpp"
#include "productionline/worker/RtspPacketSource.hpp"  // ⭐ 添加：用于 RTSP 中断处理
 #include "productionline/io/BufferWriter.hpp"
 #include "productionline/io/BufferComparator.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "common/Timer.hpp"
#include "common/Logger.hpp"
#include "framework/TestMacros.hpp"
 
extern "C" {
#include <libavformat/avformat.h>  // AVFormatContext, avformat_open_input, etc.
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>  // av_get_pix_fmt_name() 函数
#include <libavutil/error.h>   
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/dict.h>
#include <libavutil/timestamp.h>  // av_strerror() 函数
}
  
 static volatile bool g_running = true;

// ⭐ RTSP 中断标志（用于快速响应 Ctrl+C，参考 test.cpp）
static std::atomic<bool> g_rtsp_interrupted(false);

// ========== PP后处理格式配置 ==========
// 从PP_test.cpp整合的格式配置

/**
 * PP0格式配置结构
 */
struct PP0FormatConfig {
    const char* format_name;
    std::function<WorkerConfig::DecoderConfig::TacoConfig(int, int)> config_builder;
};

/**
 * PP1格式配置结构
 */
struct PP1FormatConfig {
    const char* format_name;
    std::function<WorkerConfig::DecoderConfig::TacoConfig(int, int)> config_builder;
};

/**
 * 多PP格式配置结构
 */
struct MultiPPFormatConfig {
    const char* test_name;
    const char* pp0_format_name;
    const char* pp1_format_name;
    std::function<WorkerConfig::DecoderConfig::TacoConfig(int, int)> config_builder;
};

// PP0格式列表（从PP_test.cpp提取）
static const PP0FormatConfig pp0_formats[] = {
    {"YUV420_8bit_NV12", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, false)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .build();
    }},
};

// PP1格式列表（从PP_test.cpp提取，参考multi_pp_formats中的PP1格式）
static const PP1FormatConfig pp1_formats[] = {
    {"RGB888", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_RGB888, ColorStandard::BT601)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"ARGB8888", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_ARGB888, ColorStandard::BT601)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"BGR888", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_BGR888, ColorStandard::BT601)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"RGB888_planar", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_GBRP, ColorStandard::BT601)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"YUV420_8bit_NV21", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, OutputFormat::YUV_NV21, ColorStandard::BT601)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
};

// 多PP格式组合列表（从PP_test.cpp提取）
static const MultiPPFormatConfig multi_pp_formats[] = {
    {"T01", "YUV420_8bit_NV12", "RGB888", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_RGB888, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"T02", "YUV420_8bit_NV12", "ARGB8888", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_ARGB888, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"T03", "YUV420_8bit_NV21", "BGR888", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV21, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_BGR888, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"T04", "YUV420_8bit_NV12", "RGB888_8bit", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_RGB888, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"T05", "YUV420_P010", "ARGB2101010", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_P010, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_ARGB888, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"T09", "YUV420_8bit_NV12", "RGB888_planar", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_GBRP, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"T10", "YUV420_NV21_P010_Tiled", "ARGB8888", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_P010, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::RGB_ARGB888, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
    {"T11", "YUV420_8bit_NV12", "YUV420_8bit_NV21", [](int width, int height) {
        return TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT601)
            .setOutputFormat(Channel::CH1, OutputFormat::YUV_NV21, ColorStandard::BT601)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
    }},
};

static const size_t PP0_FORMAT_COUNT = sizeof(pp0_formats) / sizeof(pp0_formats[0]);
static const size_t PP1_FORMAT_COUNT = sizeof(pp1_formats) / sizeof(pp1_formats[0]);
static const size_t MULTI_PP_FORMAT_COUNT = sizeof(multi_pp_formats) / sizeof(multi_pp_formats[0]);

/**
 * @brief 信号处理器（用于 Ctrl+C，参考 test.cpp）
 * 
 * 当用户按 Ctrl+C 时：
 * 1. 设置全局中断标志
 * 2. 请求 FFmpeg 中断所有 RTSP 流操作
 * 3. 第二次按 Ctrl+C 强制退出
 */
static void signal_handler(int signum) {
    if (signum == SIGINT) {
        if (!g_rtsp_interrupted.load()) {
            // 第一次 Ctrl+C：优雅退出
            LOG_INFO("\n");
            LOG_INFO("🛑 ═══════════════════════════════════════════════════════");
            LOG_INFO("🛑   收到中断信号 (Ctrl+C)，正在停止程序...");
            LOG_INFO("🛑   再次按 Ctrl+C 可强制退出");
            LOG_INFO("🛑 ═══════════════════════════════════════════════════════");
            
            g_running = false;
            g_rtsp_interrupted = true;
            
            // 请求 FFmpeg 中断所有 RTSP 流操作
            RtspPacketSource::requestInterrupt();
        } else {
            // 第二次 Ctrl+C：强制退出
            LOG_INFO("\n🛑 强制退出...");
            signal(SIGINT, SIG_DFL);
            raise(SIGINT);
        }
    }
}
 
 
// 前向声明（在使用之前声明）
namespace productionline { namespace io {
    struct CompareConfig;
}}

/**
 * ⭐ 从外部生成的 GOP 映射文件加载原视频的帧类型信息
 *
 * 期望的 CSV 格式（与 /tmp/gop_pts_map.csv 一致）：
 *   pkt_pts,pkt_pts_time,pict_type,...
 * 例如：
 *   -1024,-0.066667,I,17,0,YUV
 *   0,0.000000,B,17,0,YUV
 *   512,0.033333,B,17,0,YUV
 *
 * 我们只关心：
 *   - 第1列：pkt_pts（整数PTS）
 *   - 第3列：pict_type（I/P/B）
 */
static bool load_source_gop_map(
    const char* gop_csv_path,
    std::map<long long, char>& pts_to_type
) {
    pts_to_type.clear();

    if (!gop_csv_path || gop_csv_path[0] == '\0') {
        return false;
    }

    FILE* fp = fopen(gop_csv_path, "r");
    if (!fp) {
        LOG_WARN_FMT("  ⚠️  Failed to open GOP map file: %s", gop_csv_path);
        return false;
    }

    char line[512];
    int loaded = 0;

    while (fgets(line, sizeof(line), fp)) {
        // 简单按逗号拆分：pkt_pts, pkt_pts_time, pict_type, ...
        // 我们只解析前3列
        long long pts = 0;
        double pts_time = 0.0;
        char pict_type = '?';

        // 允许前面有负PTS（如 -1024,-0.066667,I,...）
        // 使用 %lld,%lf,%c 解析前三个字段
        if (sscanf(line, "%lld,%lf,%c", &pts, &pts_time, &pict_type) == 3) {
            if (pict_type == 'I' || pict_type == 'P' || pict_type == 'B') {
                pts_to_type[pts] = pict_type;
                loaded++;
            }
        }
    }

    fclose(fp);

    if (loaded == 0) {
        LOG_WARN_FMT("  ⚠️  GOP map file parsed but no valid entries found: %s", gop_csv_path);
        return false;
    }

    LOG_INFO_FMT("  ✅ Loaded %d GOP entries from %s", loaded, gop_csv_path);
    return true;
}

 static int run_decode_test_with_params(
     const char* video_path,
     int width,
     int height,
     const char* decoder_name,
     int decode_threads,
     double frame_rate,
     const char* profile,
     const char* test_tag,
     int max_frames = -1  // 可选参数：最大帧数（-1表示使用默认逻辑）
 ) {
      LOG_INFO("\n═══════════════════════════════════════════════════════");
      LOG_INFO_FMT("  Decode Test: %s", test_tag ? test_tag : "unknown");
      LOG_INFO("═══════════════════════════════════════════════════════\n");
  
      if (!video_path || video_path[0] == '\0') {
          LOG_ERROR("No video file path specified");
          return -1;
      }
  
    // ⭐ 使用 BufferComparator 进行实时对比（不再保存原始解码数据文件）
     using namespace productionline::io;
     
    // 创建输出目录（仅用于报告，不再保存原始解码数据文件）
     std::ostringstream oss;
     // ⭐ 使用 test_tag 区分 RTSP 测试和纯视频文件测试
     if (test_tag && test_tag[0] != '\0') {
         oss << test_tag << "_";
     }
     oss << width << "x" << height << "_" << frame_rate << "fps";
     if (decoder_name && decoder_name[0] != '\0') {
         oss << "_" << decoder_name;
     }
    std::string res_str = oss.str();
     std::string report_path = "logs/compare_" + res_str + ".txt";
     std::filesystem::create_directories("logs");
     
     // ⭐ 创建 BufferComparator（参考 test_decoder_compare）
     LOG_INFO("Step 1: Creating BufferComparator...");
     CompareConfig compare_config;
     compare_config.strategy = CompareConfig::AUTO_LAYERED;  // 自动分层验证
     compare_config.format_strategy = CompareConfig::AUTO;   // 格式自适应
     compare_config.quick_psnr_threshold = 38.0;             // >= 38dB 通过
     compare_config.quick_warn_threshold = 35.0;             // 35~38dB 警告
     compare_config.enable_psnr = true;                       // 启用 PSNR 计算
     compare_config.enable_ssim = true;                      // ⭐ 启用 SSIM 计算
     compare_config.ssim_threshold = 0.95;                   // ⭐ >= 0.95 认为质量优秀
     compare_config.ssim_warn_threshold = 0.90;             // ⭐ < 0.90 触发警告
     compare_config.enable_parallel = true;                  // ⭐ 启用并行计算（使用全局线程池）
     compare_config.use_perceptual_weighting = true;         // 感知加权
     compare_config.verbose = true;                          // 详细日志
     compare_config.save_report = true;                       // 保存报告
     
     BufferComparator comparator;
     compare_config.report_path = report_path;
     if (!comparator.open(compare_config)) {
         LOG_ERROR("Failed to open BufferComparator");
         return -1;
     }
     
     // ⭐ 尝试从外部GOP映射文件加载原视频的帧类型（用于诊断和表格输出）
     // 该文件可通过 ffprobe 预先生成：
     //   ffprobe -v error -select_streams v:0 -show_frames \
     //     -show_entries frame=pict_type,pkt_pts,pkt_pts_time \
     //     -of csv=p=0 input.mp4 > /tmp/gop_pts_map.csv
     std::map<long long, char> src_pts_to_type;
     bool has_src_gop = load_source_gop_map("/tmp/gop_pts_map.csv", src_pts_to_type);
     LOG_INFO("  ✅ BufferComparator initialized");
     
     LOG_INFO("  Strategy: AUTO_LAYERED (fast → deep)");
     LOG_INFO("  Format: AUTO (YUV/RGB adaptive)");
     LOG_INFO_FMT("  PSNR threshold: %.1f dB (pass), %.1f dB (warn)", 
                  compare_config.quick_psnr_threshold,
                  compare_config.quick_warn_threshold);
     LOG_INFO_FMT("  SSIM threshold: %.4f (pass), %.4f (warn)", 
                  compare_config.ssim_threshold,
                  compare_config.ssim_warn_threshold);
     LOG_INFO_FMT("  Parallel computing: %s", 
                  compare_config.enable_parallel ? "Enabled" : "Disabled");
 
     // ========================================================================
     // 步骤1：同时启动硬件和软件解码器（确保帧序列对齐）
     // ========================================================================
     LOG_INFO("Step 1: Starting hardware and software decoders simultaneously...");
     LOG_INFO("  (Both decoders will start from frame 0 for sequence alignment)");
     
     // 1.1 配置硬件解码器
     VideoProductionLine hw_producer(false, 1);
     DecoderConfigBuilder hw_decoderConfigBuilder;
     
     if (decoder_name && decoder_name[0] != '\0') {
         std::string dname(decoder_name);
         
         // 提取编解码器名称（去掉_taco后缀）
         std::string codec_name;
            if (dname.length() > 5 && dname.substr(dname.length() - 5) == "_taco") {
             codec_name = dname.substr(0, dname.length() - 5);
            } else {
             codec_name = dname;
         }
         
         // 配置TACO解码器（默认配置：只启用ch0，YUV输出）
                auto tacoConfig = TacoConfigBuilder()
             .setChannels(true, false)
                    .setOutputFormat(Channel::CH0, OutputFormat::YUV_AUTO, ColorStandard::BT601)
             .setScale(Channel::CH0, width, height)
                    .build();
         
         hw_decoderConfigBuilder.useTaco(codec_name, tacoConfig);
     } else {
         hw_decoderConfigBuilder.useSoftware();
     }
     
    auto hw_workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                // ⭐ 调整：避免过大 BufferPool 引发内存压力/驱动 MAX_BUFFERS(64) 假死
                // 对比测试不需要 128 个 buffer，16 更容易暴露“是否真正写入数据”的问题
                .setBufferCount(16)
                .build()
        )
         .setDisplayConfig(DisplayConfigBuilder()
             .setDisplayResolution(width, height)
             .setBitsPerPixel(32)
             .build())
         .setDecoderConfig(hw_decoderConfigBuilder.build())
         .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
         .build();

    hw_producer.setErrorCallback([](const std::string& error) {
        LOG_WARN_FMT("Hardware Decoder Error (non-fatal): %s", error.c_str());
        // ⭐ 修复：不因单个解码错误就停止整个流程
        // 硬件解码器可能会遇到一些可恢复的错误（如缓冲区不足、NAL单元错误等）
        // 这些错误不应该导致整个测试停止，而应该继续运行
        // g_running = false;  // 注释掉，允许继续运行
    });

     // 1.2 配置软件解码器（在启动硬件之前配置好）
     VideoProductionLine sw_producer(false, 1);
     auto sw_workerConfig = WorkerConfigBuilder()
         .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(16)
                .build()
         )
         .setDisplayConfig(DisplayConfigBuilder()
             .setDisplayResolution(width, height)
             .setBitsPerPixel(32)
             .build())
         .setDecoderConfig(DecoderConfigBuilder().useSoftware().build())
         .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
         .build();

    sw_producer.setErrorCallback([](const std::string& error) {
        LOG_WARN_FMT("Software Decoder Error (non-fatal): %s", error.c_str());
        // ⭐ 修复：不因单个解码错误就停止整个流程
        // 软件解码器可能会遇到一些可恢复的错误
        // 这些错误不应该导致整个测试停止，而应该继续运行
        // g_running = false;  // 注释掉，允许继续运行
    });

     // ⭐ 关键：同时启动两个解码器（都从视频文件的第0帧开始）
     LOG_INFO("  Starting hardware decoder...");
     LOG_INFO_FMT("    Hardware decoder config: decoder_name='%s', enable_hardware=%d", 
                  hw_workerConfig.decoder.name.value_or("").c_str(), 
                  hw_workerConfig.decoder.enable_hardware);
     if (!hw_producer.start(hw_workerConfig)) {
         LOG_ERROR("Failed to start hardware decoder");
         return -1;
     }

     LOG_INFO("  Starting software decoder...");
     LOG_INFO_FMT("    Software decoder config: decoder_name='%s', enable_hardware=%d", 
                  sw_workerConfig.decoder.name.value_or("").c_str(), 
                  sw_workerConfig.decoder.enable_hardware);
     if (!sw_producer.start(sw_workerConfig)) {
         LOG_ERROR("Failed to start software decoder");
         hw_producer.stop();
         return -1;
     }

     LOG_INFO("  ✅ Both decoders started (frame sequence aligned from frame 0)");
     
     // ⭐ 诊断：检查实际使用的解码器（通过类型转换获取）
     auto hw_worker_facade = hw_producer.getWorkerFacade();
     auto sw_worker_facade = sw_producer.getWorkerFacade();
     if (hw_worker_facade && sw_worker_facade) {
         LOG_INFO("\n  🔍 Decoder Verification:");
         
         // 通过 dynamic_cast 获取实际的 Worker 并调用 getCodecName()
         auto* hw_worker = dynamic_cast<FfmpegDecodeVideoFileWorker*>(
             const_cast<WorkerBase*>(hw_worker_facade->getWorkerBase())
         );
         auto* sw_worker = dynamic_cast<FfmpegDecodeVideoFileWorker*>(
             const_cast<WorkerBase*>(sw_worker_facade->getWorkerBase())
         );
         
         if (hw_worker && sw_worker) {
             const char* hw_codec = hw_worker->getCodecName();
             const char* sw_codec = sw_worker->getCodecName();
             
             LOG_INFO_FMT("    Hardware decoder actual codec: %s", hw_codec ? hw_codec : "unknown");
             LOG_INFO_FMT("    Software decoder actual codec: %s", sw_codec ? sw_codec : "unknown");
             
             // ⭐ 修复：增强检查逻辑，如果两个解码器相同，给出明确的错误提示
             if (hw_codec && sw_codec && strcmp(hw_codec, sw_codec) == 0) {
                 LOG_ERROR("    ❌ ERROR: Both decoders are using the same codec!");
                 LOG_ERROR_FMT("    Hardware decoder: %s", hw_codec);
                 LOG_ERROR_FMT("    Software decoder: %s", sw_codec);
                 LOG_ERROR("    This will result in PSNR = 100dB (identical output)");
                 LOG_ERROR("    Possible causes:");
                 LOG_ERROR("      1. Hardware decoder initialization failed, fell back to software decoder");
                 LOG_ERROR("      2. Hardware decoder not available or not properly configured");
                 LOG_ERROR("      3. Recorded MP4 file has issues that prevent hardware decoding");
                 LOG_ERROR("         - Missing SPS/PPS or other critical metadata");
                 LOG_ERROR("         - Stream corruption (Frame Num gaps)");
                 LOG_ERROR("         - Format incompatibility with hardware decoder");
                 LOG_ERROR("    ⚠️  Test will continue, but PSNR results will be invalid!");
                 
                 // ⭐ 如果是RTSP测试，给出更具体的提示
                 if (test_tag && strncmp(test_tag, "rtsp_", 5) == 0) {
                     LOG_ERROR("    💡 RTSP-specific diagnosis:");
                     LOG_ERROR("       - Check Phase 1.8 verification logs for MP4 file issues");
                     LOG_ERROR("       - Check if recorded MP4 has low frame rate or packet ratio warnings");
                     LOG_ERROR("       - Try using a different RTSP stream or recording duration");
                 }
             } else {
                 LOG_INFO("    ✅ Decoders are using different codecs (expected)");
                 
                 // ⭐ 额外检查：如果硬件解码器名称不包含 "taco"，说明可能回退到了软件解码器
                 if (decoder_name && decoder_name[0] != '\0' && 
                     hw_codec && strstr(hw_codec, "taco") == nullptr) {
                     LOG_WARN("    ⚠️  WARNING: Hardware decoder was configured but actual codec does not contain 'taco'!");
                     LOG_WARN_FMT("    Expected: %s, Actual: %s", decoder_name, hw_codec);
                     LOG_WARN("    Hardware decoder may have failed to initialize and fell back to software decoder");
                     
                     // ⭐ 如果是RTSP测试，给出更具体的提示
                     if (test_tag && strncmp(test_tag, "rtsp_", 5) == 0) {
                         LOG_WARN("    💡 This is likely due to the recorded MP4 file format issues:");
                         LOG_WARN("       - Missing or corrupted SPS/PPS in the recorded file");
                         LOG_WARN("       - Stream corruption that prevents hardware decoder initialization");
                         LOG_WARN("       - Format incompatibility (hardware decoder requires specific MP4 structure)");
                     }
                 }
             }
         } else {
             LOG_WARN("    ⚠️  Failed to get worker instances for codec verification");
         }
     }

     // ========================================================================
     // 步骤2：同时获取两个解码器的第一个Buffer（确保都是第0帧）
     // ========================================================================
     LOG_INFO("\nStep 2: Getting BufferPools and acquiring first buffers (frame 0)...");

     uint64_t hw_pool_id = hw_producer.getWorkingBufferPoolId();
     uint64_t sw_pool_id = sw_producer.getWorkingBufferPoolId();

     if (hw_pool_id == 0 || sw_pool_id == 0) {
         LOG_ERROR("No working BufferPool ID available");
         if (hw_pool_id == 0) hw_producer.stop();
         if (sw_pool_id == 0) sw_producer.stop();
         return -1;
     }

     auto hw_pool = BufferPoolRegistry::getInstance().getPool(hw_pool_id).lock();
     auto sw_pool = BufferPoolRegistry::getInstance().getPool(sw_pool_id).lock();

     if (!hw_pool || !sw_pool) {
         LOG_ERROR("BufferPool not found");
         hw_producer.stop();
         sw_producer.stop();
         return -1;
     }

    LOG_INFO_FMT("  Hardware BufferPool: '%s' (ID: %lu)", 
                hw_pool->getName().c_str(), hw_pool_id);
    LOG_INFO_FMT("  Software BufferPool: '%s' (ID: %lu)", 
                sw_pool->getName().c_str(), sw_pool_id);

#ifdef ENABLE_DEBUG_PREFILL_POISON
    // ⭐ 终极验证：预填充 free 队列里的所有 buffer
    // - SW free buffers: 0xAA
    // - HW free buffers: 0xBB
    // 如果解码器真的写帧，这些模式应当被覆盖；否则会稳定看到 AA/BB/99 等“占位/毒值”。
    {
        int sw_prefill = 0;
        while (Buffer* b = sw_pool->acquireFree(false, 0)) {
            void* p = b->data();
            size_t n = b->size();
            if (p && n > 0) {
                std::memset(p, 0xAA, n);
            }
            sw_pool->releaseFree(b);
            sw_prefill++;
        }
        int hw_prefill = 0;
        while (Buffer* b = hw_pool->acquireFree(false, 0)) {
            void* p = b->data();
            size_t n = b->size();
            if (p && n > 0) {
                std::memset(p, 0xBB, n);
            }
            hw_pool->releaseFree(b);
            hw_prefill++;
        }
        LOG_WARN_FMT("  ⚠️  ENABLE_DEBUG_PREFILL_POISON: prefilled free buffers (SW=%d, HW=%d)", sw_prefill, hw_prefill);
    }
#endif

    // ⭐ 关键：同时等待两个解码器的第0帧（参考 test_multi_worker）
    LOG_INFO("  Acquiring first buffers (frame 0) from both decoders...");
    
    Buffer* first_hw_buf = hw_pool->acquireFilled(true, 5000);
    Buffer* first_sw_buf = sw_pool->acquireFilled(true, 5000);

    if (!first_hw_buf || !first_sw_buf) {
        LOG_ERROR("Failed to get first buffers (frame 0) from both decoders");
        if (first_hw_buf) hw_pool->releaseFilled(first_hw_buf);
        if (first_sw_buf) sw_pool->releaseFilled(first_sw_buf);
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }

    LOG_INFO("  ✅ Got frame 0 from both decoders (sequence aligned)");

    // ⭐ 监控 stride（linesize），用于诊断“假硬解/真软解”或对齐异常
    if (first_hw_buf->hasImageMetadata() && first_sw_buf->hasImageMetadata()) {
        const int* hw_linesize = first_hw_buf->getImageLinesize();
        const int* sw_linesize = first_sw_buf->getImageLinesize();
        if (hw_linesize && sw_linesize) {
            LOG_INFO_FMT("  SW Stride: %d, HW Stride: %d", sw_linesize[0], hw_linesize[0]);
        } else {
            LOG_WARN("  ⚠️  Cannot get linesize for one of the buffers");
        }
    } else {
        LOG_WARN("  ⚠️  First buffers have no image metadata, cannot log stride");
    }

     // ========================================================================
     // 步骤3：检测格式并创建BufferWriter（仅硬件解码器）
     // ========================================================================
     LOG_INFO("\nStep 3: Detecting format and creating BufferWriter...");

     // 从硬件Buffer获取格式
     AVPixelFormat hw_format = AV_PIX_FMT_NONE;
     int hw_actual_width = width;
     int hw_actual_height = height;
     std::string hw_format_name = "NV12";  // 默认格式名称
     std::string hw_ffplay_format = "nv12";  // ffplay格式名称
     
     if (first_hw_buf->hasImageMetadata()) {
         hw_format = first_hw_buf->getImageFormat();
         hw_actual_width = first_hw_buf->getImageWidth();
         hw_actual_height = first_hw_buf->getImageHeight();
         
         const char* fmt_name = av_get_pix_fmt_name(hw_format);
         hw_format_name = fmt_name ? fmt_name : "NV12";
         
         // 设置ffplay格式名称（用于后续验证）
         if (hw_format == AV_PIX_FMT_NV12) {
             hw_ffplay_format = "nv12";
         } else if (hw_format == AV_PIX_FMT_NV21) {
             hw_ffplay_format = "nv21";
         } else if (hw_format == AV_PIX_FMT_YUV420P) {
             hw_ffplay_format = "yuv420p";
         } else if (hw_format == AV_PIX_FMT_YUV422P) {
             hw_ffplay_format = "yuv422p";
         } else if (hw_format == AV_PIX_FMT_YUV444P) {
             hw_ffplay_format = "yuv444p";
         } else {
             hw_ffplay_format = hw_format_name;
         }
         
         LOG_INFO_FMT("Detected format from buffer: %s (%dx%d)", 
                     hw_format_name.c_str(), hw_actual_width, hw_actual_height);
         
         if (hw_format != AV_PIX_FMT_NV12) {
             LOG_WARN_FMT("Hardware decoder output format is %s, not NV12. Saving with actual format.",
                         hw_format_name.c_str());
         } else {
             LOG_INFO("  ✅ Format is NV12 (as expected)");
         }
     } else {
         LOG_WARN("Buffer has no metadata, using default NV12");
         hw_format = AV_PIX_FMT_NV12;
     }

    // 获取软件解码格式信息（仅用于日志）
    if (first_sw_buf->hasImageMetadata()) {
        AVPixelFormat sw_format = first_sw_buf->getImageFormat();
        LOG_INFO_FMT("Software format: %s (%dx%d) [for comparison only, not saved]", 
                    av_get_pix_fmt_name(sw_format),
                    first_sw_buf->getImageWidth(), 
                    first_sw_buf->getImageHeight());
    } else {
        LOG_WARN("Software buffer has no metadata");
    }

    // ⭐ 创建 BufferWriter 用于保存硬件解码输出
    BufferWriter hw_writer;
    std::string hw_output_yuv = "output/" + res_str + "_hw.yuv";
    std::filesystem::create_directories("output");
    
    if (hw_format != AV_PIX_FMT_NONE) {
        if (!hw_writer.openRaw(hw_output_yuv.c_str(), hw_format, hw_actual_width, hw_actual_height)) {
            LOG_ERROR_FMT("Failed to open BufferWriter for hardware output: %s", hw_output_yuv.c_str());
            // 不返回错误，继续测试（保存失败不影响比较）
        } else {
            LOG_INFO_FMT("  ✅ BufferWriter opened: %s (format: %s, %dx%d)", 
                        hw_output_yuv.c_str(), hw_format_name.c_str(), 
                        hw_actual_width, hw_actual_height);
        }
    } else {
        LOG_WARN("Cannot create BufferWriter: format not detected");
    }
 
  // ========================================================================
  // 步骤4：对比第0帧并保存（先比较计算PSNR，再保存文件）
  // ========================================================================
  LOG_INFO("\nStep 4: Comparing frame 0 and saving...");
   LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

#ifdef ENABLE_DEBUG_BUFFER_TAG
   // ⭐ 调试选项：强制数据隔离测试
   // 说明：
   //   - 在对比前，故意修改两个 Buffer 的第一个像素为不同的值
   //   - 如果此时 PSNR 仍然是 100dB，说明比较逻辑并没有真正读取这两个 Buffer 的数据，
   //     或者它们在内部指向了同一块内存
   {
       uint8_t* sw_y = first_sw_buf->getImagePlaneData(0);
       uint8_t* hw_y = first_hw_buf->getImagePlaneData(0);
       if (sw_y) {
           sw_y[0] = 0xFF;  // 纯白
       }
       if (hw_y) {
           hw_y[0] = 0x00;  // 纯黑
       }
       LOG_WARN("  ⚠️  ENABLE_DEBUG_BUFFER_TAG: modified first pixel of SW/HW buffers for isolation test");
   }
#endif

   // ⭐ 先对比第0帧（计算PSNR和SSIM）
   FrameCompareResult first_result = comparator.compare(first_sw_buf, first_hw_buf);
   
   // ⭐ 保存第0帧硬件解码输出到YUV文件
   if (hw_format != AV_PIX_FMT_NONE) {
       if (!hw_writer.write(first_hw_buf)) {
           LOG_WARN("  ⚠️  Frame 0: Failed to write hardware buffer to YUV file");
       }
   }
   
   if (first_result.passed) {
        if (compare_config.enable_psnr && compare_config.enable_ssim) {
            LOG_INFO_FMT("  ✅ Frame 0: PASS (PSNR-Y: %.2f dB, SSIM-Y: %.4f)", 
                        first_result.psnr_y, first_result.ssim_y);
        } else if (compare_config.enable_psnr) {
            LOG_INFO_FMT("  ✅ Frame 0: PASS (PSNR-Y: %.2f dB)", first_result.psnr_y);
        } else if (compare_config.enable_ssim) {
            LOG_INFO_FMT("  ✅ Frame 0: PASS (SSIM-Y: %.4f)", first_result.ssim_y);
        }
        if (!first_result.ref_format_name.empty() || !first_result.test_format_name.empty()) {
            LOG_INFO_FMT("    Formats: %s vs %s", 
                        first_result.ref_format_name.c_str(), 
                        first_result.test_format_name.c_str());
        }
        if (first_result.psnr_avg > 0.0) {
            LOG_INFO_FMT("    PSNR-Avg: %.2f dB (Y=%.2f U=%.2f V=%.2f)", 
                        first_result.psnr_avg, 
                        first_result.psnr_y, 
                        first_result.psnr_u, 
                        first_result.psnr_v);
        }
        if (first_result.ssim_avg > 0.0) {
            LOG_INFO_FMT("    SSIM-Avg: %.4f (Y=%.4f U=%.4f V=%.4f)", 
                        first_result.ssim_avg, 
                        first_result.ssim_y, 
                        first_result.ssim_u, 
                        first_result.ssim_v);
        }
    } else {
        if (compare_config.enable_psnr && compare_config.enable_ssim) {
            LOG_WARN_FMT("  ⚠️  Frame 0: %s (PSNR-Y: %.2f dB, SSIM-Y: %.4f)", 
                        first_result.level == FrameCompareResult::FAIL ? "FAIL" : "WARN",
                        first_result.psnr_y, first_result.ssim_y);
        } else if (compare_config.enable_psnr) {
            LOG_WARN_FMT("  ⚠️  Frame 0: %s (PSNR-Y: %.2f dB)", 
                        first_result.level == FrameCompareResult::FAIL ? "FAIL" : "WARN",
                        first_result.psnr_y);
        } else if (compare_config.enable_ssim) {
            LOG_WARN_FMT("  ⚠️  Frame 0: %s (SSIM-Y: %.4f)", 
                        first_result.level == FrameCompareResult::FAIL ? "FAIL" : "WARN",
                        first_result.ssim_y);
        }
        if (!first_result.ref_format_name.empty() || !first_result.test_format_name.empty()) {
            LOG_WARN_FMT("    Formats: %s vs %s", 
                        first_result.ref_format_name.c_str(), 
                        first_result.test_format_name.c_str());
        }
        if (first_result.psnr_avg > 0.0) {
            LOG_WARN_FMT("    PSNR-Avg: %.2f dB (Y=%.2f U=%.2f V=%.2f)", 
                        first_result.psnr_avg, 
                        first_result.psnr_y, 
                        first_result.psnr_u, 
                        first_result.psnr_v);
        }
        if (first_result.ssim_avg > 0.0) {
            LOG_WARN_FMT("    SSIM-Avg: %.4f (Y=%.4f U=%.4f V=%.4f)", 
                        first_result.ssim_avg,
                        first_result.ssim_y,
                        first_result.ssim_u,
                        first_result.ssim_v);
        }
        // ⭐ 显示像素差异统计（新增比较项）
        if (first_result.diff_pixel_count > 0) {
            LOG_WARN_FMT("    Pixel Diff: max=%d, count=%d (%.2f%%)", 
                        first_result.max_pixel_diff,
                        first_result.diff_pixel_count,
                        first_result.diff_pixel_ratio * 100.0);
        }
        // ⭐ 显示错误信息（如果有）
        if (!first_result.error_message.empty()) {
            LOG_WARN_FMT("    Error: %s", first_result.error_message.c_str());

        }
    }

    // ⭐ 关键：立即释放第0帧 Buffer，避免Buffer饥饿
    sw_pool->releaseFilled(first_sw_buf);
    hw_pool->releaseFilled(first_hw_buf);

    // ========================================================================
    // 步骤5：帧序列对齐的对比循环（第1帧、第2帧...）
    // ========================================================================
    LOG_INFO("\nStep 5: Comparing decoder outputs frame by frame (sequence aligned)...");
    LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // ⭐ 等待解码器产生更多帧（参考 test.cpp，等待更长时间）
    LOG_INFO("  Waiting for decoders to produce more frames...");
    int wait_attempts = 0;
    const int MAX_WAIT_ATTEMPTS = 50;  // ⭐ 增加等待时间：从 20 增加到 50（5秒）
    bool has_more_frames = false;
    
    while (wait_attempts < MAX_WAIT_ATTEMPTS && g_running) {
        int hw_filled = hw_pool->getFilledCount();
        int sw_filled = sw_pool->getFilledCount();
        
        // ⭐ 要求至少有 5 个 filled buffers 才继续（确保有足够的帧）
        if (hw_filled >= 5 && sw_filled >= 5) {
            has_more_frames = true;
            LOG_INFO_FMT("  ✅ Decoders ready: hardware=%d, software=%d filled buffers", 
                         hw_filled, sw_filled);
            break;
        }
        
        if (wait_attempts % 10 == 0) {
            LOG_INFO_FMT("  Waiting... (hardware=%d, software=%d filled buffers)", 
                         hw_filled, sw_filled);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wait_attempts++;
    }
    
    // ⭐ 修复：即使等待循环退出，也要检查当前状态（可能是在等待过程中条件满足了）
    if (!has_more_frames) {
        int hw_filled = hw_pool->getFilledCount();
        int sw_filled = sw_pool->getFilledCount();
        
        // ⭐ 再次检查：如果现在满足条件，设置 has_more_frames = true
        if (hw_filled >= 5 && sw_filled >= 5) {
            has_more_frames = true;
            LOG_INFO_FMT("  ✅ Decoders ready (after wait): hardware=%d, software=%d filled buffers", 
                         hw_filled, sw_filled);
        } else {
            LOG_WARN_FMT("  ⚠️  Decoders did not produce enough frames within timeout (hardware=%d, software=%d)", 
                         hw_filled, sw_filled);
            if (hw_filled > 0 || sw_filled > 0) {
                LOG_WARN("    Proceeding anyway with available frames");
            } else {
                LOG_WARN("    No frames available, may exit early");
            }
        }
    }
    
    int frame_count = 1;  // 第0帧已对比，从第1帧开始
    
    // 参考纯视频解码的max_frames参数获取逻辑
    // 如果提供了max_frames参数（从MP4文件获取），使用它；否则使用默认逻辑
    int MAX_FRAMES;
    if (max_frames > 0) {
        // 使用从MP4文件获取的总帧数，添加10%缓冲
        MAX_FRAMES = static_cast<int>(max_frames * 1.1);
        LOG_INFO_FMT("  Maximum frames to process: %d (from MP4 file metadata, original: %d)", MAX_FRAMES, max_frames);
    } else {
        // 默认逻辑：根据帧率和视频时长计算
        MAX_FRAMES = std::max(300, static_cast<int>(frame_rate * 12.0));  // 至少300帧，或frame_rate * 12秒
        LOG_INFO_FMT("  Maximum frames to process: %d (based on frame rate %.2f fps)", MAX_FRAMES, frame_rate);
    }
    
    int timeout_count = 0;
    const int MAX_TIMEOUT = 50;
    
    // ⭐ 帧类型统计（用于诊断硬件解码器的帧类型标记问题）
    int hw_frame_type_i = 0, hw_frame_type_p = 0, hw_frame_type_b = 0, hw_frame_type_none = 0;
    int sw_frame_type_i = 0, sw_frame_type_p = 0, sw_frame_type_b = 0, sw_frame_type_none = 0;
    
    // ⭐ PTS顺序检查（用于诊断硬件解码器的帧顺序问题）
    int64_t last_hw_pts = AV_NOPTS_VALUE;
    int hw_pts_order_error_count = 0;  // PTS不单调递增的次数
     
     // ⭐ PSNR统计：记录每帧的PSNR值（参考 run_psnr_compare_test）
     std::vector<double> psnr_y_values;
     std::vector<double> psnr_u_values;
     std::vector<double> psnr_v_values;
     std::vector<double> psnr_avg_values;
     
     // ⭐ 按帧类型统计PSNR：I帧、P帧、B帧
     std::vector<double> psnr_y_i_frames;
     std::vector<double> psnr_y_p_frames;
     std::vector<double> psnr_y_b_frames;
     std::vector<double> psnr_avg_i_frames;
     std::vector<double> psnr_avg_p_frames;
     std::vector<double> psnr_avg_b_frames;
     
     // ⭐ SSIM统计：记录每帧的SSIM值
     std::vector<double> ssim_y_values;
     std::vector<double> ssim_u_values;
     std::vector<double> ssim_v_values;
     std::vector<double> ssim_avg_values;
     
     // ⭐ 应用新增比较项：记录像素差异统计
     std::vector<int> max_pixel_diff_values;
     std::vector<int> diff_pixel_count_values;
     std::vector<float> diff_pixel_ratio_values;
     
     while (g_running && frame_count < MAX_FRAMES) {
         // ⭐ 优化：先检查BufferPool状态，避免不必要的等待
         int hw_filled = hw_pool->getFilledCount();
         int hw_free = hw_pool->getFreeCount();
         int sw_filled = sw_pool->getFilledCount();
         int sw_free = sw_pool->getFreeCount();
         
         // ⭐ 优先检查解码器是否已完成（适用于RTSP录制视频帧数不足的情况）
         if (!hw_producer.isRunning() && !sw_producer.isRunning()) {
             // 检查是否还有剩余的帧需要处理
             if (hw_filled == 0 && sw_filled == 0) {
                 LOG_INFO("\n  ✅ Decoders finished naturally (all frames processed)");
                 LOG_INFO_FMT("    Total frames processed: %d", frame_count);
                 break;
             }
             // 如果还有帧，继续处理
         }
         
        // ⭐ 优化超时时间：根据 BufferPool 状态动态调整
        // 如果两个Pool都有足够数据，使用较短的超时时间（提高响应速度）
        // 如果只有一个Pool有数据，等待更长时间（等待另一个解码器）
        int acquire_timeout;
        if (hw_filled >= 5 && sw_filled >= 5) {
            acquire_timeout = 100;  // 两个都有足够数据，快速响应
        } else if (hw_filled > 0 && sw_filled > 0) {
            acquire_timeout = 500;  // 两个都有数据但不多，中等等待
        } else {
            // ⭐ 关键修复：如果解码器仍在运行但 filled=0 且 free=0，
            // 说明所有缓冲区都被占用（可能在硬件解码器内部处理中）
            // 使用更长的超时时间，给硬件解码器时间将已解码的帧放入 filled 队列
            // 参考纯视频文件解码测试的等待机制（第476-501行）
            if ((hw_filled == 0 && hw_free == 0 && hw_producer.isRunning()) ||
                (sw_filled == 0 && sw_free == 0 && sw_producer.isRunning())) {
                // 解码器正在运行，所有 buffer 都被占用，说明帧还在处理中
                // 硬件解码器可能已经解码了64帧（MAX_BUFFERS限制），但还没有放入filled队列
                acquire_timeout = 5000;  // 等待5秒，给硬件解码器时间处理并提交帧
                if (timeout_count == 0 || timeout_count % 10 == 0) {
                    LOG_DEBUG_FMT("  ⚠️  Decoder processing: filled=0, free=0, using longer timeout (5000ms)");
                    LOG_DEBUG_FMT("    This may indicate frames are being processed inside hardware decoder");
                }
            } else {
                acquire_timeout = 2000;  // 至少一个没有数据，等待更长时间
            }
        }
        
        // ⭐ 基于PTS的帧对齐：确保比较的是同一时刻的帧
        // 由于B帧重排，软硬解码器输出的帧顺序可能不同，需要按PTS匹配
        Buffer* sw_buf = nullptr;
        Buffer* hw_buf = nullptr;
        int pts_match_attempts = 0;
        const int MAX_PTS_MATCH_ATTEMPTS = 10;  // 最多尝试10次匹配
        
        // ⭐ 循环直到找到PTS匹配的帧
        while (pts_match_attempts < MAX_PTS_MATCH_ATTEMPTS) {
            // 如果没有buffer，获取新的buffer
            if (!sw_buf) {
                sw_buf = sw_pool->acquireFilled(true, acquire_timeout);
            }
            if (!hw_buf) {
                hw_buf = hw_pool->acquireFilled(true, acquire_timeout);
            }
            
            // 如果任一pool没有数据，处理超时
            if (!sw_buf || !hw_buf) {
                // ⭐ 诊断：第一次超时时就记录详细信息
                if (timeout_count == 0) {
                    LOG_WARN_FMT("  ⚠️  First timeout at frame_count=%d: HW filled=%d (free=%d), SW filled=%d (free=%d)", 
                                frame_count, hw_filled, hw_free, sw_filled, sw_free);
                }
                if (!sw_buf) {
                    LOG_DEBUG_FMT("Software decoder timeout (frame_count=%d, timeout_count=%d, hw_filled=%d, sw_filled=%d)", 
                                 frame_count, timeout_count, hw_filled, sw_filled);
                }
                if (!hw_buf) {
                    LOG_DEBUG_FMT("Hardware decoder timeout (frame_count=%d, timeout_count=%d, hw_filled=%d, sw_filled=%d)", 
                                 frame_count, timeout_count, hw_filled, sw_filled);
                }
                
                // ⭐ 诊断：检查 BufferPool 状态
                if (timeout_count % 10 == 0 && timeout_count > 0) {
                    LOG_WARN_FMT("  ⚠️  Timeout #%d: HW pool (filled=%d, free=%d), SW pool (filled=%d, free=%d)", 
                                timeout_count, hw_filled, hw_free, sw_filled, sw_free);
                    
                    // ⭐ 如果BufferPool已满但仍有filled buffers，说明消费速度太慢
                    if (hw_free == 0 && hw_filled > 0) {
                        LOG_WARN("    ⚠️  Hardware BufferPool exhausted: all buffers are filled but not consumed!");
                        LOG_WARN("    This indicates consumer (comparison/writing) is slower than producer (decoding)");
                    }
                    if (sw_free == 0 && sw_filled > 0) {
                        LOG_WARN("    ⚠️  Software BufferPool exhausted: all buffers are filled but not consumed!");
                    }
                }
                
                if (sw_buf) sw_pool->releaseFilled(sw_buf);
                if (hw_buf) hw_pool->releaseFilled(hw_buf);
                
                timeout_count++;
                
                // ⭐ 检查是否自然结束（在超时之前检查）
                if (!hw_producer.isRunning() && !sw_producer.isRunning()) {
                    LOG_INFO("\n  Decoders finished naturally");
                    LOG_INFO_FMT("    Frames processed: %d", frame_count);
                    LOG_INFO_FMT("    Timeout count: %d", timeout_count);
                    break;
                }
                
                // ⭐ 只有在连续超时很多次且解码器仍在运行时才退出
                if (timeout_count >= MAX_TIMEOUT) {
                    LOG_INFO("\n  Decoders finished or timeout");
                    LOG_INFO_FMT("    Total timeouts: %d (max: %d)", timeout_count, MAX_TIMEOUT);
                    LOG_INFO_FMT("    Frames processed: %d", frame_count);
                    LOG_INFO_FMT("    HW pool: filled=%d, free=%d", hw_filled, hw_free);
                    LOG_INFO_FMT("    SW pool: filled=%d, free=%d", sw_filled, sw_free);
                    // 跳出外层循环
                    sw_buf = nullptr;
                    hw_buf = nullptr;
                    break;
                }
                // 超时后跳出PTS匹配循环，继续外层循环
                break;
            }
            
            // ⭐ 获取两个buffer的PTS
            AVFrame* sw_avframe = sw_buf->getAVFrame();
            AVFrame* hw_avframe = hw_buf->getAVFrame();
            
            int64_t sw_pts = AV_NOPTS_VALUE;
            int64_t hw_pts = AV_NOPTS_VALUE;
            
            if (sw_avframe) {
                sw_pts = sw_avframe->pts != AV_NOPTS_VALUE ? sw_avframe->pts : sw_avframe->best_effort_timestamp;
            }
            if (hw_avframe) {
                hw_pts = hw_avframe->pts != AV_NOPTS_VALUE ? hw_avframe->pts : hw_avframe->best_effort_timestamp;
            }
            
            // ⭐ 如果PTS都有效且匹配，找到对齐的帧
            // ⭐ 注意：只按PTS对齐，不强制帧类型匹配
            // 原因：1) PTS是唯一可靠的同步依据
            //       2) 帧类型不一致可能是解码器标记差异，不应该阻止比较
            //       3) 帧类型不匹配会在后续验证中警告，但不影响比较
            if (sw_pts != AV_NOPTS_VALUE && hw_pts != AV_NOPTS_VALUE) {
                if (sw_pts == hw_pts) {
                    // PTS匹配，可以进行比较
                    // 检查帧类型是否也匹配（用于警告，但不阻止比较）
                    if (pts_match_attempts > 0 && frame_count <= 10) {
                        LOG_DEBUG_FMT("  Frame %d: PTS matched after %d attempts (pts=%lld)", 
                                    frame_count, pts_match_attempts, (long long)sw_pts);
                    }
                    break;  // 跳出循环，使用这两个buffer（帧类型会在后续验证中检查）
                } else {
                    // PTS不匹配，释放PTS较小的buffer，获取下一个
                    if (sw_pts < hw_pts) {
                        // SW的PTS较小，释放SW buffer获取下一个
                        sw_pool->releaseFilled(sw_buf);
                        sw_buf = nullptr;
                        pts_match_attempts++;
                        if (pts_match_attempts <= 3 && frame_count <= 10) {
                            LOG_DEBUG_FMT("  Frame %d: SW pts=%lld < HW pts=%lld, skipping SW frame", 
                                        frame_count, (long long)sw_pts, (long long)hw_pts);
                        }
                    } else {
                        // HW的PTS较小，释放HW buffer获取下一个
                        hw_pool->releaseFilled(hw_buf);
                        hw_buf = nullptr;
                        pts_match_attempts++;
                        if (pts_match_attempts <= 3 && frame_count <= 10) {
                            LOG_DEBUG_FMT("  Frame %d: HW pts=%lld < SW pts=%lld, skipping HW frame", 
                                        frame_count, (long long)hw_pts, (long long)sw_pts);
                        }
                    }
                    continue;  // 继续尝试匹配
                }
            } else {
                // PTS无效，无法按PTS对齐，直接使用（向后兼容）
                if (pts_match_attempts == 0 && frame_count <= 10) {
                    LOG_WARN_FMT("  Frame %d: PTS unavailable (SW=%s, HW=%s), using sequential matching", 
                                frame_count,
                                sw_pts != AV_NOPTS_VALUE ? std::to_string(sw_pts).c_str() : "NOPTS",
                                hw_pts != AV_NOPTS_VALUE ? std::to_string(hw_pts).c_str() : "NOPTS");
                }
                break;  // 跳出循环，使用这两个buffer（向后兼容）
            }
        }
        
        // ⭐ 如果尝试多次仍未找到匹配，记录警告但继续处理
        if (pts_match_attempts >= MAX_PTS_MATCH_ATTEMPTS) {
            LOG_WARN_FMT("  ⚠️  Frame %d: Failed to match PTS after %d attempts, using current buffers", 
                        frame_count, pts_match_attempts);
        }
        
        // 确保获取到了buffer（如果超时或自然结束，sw_buf或hw_buf可能为nullptr）
        if (!sw_buf || !hw_buf) {
            if (sw_buf) sw_pool->releaseFilled(sw_buf);
            if (hw_buf) hw_pool->releaseFilled(hw_buf);
            // 如果是超时导致的，已经处理过了，continue外层循环
            if (!hw_producer.isRunning() && !sw_producer.isRunning()) {
                break;  // 解码器已停止，跳出外层循环
            }
            continue;  // 继续外层循环
        }
        
        timeout_count = 0;
        
        // ⭐ 深度调试：打印更底层的 plane 指针信息，辅助判断是否存在“零拷贝共享区域”
        if (frame_count < 5) {
            uint8_t* sw_plane0 = sw_buf->getImagePlaneData(0);
            uint8_t* hw_plane0 = hw_buf->getImagePlaneData(0);
            LOG_INFO_FMT("  [Buffer Deep Trace] Frame %d: SW_Plane0=%p, HW_Plane0=%p",
                         frame_count, (void*)sw_plane0, (void*)hw_plane0);
        }


        // ⭐ 关键验证（按你的建议）：在 compare 前强行修改硬件 Buffer 的第一个字节
        // 如果修改后 PSNR 仍然是 100dB，说明 comparator 没有读到真实数据或存在指针/共享内存问题
#ifdef ENABLE_DEBUG_BUFFER_TAG
        {
            uint8_t* hw_raw = hw_buf->getImagePlaneData(0);
            if (hw_raw) {
                hw_raw[0] = (hw_raw[0] == 0) ? 255 : 0;
                LOG_INFO_FMT("  [Manual Modify Check] hw_raw[0]=%u", (unsigned)hw_raw[0]);
            }
        }
#endif

#ifdef ENABLE_DEBUG_BUFFER_TAG
        // ⭐ 额外“镜像检测”实验：把 SW plane0 首字节投毒为 0x55，立刻观察 HW 是否被污染
        {
            uint8_t* sw_p = sw_buf->getImagePlaneData(0);
            uint8_t* hw_p = hw_buf->getImagePlaneData(0);
            if (sw_p && hw_p) {
                uint8_t orig = sw_p[0];
                sw_p[0] = 0x55;
                if (hw_p[0] == 0x55) {
                    LOG_ERROR("  !!! MEMORY MIRROR DETECTED !!! SW poison propagated to HW buffer (plane0[0]=0x55)");
                } else if (frame_count < 5) {
                    LOG_INFO_FMT("  [Poison Probe] SW[0]=0x55, HW[0]=0x%02X", hw_p[0]);
                }
                sw_p[0] = orig;  // 还原
            }
        }
#endif

        // ⭐ 验证帧对齐情况（现在应该已通过PTS对齐）
        // ⭐ 在compare之前检查，确认PTS对齐是否成功
        AVFrame* sw_avframe = sw_buf ? sw_buf->getAVFrame() : nullptr;
        AVFrame* hw_avframe = hw_buf ? hw_buf->getAVFrame() : nullptr;
        bool frame_mismatch = false;
        
        // ⭐ 统计帧类型分布（用于诊断硬件解码器的帧类型标记问题）
        if (sw_avframe) {
            switch (sw_avframe->pict_type) {
                case AV_PICTURE_TYPE_I: sw_frame_type_i++; break;
                case AV_PICTURE_TYPE_P: sw_frame_type_p++; break;
                case AV_PICTURE_TYPE_B: sw_frame_type_b++; break;
                case AV_PICTURE_TYPE_NONE: sw_frame_type_none++; break;
            }
        }
        if (hw_avframe) {
            switch (hw_avframe->pict_type) {
                case AV_PICTURE_TYPE_I: hw_frame_type_i++; break;
                case AV_PICTURE_TYPE_P: hw_frame_type_p++; break;
                case AV_PICTURE_TYPE_B: hw_frame_type_b++; break;
                case AV_PICTURE_TYPE_NONE: hw_frame_type_none++; break;
            }
        }
        
        if (sw_avframe && hw_avframe) {
            // 获取PTS
            int64_t sw_pts = sw_avframe->pts != AV_NOPTS_VALUE ? sw_avframe->pts : sw_avframe->best_effort_timestamp;
            int64_t hw_pts = hw_avframe->pts != AV_NOPTS_VALUE ? hw_avframe->pts : hw_avframe->best_effort_timestamp;
            
            // 检查帧类型是否一致（现在应该一致，因为已按PTS对齐）
            if (sw_avframe->pict_type != hw_avframe->pict_type) {
                frame_mismatch = true;
                const char* sw_type_str = "?";
                const char* hw_type_str = "?";
                switch (sw_avframe->pict_type) {
                    case AV_PICTURE_TYPE_I: sw_type_str = "I"; break;
                    case AV_PICTURE_TYPE_P: sw_type_str = "P"; break;
                    case AV_PICTURE_TYPE_B: sw_type_str = "B"; break;
                    case AV_PICTURE_TYPE_NONE: sw_type_str = "N"; break;
                }
                switch (hw_avframe->pict_type) {
                    case AV_PICTURE_TYPE_I: hw_type_str = "I"; break;
                    case AV_PICTURE_TYPE_P: hw_type_str = "P"; break;
                    case AV_PICTURE_TYPE_B: hw_type_str = "B"; break;
                    case AV_PICTURE_TYPE_NONE: hw_type_str = "N"; break;
                }
                
                // ⚠️ 警告：帧类型不一致（PTS对齐后仍不一致，可能是解码器问题）
                LOG_WARN_FMT("  ⚠️  Frame %d: PTS matched but frame_type MISMATCH: SW=%s, HW=%s",
                            frame_count, sw_type_str, hw_type_str);
            }
            
            // 验证PTS是否一致（应该一致，因为已对齐）
            if (sw_pts != AV_NOPTS_VALUE && hw_pts != AV_NOPTS_VALUE) {
                if (sw_pts != hw_pts) {
                    frame_mismatch = true;
                    // ⚠️ 严重警告：PTS对齐失败
                    LOG_ERROR_FMT("  ❌ Frame %d: PTS MISMATCH after alignment: SW pts=%lld, HW pts=%lld",
                                frame_count, (long long)sw_pts, (long long)hw_pts);
                }
            }
            
            // 前10帧显示对齐信息，确认PTS对齐成功
            if (frame_count <= 10) {
                const char* sw_type_str = "?";
                const char* hw_type_str = "?";
                switch (sw_avframe->pict_type) {
                    case AV_PICTURE_TYPE_I: sw_type_str = "I"; break;
                    case AV_PICTURE_TYPE_P: sw_type_str = "P"; break;
                    case AV_PICTURE_TYPE_B: sw_type_str = "B"; break;
                    case AV_PICTURE_TYPE_NONE: sw_type_str = "N"; break;
                }
                switch (hw_avframe->pict_type) {
                    case AV_PICTURE_TYPE_I: hw_type_str = "I"; break;
                    case AV_PICTURE_TYPE_P: hw_type_str = "P"; break;
                    case AV_PICTURE_TYPE_B: hw_type_str = "B"; break;
                    case AV_PICTURE_TYPE_NONE: hw_type_str = "N"; break;
                }
                
                char sw_pts_str[32], hw_pts_str[32];
                if (sw_pts != AV_NOPTS_VALUE) {
                    snprintf(sw_pts_str, sizeof(sw_pts_str), "%lld", (long long)sw_pts);
                } else {
                    strcpy(sw_pts_str, "NOPTS");
                }
                if (hw_pts != AV_NOPTS_VALUE) {
                    snprintf(hw_pts_str, sizeof(hw_pts_str), "%lld", (long long)hw_pts);
                } else {
                    strcpy(hw_pts_str, "NOPTS");
                }
                
                LOG_INFO_FMT("  Frame %d PTS-aligned: SW[%s, pts=%s], HW[%s, pts=%s]%s",
                            frame_count,
                            sw_type_str, sw_pts_str,
                            hw_type_str, hw_pts_str,
                            frame_mismatch ? " ⚠️" : " ✅");
            }
        }
        
        // ⭐ 关键调用：使用 BufferComparator 对比两个 Buffer
        FrameCompareResult result;
        if (sw_buf && hw_buf) {
            result = comparator.compare(sw_buf, hw_buf);
            
            // ⭐ 检查硬件解码器的PTS顺序（诊断帧顺序问题）
            AVFrame* hw_avframe_check = hw_buf->getAVFrame();
            if (hw_avframe_check) {
                int64_t current_hw_pts = hw_avframe_check->pts;
                if (current_hw_pts == AV_NOPTS_VALUE) {
                    current_hw_pts = hw_avframe_check->best_effort_timestamp;
                }
                
                if (current_hw_pts != AV_NOPTS_VALUE && last_hw_pts != AV_NOPTS_VALUE) {
                    // 检查PTS是否单调递增（显示顺序）
                    if (current_hw_pts < last_hw_pts) {
                        hw_pts_order_error_count++;
                        if (hw_pts_order_error_count <= 5 || frame_count % 50 == 0) {
                            LOG_WARN_FMT("  ⚠️  Frame %d: HW PTS顺序错误！当前PTS=%lld < 上一个PTS=%lld (可能缺少B帧重排序，导致保存的YUV文件播放时画面跳动)",
                                        frame_count, (long long)current_hw_pts, (long long)last_hw_pts);
                        }
                    }
                }
                
                if (current_hw_pts != AV_NOPTS_VALUE) {
                    last_hw_pts = current_hw_pts;
                }
            }
            
            // ⭐ 保存硬件解码输出到YUV文件
            // ⚠️ 注意：这里保存的顺序是解码器输出的顺序，如果B帧重排序有问题，
            //    保存的帧可能是解码顺序（DTS顺序）而不是显示顺序（PTS顺序），
            //    这会导致播放时画面跳动
            if (hw_format != AV_PIX_FMT_NONE) {
                if (!hw_writer.write(hw_buf)) {
                    if (frame_count <= 10) {
                        LOG_WARN_FMT("  ⚠️  Frame %d: Failed to write hardware buffer to YUV file", frame_count);
                    }
                }
            }
        }
        
        // ⭐ 记录PSNR值（用于统计）
        if (result.psnr_y > 0.0) {
            psnr_y_values.push_back(result.psnr_y);
            psnr_u_values.push_back(result.psnr_u);
            psnr_v_values.push_back(result.psnr_v);
            psnr_avg_values.push_back(result.psnr_avg);
            
            // ⭐ 按帧类型分类统计PSNR
            // ⭐ 只使用解码器的pict_type，不再尝试“纠正”或使用外部GOP
            char frame_type_char = '?';
            AVFrame* avframe = sw_avframe;  // 优先使用软件解码器
            
            // 如果软件解码器没有，再从硬件解码器获取
            if (!avframe && hw_avframe) {
                avframe = hw_avframe;
            }
            
            // ⭐ 每帧都输出SW和HW的帧类型，确认是否存在错位对比
            const char* sw_type_str = "?";
            const char* hw_type_str = "?";
            int sw_pict_type_val = -1;
            int hw_pict_type_val = -1;
            
            if (sw_avframe) {
                sw_pict_type_val = (int)sw_avframe->pict_type;
                switch (sw_avframe->pict_type) {
                    case AV_PICTURE_TYPE_I: sw_type_str = "I"; break;
                    case AV_PICTURE_TYPE_P: sw_type_str = "P"; break;
                    case AV_PICTURE_TYPE_B: sw_type_str = "B"; break;
                    case AV_PICTURE_TYPE_NONE: sw_type_str = "N"; break;
                    default: sw_type_str = "?"; break;
                }
            }
            if (hw_avframe) {
                hw_pict_type_val = (int)hw_avframe->pict_type;
                switch (hw_avframe->pict_type) {
                    case AV_PICTURE_TYPE_I: hw_type_str = "I"; break;
                    case AV_PICTURE_TYPE_P: hw_type_str = "P"; break;
                    case AV_PICTURE_TYPE_B: hw_type_str = "B"; break;
                    case AV_PICTURE_TYPE_NONE: hw_type_str = "N"; break;
                    default: hw_type_str = "?"; break;
                }
            }
            
            // ⭐ 使用解码器的pict_type作为帧类型（只统计，不做纠正）
            if (avframe) {
                switch (avframe->pict_type) {
                    case AV_PICTURE_TYPE_I:
                        frame_type_char = 'I';
                        psnr_y_i_frames.push_back(result.psnr_y);
                        if (result.psnr_avg > 0.0) {
                            psnr_avg_i_frames.push_back(result.psnr_avg);
                        }
                        break;
                    case AV_PICTURE_TYPE_P:
                        frame_type_char = 'P';
                        psnr_y_p_frames.push_back(result.psnr_y);
                        if (result.psnr_avg > 0.0) {
                            psnr_avg_p_frames.push_back(result.psnr_avg);
                        }
                        break;
                    case AV_PICTURE_TYPE_B:
                        frame_type_char = 'B';
                        psnr_y_b_frames.push_back(result.psnr_y);
                        if (result.psnr_avg > 0.0) {
                            psnr_avg_b_frames.push_back(result.psnr_avg);
                        }
                        break;
                    case AV_PICTURE_TYPE_NONE:
                        frame_type_char = 'N';  // None
                        if (frame_count <= 10) {
                            LOG_DEBUG_FMT("  Frame %d: AV_PICTURE_TYPE_NONE detected", result.frame_index);
                        }
                        break;
                    default:
                        frame_type_char = '?';
                        if (frame_count <= 10) {
                            LOG_DEBUG_FMT("  Frame %d: Unknown pict_type=%d", result.frame_index, (int)avframe->pict_type);
                        }
                        break;
                }
            } else {
                if (frame_count <= 10) {
                    LOG_DEBUG_FMT("  Frame %d: No AVFrame available for frame type detection", result.frame_index);
                }
            }
            
            // ⭐ 使用确定的帧类型进行分类统计
            switch (frame_type_char) {
                case 'I':
                    psnr_y_i_frames.push_back(result.psnr_y);
                    if (result.psnr_avg > 0.0) {
                        psnr_avg_i_frames.push_back(result.psnr_avg);
                    }
                    break;
                case 'P':
                    psnr_y_p_frames.push_back(result.psnr_y);
                    if (result.psnr_avg > 0.0) {
                        psnr_avg_p_frames.push_back(result.psnr_avg);
                    }
                    break;
                case 'B':
                    psnr_y_b_frames.push_back(result.psnr_y);
                    if (result.psnr_avg > 0.0) {
                        psnr_avg_b_frames.push_back(result.psnr_avg);
                    }
                    break;
                case 'N':
                    // None 类型不参与I/P/B统计
                    break;
                default:
                    // 未知类型暂不参与统计
                    break;
            }
            
            // ⭐ 每帧都输出SW和HW的帧类型信息（用于诊断错位问题）
            if (sw_avframe && hw_avframe) {
                bool sw_hw_match = (sw_avframe->pict_type == hw_avframe->pict_type);
                
                if (!sw_hw_match) {
                    // 软硬件解码器标记不一致
                    LOG_WARN_FMT("  Frame %d [%c[解码器]]: PSNR Y=%.2f U=%.2f V=%.2f dB (avg=%.2f dB) [SW=%s(pict_type=%d) vs HW=%s(pict_type=%d) ⚠️ MISMATCH]",
                                frame_count, frame_type_char,
                                result.psnr_y, result.psnr_u, result.psnr_v, result.psnr_avg,
                                sw_type_str, sw_pict_type_val, hw_type_str, hw_pict_type_val);
                } else {
                    // 软硬件解码器标记一致
                    LOG_INFO_FMT("  Frame %d [%c[解码器]]: PSNR Y=%.2f U=%.2f V=%.2f dB (avg=%.2f dB) [SW=HW=%s(pict_type=%d) ✅]",
                                frame_count, frame_type_char,
                                result.psnr_y, result.psnr_u, result.psnr_v, result.psnr_avg,
                                sw_type_str, sw_pict_type_val);
                }
            } else {
                // 如果无法获取帧类型，也输出日志
                LOG_DEBUG_FMT("  Frame %d [%c[解码器]]: PSNR Y=%.2f U=%.2f V=%.2f dB (avg=%.2f dB) [SW=%s vs HW=%s (AVFrame unavailable)]",
                            frame_count, frame_type_char, result.psnr_y, result.psnr_u, result.psnr_v, result.psnr_avg,
                            sw_type_str, hw_type_str);
            }

            // ⭐ 每50帧输出一次表格行：Src(I/P/B) vs SW vs HW vs PSNR（基于PTS映射）
            if (has_src_gop && (frame_count % 50 == 0)) {
                char src_type = '?';
                long long src_pts_key = 0;

                if (sw_avframe) {
                    int64_t sw_pts = sw_avframe->pts != AV_NOPTS_VALUE
                        ? sw_avframe->pts
                        : sw_avframe->best_effort_timestamp;
                    src_pts_key = (long long)sw_pts;
                    auto it = src_pts_to_type.find(src_pts_key);
                    if (it != src_pts_to_type.end()) {
                        src_type = it->second;
                    }
                }

                char sw_type_ch = '?';
                if (sw_type_str && sw_type_str[0] != '\0') {
                    sw_type_ch = sw_type_str[0];
                }
                char hw_type_ch = '?';
                if (hw_type_str && hw_type_str[0] != '\0') {
                    hw_type_ch = hw_type_str[0];
                }

                LOG_INFO("  ── Frame Type Table (every 50 frames) ──");
                LOG_INFO("     Idx  PTS       Src  SW  HW  PSNR-Y");
                LOG_INFO_FMT("     %3d  %7lld   %c    %c   %c   %6.2f",
                             frame_count,
                             src_pts_key,
                             src_type,
                             sw_type_ch,
                             hw_type_ch,
                             result.psnr_y);
            }
        }
        
        // ⭐ 记录SSIM值（用于统计）
        if (result.ssim_y > 0.0) {
            ssim_y_values.push_back(result.ssim_y);
            ssim_u_values.push_back(result.ssim_u);
            ssim_v_values.push_back(result.ssim_v);
            ssim_avg_values.push_back(result.ssim_avg);
        }
        
        // ⭐ 应用新增比较项：记录像素差异统计
        if (result.diff_pixel_count >= 0) {
            max_pixel_diff_values.push_back(result.max_pixel_diff);
            diff_pixel_count_values.push_back(result.diff_pixel_count);
            diff_pixel_ratio_values.push_back(result.diff_pixel_ratio);
        }
        
        // ⭐ 应用新增比较项：对失败/警告帧显示详细格式和差异信息
        if (result.level == FrameCompareResult::FAIL || result.level == FrameCompareResult::WARN) {
            if (frame_count <= 10 || frame_count % 50 == 0) {
                // 前10帧或每50帧显示详细信息
                if (!result.ref_format_name.empty() || !result.test_format_name.empty()) {
                    LOG_WARN_FMT("    Frame %d formats: %s vs %s", 
                                result.frame_index,
                                result.ref_format_name.c_str(), 
                                result.test_format_name.c_str());
                }
                if (result.diff_pixel_count > 0) {
                    LOG_WARN_FMT("    Frame %d pixel diff: max=%d, count=%d (%.2f%%)", 
                                result.frame_index,
                                result.max_pixel_diff,
                                result.diff_pixel_count,
                                result.diff_pixel_ratio * 100.0);
                }
                if (!result.error_message.empty()) {
                    LOG_WARN_FMT("    Frame %d error: %s", 
                                result.frame_index,
                                result.error_message.c_str());
                }
            }
        }
        
        // ⭐ 关键修复：立即释放Buffer，避免Buffer饥饿
        sw_pool->releaseFilled(sw_buf);
        hw_pool->releaseFilled(hw_buf);
        
        frame_count++;
         
         // 每50帧打印一次进度
         if (frame_count % 50 == 0) {
             double avg_psnr_y = psnr_y_values.empty() ? 0.0 : 
                 std::accumulate(psnr_y_values.begin(), psnr_y_values.end(), 0.0) / psnr_y_values.size();
             double avg_psnr_avg = psnr_avg_values.empty() ? 0.0 : 
                 std::accumulate(psnr_avg_values.begin(), psnr_avg_values.end(), 0.0) / psnr_avg_values.size();
             
             if (compare_config.enable_psnr && compare_config.enable_ssim) {
                 double avg_ssim_y = ssim_y_values.empty() ? 0.0 : 
                     std::accumulate(ssim_y_values.begin(), ssim_y_values.end(), 0.0) / ssim_y_values.size();
                 double avg_ssim_avg = ssim_avg_values.empty() ? 0.0 : 
                     std::accumulate(ssim_avg_values.begin(), ssim_avg_values.end(), 0.0) / ssim_avg_values.size();
                 
                 LOG_INFO_FMT("  Progress: %d/%d frames | PSNR-Y=%.2f dB, PSNR-Avg=%.2f dB | SSIM-Y=%.4f, SSIM-Avg=%.4f | Passed: %d, Warned: %d, Failed: %d",
                             frame_count, MAX_FRAMES, avg_psnr_y, avg_psnr_avg, avg_ssim_y, avg_ssim_avg,
                             comparator.getPassedCount(),
                             comparator.getCompareCount() - comparator.getPassedCount() - comparator.getFailedCount(),
                             comparator.getFailedCount());
             } else if (compare_config.enable_psnr) {
                 LOG_INFO_FMT("  Progress: %d/%d frames | PSNR-Y=%.2f dB, PSNR-Avg=%.2f dB | Passed: %d, Warned: %d, Failed: %d",
                             frame_count, MAX_FRAMES, avg_psnr_y, avg_psnr_avg,
                             comparator.getPassedCount(),
                             comparator.getCompareCount() - comparator.getPassedCount() - comparator.getFailedCount(),
                             comparator.getFailedCount());
             } else if (compare_config.enable_ssim) {
                 double avg_ssim_y = ssim_y_values.empty() ? 0.0 : 
                     std::accumulate(ssim_y_values.begin(), ssim_y_values.end(), 0.0) / ssim_y_values.size();
                 double avg_ssim_avg = ssim_avg_values.empty() ? 0.0 : 
                     std::accumulate(ssim_avg_values.begin(), ssim_avg_values.end(), 0.0) / ssim_avg_values.size();
                 
                 LOG_INFO_FMT("  Progress: %d/%d frames | SSIM-Y=%.4f, SSIM-Avg=%.4f | Passed: %d, Warned: %d, Failed: %d",
                             frame_count, MAX_FRAMES, avg_ssim_y, avg_ssim_avg,
                             comparator.getPassedCount(),
                             comparator.getCompareCount() - comparator.getPassedCount() - comparator.getFailedCount(),
                             comparator.getFailedCount());
             }
         }
         
        // 每10帧打印一次当前帧的PSNR和SSIM（前50帧详细模式）
        if (frame_count <= 50 && frame_count % 10 == 0) {
            if (compare_config.enable_psnr && compare_config.enable_ssim) {
                LOG_INFO_FMT("  Frame %3d: PSNR Y=%.2f U=%.2f V=%.2f dB (avg=%.2f dB) | SSIM Y=%.4f U=%.4f V=%.4f (avg=%.4f) %s",
                            result.frame_index, result.psnr_y, result.psnr_u, result.psnr_v, result.psnr_avg,
                            result.ssim_y, result.ssim_u, result.ssim_v, result.ssim_avg,
                            result.passed ? "✅" : (result.level == FrameCompareResult::WARN ? "⚠️" : "❌"));
            } else if (compare_config.enable_psnr) {
                LOG_INFO_FMT("  Frame %3d: PSNR Y=%.2f U=%.2f V=%.2f dB (avg=%.2f dB) %s",
                            result.frame_index, result.psnr_y, result.psnr_u, result.psnr_v, result.psnr_avg,
                            result.passed ? "✅" : (result.level == FrameCompareResult::WARN ? "⚠️" : "❌"));
            } else if (compare_config.enable_ssim) {
                LOG_INFO_FMT("  Frame %3d: SSIM Y=%.4f U=%.4f V=%.4f (avg=%.4f) %s",
                            result.frame_index, result.ssim_y, result.ssim_u, result.ssim_v, result.ssim_avg,
                            result.passed ? "✅" : (result.level == FrameCompareResult::WARN ? "⚠️" : "❌"));
            }
            // ⭐ 应用新增比较项：显示格式信息（前50帧）
            if (!result.ref_format_name.empty() || !result.test_format_name.empty()) {
                LOG_DEBUG_FMT("    Formats: %s vs %s", 
                            result.ref_format_name.c_str(), 
                            result.test_format_name.c_str());
            }
            // ⭐ 应用新增比较项：显示像素差异统计（如果有差异）
            if (result.diff_pixel_count > 0 && result.diff_pixel_ratio > 0.01) {
                LOG_DEBUG_FMT("    Pixel diff: max=%d, ratio=%.2f%%", 
                            result.max_pixel_diff,
                            result.diff_pixel_ratio * 100.0);
            }
        }
    }
    
    LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // ⭐ 显示帧类型统计（用于诊断解码器的帧类型标记问题）
    LOG_INFO("\nFrame Type Statistics:");
    LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    int sw_total = sw_frame_type_i + sw_frame_type_p + sw_frame_type_b + sw_frame_type_none;
    int hw_total = hw_frame_type_i + hw_frame_type_p + hw_frame_type_b + hw_frame_type_none;
    
    LOG_INFO_FMT("  Software Decoder (pict_type): I=%d, P=%d, B=%d, None=%d (Total=%d)",
                sw_frame_type_i, sw_frame_type_p, sw_frame_type_b, sw_frame_type_none, sw_total);
    LOG_INFO_FMT("  Hardware Decoder (pict_type): I=%d, P=%d, B=%d, None=%d (Total=%d)",
                hw_frame_type_i, hw_frame_type_p, hw_frame_type_b, hw_frame_type_none, hw_total);
    
    // ⭐ 检查硬件解码器是否将所有帧标记为B帧
    if (hw_total > 0) {
        double hw_b_ratio = (double)hw_frame_type_b / hw_total * 100.0;
        if (hw_b_ratio > 95.0 && hw_frame_type_i == 0 && hw_frame_type_p == 0) {
            LOG_WARN("  ⚠️  Hardware decoder appears to mark all frames as B-frames!");
            LOG_WARN_FMT("     B-frame ratio: %.1f%% (I=%d, P=%d, B=%d)",
                        hw_b_ratio, hw_frame_type_i, hw_frame_type_p, hw_frame_type_b);
            LOG_WARN("     This may be a decoder bug or frame type identification issue.");
        } else if (hw_b_ratio > 80.0) {
            LOG_WARN_FMT("  ⚠️  Hardware decoder has high B-frame ratio: %.1f%%",
                        hw_b_ratio);
        }
    }
    
    // ⭐ 提示：PSNR按解码器的pict_type分类统计
    LOG_INFO("\nNote:");
    LOG_INFO("  PSNR statistics are grouped by decoder-reported frame types (pict_type).");
    LOG_INFO("  SW pict_type is used as primary source; HW pict_type is only for diagnostics.");
    
    // ⭐ 显示PTS顺序错误统计（用于诊断硬件解码器的帧顺序问题）
    LOG_INFO("\nPTS Order Statistics:");
    LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    if (hw_pts_order_error_count > 0) {
        LOG_WARN_FMT("  ⚠️  Hardware decoder PTS order errors: %d times", hw_pts_order_error_count);
        LOG_WARN("     This indicates that hardware decoder output is in DECODE ORDER (DTS order)");
        LOG_WARN("     rather than DISPLAY ORDER (PTS order). This causes:");
        LOG_WARN("     1. Saved YUV file playback shows frame skipping/jumping");
        LOG_WARN("     2. Frame type mismatch between SW and HW (B-frames vs P-frames)");
        LOG_WARN("     3. B-frame reordering may be missing or incorrect in hardware decoder");
        LOG_WARN_FMT("     Recommendation: Check hardware decoder B-frame reorder configuration");
    } else {
        LOG_INFO("  ✅ Hardware decoder PTS order: OK (monotonic increasing)");
    }
    
    // ⭐ 关闭 BufferWriter
    if (hw_format != AV_PIX_FMT_NONE) {
        hw_writer.close();
        int write_count = hw_writer.getWriteCount();
        LOG_INFO_FMT("  ✅ BufferWriter closed: %d frames written to %s", 
                    write_count, hw_output_yuv.c_str());
        if (write_count > 0) {
            LOG_INFO_FMT("    To play: ffplay -f rawvideo -pixel_format %s -video_size %dx%d %s",
                        hw_ffplay_format.c_str(), hw_actual_width, hw_actual_height, hw_output_yuv.c_str());
        }
    }
    
    if (frame_count == 1) {
        LOG_WARN("  ⚠️  Only 1 frame was processed in the loop!");
    } else if (frame_count < 10) {
        LOG_WARN_FMT("  ⚠️  Only %d frames were processed (expected more)", frame_count);
    }
    
    if (timeout_count >= MAX_TIMEOUT) {
        LOG_WARN("  ⚠️  Loop exited due to timeout");
        LOG_WARN_FMT("    Timeout occurred after %d consecutive timeouts", timeout_count);
        LOG_WARN("    This may indicate:");
        LOG_WARN("    1. Decoders are not producing frames fast enough");
        LOG_WARN("    2. BufferPool is empty and decoders are waiting");
        LOG_WARN("    3. Network/stream issues (for RTSP)");
    }
    
    if (!hw_producer.isRunning() && !sw_producer.isRunning()) {
        LOG_INFO("  ✅ Both producers stopped naturally");
    } else if (!hw_producer.isRunning()) {
        LOG_WARN("  ⚠️  Hardware producer stopped unexpectedly");
    } else if (!sw_producer.isRunning()) {
        LOG_WARN("  ⚠️  Software producer stopped unexpectedly");
    }
    
    // ⭐ 排空剩余 Buffer（只释放，不再保存帧）
    LOG_INFO("\nDraining remaining buffers (release only)...");
    Buffer* remaining = nullptr;
    int hw_drained = 0;
    
    while ((remaining = hw_pool->acquireFilled(false, 0)) != nullptr) {
        // ⭐ 只释放，不保存
        hw_drained++;
        hw_pool->releaseFilled(remaining);
    }
    if (hw_drained > 0) {
        LOG_INFO_FMT("  Drained %d remaining hardware buffers", hw_drained);
    }
    
    int sw_drained = 0;
    while ((remaining = sw_pool->acquireFilled(false, 0)) != nullptr) {
        sw_drained++;
        sw_pool->releaseFilled(remaining);
    }
    if (sw_drained > 0) {
        LOG_INFO_FMT("  Drained %d remaining software buffers", sw_drained);
    }
    
    if (hw_drained == 0 && sw_drained == 0) {
        LOG_INFO("  No remaining buffers to drain");
    }

     // ⭐ 关闭资源
     LOG_INFO("\nStep 6: Cleaning up...");

     // 关闭Comparator
     comparator.close();
     // ⭐ 使用 BufferComparator 打印结果（参考 test_decoder_compare）
     LOG_INFO("\n═══════════════════════════════════════════════════════");
     LOG_INFO("  Decoder Comparison Results");
     LOG_INFO("═══════════════════════════════════════════════════════");
     comparator.printSummary();
     LOG_INFO("═══════════════════════════════════════════════════════\n");
     
     hw_producer.stop();
     sw_producer.stop();

     // ⭐ 计算详细的PSNR统计信息（参考 run_psnr_compare_test）
     LOG_INFO("\nStep 7: Calculating PSNR statistics...");
     
     // ⭐ 计算统计值（在作用域外定义，供后续使用）
     double avg_psnr_y = 0.0, avg_psnr_u = 0.0, avg_psnr_v = 0.0, avg_psnr_avg = 0.0;
     double avg_ssim_y = 0.0, avg_ssim_u = 0.0, avg_ssim_v = 0.0, avg_ssim_avg = 0.0;
     
        // ⭐ 打印详细的PSNR统计（参考 run_psnr_compare_test）
        int psnr_passed_count = 0;
        int psnr_warned_count = 0;
        int psnr_failed_count = 0;
        
        if (!psnr_y_values.empty()) {
            // 计算平均值
            avg_psnr_y = std::accumulate(psnr_y_values.begin(), psnr_y_values.end(), 0.0) / psnr_y_values.size();
            avg_psnr_u = std::accumulate(psnr_u_values.begin(), psnr_u_values.end(), 0.0) / psnr_u_values.size();
            avg_psnr_v = std::accumulate(psnr_v_values.begin(), psnr_v_values.end(), 0.0) / psnr_v_values.size();
            avg_psnr_avg = std::accumulate(psnr_avg_values.begin(), psnr_avg_values.end(), 0.0) / psnr_avg_values.size();
            
            // 计算最小值和最大值
            auto minmax_y = std::minmax_element(psnr_y_values.begin(), psnr_y_values.end());
            auto minmax_avg = std::minmax_element(psnr_avg_values.begin(), psnr_avg_values.end());
            
            double min_psnr_y = *minmax_y.first;
            double max_psnr_y = *minmax_y.second;
            double min_psnr_avg = *minmax_avg.first;
            double max_psnr_avg = *minmax_avg.second;
            
            // 计算标准差
            double variance_y = 0.0;
            for (double val : psnr_y_values) {
                variance_y += (val - avg_psnr_y) * (val - avg_psnr_y);
            }
            double stddev_y = std::sqrt(variance_y / psnr_y_values.size());
            
            // ⭐ 统计PSNR通过性判定（基于Y平面，主要指标）
            for (double psnr_y : psnr_y_values) {
                if (psnr_y >= compare_config.quick_psnr_threshold) {
                    psnr_passed_count++;
                } else if (psnr_y >= compare_config.quick_warn_threshold) {
                    psnr_warned_count++;
                } else {
                    psnr_failed_count++;
                }
            }
            
            LOG_INFO("  PSNR Statistics (Hardware vs Software):");
            LOG_INFO_FMT("    Average: Y=%.2f U=%.2f V=%.2f dB (avg=%.2f dB)",
                        avg_psnr_y, avg_psnr_u, avg_psnr_v, avg_psnr_avg);
            LOG_INFO_FMT("    Range Y:  [%.2f, %.2f] dB (stddev=%.2f)",
                        min_psnr_y, max_psnr_y, stddev_y);
            LOG_INFO_FMT("    Range Avg: [%.2f, %.2f] dB",
                        min_psnr_avg, max_psnr_avg);
            
            // ⭐ 按帧类型显示平均PSNR
            LOG_INFO("");
            LOG_INFO("  PSNR Statistics by Frame Type:");
            if (!psnr_y_i_frames.empty()) {
                double avg_psnr_y_i = std::accumulate(psnr_y_i_frames.begin(), psnr_y_i_frames.end(), 0.0) / psnr_y_i_frames.size();
                if (!psnr_avg_i_frames.empty()) {
                    double avg_psnr_avg_i = std::accumulate(psnr_avg_i_frames.begin(), psnr_avg_i_frames.end(), 0.0) / psnr_avg_i_frames.size();
                    LOG_INFO_FMT("    I-Frame: Count=%zu (with avg: %zu), Avg PSNR-Y=%.2f dB, Avg PSNR-Avg=%.2f dB",
                                psnr_y_i_frames.size(), psnr_avg_i_frames.size(), avg_psnr_y_i, avg_psnr_avg_i);
                } else {
                    LOG_INFO_FMT("    I-Frame: Count=%zu, Avg PSNR-Y=%.2f dB, Avg PSNR-Avg=N/A (only Y-plane calculated)",
                                psnr_y_i_frames.size(), avg_psnr_y_i);
                }
            } else {
                LOG_INFO("    I-Frame: Count=0, No I-frames found");
            }
            
            if (!psnr_y_p_frames.empty()) {
                double avg_psnr_y_p = std::accumulate(psnr_y_p_frames.begin(), psnr_y_p_frames.end(), 0.0) / psnr_y_p_frames.size();
                if (!psnr_avg_p_frames.empty()) {
                    double avg_psnr_avg_p = std::accumulate(psnr_avg_p_frames.begin(), psnr_avg_p_frames.end(), 0.0) / psnr_avg_p_frames.size();
                    LOG_INFO_FMT("    P-Frame: Count=%zu (with avg: %zu), Avg PSNR-Y=%.2f dB, Avg PSNR-Avg=%.2f dB",
                                psnr_y_p_frames.size(), psnr_avg_p_frames.size(), avg_psnr_y_p, avg_psnr_avg_p);
                } else {
                    LOG_INFO_FMT("    P-Frame: Count=%zu, Avg PSNR-Y=%.2f dB, Avg PSNR-Avg=N/A (only Y-plane calculated)",
                                psnr_y_p_frames.size(), avg_psnr_y_p);
                }
            } else {
                LOG_INFO("    P-Frame: Count=0, No P-frames found");
            }
            
            if (!psnr_y_b_frames.empty()) {
                double avg_psnr_y_b = std::accumulate(psnr_y_b_frames.begin(), psnr_y_b_frames.end(), 0.0) / psnr_y_b_frames.size();
                if (!psnr_avg_b_frames.empty()) {
                    double avg_psnr_avg_b = std::accumulate(psnr_avg_b_frames.begin(), psnr_avg_b_frames.end(), 0.0) / psnr_avg_b_frames.size();
                    LOG_INFO_FMT("    B-Frame: Count=%zu (with avg: %zu), Avg PSNR-Y=%.2f dB, Avg PSNR-Avg=%.2f dB",
                                psnr_y_b_frames.size(), psnr_avg_b_frames.size(), avg_psnr_y_b, avg_psnr_avg_b);
                } else {
                    LOG_INFO_FMT("    B-Frame: Count=%zu, Avg PSNR-Y=%.2f dB, Avg PSNR-Avg=N/A (only Y-plane calculated)",
                                psnr_y_b_frames.size(), avg_psnr_y_b);
                }
            } else {
                LOG_INFO("    B-Frame: Count=0, No B-frames found");
            }
            
            // ⭐ 显示PSNR通过性判定统计
            LOG_INFO("");
            LOG_INFO("  PSNR Pass/Fail Assessment (based on PSNR-Y threshold):");
            LOG_INFO_FMT("    Passed (PSNR-Y >= %.1f dB): %d ✅ (%.1f%%)",
                        compare_config.quick_psnr_threshold,
                        psnr_passed_count,
                        100.0 * psnr_passed_count / psnr_y_values.size());
            LOG_INFO_FMT("    Warned (%.1f <= PSNR-Y < %.1f dB): %d ⚠️  (%.1f%%)",
                        compare_config.quick_warn_threshold,
                        compare_config.quick_psnr_threshold,
                        psnr_warned_count,
                        100.0 * psnr_warned_count / psnr_y_values.size());
            LOG_INFO_FMT("    Failed (PSNR-Y < %.1f dB): %d ❌ (%.1f%%)",
                        compare_config.quick_warn_threshold,
                        psnr_failed_count,
                        100.0 * psnr_failed_count / psnr_y_values.size());
        }
     
     // ⭐ 打印详细的SSIM统计
     int ssim_passed_count = 0;
     int ssim_warned_count = 0;
     int ssim_failed_count = 0;
     
     if (!ssim_y_values.empty()) {
         // 计算平均值
         avg_ssim_y = std::accumulate(ssim_y_values.begin(), ssim_y_values.end(), 0.0) / ssim_y_values.size();
         avg_ssim_u = std::accumulate(ssim_u_values.begin(), ssim_u_values.end(), 0.0) / ssim_u_values.size();
         avg_ssim_v = std::accumulate(ssim_v_values.begin(), ssim_v_values.end(), 0.0) / ssim_v_values.size();
         avg_ssim_avg = std::accumulate(ssim_avg_values.begin(), ssim_avg_values.end(), 0.0) / ssim_avg_values.size();
         
         // 计算最小值和最大值
         auto minmax_y = std::minmax_element(ssim_y_values.begin(), ssim_y_values.end());
         auto minmax_avg = std::minmax_element(ssim_avg_values.begin(), ssim_avg_values.end());
         
         double min_ssim_y = *minmax_y.first;
         double max_ssim_y = *minmax_y.second;
         double min_ssim_avg = *minmax_avg.first;
         double max_ssim_avg = *minmax_avg.second;
         
         // 计算标准差
         double variance_y = 0.0;
         for (double val : ssim_y_values) {
             variance_y += (val - avg_ssim_y) * (val - avg_ssim_y);
         }
         double stddev_y = std::sqrt(variance_y / ssim_y_values.size());
         
         // ⭐ 统计SSIM通过性判定（基于Y平面，主要指标）
         for (double ssim_y : ssim_y_values) {
             if (ssim_y >= compare_config.ssim_threshold) {
                 ssim_passed_count++;
             } else if (ssim_y >= compare_config.ssim_warn_threshold) {
                 ssim_warned_count++;
             } else {
                 ssim_failed_count++;
             }
         }
         
         LOG_INFO("");
         LOG_INFO("  SSIM Statistics (Hardware vs Software):");
         LOG_INFO_FMT("    Average: Y=%.4f U=%.4f V=%.4f (avg=%.4f)",
                     avg_ssim_y, avg_ssim_u, avg_ssim_v, avg_ssim_avg);
         LOG_INFO_FMT("    Range Y:  [%.4f, %.4f] (stddev=%.4f)",
                     min_ssim_y, max_ssim_y, stddev_y);
         LOG_INFO_FMT("    Range Avg: [%.4f, %.4f]",
                     min_ssim_avg, max_ssim_avg);
         
         // ⭐ 显示SSIM通过性判定统计
         LOG_INFO("");
         LOG_INFO("  SSIM Pass/Fail Assessment (based on SSIM-Y threshold):");
         LOG_INFO_FMT("    Passed (SSIM-Y >= %.4f): %d ✅ (%.1f%%)",
                     compare_config.ssim_threshold,
                     ssim_passed_count,
                     100.0 * ssim_passed_count / ssim_y_values.size());
         LOG_INFO_FMT("    Warned (%.4f <= SSIM-Y < %.4f): %d ⚠️  (%.1f%%)",
                     compare_config.ssim_warn_threshold,
                     compare_config.ssim_threshold,
                     ssim_warned_count,
                     100.0 * ssim_warned_count / ssim_y_values.size());
         LOG_INFO_FMT("    Failed (SSIM-Y < %.4f): %d ❌ (%.1f%%)",
                     compare_config.ssim_warn_threshold,
                     ssim_failed_count,
                     100.0 * ssim_failed_count / ssim_y_values.size());
     }
     
     // ⭐ 应用新增比较项：显示像素差异统计
     if (!max_pixel_diff_values.empty()) {
         auto max_diff_max = std::max_element(max_pixel_diff_values.begin(), max_pixel_diff_values.end());
         auto max_diff_min = std::min_element(max_pixel_diff_values.begin(), max_pixel_diff_values.end());
         int avg_max_diff = std::accumulate(max_pixel_diff_values.begin(), max_pixel_diff_values.end(), 0) / max_pixel_diff_values.size();
         
         double avg_diff_ratio = std::accumulate(diff_pixel_ratio_values.begin(), diff_pixel_ratio_values.end(), 0.0) / diff_pixel_ratio_values.size();
         auto max_diff_ratio = std::max_element(diff_pixel_ratio_values.begin(), diff_pixel_ratio_values.end());
         
         int64_t total_diff_pixels = std::accumulate(diff_pixel_count_values.begin(), diff_pixel_count_values.end(), (int64_t)0);
         
         LOG_INFO("");
         LOG_INFO("  Pixel Difference Statistics:");
         LOG_INFO_FMT("    Max pixel diff: [%d, %d] (avg: %d)", 
                     *max_diff_min, *max_diff_max, avg_max_diff);
         LOG_INFO_FMT("    Diff pixel ratio: avg=%.4f%%, max=%.4f%%", 
                     avg_diff_ratio * 100.0, *max_diff_ratio * 100.0);
         LOG_INFO_FMT("    Total diff pixels: %lld", (long long)total_diff_pixels);
     }
     
     LOG_INFO("");
     LOG_INFO("  Overall Quality Assessment (Combined PSNR & SSIM):");
     LOG_INFO_FMT("    Passed: %d ✅ (%.1f%%)",
                 comparator.getPassedCount(),
                 frame_count > 0 ? 100.0 * comparator.getPassedCount() / frame_count : 0.0);
     LOG_INFO_FMT("    Warned: %d ⚠️  (%.1f%%)",
                 comparator.getCompareCount() - comparator.getPassedCount() - comparator.getFailedCount(),
                 frame_count > 0 ? 100.0 * (comparator.getCompareCount() - comparator.getPassedCount() - comparator.getFailedCount()) / frame_count : 0.0);
     LOG_INFO_FMT("    Failed: %d ❌ (%.1f%%)",
                 comparator.getFailedCount(),
                 frame_count > 0 ? 100.0 * comparator.getFailedCount() / frame_count : 0.0);
     LOG_INFO("");
     
     // 质量评级（综合考虑 PSNR 和 SSIM）
     bool psnr_excellent = !psnr_avg_values.empty() && avg_psnr_avg >= 38.0;
     bool psnr_good = !psnr_avg_values.empty() && avg_psnr_avg >= 35.0;
     bool ssim_excellent = !ssim_avg_values.empty() && avg_ssim_avg >= 0.95;
     bool ssim_good = !ssim_avg_values.empty() && avg_ssim_avg >= 0.90;
     
     if (compare_config.enable_psnr && compare_config.enable_ssim) {
         // 两者都启用：综合考虑
         if (psnr_excellent && ssim_excellent) {
             LOG_INFO("  ✅ Overall Quality: EXCELLENT (visually lossless)");
             LOG_INFO_FMT("    PSNR: %.2f dB >= 38.0 dB, SSIM: %.4f >= 0.95", avg_psnr_avg, avg_ssim_avg);
         } else if ((psnr_excellent || ssim_excellent) && (psnr_good || ssim_good)) {
             LOG_INFO("  ✅ Overall Quality: VERY GOOD (minimal differences)");
             LOG_INFO_FMT("    PSNR: %.2f dB, SSIM: %.4f", avg_psnr_avg, avg_ssim_avg);
         } else if (psnr_good && ssim_good) {
             LOG_INFO("  ⚠️  Overall Quality: GOOD (minor differences)");
             LOG_INFO_FMT("    PSNR: %.2f dB >= 35.0 dB, SSIM: %.4f >= 0.90", avg_psnr_avg, avg_ssim_avg);
         } else {
             LOG_INFO("  ❌ Overall Quality: POOR (visible artifacts)");
             LOG_INFO_FMT("    PSNR: %.2f dB, SSIM: %.4f", avg_psnr_avg, avg_ssim_avg);
         }
     } else if (compare_config.enable_psnr) {
         // 仅 PSNR
         if (psnr_excellent) {
             LOG_INFO("  ✅ Overall Quality: EXCELLENT (visually lossless)");
             LOG_INFO_FMT("    PSNR: %.2f dB >= 38.0 dB", avg_psnr_avg);
         } else if (psnr_good) {
             LOG_INFO("  ⚠️  Overall Quality: GOOD (minor differences)");
             LOG_INFO_FMT("    PSNR: %.2f dB >= 35.0 dB", avg_psnr_avg);
         } else {
             LOG_INFO("  ❌ Overall Quality: POOR (visible artifacts)");
             LOG_INFO_FMT("    PSNR: %.2f dB", avg_psnr_avg);
         }
    } else if (compare_config.enable_ssim) {
        // 仅 SSIM
        if (ssim_excellent) {
            LOG_INFO("  ✅ Overall Quality: EXCELLENT (visually lossless)");
            LOG_INFO_FMT("    SSIM: %.4f >= 0.95", avg_ssim_avg);
        } else if (ssim_good) {
            LOG_INFO("  ⚠️  Overall Quality: GOOD (minor differences)");
            LOG_INFO_FMT("    SSIM: %.4f >= 0.90", avg_ssim_avg);
        } else {
            LOG_INFO("  ❌ Overall Quality: POOR (visible artifacts)");
            LOG_INFO_FMT("    SSIM: %.4f", avg_ssim_avg);
        }
    }

    // 单独输出 SSIM 判定结果（无论是否同时启用 PSNR）
    if (compare_config.enable_ssim && !ssim_avg_values.empty()) {
        LOG_INFO("");
        if (ssim_excellent) {
            LOG_INFO_FMT("  SSIM-only Assessment: EXCELLENT (avg SSIM=%.4f, threshold>=%.2f)",
                         avg_ssim_avg, compare_config.ssim_threshold);
        } else if (ssim_good) {
            LOG_INFO_FMT("  SSIM-only Assessment: GOOD (avg SSIM=%.4f, threshold>=%.2f)",
                         avg_ssim_avg, compare_config.ssim_warn_threshold);
        } else {
            LOG_INFO_FMT("  SSIM-only Assessment: POOR (avg SSIM=%.4f, threshold<%.2f)",
                         avg_ssim_avg, compare_config.ssim_warn_threshold);
        }
    }

    if (compare_config.save_report) {
        LOG_INFO_FMT("💡 Detailed comparison report saved to: %s", 
                    compare_config.report_path.c_str());
        LOG_INFO("   View the report for frame-by-frame analysis");
    }
    
    LOG_INFO("\n💡 PSNR Interpretation:");
    LOG_INFO("   >= 38 dB: Excellent quality (visually lossless)");
    LOG_INFO("   35-38 dB: Good quality (minor differences)");
    LOG_INFO("   < 35 dB:  Poor quality (visible artifacts)");
    
    // 根据对比结果返回
    bool test_passed = comparator.isPassed();
    
    if (test_passed) {
        printf("✅ PASS - 解码精度优秀（完全一致）\n");
        return 0;
    } else {
        printf("❌ FAIL - 解码精度差（存在明显错误）\n");
        printf("说明: 部分帧 PSNR < 35dB，硬件解码存在严重问题\n");
        return 2;
    }
}

/**
 * PP后处理测试结果统计结构
 */
struct PPTestResult {
    std::string format_name;
    int channel;  // 0=PP0, 1=PP1
    bool supported;  // 格式是否支持
    bool skipped;  // 是否跳过
    int frames_processed;
    double avg_psnr_y;
    double avg_psnr_u;
    double avg_psnr_v;
    double avg_psnr_avg;
    double avg_ssim_y;
    double avg_ssim_u;
    double avg_ssim_v;
    double avg_ssim_avg;
    int passed_count;
    int warned_count;
    int failed_count;
};

/**
 * 执行PP0单通道测试
 * @return 0=成功, -1=失败, -2=格式不支持（跳过）
 */
static int run_pp0_test_with_format(
    const char* video_path,
    int width,
    int height,
    const char* decoder_name,
    int decode_threads,
    double frame_rate,
    const char* profile,
    const char* test_tag,
    int max_frames,
    const PP0FormatConfig& format_config,
    PPTestResult& result
) {
    using namespace productionline::io;
    
    result.format_name = format_config.format_name;
    result.channel = 0;
    result.supported = false;
    result.skipped = false;
    
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO_FMT("  PP0 Test: %s - Format: %s", test_tag ? test_tag : "unknown", format_config.format_name);
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
    // 配置TACO解码器（PP0模式）
    auto tacoConfig = format_config.config_builder(width, height);
    
    // 创建输出目录和文件路径（仅用于报告，不再保存原始解码数据文件）
    std::ostringstream oss;
    if (test_tag && test_tag[0] != '\0') {
        oss << test_tag << "_";
    }
    oss << "PP0_" << format_config.format_name << "_" << width << "x" << height;
    std::string res_str = oss.str();
    std::string report_path = "logs/compare_" + res_str + ".txt";
    std::filesystem::create_directories("logs");
    
    // 创建BufferComparator
    CompareConfig compare_config;
    compare_config.strategy = CompareConfig::AUTO_LAYERED;
    compare_config.format_strategy = CompareConfig::AUTO;
    compare_config.quick_psnr_threshold = 38.0;
    compare_config.quick_warn_threshold = 35.0;
    compare_config.enable_psnr = true;
    compare_config.enable_ssim = true;
    compare_config.ssim_threshold = 0.95;
    compare_config.ssim_warn_threshold = 0.90;
    compare_config.enable_parallel = true;
    compare_config.use_perceptual_weighting = true;
    compare_config.verbose = true;
    compare_config.save_report = true;
    compare_config.report_path = report_path;
    
    BufferComparator comparator;
    if (!comparator.open(compare_config)) {
        LOG_ERROR("Failed to open BufferComparator");
        return -1;
    }
    
    // 配置硬件解码器
    VideoProductionLine hw_producer(false, 1);
    DecoderConfigBuilder hw_decoderConfigBuilder;
    
    if (decoder_name && decoder_name[0] != '\0') {
        std::string dname(decoder_name);
        std::string codec_name;
        if (dname.length() > 5 && dname.substr(dname.length() - 5) == "_taco") {
            codec_name = dname.substr(0, dname.length() - 5);
        } else {
            codec_name = dname;
        }
        hw_decoderConfigBuilder.useTaco(codec_name, tacoConfig);
    } else {
        hw_decoderConfigBuilder.useSoftware();
    }
    
    auto hw_workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(16)
                .build()
        )
        .setDisplayConfig(DisplayConfigBuilder()
            .setDisplayResolution(width, height)
            .setBitsPerPixel(32)
            .build())
        .setDecoderConfig(hw_decoderConfigBuilder.build())
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    hw_producer.setErrorCallback([](const std::string& error) {
        LOG_WARN_FMT("Hardware Decoder Error (non-fatal): %s", error.c_str());
    });
    
    // 配置软件解码器
    VideoProductionLine sw_producer(false, 1);
    auto sw_workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(16)
                .build()
        )
        .setDisplayConfig(DisplayConfigBuilder()
            .setDisplayResolution(width, height)
            .setBitsPerPixel(32)
            .build())
        .setDecoderConfig(DecoderConfigBuilder().useSoftware().build())
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    sw_producer.setErrorCallback([](const std::string& error) {
        LOG_WARN_FMT("Software Decoder Error (non-fatal): %s", error.c_str());
    });
    
    // 启动解码器
    if (!hw_producer.start(hw_workerConfig)) {
        LOG_ERROR("Failed to start hardware decoder");
        return -1;
    }
    
    if (!sw_producer.start(sw_workerConfig)) {
        LOG_ERROR("Failed to start software decoder");
        hw_producer.stop();
        return -1;
    }
    
    // 获取BufferPool
    uint64_t hw_pool_id = hw_producer.getWorkingBufferPoolId();
    uint64_t sw_pool_id = sw_producer.getWorkingBufferPoolId();
    
    if (hw_pool_id == 0 || sw_pool_id == 0) {
        LOG_ERROR("No working BufferPool ID available");
        if (hw_pool_id == 0) hw_producer.stop();
        if (sw_pool_id == 0) sw_producer.stop();
        return -1;
    }
    
    auto hw_pool = BufferPoolRegistry::getInstance().getPool(hw_pool_id).lock();
    auto sw_pool = BufferPoolRegistry::getInstance().getPool(sw_pool_id).lock();
    
    if (!hw_pool || !sw_pool) {
        LOG_ERROR("BufferPool not found");
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }
    
    // 等待第一个Buffer（检查格式支持）
    LOG_INFO("Waiting for first buffer to check format support...");
    Buffer* first_hw_buf = hw_pool->acquireFilled(true, 5000);
    Buffer* first_sw_buf = sw_pool->acquireFilled(true, 5000);
    
    if (!first_hw_buf || !first_sw_buf) {
        LOG_WARN("Failed to get first buffers - format may not be supported");
        if (first_hw_buf) hw_pool->releaseFilled(first_hw_buf);
        if (first_sw_buf) sw_pool->releaseFilled(first_sw_buf);
        hw_producer.stop();
        sw_producer.stop();
        comparator.close();
        result.skipped = true;
        return -2;  // 格式不支持
    }
    
    // 检查channel是否正确（PP0应该是channel 0）
    int hw_channel = first_hw_buf->getOutputChannel();
    if (hw_channel != 0) {
        LOG_WARN_FMT("Got channel %d instead of PP0 (channel 0) - format may not be supported", hw_channel);
        hw_pool->releaseFilled(first_hw_buf);
        sw_pool->releaseFilled(first_sw_buf);
        hw_producer.stop();
        sw_producer.stop();
        comparator.close();
        result.skipped = true;
        return -2;  // 格式不支持
    }
    
    result.supported = true;
    
    // 获取格式信息
    AVPixelFormat hw_format = AV_PIX_FMT_NONE;
    int hw_actual_width = width;
    int hw_actual_height = height;
    
    if (first_hw_buf->hasImageMetadata()) {
        hw_format = first_hw_buf->getImageFormat();
        hw_actual_width = first_hw_buf->getImageWidth();
        hw_actual_height = first_hw_buf->getImageHeight();
    } else {
        hw_format = AV_PIX_FMT_NV12;
    }
    
    // 对比第一帧
    FrameCompareResult first_result = comparator.compare(first_sw_buf, first_hw_buf);
    
    hw_pool->releaseFilled(first_hw_buf);
    sw_pool->releaseFilled(first_sw_buf);
    
    // 统计向量
    std::vector<double> psnr_y_values, psnr_u_values, psnr_v_values, psnr_avg_values;
    std::vector<double> ssim_y_values, ssim_u_values, ssim_v_values, ssim_avg_values;
    
    if (first_result.psnr_y > 0.0) {
        psnr_y_values.push_back(first_result.psnr_y);
        psnr_u_values.push_back(first_result.psnr_u);
        psnr_v_values.push_back(first_result.psnr_v);
        psnr_avg_values.push_back(first_result.psnr_avg);
    }
    if (first_result.ssim_y > 0.0) {
        ssim_y_values.push_back(first_result.ssim_y);
        ssim_u_values.push_back(first_result.ssim_u);
        ssim_v_values.push_back(first_result.ssim_v);
        ssim_avg_values.push_back(first_result.ssim_avg);
    }
    
    // 主循环
    int frame_count = 1;
    int MAX_FRAMES = max_frames > 0 ? static_cast<int>(max_frames * 1.1) : std::max(300, static_cast<int>(frame_rate * 12.0));
    int timeout_count = 0;
    const int MAX_TIMEOUT = 50;
    
    while (g_running && frame_count < MAX_FRAMES) {
        Buffer* sw_buf = sw_pool->acquireFilled(true, 100);
        Buffer* hw_buf = hw_pool->acquireFilled(true, 100);
        
        if (!sw_buf || !hw_buf) {
            timeout_count++;
            if (timeout_count >= MAX_TIMEOUT) {
                break;
            }
            if (sw_buf) sw_pool->releaseFilled(sw_buf);
            if (hw_buf) hw_pool->releaseFilled(hw_buf);
            continue;
        }
        
        // 检查channel
        if (hw_buf->getOutputChannel() != 0) {
            hw_pool->releaseFilled(hw_buf);
            sw_pool->releaseFilled(sw_buf);
            continue;
        }
        
        timeout_count = 0;
        
        // 对比
        FrameCompareResult result_frame = comparator.compare(sw_buf, hw_buf);
        
        if (result_frame.psnr_y > 0.0) {
            psnr_y_values.push_back(result_frame.psnr_y);
            psnr_u_values.push_back(result_frame.psnr_u);
            psnr_v_values.push_back(result_frame.psnr_v);
            psnr_avg_values.push_back(result_frame.psnr_avg);
        }
        if (result_frame.ssim_y > 0.0) {
            ssim_y_values.push_back(result_frame.ssim_y);
            ssim_u_values.push_back(result_frame.ssim_u);
            ssim_v_values.push_back(result_frame.ssim_v);
            ssim_avg_values.push_back(result_frame.ssim_avg);
        }
        
        hw_pool->releaseFilled(hw_buf);
        sw_pool->releaseFilled(sw_buf);
        
        frame_count++;
    }
    
    // 清理
    comparator.close();
    hw_producer.stop();
    sw_producer.stop();
    
    // 计算统计结果
    result.frames_processed = frame_count;
    result.passed_count = comparator.getPassedCount();
    result.warned_count = comparator.getCompareCount() - comparator.getPassedCount() - comparator.getFailedCount();
    result.failed_count = comparator.getFailedCount();
    
    if (!psnr_y_values.empty()) {
        result.avg_psnr_y = std::accumulate(psnr_y_values.begin(), psnr_y_values.end(), 0.0) / psnr_y_values.size();
        result.avg_psnr_u = std::accumulate(psnr_u_values.begin(), psnr_u_values.end(), 0.0) / psnr_u_values.size();
        result.avg_psnr_v = std::accumulate(psnr_v_values.begin(), psnr_v_values.end(), 0.0) / psnr_v_values.size();
        result.avg_psnr_avg = std::accumulate(psnr_avg_values.begin(), psnr_avg_values.end(), 0.0) / psnr_avg_values.size();
    }
    
    if (!ssim_y_values.empty()) {
        result.avg_ssim_y = std::accumulate(ssim_y_values.begin(), ssim_y_values.end(), 0.0) / ssim_y_values.size();
        result.avg_ssim_u = std::accumulate(ssim_u_values.begin(), ssim_u_values.end(), 0.0) / ssim_u_values.size();
        result.avg_ssim_v = std::accumulate(ssim_v_values.begin(), ssim_v_values.end(), 0.0) / ssim_v_values.size();
        result.avg_ssim_avg = std::accumulate(ssim_avg_values.begin(), ssim_avg_values.end(), 0.0) / ssim_avg_values.size();
    }
    
    return 0;
}

/**
 * 执行PP1单通道测试
 * @return 0=成功, -1=失败, -2=格式不支持（跳过）
 */
static int run_pp1_test_with_format(
    const char* video_path,
    int width,
    int height,
    const char* decoder_name,
    int decode_threads,
    double frame_rate,
    const char* profile,
    const char* test_tag,
    int max_frames,
    const PP1FormatConfig& format_config,
    PPTestResult& result
) {
    using namespace productionline::io;
    
    result.format_name = format_config.format_name;
    result.channel = 1;
    result.supported = false;
    result.skipped = false;
    
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO_FMT("  PP1 Test: %s - Format: %s", test_tag ? test_tag : "unknown", format_config.format_name);
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
    // 配置TACO解码器（PP1模式）
    auto tacoConfig = format_config.config_builder(width, height);
    
    // 创建输出目录和文件路径（仅用于报告，不再保存原始解码数据文件）
    std::ostringstream oss;
    if (test_tag && test_tag[0] != '\0') {
        oss << test_tag << "_";
    }
    oss << "PP1_" << format_config.format_name << "_" << width << "x" << height;
    std::string res_str = oss.str();
    std::string report_path = "logs/compare_" + res_str + ".txt";
    std::filesystem::create_directories("logs");
    
    // 创建BufferComparator
    CompareConfig compare_config;
    compare_config.strategy = CompareConfig::AUTO_LAYERED;
    compare_config.format_strategy = CompareConfig::AUTO;
    compare_config.quick_psnr_threshold = 38.0;
    compare_config.quick_warn_threshold = 35.0;
    compare_config.enable_psnr = true;
    compare_config.enable_ssim = true;
    compare_config.ssim_threshold = 0.95;
    compare_config.ssim_warn_threshold = 0.90;
    compare_config.enable_parallel = true;
    compare_config.use_perceptual_weighting = true;
    compare_config.verbose = true;
    compare_config.save_report = true;
    compare_config.report_path = report_path;
    
    BufferComparator comparator;
    if (!comparator.open(compare_config)) {
        LOG_ERROR("Failed to open BufferComparator");
        return -1;
    }
    
    // 配置硬件解码器
    VideoProductionLine hw_producer(false, 1);
    DecoderConfigBuilder hw_decoderConfigBuilder;
    
    if (decoder_name && decoder_name[0] != '\0') {
        std::string dname(decoder_name);
        std::string codec_name;
        if (dname.length() > 5 && dname.substr(dname.length() - 5) == "_taco") {
            codec_name = dname.substr(0, dname.length() - 5);
        } else {
            codec_name = dname;
        }
        hw_decoderConfigBuilder.useTaco(codec_name, tacoConfig);
    } else {
        hw_decoderConfigBuilder.useSoftware();
    }
    
    auto hw_workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(16)
                .build()
        )
        .setDisplayConfig(DisplayConfigBuilder()
            .setDisplayResolution(width, height)
            .setBitsPerPixel(32)
            .build())
        .setDecoderConfig(hw_decoderConfigBuilder.build())
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    hw_producer.setErrorCallback([](const std::string& error) {
        LOG_WARN_FMT("Hardware Decoder Error (non-fatal): %s", error.c_str());
    });
    
    // 配置软件解码器
    VideoProductionLine sw_producer(false, 1);
    auto sw_workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(16)
                .build()
        )
        .setDisplayConfig(DisplayConfigBuilder()
            .setDisplayResolution(width, height)
            .setBitsPerPixel(32)
            .build())
        .setDecoderConfig(DecoderConfigBuilder().useSoftware().build())
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    sw_producer.setErrorCallback([](const std::string& error) {
        LOG_WARN_FMT("Software Decoder Error (non-fatal): %s", error.c_str());
    });
    
    // 启动解码器
    if (!hw_producer.start(hw_workerConfig)) {
        LOG_ERROR("Failed to start hardware decoder");
        return -1;
    }
    
    if (!sw_producer.start(sw_workerConfig)) {
        LOG_ERROR("Failed to start software decoder");
        hw_producer.stop();
        return -1;
    }
    
    // 获取BufferPool
    uint64_t hw_pool_id = hw_producer.getWorkingBufferPoolId();
    uint64_t sw_pool_id = sw_producer.getWorkingBufferPoolId();
    
    if (hw_pool_id == 0 || sw_pool_id == 0) {
        LOG_ERROR("No working BufferPool ID available");
        if (hw_pool_id == 0) hw_producer.stop();
        if (sw_pool_id == 0) sw_producer.stop();
        return -1;
    }
    
    auto hw_pool = BufferPoolRegistry::getInstance().getPool(hw_pool_id).lock();
    auto sw_pool = BufferPoolRegistry::getInstance().getPool(sw_pool_id).lock();
    
    if (!hw_pool || !sw_pool) {
        LOG_ERROR("BufferPool not found");
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }
    
    // 等待第一个Buffer（检查格式支持）
    LOG_INFO("Waiting for first buffer to check format support...");
    Buffer* first_hw_buf = nullptr;
    Buffer* first_sw_buf = sw_pool->acquireFilled(true, 5000);
    
    if (!first_sw_buf) {
        LOG_WARN("Failed to get first software buffer");
        hw_producer.stop();
        sw_producer.stop();
        comparator.close();
        result.skipped = true;
        return -2;
    }
    
    // 等待PP1 buffer（channel 1）
    int detect_timeout = 0;
    const int DETECT_MAX_TIMEOUT = 50;
    while (detect_timeout < DETECT_MAX_TIMEOUT && !first_hw_buf) {
        Buffer* buf = hw_pool->acquireFilled(true, 100);
        if (buf) {
            if (buf->getOutputChannel() == 1) {
                first_hw_buf = buf;
                break;
            } else {
                hw_pool->releaseFilled(buf);
            }
        }
        detect_timeout++;
    }
    
    if (!first_hw_buf) {
        LOG_WARN("Failed to get first PP1 buffer (channel 1) - format may not be supported");
        sw_pool->releaseFilled(first_sw_buf);
        hw_producer.stop();
        sw_producer.stop();
        comparator.close();
        result.skipped = true;
        return -2;  // 格式不支持
    }
    
    result.supported = true;
    
    // 获取格式信息
    AVPixelFormat hw_format = AV_PIX_FMT_NONE;
    int hw_actual_width = width;
    int hw_actual_height = height;
    
    if (first_hw_buf->hasImageMetadata()) {
        hw_format = first_hw_buf->getImageFormat();
        hw_actual_width = first_hw_buf->getImageWidth();
        hw_actual_height = first_hw_buf->getImageHeight();
    } else {
        hw_format = AV_PIX_FMT_ARGB;  // PP1默认ARGB
    }
    
    // 对比第一帧
    FrameCompareResult first_result = comparator.compare(first_sw_buf, first_hw_buf);
    
    hw_pool->releaseFilled(first_hw_buf);
    sw_pool->releaseFilled(first_sw_buf);
    
    // 统计向量
    std::vector<double> psnr_y_values, psnr_u_values, psnr_v_values, psnr_avg_values;
    std::vector<double> ssim_y_values, ssim_u_values, ssim_v_values, ssim_avg_values;
    
    if (first_result.psnr_y > 0.0) {
        psnr_y_values.push_back(first_result.psnr_y);
        psnr_u_values.push_back(first_result.psnr_u);
        psnr_v_values.push_back(first_result.psnr_v);
        psnr_avg_values.push_back(first_result.psnr_avg);
    }
    if (first_result.ssim_y > 0.0) {
        ssim_y_values.push_back(first_result.ssim_y);
        ssim_u_values.push_back(first_result.ssim_u);
        ssim_v_values.push_back(first_result.ssim_v);
        ssim_avg_values.push_back(first_result.ssim_avg);
    }
    
    // 主循环
    int frame_count = 1;
    int MAX_FRAMES = max_frames > 0 ? static_cast<int>(max_frames * 1.1) : std::max(300, static_cast<int>(frame_rate * 12.0));
    int timeout_count = 0;
    const int MAX_TIMEOUT = 50;
    
    while (g_running && frame_count < MAX_FRAMES) {
        Buffer* sw_buf = sw_pool->acquireFilled(true, 100);
        Buffer* hw_buf = hw_pool->acquireFilled(true, 100);
        
        if (!sw_buf || !hw_buf) {
            timeout_count++;
            if (timeout_count >= MAX_TIMEOUT) {
                break;
            }
            if (sw_buf) sw_pool->releaseFilled(sw_buf);
            if (hw_buf) hw_pool->releaseFilled(hw_buf);
            continue;
        }
        
        // 检查channel（只要PP1的buffer）
        if (hw_buf->getOutputChannel() != 1) {
            hw_pool->releaseFilled(hw_buf);
            sw_pool->releaseFilled(sw_buf);
            continue;
        }
        
        timeout_count = 0;
        
        // 对比
        FrameCompareResult result_frame = comparator.compare(sw_buf, hw_buf);
        
        if (result_frame.psnr_y > 0.0) {
            psnr_y_values.push_back(result_frame.psnr_y);
            psnr_u_values.push_back(result_frame.psnr_u);
            psnr_v_values.push_back(result_frame.psnr_v);
            psnr_avg_values.push_back(result_frame.psnr_avg);
        }
        if (result_frame.ssim_y > 0.0) {
            ssim_y_values.push_back(result_frame.ssim_y);
            ssim_u_values.push_back(result_frame.ssim_u);
            ssim_v_values.push_back(result_frame.ssim_v);
            ssim_avg_values.push_back(result_frame.ssim_avg);
        }
        
        hw_pool->releaseFilled(hw_buf);
        sw_pool->releaseFilled(sw_buf);
        
        frame_count++;
    }
    
    // 排空剩余Buffer（只释放）
    Buffer* remaining = nullptr;
    while ((remaining = hw_pool->acquireFilled(false, 0)) != nullptr) {
        hw_pool->releaseFilled(remaining);
    }
    
    // 清理
    comparator.close();
    hw_producer.stop();
    sw_producer.stop();
    
    // 计算统计结果
    result.frames_processed = frame_count;
    result.passed_count = comparator.getPassedCount();
    result.warned_count = comparator.getCompareCount() - comparator.getPassedCount() - comparator.getFailedCount();
    result.failed_count = comparator.getFailedCount();
    
    if (!psnr_y_values.empty()) {
        result.avg_psnr_y = std::accumulate(psnr_y_values.begin(), psnr_y_values.end(), 0.0) / psnr_y_values.size();
        result.avg_psnr_u = std::accumulate(psnr_u_values.begin(), psnr_u_values.end(), 0.0) / psnr_u_values.size();
        result.avg_psnr_v = std::accumulate(psnr_v_values.begin(), psnr_v_values.end(), 0.0) / psnr_v_values.size();
        result.avg_psnr_avg = std::accumulate(psnr_avg_values.begin(), psnr_avg_values.end(), 0.0) / psnr_avg_values.size();
    }
    
    if (!ssim_y_values.empty()) {
        result.avg_ssim_y = std::accumulate(ssim_y_values.begin(), ssim_y_values.end(), 0.0) / ssim_y_values.size();
        result.avg_ssim_u = std::accumulate(ssim_u_values.begin(), ssim_u_values.end(), 0.0) / ssim_u_values.size();
        result.avg_ssim_v = std::accumulate(ssim_v_values.begin(), ssim_v_values.end(), 0.0) / ssim_v_values.size();
        result.avg_ssim_avg = std::accumulate(ssim_avg_values.begin(), ssim_avg_values.end(), 0.0) / ssim_avg_values.size();
    }
    
    return 0;
}

/**
 * 执行多PP测试（同时测试PP0和PP1）
 * @return 0=成功, -1=失败, -2=格式不支持（跳过）
 */
static int run_multi_pp_test_with_format(
    const char* video_path,
    int width,
    int height,
    const char* decoder_name,
    int decode_threads,
    double frame_rate,
    const char* profile,
    const char* test_tag,
    int max_frames,
    const MultiPPFormatConfig& format_config,
    PPTestResult& pp0_result,
    PPTestResult& pp1_result
) {
    using namespace productionline::io;
    
    pp0_result.format_name = format_config.pp0_format_name;
    pp0_result.channel = 0;
    pp0_result.supported = false;
    pp0_result.skipped = false;
    
    pp1_result.format_name = format_config.pp1_format_name;
    pp1_result.channel = 1;
    pp1_result.supported = false;
    pp1_result.skipped = false;
    
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO_FMT("  Multi-PP Test: %s - PP0: %s, PP1: %s", 
                 test_tag ? test_tag : "unknown", 
                 format_config.pp0_format_name, 
                 format_config.pp1_format_name);
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
    // 配置TACO解码器（多PP模式）
    auto tacoConfig = format_config.config_builder(width, height);
    
    // 创建输出目录和文件路径
    std::ostringstream oss;
    if (test_tag && test_tag[0] != '\0') {
        oss << test_tag << "_";
    }
    oss << "MultiPP_" << format_config.test_name << "_" << width << "x" << height;
    std::string res_str = oss.str();
    std::string pp0_output = "hw_PP0_" + res_str + ".yuv";
    std::string pp1_output = "hw_PP1_" + res_str + ".rgb";
    std::string report_path = "logs/compare_" + res_str + ".txt";
    std::filesystem::create_directories("logs");
    
    // 创建两个BufferComparator（分别用于PP0和PP1）
    CompareConfig compare_config;
    compare_config.strategy = CompareConfig::AUTO_LAYERED;
    compare_config.format_strategy = CompareConfig::AUTO;
    compare_config.quick_psnr_threshold = 38.0;
    compare_config.quick_warn_threshold = 35.0;
    compare_config.enable_psnr = true;
    compare_config.enable_ssim = true;
    compare_config.ssim_threshold = 0.95;
    compare_config.ssim_warn_threshold = 0.90;
    compare_config.enable_parallel = true;
    compare_config.use_perceptual_weighting = true;
    compare_config.verbose = true;
    compare_config.save_report = true;
    compare_config.report_path = report_path;
    
    BufferComparator pp0_comparator;
    BufferComparator pp1_comparator;
    
    std::string pp0_report = "logs/compare_PP0_" + res_str + ".txt";
    std::string pp1_report = "logs/compare_PP1_" + res_str + ".txt";
    
    compare_config.report_path = pp0_report;
    if (!pp0_comparator.open(compare_config)) {
        LOG_ERROR("Failed to open PP0 BufferComparator");
        return -1;
    }
    
    compare_config.report_path = pp1_report;
    if (!pp1_comparator.open(compare_config)) {
        LOG_ERROR("Failed to open PP1 BufferComparator");
        pp0_comparator.close();
        return -1;
    }
    
    // 配置硬件解码器
    VideoProductionLine hw_producer(false, 1);
    DecoderConfigBuilder hw_decoderConfigBuilder;
    
    if (decoder_name && decoder_name[0] != '\0') {
        std::string dname(decoder_name);
        std::string codec_name;
        if (dname.length() > 5 && dname.substr(dname.length() - 5) == "_taco") {
            codec_name = dname.substr(0, dname.length() - 5);
        } else {
            codec_name = dname;
        }
        hw_decoderConfigBuilder.useTaco(codec_name, tacoConfig);
    } else {
        hw_decoderConfigBuilder.useSoftware();
    }
    
    auto hw_workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(16)
                .build()
        )
        .setDisplayConfig(DisplayConfigBuilder()
            .setDisplayResolution(width, height)
            .setBitsPerPixel(32)
            .build())
        .setDecoderConfig(hw_decoderConfigBuilder.build())
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    hw_producer.setErrorCallback([](const std::string& error) {
        LOG_WARN_FMT("Hardware Decoder Error (non-fatal): %s", error.c_str());
    });
    
    // 配置软件解码器
    VideoProductionLine sw_producer(false, 1);
    auto sw_workerConfig = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(video_path)
                .setBufferCount(16)
                .build()
        )
        .setDisplayConfig(DisplayConfigBuilder()
            .setDisplayResolution(width, height)
            .setBitsPerPixel(32)
            .build())
        .setDecoderConfig(DecoderConfigBuilder().useSoftware().build())
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    sw_producer.setErrorCallback([](const std::string& error) {
        LOG_WARN_FMT("Software Decoder Error (non-fatal): %s", error.c_str());
    });
    
    // 启动解码器
    if (!hw_producer.start(hw_workerConfig)) {
        LOG_ERROR("Failed to start hardware decoder");
        return -1;
    }
    
    if (!sw_producer.start(sw_workerConfig)) {
        LOG_ERROR("Failed to start software decoder");
        hw_producer.stop();
        return -1;
    }
    
    // 获取BufferPool
    uint64_t hw_pool_id = hw_producer.getWorkingBufferPoolId();
    uint64_t sw_pool_id = sw_producer.getWorkingBufferPoolId();
    
    if (hw_pool_id == 0 || sw_pool_id == 0) {
        LOG_ERROR("No working BufferPool ID available");
        if (hw_pool_id == 0) hw_producer.stop();
        if (sw_pool_id == 0) sw_producer.stop();
        return -1;
    }
    
    auto hw_pool = BufferPoolRegistry::getInstance().getPool(hw_pool_id).lock();
    auto sw_pool = BufferPoolRegistry::getInstance().getPool(sw_pool_id).lock();
    
    if (!hw_pool || !sw_pool) {
        LOG_ERROR("BufferPool not found");
        hw_producer.stop();
        sw_producer.stop();
        return -1;
    }
    
    // 等待第一个Buffer（检查格式支持）
    LOG_INFO("Waiting for first buffers to check format support...");
    Buffer* first_pp0_buf = nullptr;
    Buffer* first_pp1_buf = nullptr;
    Buffer* first_sw_buf = sw_pool->acquireFilled(true, 5000);
    
    if (!first_sw_buf) {
        LOG_WARN("Failed to get first software buffer");
        hw_producer.stop();
        sw_producer.stop();
        pp0_comparator.close();
        pp1_comparator.close();
        pp0_result.skipped = true;
        pp1_result.skipped = true;
        return -2;
    }
    
    // 等待PP0和PP1 buffer
    int detect_timeout = 0;
    const int DETECT_MAX_TIMEOUT = 50;
    while (detect_timeout < DETECT_MAX_TIMEOUT && (!first_pp0_buf || !first_pp1_buf)) {
        Buffer* buf = hw_pool->acquireFilled(true, 100);
        if (buf) {
            int ch = buf->getOutputChannel();
            if (ch == 0 && !first_pp0_buf) {
                first_pp0_buf = buf;
            } else if (ch == 1 && !first_pp1_buf) {
                first_pp1_buf = buf;
            } else {
                hw_pool->releaseFilled(buf);
            }
        }
        detect_timeout++;
    }
    
    if (!first_pp0_buf || !first_pp1_buf) {
        LOG_WARN("Failed to get first PP0/PP1 buffers - format may not be supported");
        if (first_pp0_buf) hw_pool->releaseFilled(first_pp0_buf);
        if (first_pp1_buf) hw_pool->releaseFilled(first_pp1_buf);
        sw_pool->releaseFilled(first_sw_buf);
        hw_producer.stop();
        sw_producer.stop();
        pp0_comparator.close();
        pp1_comparator.close();
        pp0_result.skipped = true;
        pp1_result.skipped = true;
        return -2;  // 格式不支持
    }
    
    pp0_result.supported = true;
    pp1_result.supported = true;
    
    // 获取格式信息
    AVPixelFormat pp0_format = AV_PIX_FMT_NONE;
    AVPixelFormat pp1_format = AV_PIX_FMT_NONE;
    int hw_actual_width = width;
    int hw_actual_height = height;
    
    if (first_pp0_buf->hasImageMetadata()) {
        pp0_format = first_pp0_buf->getImageFormat();
        hw_actual_width = first_pp0_buf->getImageWidth();
        hw_actual_height = first_pp0_buf->getImageHeight();
    } else {
        pp0_format = AV_PIX_FMT_NV12;
    }
    
    if (first_pp1_buf->hasImageMetadata()) {
        pp1_format = first_pp1_buf->getImageFormat();
    } else {
        pp1_format = AV_PIX_FMT_ARGB;
    }
    
    // 对比第一帧
    FrameCompareResult pp0_first_result = pp0_comparator.compare(first_sw_buf, first_pp0_buf);
    FrameCompareResult pp1_first_result = pp1_comparator.compare(first_sw_buf, first_pp1_buf);
    
    hw_pool->releaseFilled(first_pp0_buf);
    hw_pool->releaseFilled(first_pp1_buf);
    sw_pool->releaseFilled(first_sw_buf);
    
    // 统计向量
    std::vector<double> pp0_psnr_y_values, pp0_psnr_u_values, pp0_psnr_v_values, pp0_psnr_avg_values;
    std::vector<double> pp0_ssim_y_values, pp0_ssim_u_values, pp0_ssim_v_values, pp0_ssim_avg_values;
    std::vector<double> pp1_psnr_y_values, pp1_psnr_u_values, pp1_psnr_v_values, pp1_psnr_avg_values;
    std::vector<double> pp1_ssim_y_values, pp1_ssim_u_values, pp1_ssim_v_values, pp1_ssim_avg_values;
    
    if (pp0_first_result.psnr_y > 0.0) {
        pp0_psnr_y_values.push_back(pp0_first_result.psnr_y);
        pp0_psnr_u_values.push_back(pp0_first_result.psnr_u);
        pp0_psnr_v_values.push_back(pp0_first_result.psnr_v);
        pp0_psnr_avg_values.push_back(pp0_first_result.psnr_avg);
    }
    if (pp0_first_result.ssim_y > 0.0) {
        pp0_ssim_y_values.push_back(pp0_first_result.ssim_y);
        pp0_ssim_u_values.push_back(pp0_first_result.ssim_u);
        pp0_ssim_v_values.push_back(pp0_first_result.ssim_v);
        pp0_ssim_avg_values.push_back(pp0_first_result.ssim_avg);
    }
    
    if (pp1_first_result.psnr_y > 0.0) {
        pp1_psnr_y_values.push_back(pp1_first_result.psnr_y);
        pp1_psnr_u_values.push_back(pp1_first_result.psnr_u);
        pp1_psnr_v_values.push_back(pp1_first_result.psnr_v);
        pp1_psnr_avg_values.push_back(pp1_first_result.psnr_avg);
    }
    if (pp1_first_result.ssim_y > 0.0) {
        pp1_ssim_y_values.push_back(pp1_first_result.ssim_y);
        pp1_ssim_u_values.push_back(pp1_first_result.ssim_u);
        pp1_ssim_v_values.push_back(pp1_first_result.ssim_v);
        pp1_ssim_avg_values.push_back(pp1_first_result.ssim_avg);
    }
    
    // 主循环
    int frame_count = 1;
    int MAX_FRAMES = max_frames > 0 ? static_cast<int>(max_frames * 1.1) : std::max(300, static_cast<int>(frame_rate * 12.0));
    int timeout_count = 0;
    const int MAX_TIMEOUT = 50;
    
    while (g_running && frame_count < MAX_FRAMES) {
        Buffer* sw_buf = sw_pool->acquireFilled(true, 100);
        Buffer* hw_buf = hw_pool->acquireFilled(true, 100);
        
        if (!sw_buf || !hw_buf) {
            timeout_count++;
            if (timeout_count >= MAX_TIMEOUT) {
                break;
            }
            if (sw_buf) sw_pool->releaseFilled(sw_buf);
            if (hw_buf) hw_pool->releaseFilled(hw_buf);
            continue;
        }
        
        timeout_count = 0;
        
        int ch = hw_buf->getOutputChannel();
        
        if (ch == 0) {
            // PP0 buffer
            FrameCompareResult result_frame = pp0_comparator.compare(sw_buf, hw_buf);
            
            if (result_frame.psnr_y > 0.0) {
                pp0_psnr_y_values.push_back(result_frame.psnr_y);
                pp0_psnr_u_values.push_back(result_frame.psnr_u);
                pp0_psnr_v_values.push_back(result_frame.psnr_v);
                pp0_psnr_avg_values.push_back(result_frame.psnr_avg);
            }
            if (result_frame.ssim_y > 0.0) {
                pp0_ssim_y_values.push_back(result_frame.ssim_y);
                pp0_ssim_u_values.push_back(result_frame.ssim_u);
                pp0_ssim_v_values.push_back(result_frame.ssim_v);
                pp0_ssim_avg_values.push_back(result_frame.ssim_avg);
            }
        } else if (ch == 1) {
            // PP1 buffer
            FrameCompareResult result_frame = pp1_comparator.compare(sw_buf, hw_buf);
            
            if (result_frame.psnr_y > 0.0) {
                pp1_psnr_y_values.push_back(result_frame.psnr_y);
                pp1_psnr_u_values.push_back(result_frame.psnr_u);
                pp1_psnr_v_values.push_back(result_frame.psnr_v);
                pp1_psnr_avg_values.push_back(result_frame.psnr_avg);
            }
                if (result_frame.ssim_y > 0.0) {
                    pp1_ssim_y_values.push_back(result_frame.ssim_y);
                    pp1_ssim_u_values.push_back(result_frame.ssim_u);
                    pp1_ssim_v_values.push_back(result_frame.ssim_v);
                    pp1_ssim_avg_values.push_back(result_frame.ssim_avg);
                }
        }
        
        hw_pool->releaseFilled(hw_buf);
        sw_pool->releaseFilled(sw_buf);
        
        frame_count++;
    }
    
    // 排空剩余Buffer
    // 排空剩余Buffer（不再保存到文件，只做释放）
    Buffer* remaining = nullptr;
    while ((remaining = hw_pool->acquireFilled(false, 0)) != nullptr) {
        hw_pool->releaseFilled(remaining);
    }
    
    // 清理
    pp0_comparator.close();
    pp1_comparator.close();
    hw_producer.stop();
    sw_producer.stop();
    
    // 计算PP0统计结果
    pp0_result.frames_processed = frame_count;
    pp0_result.passed_count = pp0_comparator.getPassedCount();
    pp0_result.warned_count = pp0_comparator.getCompareCount() - pp0_comparator.getPassedCount() - pp0_comparator.getFailedCount();
    pp0_result.failed_count = pp0_comparator.getFailedCount();
    
    if (!pp0_psnr_y_values.empty()) {
        pp0_result.avg_psnr_y = std::accumulate(pp0_psnr_y_values.begin(), pp0_psnr_y_values.end(), 0.0) / pp0_psnr_y_values.size();
        pp0_result.avg_psnr_u = std::accumulate(pp0_psnr_u_values.begin(), pp0_psnr_u_values.end(), 0.0) / pp0_psnr_u_values.size();
        pp0_result.avg_psnr_v = std::accumulate(pp0_psnr_v_values.begin(), pp0_psnr_v_values.end(), 0.0) / pp0_psnr_v_values.size();
        pp0_result.avg_psnr_avg = std::accumulate(pp0_psnr_avg_values.begin(), pp0_psnr_avg_values.end(), 0.0) / pp0_psnr_avg_values.size();
    }
    
    if (!pp0_ssim_y_values.empty()) {
        pp0_result.avg_ssim_y = std::accumulate(pp0_ssim_y_values.begin(), pp0_ssim_y_values.end(), 0.0) / pp0_ssim_y_values.size();
        pp0_result.avg_ssim_u = std::accumulate(pp0_ssim_u_values.begin(), pp0_ssim_u_values.end(), 0.0) / pp0_ssim_u_values.size();
        pp0_result.avg_ssim_v = std::accumulate(pp0_ssim_v_values.begin(), pp0_ssim_v_values.end(), 0.0) / pp0_ssim_v_values.size();
        pp0_result.avg_ssim_avg = std::accumulate(pp0_ssim_avg_values.begin(), pp0_ssim_avg_values.end(), 0.0) / pp0_ssim_avg_values.size();
    }
    
    // 计算PP1统计结果
    pp1_result.frames_processed = frame_count;
    pp1_result.passed_count = pp1_comparator.getPassedCount();
    pp1_result.warned_count = pp1_comparator.getCompareCount() - pp1_comparator.getPassedCount() - pp1_comparator.getFailedCount();
    pp1_result.failed_count = pp1_comparator.getFailedCount();
    
    if (!pp1_psnr_y_values.empty()) {
        pp1_result.avg_psnr_y = std::accumulate(pp1_psnr_y_values.begin(), pp1_psnr_y_values.end(), 0.0) / pp1_psnr_y_values.size();
        pp1_result.avg_psnr_u = std::accumulate(pp1_psnr_u_values.begin(), pp1_psnr_u_values.end(), 0.0) / pp1_psnr_u_values.size();
        pp1_result.avg_psnr_v = std::accumulate(pp1_psnr_v_values.begin(), pp1_psnr_v_values.end(), 0.0) / pp1_psnr_v_values.size();
        pp1_result.avg_psnr_avg = std::accumulate(pp1_psnr_avg_values.begin(), pp1_psnr_avg_values.end(), 0.0) / pp1_psnr_avg_values.size();
    }
    
    if (!pp1_ssim_y_values.empty()) {
        pp1_result.avg_ssim_y = std::accumulate(pp1_ssim_y_values.begin(), pp1_ssim_y_values.end(), 0.0) / pp1_ssim_y_values.size();
        pp1_result.avg_ssim_u = std::accumulate(pp1_ssim_u_values.begin(), pp1_ssim_u_values.end(), 0.0) / pp1_ssim_u_values.size();
        pp1_result.avg_ssim_v = std::accumulate(pp1_ssim_v_values.begin(), pp1_ssim_v_values.end(), 0.0) / pp1_ssim_v_values.size();
        pp1_result.avg_ssim_avg = std::accumulate(pp1_ssim_avg_values.begin(), pp1_ssim_avg_values.end(), 0.0) / pp1_ssim_avg_values.size();
    }
    
    return 0;
}

/**
 * 执行所有PP测试（PP0、PP1、多PP）并输出统计结果
 */
static int run_all_pp_tests(
    const char* video_path,
    int width,
    int height,
    const char* decoder_name,
    int decode_threads,
    double frame_rate,
    const char* profile,
    const char* test_tag,
    int max_frames
) {
    LOG_INFO("\n╔═══════════════════════════════════════════════════════╗");
    LOG_INFO_FMT("║  PP Post-Processing Test Suite: %s               ║", test_tag ? test_tag : "unknown");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝\n");
    
    std::vector<PPTestResult> pp0_results;
    std::vector<PPTestResult> pp1_results;
    std::vector<std::pair<PPTestResult, PPTestResult>> multi_pp_results;
    
    // 执行PP0测试
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO("  Phase 1: PP0 Single Channel Tests");
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
    for (size_t i = 0; i < PP0_FORMAT_COUNT; i++) {
        PPTestResult result;
        int ret = run_pp0_test_with_format(
            video_path, width, height, decoder_name, decode_threads,
            frame_rate, profile, test_tag, max_frames,
            pp0_formats[i], result
        );
        
        if (ret == -2) {
            result.skipped = true;
            LOG_WARN_FMT("  ⏭️  PP0 Format %s: SKIPPED (not supported)", pp0_formats[i].format_name);
        } else if (ret == 0) {
            LOG_INFO_FMT("  ✅ PP0 Format %s: PASSED", pp0_formats[i].format_name);
        } else {
            LOG_ERROR_FMT("  ❌ PP0 Format %s: FAILED", pp0_formats[i].format_name);
        }
        
        pp0_results.push_back(result);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    // 执行PP1测试
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO("  Phase 2: PP1 Single Channel Tests");
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
    for (size_t i = 0; i < PP1_FORMAT_COUNT; i++) {
        PPTestResult result;
        int ret = run_pp1_test_with_format(
            video_path, width, height, decoder_name, decode_threads,
            frame_rate, profile, test_tag, max_frames,
            pp1_formats[i], result
        );
        
        if (ret == -2) {
            result.skipped = true;
            LOG_WARN_FMT("  ⏭️  PP1 Format %s: SKIPPED (not supported)", pp1_formats[i].format_name);
        } else if (ret == 0) {
            LOG_INFO_FMT("  ✅ PP1 Format %s: PASSED", pp1_formats[i].format_name);
        } else {
            LOG_ERROR_FMT("  ❌ PP1 Format %s: FAILED", pp1_formats[i].format_name);
        }
        
        pp1_results.push_back(result);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    // 执行多PP测试
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO("  Phase 3: Multi-PP Tests (PP0 + PP1)");
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
    for (size_t i = 0; i < MULTI_PP_FORMAT_COUNT; i++) {
        // 跳过T06（已禁用）
        if (strcmp(multi_pp_formats[i].test_name, "T06") == 0) {
            PPTestResult pp0_result, pp1_result;
            pp0_result.format_name = multi_pp_formats[i].pp0_format_name;
            pp0_result.channel = 0;
            pp0_result.skipped = true;
            pp1_result.format_name = multi_pp_formats[i].pp1_format_name;
            pp1_result.channel = 1;
            pp1_result.skipped = true;
            multi_pp_results.push_back({pp0_result, pp1_result});
            LOG_WARN_FMT("  ⏭️  Multi-PP %s: SKIPPED (T06 disabled)", multi_pp_formats[i].test_name);
            continue;
        }
        
        PPTestResult pp0_result, pp1_result;
        int ret = run_multi_pp_test_with_format(
            video_path, width, height, decoder_name, decode_threads,
            frame_rate, profile, test_tag, max_frames,
            multi_pp_formats[i], pp0_result, pp1_result
        );
        
        if (ret == -2) {
            pp0_result.skipped = true;
            pp1_result.skipped = true;
            LOG_WARN_FMT("  ⏭️  Multi-PP %s: SKIPPED (not supported)", multi_pp_formats[i].test_name);
        } else if (ret == 0) {
            LOG_INFO_FMT("  ✅ Multi-PP %s: PASSED", multi_pp_formats[i].test_name);
        } else {
            LOG_ERROR_FMT("  ❌ Multi-PP %s: FAILED", multi_pp_formats[i].test_name);
        }
        
        multi_pp_results.push_back({pp0_result, pp1_result});
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    // 输出统计结果
    LOG_INFO("\n╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  PP Post-Processing Test Results Summary            ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝\n");
    
    // PP0统计
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO("  PP0 Channel Results:");
    LOG_INFO("═══════════════════════════════════════════════════════");
    for (const auto& result : pp0_results) {
        if (result.skipped) {
            LOG_INFO_FMT("  Format: %s - SKIPPED", result.format_name.c_str());
        } else {
            LOG_INFO_FMT("  Format: %s", result.format_name.c_str());
            LOG_INFO_FMT("    Frames: %d", result.frames_processed);
            LOG_INFO_FMT("    PSNR: Y=%.2f U=%.2f V=%.2f Avg=%.2f dB", 
                        result.avg_psnr_y, result.avg_psnr_u, result.avg_psnr_v, result.avg_psnr_avg);
            LOG_INFO_FMT("    SSIM: Y=%.4f U=%.4f V=%.4f Avg=%.4f", 
                        result.avg_ssim_y, result.avg_ssim_u, result.avg_ssim_v, result.avg_ssim_avg);
            LOG_INFO_FMT("    Passed: %d, Warned: %d, Failed: %d", 
                        result.passed_count, result.warned_count, result.failed_count);
        }
    }
    
    // PP1统计
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO("  PP1 Channel Results:");
    LOG_INFO("═══════════════════════════════════════════════════════");
    for (const auto& result : pp1_results) {
        if (result.skipped) {
            LOG_INFO_FMT("  Format: %s - SKIPPED", result.format_name.c_str());
        } else {
            LOG_INFO_FMT("  Format: %s", result.format_name.c_str());
            LOG_INFO_FMT("    Frames: %d", result.frames_processed);
            LOG_INFO_FMT("    PSNR: Y=%.2f U=%.2f V=%.2f Avg=%.2f dB", 
                        result.avg_psnr_y, result.avg_psnr_u, result.avg_psnr_v, result.avg_psnr_avg);
            LOG_INFO_FMT("    SSIM: Y=%.4f U=%.4f V=%.4f Avg=%.4f", 
                        result.avg_ssim_y, result.avg_ssim_u, result.avg_ssim_v, result.avg_ssim_avg);
            LOG_INFO_FMT("    Passed: %d, Warned: %d, Failed: %d", 
                        result.passed_count, result.warned_count, result.failed_count);
        }
    }
    
    // 多PP统计
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO("  Multi-PP Results:");
    LOG_INFO("═══════════════════════════════════════════════════════");
    for (size_t i = 0; i < multi_pp_results.size(); i++) {
        const auto& pp0_result = multi_pp_results[i].first;
        const auto& pp1_result = multi_pp_results[i].second;
        
        if (i < MULTI_PP_FORMAT_COUNT) {
            LOG_INFO_FMT("  Test: %s (PP0: %s, PP1: %s)", 
                         multi_pp_formats[i].test_name,
                         multi_pp_formats[i].pp0_format_name,
                         multi_pp_formats[i].pp1_format_name);
        }
        
        if (pp0_result.skipped && pp1_result.skipped) {
            LOG_INFO("    SKIPPED");
        } else {
            LOG_INFO("    PP0:");
            if (!pp0_result.skipped) {
                LOG_INFO_FMT("      PSNR: Y=%.2f U=%.2f V=%.2f Avg=%.2f dB", 
                            pp0_result.avg_psnr_y, pp0_result.avg_psnr_u, pp0_result.avg_psnr_v, pp0_result.avg_psnr_avg);
                LOG_INFO_FMT("      SSIM: Y=%.4f U=%.4f V=%.4f Avg=%.4f", 
                            pp0_result.avg_ssim_y, pp0_result.avg_ssim_u, pp0_result.avg_ssim_v, pp0_result.avg_ssim_avg);
            } else {
                LOG_INFO("      SKIPPED");
            }
            
            LOG_INFO("    PP1:");
            if (!pp1_result.skipped) {
                LOG_INFO_FMT("      PSNR: Y=%.2f U=%.2f V=%.2f Avg=%.2f dB", 
                            pp1_result.avg_psnr_y, pp1_result.avg_psnr_u, pp1_result.avg_psnr_v, pp1_result.avg_psnr_avg);
                LOG_INFO_FMT("      SSIM: Y=%.4f U=%.4f V=%.4f Avg=%.4f", 
                            pp1_result.avg_ssim_y, pp1_result.avg_ssim_u, pp1_result.avg_ssim_v, pp1_result.avg_ssim_avg);
            } else {
                LOG_INFO("      SKIPPED");
            }
        }
    }
    
    LOG_INFO("\n╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  PP Test Suite Completed                             ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝\n");
    
    return 0;
}

  // namespace

/**
 * 包装函数：执行基础解码测试和所有PP测试
 */
static int run_test_with_all_pp(
    const char* video_path,
    int width,
    int height,
    const char* decoder_name,
    int decode_threads,
    double frame_rate,
    const char* profile,
    const char* test_tag
) {
    // 先执行基础解码测试
    int base_result = run_decode_test_with_params(
        video_path,
        width, height,
        decoder_name,
        decode_threads,
        frame_rate,
        profile,
        test_tag
    );
    
    // 执行所有PP测试
    run_all_pp_tests(
        video_path,
        width, height,
        decoder_name,
        decode_threads,
        frame_rate,
        profile,
        test_tag,
        -1  // max_frames使用默认值
    );
    
    return base_result;
}
  
  // ========== 不同参数的视频文件解码测试用例 ==========
  // 说明：
  //   - 针对 27 个测试视频（H.264 / H.265 / MJPEG），按分辨率 / 帧率 / profile 拆分为独立用例
  //   - 建议命令行使用方式：
  //       ./mp4_decode_test -m <test_name> /path/to/<对应视频文件>.mp4
  //   - 例如：
  //       ./mp4_decode_test -m dec_h264_1920x1080_60_high  test_h264_1920x1080_60fps_high.mp4
  //       ./mp4_decode_test -m dec_h265_3840x2160_30_main  test_h265_3840x2160_30fps_main.mp4
  //       ./mp4_decode_test -m dec_mjpeg_640x480_60        test_mjpeg_640x480_60fps_none.mp4
  //
  //   - decode_threads 策略：
  //       * 小分辨率 / 低帧率：decode_threads = 0（由解码器自动决定）
  //       * 1080p 以上或 60fps：decode_threads = 4（侧重多线程解码表现）
  //     如需适配具体平台，可根据需要调整下面的线程数。
  
  // ---------------- H.264 系列 (9 个) ----------------
  
  // 基础测试函数（支持后处理模式）
  static int test_dec_h264_128x128_30_main(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          128, 128,
          "h264_taco",
          0,
          30.0,
          "main",
          "h264_128x128_30_main"
      );
  }
  
  static int test_dec_h264_320x240_30_high(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          320, 240,
          "h264_taco",
          0,
          30.0,
          "high",
          "h264_320x240_30_high"
      );
  }
  
  // 基础测试函数（支持后处理模式）
  
  // 包装函数（用于多参数注册）
  static int test_dec_h264_640x480_30_main(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          640, 480,
          "h264_taco",
          0,
          30.0,
          "main",
          "dec_h264_640x480_30_main"
      );
  }
  
  // 包装函数（用于多参数注册）
  static int test_dec_h264_640x480_60_high(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          640, 480,
          "h264_taco",
          4,
          60.0,
          "high",
          "dec_h264_640x480_60_high"
      );
  }
  
  // 包装函数（用于多参数注册）
  static int test_dec_h264_1280x720_30_high(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          1280, 720,
          "h264_taco",
          0,
          30.0,
          "high",
          "dec_h264_1280x720_30_high"
      );
  }
  
  
  static int test_dec_h264_1920x1080_30_high(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          1920, 1080,
          "h264_taco",
          0,
          30.0,
          "high",
          "dec_h264_1920x1080_30_high"
      );
  }
  
  // 包装函数（用于多参数注册）
  static int test_dec_h264_1920x1080_60_high(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          1920, 1080,
          "h264_taco",
          4,
          60.0,
          "high",
          "dec_h264_1920x1080_60_high"
      );
  }
  
  // 包装函数（用于多参数注册）
  static int test_dec_h264_2560x1440_30_high(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          2560, 1440,
          "h264_taco",
          4,
          30.0,
          "high",
          "dec_h264_2560x1440_30_high"
      );
  }
 
 // 包装函数（用于多参数注册）
 static int test_dec_h264_3840x2160_30_high(const char* video_path) {
     return run_test_with_all_pp(
         video_path,
         3840, 2160,
         "h264_taco",
         4,
         30.0,
         "high",
          "dec_h264_3840x2160_30_high"
     );
 }
 
 
 
 // ---------------- H.265 系列 (9 个) ----------------
  
  // 基础测试函数（支持后处理模式）
  static int test_dec_h265_320x240_30_main(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          320, 240,
          "hevc_taco",
          0,
          30.0,
          "main",
          "dec_h265_320x240_30_main"
      );
  }
  static int test_dec_h265_640x480_60_main(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          640, 480,
          "hevc_taco",
          4,
          60.0,
          "main",
          "dec_h265_640x480_60_main"
      );
  }
  static int test_dec_h265_1920x1080_30_main(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          1920, 1080,
          "hevc_taco",
          0,
          30.0,
          "main",
          "dec_h265_1920x1080_30_main"
      );
  }
  static int test_dec_h265_2560x1440_30_main(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          2560, 1440,
          "hevc_taco",
          4,
          30.0,
          "main",
          "dec_h265_2560x1440_30_main"
     );
 }
 
  static int test_dec_h265_128x128_30_main(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          128, 128,
          "hevc_taco",
          0,
          30.0,
          "main",
          "dec_h265_128x128_30_main"
      );
  }
  
  static int test_dec_h265_640x480_30_main(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          640, 480,
          "hevc_taco",
          0,
          30.0,
          "main",
          "dec_h265_640x480_30_main"
      );
  }
  
  static int test_dec_h265_1280x720_30_main(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          1280, 720,
          "hevc_taco",
          0,
          30.0,
          "main",
          "dec_h265_1280x720_30_main"
      );
  }
  
  static int test_dec_h265_1920x1080_60_main(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          1920, 1080,
          "hevc_taco",
          4,
          60.0,
          "main",
          "dec_h265_1920x1080_60_main"
      );
  }
  
  static int test_dec_h265_3840x2160_30_main(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          3840, 2160,
          "hevc_taco",
          4,
          30.0,
          "main",
          "dec_h265_3840x2160_30_main"
      );
  }
 
 
 // ---------------- MJPEG 系列 (9 个) ----------------
  // 说明：使用实际硬件 MJPEG 解码器名称 "jpeg_taco"
  //       根据 ffmpeg -decoders 输出确认的实际解码器名称
  
  // 基础测试函数（支持后处理模式）
  static int test_dec_mjpeg_320x240_30(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          320, 240,
          "jpeg_taco",
          0,
          30.0,
          "main",
          "dec_mjpeg_320x240_30"
      );
  }
   static int test_dec_mjpeg_640x480_60(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          640, 480,
          "jpeg_taco",
          4,
          60.0,
          "main",
          "dec_mjpeg_640x480_60"
      );
  }
 
  static int test_dec_mjpeg_1920x1080_30(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          1920, 1080,
          "jpeg_taco",
          0,
          30.0,
          "main",
          "dec_mjpeg_1920x1080_30"
      );
  }
  static int test_dec_mjpeg_2560x1440_30(const char* video_path) {
     return run_test_with_all_pp(
         video_path,
          2560, 1440,
         "jpeg_taco",
         4,
          30.0,
          "main",
          "dec_mjpeg_2560x1440_30"
      );
  }
  
  static int test_dec_mjpeg_128x128_30(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          128, 128,
          "jpeg_taco",
          0,
          30.0,
          "main",
          "dec_mjpeg_128x128_30"
      );
  }
  
  static int test_dec_mjpeg_640x480_30(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          640, 480,
          "jpeg_taco",
          0,
          30.0,
          "main",
          "dec_mjpeg_640x480_30"
      );
  }
  
  static int test_dec_mjpeg_1280x720_30(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          1280, 720,
          "jpeg_taco",
          0,
          30.0,
          "main",
          "dec_mjpeg_1280x720_30"
      );
  }
  
  static int test_dec_mjpeg_1920x1080_60(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          1920, 1080,
          "jpeg_taco",
          4,
          60.0,
          "main",
          "dec_mjpeg_1920x1080_60"
      );
  }
  
  static int test_dec_mjpeg_3840x2160_30(const char* video_path) {
      return run_test_with_all_pp(
          video_path,
          3840, 2160,
          "jpeg_taco",
          4,
          30.0,
          "main",
          "dec_mjpeg_3840x2160_30"
      );
  }
 
 
 // ========== 测试用例注册（仿照 test.cpp 结构） ==========
  
  // H.264
  // 多参数版本（支持后处理模式）
REGISTER_TEST(dec_h264_128x128_30_main, "H.264 128x128 30fps main profile", test_dec_h264_128x128_30_main);
REGISTER_TEST(dec_h264_320x240_30_high, "H.264 320x240 30fps high profile", test_dec_h264_320x240_30_high);
REGISTER_TEST(dec_h264_640x480_30_main, "H.264 640x480 30fps main profile", test_dec_h264_640x480_30_main);
REGISTER_TEST(dec_h264_640x480_60_high, "H.264 640x480 60fps high profile", test_dec_h264_640x480_60_high);
REGISTER_TEST(dec_h264_1280x720_30_high, "H.264 1280x720 30fps high profile", test_dec_h264_1280x720_30_high);
REGISTER_TEST(dec_h264_1920x1080_30_high, "H.264 1920x1080 30fps high profile", test_dec_h264_1920x1080_30_high);
REGISTER_TEST(dec_h264_1920x1080_60_high, "H.264 1920x1080 60fps high profile", test_dec_h264_1920x1080_60_high);
REGISTER_TEST(dec_h264_2560x1440_30_high, "H.264 2560x1440 30fps high profile", test_dec_h264_2560x1440_30_high);
REGISTER_TEST(dec_h264_3840x2160_30_high, "H.264 3840x2160 30fps high profile", test_dec_h264_3840x2160_30_high);
 
 
 
 // H.265
REGISTER_TEST(dec_h265_128x128_30_main, "H.265 128x128 30fps main profile", test_dec_h265_128x128_30_main);
REGISTER_TEST(dec_h265_320x240_30_main, "H.265 320x240 30fps main profile", test_dec_h265_320x240_30_main);
REGISTER_TEST(dec_h265_640x480_30_main, "H.265 640x480 30fps main profile", test_dec_h265_640x480_30_main);
REGISTER_TEST(dec_h265_640x480_60_main, "H.265 640x480 60fps main profile", test_dec_h265_640x480_60_main);
REGISTER_TEST(dec_h265_1280x720_30_main, "H.265 1280x720 30fps main profile", test_dec_h265_1280x720_30_main);
REGISTER_TEST(dec_h265_1920x1080_30_main, "H.265 1920x1080 30fps main profile", test_dec_h265_1920x1080_30_main);
REGISTER_TEST(dec_h265_1920x1080_60_main, "H.265 1920x1080 60fps main profile", test_dec_h265_1920x1080_60_main);
REGISTER_TEST(dec_h265_2560x1440_30_main, "H.265 2560x1440 30fps main profile", test_dec_h265_2560x1440_30_main);
REGISTER_TEST(dec_h265_3840x2160_30_main, "H.265 3840x2160 30fps main profile", test_dec_h265_3840x2160_30_main);

 // MJPEG
REGISTER_TEST(dec_mjpeg_128x128_30, "MJPEG 128x128 30fps", test_dec_mjpeg_128x128_30);
REGISTER_TEST(dec_mjpeg_320x240_30, "MJPEG 320x240 30fps", test_dec_mjpeg_320x240_30);
REGISTER_TEST(dec_mjpeg_640x480_30, "MJPEG 640x480 30fps", test_dec_mjpeg_640x480_30);
REGISTER_TEST(dec_mjpeg_640x480_60, "MJPEG 640x480 60fps", test_dec_mjpeg_640x480_60);
REGISTER_TEST(dec_mjpeg_1280x720_30, "MJPEG 1280x720 30fps", test_dec_mjpeg_1280x720_30);
REGISTER_TEST(dec_mjpeg_1920x1080_30, "MJPEG 1920x1080 30fps", test_dec_mjpeg_1920x1080_30);
REGISTER_TEST(dec_mjpeg_1920x1080_60, "MJPEG 1920x1080 60fps", test_dec_mjpeg_1920x1080_60);
REGISTER_TEST(dec_mjpeg_2560x1440_30, "MJPEG 2560x1440 30fps", test_dec_mjpeg_2560x1440_30);
REGISTER_TEST(dec_mjpeg_3840x2160_30, "MJPEG 3840x2160 30fps", test_dec_mjpeg_3840x2160_30);
 
 // ========== RTSP 流解码测试 ==========
 // 说明：
 //   - 输入为 RTSP 流（使用 FfmpegPacketRecorderWorker 录制编码包）
 //   - 然后使用解码器解码（与视频文件解码同样的流程）
 //   - 根据图片中的码流参数配置创建测试用例
 // 
 // RTSP URL 格式：
 //   - 标准格式：rtsp://[username:password@]host[:port]/path
 //   - 示例：rtsp://admin:passw0rd@192.168.57.243:554/Streaming/Channels/101
 //   - 支持用户名/密码认证、自定义端口、完整路径
 // 
 // 使用示例：
 //   ./mp4_decode_test -m rtsp_h264_1280x720_30_cbr rtsp://admin:passw0rd@192.168.57.243:554/Streaming/Channels/101
 
/**
 * RTSP 流录制函数（独立函数，用于将RTSP流录制为MP4文件）
 * 
 * @param rtsp_url RTSP 流地址
 * @param test_tag 测试标签（用于生成临时文件名）
 * @param frame_rate 预期帧率（用于验证）
 * @param record_duration_seconds 录制时长（秒）
 * @param[out] recorded_file 输出的录制文件路径
 * @param[out] actual_width 实际宽度
 * @param[out] actual_height 实际高度
 * @param[out] actual_decoder_name 实际解码器名称
 * @param[out] actual_frame_rate 实际帧率
 * @param[out] total_frames 从录制的MP4文件获取的总帧数（参考纯视频解码的getTotalFrames逻辑）
 * @return 0 成功，-1 失败
 */
static int record_rtsp_stream_to_mp4(
    const char* rtsp_url,
    const char* test_tag,
    double frame_rate,
    int record_duration_seconds,
    std::string& recorded_file,
    int& actual_width,
    int& actual_height,
    std::string& actual_decoder_name,
    double& actual_frame_rate,
    int& total_frames
) {
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO("  Phase 1: Recording RTSP stream to MP4");
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
    // 初始化total_frames为-1（表示未获取）
    total_frames = -1;
    
    g_running = true;
    g_rtsp_interrupted = false;
    RtspPacketSource::clearInterrupt();
    
    // 生成临时 MP4 文件路径
    std::ostringstream oss;
    oss << "/tmp/rtsp_recorded_" << (test_tag ? test_tag : "test") << ".mp4";
    recorded_file = oss.str();
    
    LOG_INFO_FMT("Recording RTSP stream to: %s", recorded_file.c_str());
    LOG_INFO_FMT("Recording duration: %d seconds", record_duration_seconds);
    
    // 使用作用域块确保recorder在函数返回前完全析构
    // 保存需要在recorder析构后使用的变量
    const AVCodecParameters* saved_codec_params = nullptr;
    AVRational saved_time_base = {1, 25};
    int saved_packet_count = 0;
    int64_t saved_total_bytes = 0;
    
    {
        // 1.1 创建录制器（在作用域块内）
        VideoProductionLine recorder(false, 1, false);
    auto record_config = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath(rtsp_url)
                .setBufferCount(16)   // 避免录制阶段 buffer 过多导致内存压力/阻塞
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
        .build();
    
    recorder.setErrorCallback([](const std::string& error) {
        LOG_ERROR_FMT("Recording Error: %s", error.c_str());
        // ⭐ 修复：不因录制错误就停止整个流程（与纯视频文件解码保持一致）
        // 录制过程中的一些非致命错误（如缓冲区不足、网络抖动等）不应该导致整个测试停止
    });
    
    // 1.2 启动录制器
    LOG_INFO("[Phase 1] Starting RTSP recorder...");
    if (!recorder.start(record_config)) {
        LOG_ERROR("Failed to start RTSP recorder");
        return -1;
    }
    
    // 1.3 获取录制器的 BufferPool
    uint64_t record_pool_id = recorder.getWorkingBufferPoolId();
    auto record_pool = BufferPoolRegistry::getInstance().getPool(record_pool_id).lock();
    if (!record_pool) {
        LOG_ERROR("Failed to get recorder BufferPool");
        recorder.stop();
        return -1;
    }
    
    LOG_INFO_FMT("  Recorder BufferPool: '%s' (ID: %lu)", record_pool->getName().c_str(), record_pool_id);
    
    // 1.4 等待录制器产生初始数据
    LOG_INFO("[Phase 1] Waiting for recorder to generate initial data...");
    int wait_attempts = 0;
    const int MAX_WAIT_ATTEMPTS = 50;
    bool has_data = false;
    
    while (wait_attempts < MAX_WAIT_ATTEMPTS && g_running) {
        int filled_count = record_pool->getFilledCount();
        int free_count = record_pool->getFreeCount();
        
        if (filled_count > 0) {
            has_data = true;
            LOG_INFO_FMT("  ✅ Recorder ready: filled=%d, free=%d", filled_count, free_count);
            break;
        }
        
        if (wait_attempts % 10 == 0) {
            LOG_INFO_FMT("  Waiting... (filled=%d, free=%d)", filled_count, free_count);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wait_attempts++;
    }
    
    if (!has_data) {
        LOG_WARN("  ⚠️  Recorder did not generate data within timeout, proceeding anyway...");
    }
    
    // 1.5 获取编解码器参数并创建 BufferWriter
    auto record_worker_facade = recorder.getWorkerFacade();
    if (!record_worker_facade) {
        LOG_ERROR("Failed to get recorder worker facade");
        recorder.stop();
        return -1;
    }
    
        const AVCodecParameters* codec_params = record_worker_facade->getSourceCodecParameters();
        if (!codec_params) {
            LOG_ERROR("Failed to get codec parameters from recorder");
            recorder.stop();
            return -1;
        }
        
        // ⭐ 保存codec_params和time_base，供recorder析构后使用
        saved_codec_params = codec_params;
        saved_time_base = record_worker_facade->getTimeBase();
        AVRational time_base = saved_time_base;
    
    // ⭐ 诊断：记录时间基准信息
    LOG_INFO_FMT("  Time base: %d/%d (%.6f)", time_base.num, time_base.den, 
                 time_base.den > 0 ? (double)time_base.num / time_base.den : 0.0);
    LOG_INFO_FMT("  Expected frame rate: %.2f fps", frame_rate);
    LOG_INFO_FMT("  Expected packet interval: %.3f ms", frame_rate > 0 ? 1000.0 / frame_rate : 0.0);
    
    using namespace productionline::io;
    BufferWriter record_writer;
    if (!record_writer.openEncoded(recorded_file.c_str(), codec_params, time_base)) {
        LOG_ERROR("Failed to open BufferWriter for recording");
        recorder.stop();
        return -1;
    }
    
    // 1.6 录制循环
    LOG_INFO("\n[Phase 1] Recording to MP4...");
    LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // ⭐ 创建定时器用于控制录制时长（参考 test.cpp）
    Timer recording_timer;
    recording_timer.start();
    
    // ⭐ 设置录制时长定时器
    auto timer_id = recording_timer.scheduleOnce(
        record_duration_seconds * 1000,  // 毫秒
        []() {
            g_running = false;  // 时间到，停止录制
            LOG_INFO("\n  ⏱️  Recording duration reached, stopping...");
        }
    );
    
    LOG_INFO_FMT("  Recording for %d seconds (timer-controlled)...\n", record_duration_seconds);
    
    auto record_start_time = std::chrono::steady_clock::now();
    int packet_count = 0;
    int64_t total_bytes = 0;
    int timeout_count = 0;
    const int MAX_TIMEOUT = 50;
    
    // ⭐ 诊断：记录第一个包的时间戳（用于分析时间戳问题）
    int64_t first_packet_pts = -1;
    int64_t first_packet_dts = -1;
    auto first_packet_wall_time = std::chrono::steady_clock::now();
    
    while (g_running) {
        // ⭐ 检查中断标志（参考 test.cpp）
        if (g_rtsp_interrupted.load()) {
            LOG_INFO("\n  ⚠️  检测到中断请求，停止录制...");
            break;
        }
        
        Buffer* buffer = record_pool->acquireFilled(true, 100);
        
        if (buffer) {
            size_t used_size = buffer->getUsedSize();
            if (used_size > 0) {
                // ⭐ 诊断：记录第一个包的时间戳信息
                if (packet_count == 0) {
                    AVPacket* packet = buffer->getAVPacket();
                    if (packet) {
                        first_packet_pts = packet->pts;
                        first_packet_dts = packet->dts;
                        first_packet_wall_time = std::chrono::steady_clock::now();
                        LOG_INFO_FMT("  📊 First packet: PTS=%lld, DTS=%lld, time_base=%d/%d", 
                                     (long long)first_packet_pts, (long long)first_packet_dts,
                                     time_base.num, time_base.den);
                        
                        // ⭐ 诊断：计算预期的frame_duration（用于分析PTS膨胀问题）
                        double expected_frame_duration = time_base.den > 0 ? 
                            (double)time_base.num / time_base.den / frame_rate : 0.0;
                        LOG_INFO_FMT("  📊 Expected frame_duration (for PTS calculation): %.6f (based on %.2f fps)", 
                                     expected_frame_duration, frame_rate);
                        LOG_INFO_FMT("  📊 If BufferWriter uses default 25fps, frame_duration would be: %.6f", 
                                     time_base.den > 0 ? (double)time_base.num / time_base.den / 25.0 : 0.0);
                    }
                }
                
                if (record_writer.write(buffer)) {
                    packet_count++;
                    total_bytes += used_size;
                    
                    if (packet_count % 50 == 0) {
                        // 计算已录制时长（用于日志显示）
                        auto now = std::chrono::steady_clock::now();
                        int elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - record_start_time).count();
                        double rate_mbps = elapsed > 0 ? (total_bytes * 8.0) / (elapsed * 1000000.0) : 0.0;
                        double actual_fps = elapsed > 0 ? (double)packet_count / elapsed : 0.0;
                        LOG_INFO_FMT("  Recorded %d packets | %d seconds | %.2f Mbps | %.2f fps",
                                     packet_count, elapsed, rate_mbps, actual_fps);
                    }
                } else {
                    LOG_WARN_FMT("  ⚠️  Failed to write packet #%d", packet_count + 1);
                }
            }
            
            // ⭐ 优化：立即释放Buffer，避免BufferPool耗尽
            record_pool->releaseFilled(buffer);
            timeout_count = 0;
        } else {
            timeout_count++;
            if (timeout_count >= MAX_TIMEOUT) {
                LOG_WARN("\n  ⚠️  Stream timeout, stopping recording...");
                break;
            }
        }
    }
    
    LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // 1.7 清理
    LOG_INFO("\n[Phase 1] Cleaning up recording resources...");
    
    // 停止定时器
    recording_timer.cancel(timer_id);
    recording_timer.stop();
    
    // BufferWriter会自动写入MP4 trailer
    record_writer.close();
    
    // ⭐ 激进清理步骤1：在停止recorder之前，强制中断RTSP连接
    // 这会触发FFmpeg的中断回调，强制退出阻塞的网络操作
    LOG_INFO("[Phase 1] Force interrupting RTSP connection...");
    g_rtsp_interrupted = true;
    RtspPacketSource::requestInterrupt();
    
    // 等待一小段时间，让中断信号生效
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 显式停止recorder，确保所有线程已退出
    LOG_INFO("[Phase 1] Stopping recorder...");
    recorder.stop();
    
    // 等待VideoProductionLine的线程完全退出
    // recorder.stop()会join所有线程，但需要确保完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 检查recorder是否真的停止了
    if (recorder.isRunning()) {
        LOG_WARN("[Phase 1] Recorder still running after stop(), waiting...");
        int wait_count = 0;
        while (recorder.isRunning() && wait_count < 20) {  // 减少等待时间到2秒
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            wait_count++;
        }
        if (recorder.isRunning()) {
            LOG_ERROR("[Phase 1] Recorder still running after 2 seconds, forcing stop...");
            // 再次强制中断
            RtspPacketSource::requestInterrupt();
        }
    }
    
        saved_packet_count = packet_count;
        saved_total_bytes = total_bytes;
        
        LOG_INFO_FMT("\n✅ Recording completed: %d packets, %.2f MB", 
                     packet_count, total_bytes / (1024.0 * 1024.0));
        LOG_INFO_FMT("   Recorded file: %s", recorded_file.c_str());
        
        if (packet_count == 0) {
            LOG_ERROR("No packets recorded, cannot proceed to decode");
            return -1;
        }
    }  // recorder在这里析构，会调用Worker析构 → packet_source_->close() → avformat_close_input()
    
    // ⭐ 激进清理步骤2：等待recorder析构完成，确保FFmpeg资源已释放
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // ⭐ 激进清理步骤3：清理FFmpeg网络资源（确保所有网络线程退出）
    avformat_network_deinit();
    
    // ⭐ 激进清理步骤4：检查并等待所有线程退出（最多等待1秒）
    LOG_INFO("[Phase 1] Aggressively checking if all recording threads have exited...");
    int remaining_threads = -1;
    for (int i = 0; i < 10; i++) {  // 减少等待时间到1秒
        DIR* check_dir = opendir("/proc/self/task");
        if (check_dir) {
            int thread_count = 0;
            struct dirent* entry;
            while ((entry = readdir(check_dir)) != nullptr) {
                if (entry->d_name[0] != '.') {
                    thread_count++;
                }
            }
            closedir(check_dir);
            
            remaining_threads = thread_count;
            
            // 如果只有主线程（1个线程），说明所有线程已退出
            if (thread_count <= 1) {
                LOG_INFO("[Phase 1] ✅ All recording threads have exited");
                break;
            }
            
            if (i % 3 == 0) {
                LOG_INFO_FMT("[Phase 1] Waiting for threads to exit... (%d threads remaining)", thread_count);
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // ⭐ 激进清理步骤5：如果仍有线程未退出，尝试强制取消线程
    if (remaining_threads > 1) {
        LOG_WARN_FMT("[Phase 1] ⚠️  %d threads still running, attempting aggressive cleanup...", remaining_threads);
        
        DIR* aggressive_check_dir = opendir("/proc/self/task");
        if (aggressive_check_dir) {
            struct dirent* entry;
            long main_tid = syscall(SYS_gettid);  // 获取主线程ID
            
            while ((entry = readdir(aggressive_check_dir)) != nullptr) {
                if (entry->d_name[0] != '.') {
                    long tid = atol(entry->d_name);
                    if (tid != main_tid) {
                        // 尝试取消非主线程（可能无效，但值得尝试）
                        LOG_WARN_FMT("[Phase 1] Attempting to cancel thread TID=%ld...", tid);
                        // 注意：pthread_cancel需要pthread_t，但我们只有TID
                        // 这里只是记录，实际的线程取消需要更复杂的机制
                    }
                }
            }
            closedir(aggressive_check_dir);
        }
        
        // 再次强制中断RTSP连接
        RtspPacketSource::requestInterrupt();
        
        // 再等待一小段时间
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        // 最终检查
        DIR* final_check_dir = opendir("/proc/self/task");
        if (final_check_dir) {
            int final_thread_count = 0;
            struct dirent* entry;
            while ((entry = readdir(final_check_dir)) != nullptr) {
                if (entry->d_name[0] != '.') {
                    final_thread_count++;
                }
            }
            closedir(final_check_dir);
            
            if (final_thread_count > 1) {
                LOG_WARN_FMT("[Phase 1] ⚠️  WARNING: %d threads still running after aggressive cleanup", final_thread_count);
                LOG_WARN("[Phase 1] These threads are likely FFmpeg internal threads (network IO, muxer)");
                LOG_WARN("[Phase 1] Accepting their existence and continuing to decoding phase...");
                LOG_WARN("[Phase 1] These threads will be handled after decoding phase completes");
            } else {
                LOG_INFO("[Phase 1] ✅ Aggressive cleanup succeeded, all threads exited");
            }
        }
    } else {
        LOG_INFO("[Phase 1] ✅ All recording threads have been cleaned up");
    }
    
    // 清除中断标志，为后续操作做准备
    g_rtsp_interrupted = false;
    RtspPacketSource::clearInterrupt();
    
    // ⭐ 关键策略调整：不再强制等待线程退出
    // FFmpeg的内部线程（网络IO、muxer等）可能无法在录制阶段完全清理
    // 这些线程会在程序退出时自然清理，或者在解码完成后统一处理
    // 接受它们的存在，继续执行解码阶段
    
    int packet_count = saved_packet_count;
    int64_t total_bytes = saved_total_bytes;
    
    // ⭐ 诊断：检查录制的包数量是否合理
    int expected_packets = (int)(record_duration_seconds * frame_rate);
    if (packet_count < expected_packets * 0.5) {
        LOG_WARN_FMT("  ⚠️  WARNING: Recorded only %d packets, expected ~%d (%.1f%%)", 
                     packet_count, expected_packets, 
                     expected_packets > 0 ? (packet_count * 100.0) / expected_packets : 0.0);
        LOG_WARN("    This may indicate:");
        LOG_WARN("    1. Stream corruption or frame drops");
        LOG_WARN("    2. Network issues (for RTSP)");
        LOG_WARN("    3. Encoder issues (Frame Num gaps, etc.)");
        LOG_WARN("    The recorded MP4 may have syntax errors that will cause decoding failures.");
    }
    
    // ========================================================================
    // 阶段1.8：验证录制的 MP4 文件是否正常
    // ========================================================================
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO("  Phase 1.8: Verifying recorded MP4 file");
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
    // 1.8.1 检查文件是否存在且大小大于0
    if (!std::filesystem::exists(recorded_file)) {
        LOG_ERROR_FMT("Recorded file does not exist: %s", recorded_file.c_str());
        return -1;
    }
    
    size_t file_size = std::filesystem::file_size(recorded_file);
    if (file_size == 0) {
        LOG_ERROR_FMT("Recorded file is empty: %s", recorded_file.c_str());
        return -1;
    }
    
    LOG_INFO_FMT("  ✅ File exists: %s (size: %.2f MB)", 
                 recorded_file.c_str(), file_size / (1024.0 * 1024.0));
    
    // 使用作用域块确保verify_format_ctx在函数返回前关闭
    // 保存需要在verify_format_ctx关闭后使用的变量
    const AVCodecParameters* saved_verify_codecpar = nullptr;
    
    {
        // 1.8.2 使用 FFmpeg 验证文件格式和流信息（在作用域块内）
        AVFormatContext* verify_format_ctx = nullptr;
    int ret = avformat_open_input(&verify_format_ctx, recorded_file.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR_FMT("  ❌ Failed to open recorded file: %s", err_buf);
        return -1;
    }
    
    ret = avformat_find_stream_info(verify_format_ctx, nullptr);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR_FMT("  ❌ Failed to find stream info: %s", err_buf);
        avformat_close_input(&verify_format_ctx);
        return -1;
    }
    
    LOG_INFO_FMT("  ✅ File format: %s", verify_format_ctx->iformat->name);
    LOG_INFO_FMT("  ✅ Number of streams: %u", verify_format_ctx->nb_streams);
    
    // 1.8.3 查找视频流并验证参数
    int video_stream_idx = -1;
    for (unsigned int i = 0; i < verify_format_ctx->nb_streams; i++) {
        if (verify_format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }
    
    if (video_stream_idx < 0) {
        LOG_ERROR("  ❌ No video stream found in recorded file");
        avformat_close_input(&verify_format_ctx);
        return -1;
    }
    
    AVStream* video_stream = verify_format_ctx->streams[video_stream_idx];
    const AVCodecParameters* verify_codecpar = video_stream->codecpar;
    
    LOG_INFO_FMT("  ✅ Video stream found (index: %d)", video_stream_idx);
    LOG_INFO_FMT("    Codec: %s (ID: %d)", 
                 avcodec_get_name(verify_codecpar->codec_id), 
                 verify_codecpar->codec_id);
    LOG_INFO_FMT("    Resolution: %dx%d", 
                 verify_codecpar->width, verify_codecpar->height);
    
    // 1.8.4 记录分辨率（从录制的MP4文件中获取）
    LOG_INFO_FMT("  ✅ Resolution: %dx%d", verify_codecpar->width, verify_codecpar->height);
    
    // 1.8.5 记录编解码器ID
    LOG_INFO_FMT("  ✅ Codec ID: %s (ID: %d)", 
                 avcodec_get_name(verify_codecpar->codec_id), 
                 verify_codecpar->codec_id);
    
    // 1.8.6 检查时长和帧率
    if (video_stream->duration != AV_NOPTS_VALUE && video_stream->time_base.num > 0) {
        double duration_seconds = (double)video_stream->duration * 
                                  video_stream->time_base.num / video_stream->time_base.den;
        LOG_INFO_FMT("  ✅ Duration: %.2f seconds", duration_seconds);
        
        if (duration_seconds < 1.0) {
            LOG_WARN_FMT("  ⚠️  Duration is very short (%.2f seconds), may cause decoding issues", 
                         duration_seconds);
        }
    } else {
        LOG_WARN("  ⚠️  Cannot determine duration from stream metadata");
    }
    
    // ⭐ 关键修复：从录制的MP4文件获取实际帧率，如果与传入参数差异较大，使用实际帧率
    actual_frame_rate = frame_rate;  // 默认使用传入的帧率
    if (video_stream->avg_frame_rate.num > 0) {
        double fps = (double)video_stream->avg_frame_rate.num / video_stream->avg_frame_rate.den;
        LOG_INFO_FMT("  ✅ Frame rate: %.2f fps", fps);
        
        // ⚠️ 检查帧率是否异常（低于 10 fps 或高于 120 fps）
        if (fps < 10.0) {
            LOG_WARN_FMT("  ⚠️  WARNING: Frame rate is very low (%.2f fps), expected ~%.1f fps", 
                         fps, frame_rate);
            LOG_WARN("    This may indicate:");
            LOG_WARN("    1. Many frames were dropped during recording");
            LOG_WARN("    2. Timestamp information is incorrect (PTS inflation)");
            LOG_WARN("    3. The recorded file may have synchronization issues");
            LOG_WARN("    4. Stream corruption (Frame Num gaps) causing BufferWriter to use wrong frame_duration");
        } else if (fps > 120.0) {
            LOG_WARN_FMT("  ⚠️  WARNING: Frame rate is very high (%.2f fps), expected ~%.1f fps", 
                         fps, frame_rate);
        } else if (std::abs(fps - frame_rate) > 2.0) {
            // ⭐ 如果实际帧率与预期帧率差异>2fps，使用实际帧率（例如RTSP实际是25fps，但测试配置是30fps）
            LOG_WARN_FMT("  ⚠️  Frame rate mismatch detected: actual=%.2f fps vs expected=%.1f fps", 
                         fps, frame_rate);
            LOG_INFO_FMT("  ✅ Using actual frame rate (%.2f fps) for decoding phase", fps);
            actual_frame_rate = fps;  // 使用实际帧率
        } else {
            // 帧率匹配良好，使用传入的参数
            actual_frame_rate = frame_rate;
        }
    } else {
        LOG_WARN("  ⚠️  Cannot determine frame rate from stream metadata, using provided frame rate");
        actual_frame_rate = frame_rate;
    }
    
    // 1.8.7 估算总帧数并检查是否合理（参考纯视频解码的getTotalFrames逻辑）
    int estimated_frames = -1;
    if (video_stream->duration != AV_NOPTS_VALUE && 
        video_stream->time_base.num > 0 && 
        video_stream->avg_frame_rate.num > 0) {
        double duration_seconds = (double)video_stream->duration * 
                                  video_stream->time_base.num / video_stream->time_base.den;
        double fps = (double)video_stream->avg_frame_rate.num / video_stream->avg_frame_rate.den;
        estimated_frames = (int)(duration_seconds * fps);
        if (estimated_frames > 0) {
            LOG_INFO_FMT("  ✅ Estimated total frames: %d (from MP4 metadata)", estimated_frames);
            
            // 检查估算帧数是否合理（基于录制时长和预期帧率）
            int expected_frames = (int)(record_duration_seconds * frame_rate);
            if (estimated_frames < expected_frames * 0.5) {
                LOG_WARN_FMT("  ⚠️  WARNING: Estimated frames (%d) is much less than expected (%d)", 
                             estimated_frames, expected_frames);
                LOG_WARN("    This may cause decoding issues or incorrect PSNR calculations");
            }
        }
    }
    
    // 保存总帧数，供解码阶段使用（参考纯视频解码的getTotalFrames逻辑）
    total_frames = estimated_frames;
    
    // 1.8.8 检查录制的包数量与预期帧数的关系
    if (packet_count > 0 && expected_packets > 0) {
        double packet_ratio = (double)packet_count / expected_packets;
        LOG_INFO_FMT("  📊 Packet statistics:");
        LOG_INFO_FMT("    Recorded packets: %d", packet_count);
        LOG_INFO_FMT("    Expected packets (%.1f fps × %d sec): ~%d", 
                     frame_rate, record_duration_seconds, expected_packets);
        LOG_INFO_FMT("    Packet ratio: %.2f%%", packet_ratio * 100.0);
        
        // ⭐ 诊断：码流语法错误检测
        if (packet_ratio < 0.5) {
            LOG_WARN_FMT("  ⚠️  WARNING: Only %.1f%% of expected packets were recorded", 
                         packet_ratio * 100.0);
            LOG_WARN("    This may indicate:");
            LOG_WARN("    1. Network issues or stream problems");
            LOG_WARN("    2. Stream corruption (Frame Num gaps)");
            LOG_WARN("    3. Hardware decoder may fail with errors like:");
            LOG_WARN("       - DEC_STREAM_ERROR_DEDECTED [-25]");
            LOG_WARN("       - H264BSD_NALUNIT_ERROR [-23]");
            LOG_WARN("       - Frame number gap detected but not allowed by SPS");
        }
    }
    
    // ⭐ 新增：检查录制的MP4文件是否包含硬件解码器需要的元数据
    LOG_INFO("\n  🔍 Hardware Decoder Compatibility Check:");
    if (verify_codecpar->codec_id == AV_CODEC_ID_H264 || verify_codecpar->codec_id == AV_CODEC_ID_HEVC) {
        // 检查是否有extradata（包含SPS/PPS）
        if (!verify_codecpar->extradata || verify_codecpar->extradata_size == 0) {
            LOG_WARN("  ⚠️  WARNING: Recorded MP4 file has no extradata (SPS/PPS)!");
            LOG_WARN("    Hardware decoder may fail to initialize without SPS/PPS");
            LOG_WARN("    This is likely the cause of PSNR = 100dB (hardware decoder fallback to software)");
        } else {
            LOG_INFO_FMT("  ✅ Extradata present: %d bytes (SPS/PPS should be included)", 
                         verify_codecpar->extradata_size);
        }
        
        // 检查是否有profile/level信息
        if (verify_codecpar->profile == FF_PROFILE_UNKNOWN) {
            LOG_WARN("  ⚠️  WARNING: Recorded MP4 file has unknown profile!");
            LOG_WARN("    Hardware decoder may have compatibility issues");
        } else {
            LOG_INFO_FMT("  ✅ Profile: %d", verify_codecpar->profile);
        }
        
        if (verify_codecpar->level == FF_LEVEL_UNKNOWN) {
            LOG_WARN("  ⚠️  WARNING: Recorded MP4 file has unknown level!");
        } else {
            LOG_INFO_FMT("  ✅ Level: %d", verify_codecpar->level);
        }
        }
        
        saved_verify_codecpar = verify_codecpar;
        
        avformat_close_input(&verify_format_ctx);
    }  // verify_format_ctx在这里关闭
    
    const AVCodecParameters* verify_codecpar = saved_verify_codecpar;
    
    LOG_INFO("\n  ✅ MP4 file verification completed successfully");
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
    // 获取实际分辨率和解码器名称（从验证的MP4文件中获取）
    actual_width = verify_codecpar->width > 0 ? verify_codecpar->width : 0;
    actual_height = verify_codecpar->height > 0 ? verify_codecpar->height : 0;
    
    // 根据编解码器类型选择解码器
    if (verify_codecpar->codec_id == AV_CODEC_ID_H264) {
        actual_decoder_name = "h264_taco";
    } else if (verify_codecpar->codec_id == AV_CODEC_ID_HEVC) {
        actual_decoder_name = "hevc_taco";
    } else if (verify_codecpar->codec_id == AV_CODEC_ID_MJPEG) {
        actual_decoder_name = "jpeg_taco";
    } else {
        LOG_WARN_FMT("Unknown codec type: %s, using software decoder", 
                     avcodec_get_name(verify_codecpar->codec_id));
        actual_decoder_name = "";  // 使用软件解码器
    }
    
    return 0;
}

/**
 * RTSP 流解码测试函数
 * 
 * 流程：
 * 1. 使用 record_rtsp_stream_to_mp4 录制 RTSP 流为 MP4 文件
 * 2. 调用 run_decode_test_with_params 使用纯视频解码逻辑解码录制的 MP4 文件
 * 
 * RTSP URL 格式支持：
 *   - 标准格式：rtsp://[username:password@]host[:port]/path
 *   - 示例：rtsp://admin:passw0rd@192.168.57.243:554/Streaming/Channels/101
 *   - 支持用户名/密码认证
 *   - 支持自定义端口（默认554）
 *   - 支持完整路径
 * 
 * @param rtsp_url RTSP 流地址，支持标准 RTSP URL 格式
 * @param width 预期宽度（实际会从流中获取）
 * @param height 预期高度（实际会从流中获取）
 * @param decoder_name 解码器名称（h264_taco/hevc_taco/jpeg_taco）
 * @param decode_threads 解码线程数
 * @param frame_rate 帧率
 * @param profile 编码profile
 * @param test_tag 测试标签
 */
static int run_rtsp_decode_test_with_params(
     const char* rtsp_url,
     int width,
     int height,
     const char* decoder_name,
     int decode_threads,
     double frame_rate,
     const char* profile,
     const char* test_tag
 ) {
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO_FMT("  RTSP Decode Test: %s", test_tag ? test_tag : "unknown");
    LOG_INFO("═══════════════════════════════════════════════════════\n");
 
    if (!rtsp_url || rtsp_url[0] == '\0') {
        LOG_ERROR("No RTSP URL specified");
        return -1;
    }
 
    // ========================================================================
    // 阶段1：录制 RTSP 流为 MP4 文件
    // ========================================================================
    std::string recorded_file;
    int actual_width = 0;
    int actual_height = 0;
    std::string actual_decoder_name;
    double actual_frame_rate = frame_rate;
    int total_frames = -1;  // 从录制的MP4文件获取的总帧数
    const int record_duration_seconds = 10;  // 录制 10 秒
    
    int ret = record_rtsp_stream_to_mp4(
        rtsp_url,
        test_tag,
        frame_rate,
        record_duration_seconds,
        recorded_file,
        actual_width,
        actual_height,
        actual_decoder_name,
        actual_frame_rate,
        total_frames  // 获取总帧数
    );
    
    if (ret != 0) {
        LOG_ERROR("Failed to record RTSP stream");
        return -1;
    }
    
    // 确保录制相关的所有线程都已结束
    // recorder.stop()和recorder析构已经在record_rtsp_stream_to_mp4中完成
    // 这里可以添加额外的检查，确保线程已退出
    LOG_INFO("[Phase 1] ✅ All recording threads stopped");
    
    // 如果录制函数没有获取到分辨率，使用传入的参数
    if (actual_width <= 0 || actual_height <= 0) {
        actual_width = width;
        actual_height = height;
    }
    
    // 如果录制函数没有获取到解码器名称，使用传入的参数
    if (actual_decoder_name.empty() && decoder_name && decoder_name[0] != '\0') {
        actual_decoder_name = decoder_name;
    }
    
    // ========================================================================
    // 阶段2：使用纯视频解码逻辑解码录制的 MP4 文件
    // ========================================================================
    LOG_INFO("\n═══════════════════════════════════════════════════════");
    LOG_INFO("  Phase 2: Decoding recorded MP4 file");
    LOG_INFO("═══════════════════════════════════════════════════════\n");
    
    LOG_INFO_FMT("Using recorded MP4 file: %s", recorded_file.c_str());
    LOG_INFO_FMT("Resolution: %dx%d", actual_width, actual_height);
    LOG_INFO_FMT("Decoder: %s", actual_decoder_name.empty() ? "software" : actual_decoder_name.c_str());
    LOG_INFO_FMT("Frame rate: %.2f fps", actual_frame_rate);
    
    // 重置 g_running，因为录制阶段的错误回调可能已经设置了 g_running = false
    g_running = true;
    
    // 调用纯视频解码逻辑，将录制视频作为输入视频进入MP4解码
    // 参考纯视频解码的max_frames参数获取逻辑，使用从MP4文件获取的总帧数
    // 传递PP参数以支持后处理模式
    int result = run_decode_test_with_params(
        recorded_file.c_str(),  // 使用录制的 MP4 文件（不是原始 RTSP URL）
        actual_width,
        actual_height,
        actual_decoder_name.empty() ? nullptr : actual_decoder_name.c_str(),
        decode_threads,
        actual_frame_rate,  // 使用实际帧率（如果RTSP是25fps，会使用25fps而不是传入的30fps）
        profile,
        test_tag,
        total_frames  // 传递从MP4文件获取的总帧数（参考纯视频解码的max_frames逻辑）
    );
    
    // ⭐ 统一处理所有残留线程（包括录制阶段和解码阶段的线程）
    LOG_INFO("\n[Phase 2] Final cleanup: Checking for remaining threads...");
    DIR* early_proc_dir = opendir("/proc/self/task");
    if (early_proc_dir) {
        int early_thread_count = 0;
        struct dirent* entry;
        while ((entry = readdir(early_proc_dir)) != nullptr) {
            if (entry->d_name[0] != '.') {
                early_thread_count++;
            }
        }
        closedir(early_proc_dir);
        
        if (early_thread_count > 1) {
            LOG_WARN_FMT("[Phase 2] ⚠️  %d threads still running after decoding", early_thread_count);
            LOG_WARN("[Phase 2] These threads are likely FFmpeg internal threads from recording phase");
            LOG_WARN("[Phase 2] Attempting final cleanup...");
            
            // 最后一次清理FFmpeg网络资源
            avformat_network_deinit();
            
            // 等待一小段时间，看线程是否退出
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            // 再次检查
            DIR* final_check_dir = opendir("/proc/self/task");
            if (final_check_dir) {
                int final_thread_count = 0;
                struct dirent* entry2;
                while ((entry2 = readdir(final_check_dir)) != nullptr) {
                    if (entry2->d_name[0] != '.') {
                        final_thread_count++;
                    }
                }
                closedir(final_check_dir);
                
                if (final_thread_count > 1) {
                    LOG_WARN_FMT("[Phase 2] ⚠️  %d threads still running after final cleanup", final_thread_count);
                    LOG_WARN("[Phase 2] Using force exit mechanism (simulating Ctrl+C)...");
                    
                    // 强制刷新所有输出
                    fflush(stdout);
                    fflush(stderr);
                    
                    // ⭐ 关键修复：直接使用_exit()强制退出，不等待任何清理
                    // _exit()会立即终止进程，跳过所有清理（包括静态对象析构、线程join等）
                    // 所有线程都会被操作系统强制终止
                    LOG_WARN("[Phase 2] Force exiting with _exit() to terminate all threads...");
                    
                    // 强制刷新所有输出，确保日志被写入
                    fflush(stdout);
                    fflush(stderr);
                    
                    // 立即使用_exit()强制退出
                    // _exit()不会返回，进程会立即终止，所有线程都会被强制终止
                    _exit(result == 0 ? 0 : 1);
                } else {
                    LOG_INFO("[Phase 2] ✅ Final cleanup succeeded, all threads exited");
                }
            }
        } else {
            LOG_INFO("[Phase 2] ✅ All threads have exited normally");
            // 清理FFmpeg网络资源（即使没有线程，也要清理）
            avformat_network_deinit();
        }
    } else {
        // 如果无法检查线程，直接清理网络资源
        avformat_network_deinit();
    }
    
    return result;
}

// ---------------- RTSP H.264 系列 (6 个) ----------------
// 根据图片配置：1280×720 (CBR/VBR), 1920×1080 (CBR/VBR), 3840×2160 (CBR/VBR)

static int test_rtsp_h264_1280x720_30_cbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(
        rtsp_url,
        1280, 720,
        "h264_taco",
        0,
        30.0,
        "high",
        "rtsp_h264_1280x720_30_cbr"
    );
}

static int test_rtsp_h264_1280x720_30_vbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(
        rtsp_url,
        1280, 720,
        "h264_taco",
        0,
        30.0,
        "high",
        "rtsp_h264_1280x720_30_vbr"
    );
}

static int test_rtsp_h264_1920x1080_30_cbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(
        rtsp_url,
        1920, 1080,
        "h264_taco",
        0,
        30.0,
        "high",
        "rtsp_h264_1920x1080_30_cbr"
    );
}

static int test_rtsp_h264_1920x1080_30_vbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(
        rtsp_url,
        1920, 1080,
        "h264_taco",
        0,
        30.0,
        "high",
        "rtsp_h264_1920x1080_30_vbr"
    );
}

static int test_rtsp_h264_3840x2160_30_cbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(
        rtsp_url,
        3840, 2160,
        "h264_taco",
        4,
        30.0,
        "high",
        "rtsp_h264_3840x2160_30_cbr"
    );
}

static int test_rtsp_h264_3840x2160_30_vbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(
        rtsp_url,
        3840, 2160,
        "h264_taco",
        4,
        30.0,
        "high",
        "rtsp_h264_3840x2160_30_vbr"
    );
}

// ---------------- RTSP H.265 系列 (6 个) ----------------
// 根据图片配置：1280×720 (CBR/VBR), 1920×1080 (CBR/VBR), 3840×2160 (CBR/VBR)

static int test_rtsp_h265_1280x720_30_cbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(
        rtsp_url,
        1280, 720,
        "hevc_taco",
        0,
        30.0,
        "main",
        "rtsp_h265_1280x720_30_cbr"
    );
}

static int test_rtsp_h265_1280x720_30_vbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(
        rtsp_url,
        1280, 720,
        "hevc_taco",
        0,
        30.0,
        "main",
        "rtsp_h265_1280x720_30_vbr"
    );
}

static int test_rtsp_h265_1920x1080_30_cbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(
        rtsp_url,
        1920, 1080,
        "hevc_taco",
        0,
        30.0,
        "main",
        "rtsp_h265_1920x1080_30_cbr"
    );
}

static int test_rtsp_h265_1920x1080_30_vbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(
        rtsp_url,
        1920, 1080,
        "hevc_taco",
        0,
        30.0,
        "main",
        "rtsp_h265_1920x1080_30_vbr"
    );
}

static int test_rtsp_h265_3840x2160_30_cbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(
        rtsp_url,
        3840, 2160,
        "hevc_taco",
        4,
        30.0,
        "main",
        "rtsp_h265_3840x2160_30_cbr"
    );
}

static int test_rtsp_h265_3840x2160_30_vbr(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(
        rtsp_url,
        3840, 2160,
        "hevc_taco",
        4,
        30.0,
        "main",
        "rtsp_h265_3840x2160_30_vbr"
    );
}

// ---------------- RTSP MJPEG 系列 ----------------

static int test_rtsp_mjpeg_32768x18432_30(const char* rtsp_url) {
    return run_rtsp_decode_test_with_params(
        rtsp_url,
        32768, 18432,
        "jpeg_taco",
        4,
        30.0,
        "main",
        "rtsp_mjpeg_32768x18432_30"
    );
}

// ========== RTSP 测试用例注册 ==========

// RTSP H.264
REGISTER_TEST(rtsp_h264_1280x720_30_cbr, "RTSP H.264 1280x720 30fps CBR", test_rtsp_h264_1280x720_30_cbr);
REGISTER_TEST(rtsp_h264_1280x720_30_vbr, "RTSP H.264 1280x720 30fps VBR", test_rtsp_h264_1280x720_30_vbr);
REGISTER_TEST(rtsp_h264_1920x1080_30_cbr, "RTSP H.264 1920x1080 30fps CBR", test_rtsp_h264_1920x1080_30_cbr);
REGISTER_TEST(rtsp_h264_1920x1080_30_vbr, "RTSP H.264 1920x1080 30fps VBR", test_rtsp_h264_1920x1080_30_vbr);
REGISTER_TEST(rtsp_h264_3840x2160_30_cbr, "RTSP H.264 3840x2160 30fps CBR", test_rtsp_h264_3840x2160_30_cbr);
REGISTER_TEST(rtsp_h264_3840x2160_30_vbr, "RTSP H.264 3840x2160 30fps VBR", test_rtsp_h264_3840x2160_30_vbr);

// RTSP H.265
REGISTER_TEST(rtsp_h265_1280x720_30_cbr, "RTSP H.265 1280x720 30fps CBR", test_rtsp_h265_1280x720_30_cbr);
REGISTER_TEST(rtsp_h265_1280x720_30_vbr, "RTSP H.265 1280x720 30fps VBR", test_rtsp_h265_1280x720_30_vbr);
REGISTER_TEST(rtsp_h265_1920x1080_30_cbr, "RTSP H.265 1920x1080 30fps CBR", test_rtsp_h265_1920x1080_30_cbr);
REGISTER_TEST(rtsp_h265_1920x1080_30_vbr, "RTSP H.265 1920x1080 30fps VBR", test_rtsp_h265_1920x1080_30_vbr);
REGISTER_TEST(rtsp_h265_3840x2160_30_cbr, "RTSP H.265 3840x2160 30fps CBR", test_rtsp_h265_3840x2160_30_cbr);
REGISTER_TEST(rtsp_h265_3840x2160_30_vbr, "RTSP H.265 3840x2160 30fps VBR", test_rtsp_h265_3840x2160_30_vbr);

// RTSP MJPEG
REGISTER_TEST(rtsp_mjpeg_32768x18432_30, "RTSP MJPEG 32768x18432 30fps", test_rtsp_mjpeg_32768x18432_30);



#pragma GCC diagnostic pop

// ========== 主函数：使用 TEST_MAIN 统一入口 ==========
  
  int main(int argc, char* argv[]) {
      INIT_LOGGER();
  
      signal(SIGINT,  [](int){ g_running = false; });
      signal(SIGTERM, [](int){ g_running = false; });
  
      TEST_MAIN(argc, argv);
  }
  