/**
 * Post-Processing (PP) Test
 * 
 * 后处理功能测试程序，用于测试TACO解码器的后处理功能
 * 
 * 功能：
 * - 单PP测试：测试单个后处理功能（裁剪或缩放）
 * - 多PP测试：测试多个后处理功能组合（裁剪+缩放）
 * - 裁剪测试：测试视频裁剪功能
 * - 支持保存处理后的帧用于验证
 * 
 * 编译：
 *   通过 Buildroot 构建系统：
 *     cd /home/zyko/workshop-debian
 *     make components-rebuild
 * 
 * 使用方法：
 *   # 单PP测试：裁剪
 *   ./test_pp video.mp4 --single-pp --crop 100,100,800,600
 *   
 *   # 单PP测试：缩放
 *   ./test_pp video.mp4 --single-pp --scale 1280x720
 *   
 *   # 多PP测试：裁剪+缩放
 *   ./test_pp video.mp4 --multi-pp --crop 100,100,800,600 --scale 1280x720
 *   
 *   # 仅裁剪测试
 *   ./test_pp video.mp4 --crop-only --crop 100,100,800,600
 *   
 *   # 保存处理后的帧
 *   ./test_pp video.mp4 --single-pp --scale 1280x720 --save-frames 100
 * 
 * 验证结果：
 *   ffplay -f rawvideo -pix_fmt argb -s 1280x720 /tmp/pp_output_*.rgb
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

// Components 头文件
#include "productionline/VideoProductionLine.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/io/BufferWriter.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "display/LinuxFramebufferDevice.hpp"
#include "monitor/PerformanceMonitor.hpp"
#include "common/Logger.hpp"

// 全局变量
static std::atomic<bool> g_running(true);

// 信号处理函数
void signal_handler(int signum) {
    printf("\n[Signal] Caught signal %d, stopping...\n", signum);
    g_running = false;
}

/**
 * @brief 保存ARGB帧数据到文件
 */
bool save_argb_frame(FILE* fp, Buffer* buffer, int width, int height) {
    if (!fp || !buffer) {
        return false;
    }
    
    uint8_t* data = (uint8_t*)buffer->getVirtualAddress();
    if (!data) {
        LOG_ERROR("[ERROR] No virtual address available");
        return false;
    }
    
    size_t frame_size = width * height * 4; // ARGB = 4 bytes per pixel
    if (fwrite(data, 1, frame_size, fp) != frame_size) {
        LOG_ERROR("[ERROR] Failed to write frame data");
        return false;
    }
    
    return true;
}

/**
 * @brief 解析裁剪参数 "x,y,width,height"
 */
bool parse_crop(const char* crop_str, int& x, int& y, int& width, int& height) {
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
bool parse_resolution(const char* res_str, int& width, int& height) {
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
 * @brief 解析命令行参数
 */
struct TestConfig {
    std::string video_file;
    std::string codec = "h264";
    bool single_pp = false;
    bool multi_pp = false;
    bool crop_only = false;
    int crop_x = 0;
    int crop_y = 0;
    int crop_width = 0;
    int crop_height = 0;
    int scale_width = 0;
    int scale_height = 0;
    int save_frames = 0;
    int max_frames = -1;
    int threads = 2;
    bool enable_display = false;
    std::string output_file;
    bool auto_test = false;  // 自动执行所有测试用例
    
    // PP格式配置（用于多PP测试）
    std::string pp0_format;  // PP0输出格式
    std::string pp1_format;  // PP1输出格式
    
    bool has_crop() const {
        return crop_width > 0 && crop_height > 0;
    }
    
    bool has_scale() const {
        return scale_width > 0 && scale_height > 0;
    }
};

/**
 * @brief 测试用例结构体
 */
struct TestCase {
    const char* name;           // 测试用例名称
    const char* description;    // 测试描述
    std::string pp0_format;     // PP0输出格式
    std::string pp1_format;     // PP1输出格式
    int crop_x, crop_y, crop_w, crop_h;  // 裁剪参数（如果适用）
    int scale_w, scale_h;       // 缩放参数（如果适用）
    bool is_crop_test;          // 是否为裁剪测试
    bool is_multi_pp;           // 是否为多PP测试
};

// 多PP测试用例（根据图片中的表格）
static const TestCase MULTI_PP_TESTS[] = {
    {"T01", "PP0: YUV420 8-bit NV12 -> PP1: RGB888", "nv12", "rgb888", 0, 0, 0, 0, 0, 0, false, true},
    {"T02", "PP0: YUV420 8-bit NV12 -> PP1: ARGB8888", "nv12", "argb8888", 0, 0, 0, 0, 0, 0, false, true},
    {"T03", "PP0: YUV420 8-bit NV21 -> PP1: BGR888", "nv21", "bgr888", 0, 0, 0, 0, 0, 0, false, true},
    {"T04", "PP0: YUV420 8-bit NV12 -> PP1: RGB888 (8-bit)", "nv12", "rgb888", 0, 0, 0, 0, 0, 0, false, true},
    {"T05", "PP0: YUV420 P010 (10-bit) -> PP1: ARGB2101010 (10-bit)", "p010", "argb2101010", 0, 0, 0, 0, 0, 0, false, true},
    {"T06", "PP0: YUV420 I010 (10-bit) -> PP1: RGB161616 (16-bit)", "i010", "rgb161616", 0, 0, 0, 0, 0, 0, false, true},
    {"T07", "PP0: YUV400 8-bit -> PP1: RGB888", "yuv400", "rgb888", 0, 0, 0, 0, 0, 0, false, true},
    {"T09", "PP0: YUV420 8-bit NV12 -> PP1: RGB888 planar", "nv12", "rgb888_planar", 0, 0, 0, 0, 0, 0, false, true},
    {"T10", "PP0: YUV420 NV21 P010 -> PP1: ARGB8888", "nv21_p010", "argb8888", 0, 0, 0, 0, 0, 0, false, true},
    {"T11", "PP0: YUV420 8-bit NV12 -> PP1: YUV420 8-bit NV21", "nv12", "nv21", 0, 0, 0, 0, 0, 0, false, true},
};

// 多PP+裁剪测试用例（根据图片中的表格）
static const TestCase MULTI_PP_CROP_TESTS[] = {
    {"Crop1", "PP0 crop: 4096x2160 -> 1920x1080", "", "", 0, 0, 1920, 1080, 0, 0, true, false},
    {"Crop2", "PP0 down-scale: 32768x32768 -> 1280x720", "", "", 0, 0, 0, 0, 1280, 720, false, false},
    {"Crop3", "PP1 crop: 4096x2160 -> 1920x1080", "", "", 0, 0, 1920, 1080, 0, 0, true, false},
    {"Crop4", "PP1 down-scale: 32768x32768 -> 1280x720", "", "", 0, 0, 0, 0, 1280, 720, false, false},
    {"Crop5", "PP0 down-scale: 32768x32768 -> 256x256", "", "", 0, 0, 0, 0, 256, 256, false, false},
    {"Crop6", "PP1 down-scale: 4096x2160 -> 128x128", "", "", 0, 0, 0, 0, 128, 128, false, false},
};

static const int NUM_MULTI_PP_TESTS = sizeof(MULTI_PP_TESTS) / sizeof(MULTI_PP_TESTS[0]);
static const int NUM_MULTI_PP_CROP_TESTS = sizeof(MULTI_PP_CROP_TESTS) / sizeof(MULTI_PP_CROP_TESTS[0]);

bool parse_arguments(int argc, char* argv[], TestConfig& config, char* program_name) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--single-pp") == 0) {
            config.single_pp = true;
        } else if (strcmp(argv[i], "--multi-pp") == 0) {
            config.multi_pp = true;
        } else if (strcmp(argv[i], "--crop-only") == 0) {
            config.crop_only = true;
        } else if (strcmp(argv[i], "--crop") == 0 && i + 1 < argc) {
            if (!parse_crop(argv[++i], config.crop_x, config.crop_y, 
                           config.crop_width, config.crop_height)) {
                fprintf(stderr, "Invalid crop format: %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            if (!parse_resolution(argv[++i], config.scale_width, config.scale_height)) {
                fprintf(stderr, "Invalid scale format: %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            config.codec = argv[++i];
            // 标准化编解码器名称
            if (config.codec == "h265" || config.codec == "hevc") {
                config.codec = "h265";
            } else if (config.codec == "h264") {
                config.codec = "h264";
            } else if (config.codec == "mjpeg" || config.codec == "motion-jpeg") {
                config.codec = "mjpeg";
            } else {
                fprintf(stderr, "Warning: Unknown codec '%s', using as-is\n", config.codec.c_str());
                fprintf(stderr, "Supported codecs: h264, h265/hevc, mjpeg\n");
            }
        } else if (strcmp(argv[i], "--save-frames") == 0 && i + 1 < argc) {
            config.save_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-frames") == 0 && i + 1 < argc) {
            config.max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            config.threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--display") == 0) {
            config.enable_display = true;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            config.output_file = argv[++i];
        } else if (strcmp(argv[i], "--auto-test") == 0 || strcmp(argv[i], "--all-tests") == 0) {
            config.auto_test = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s <video_file> [options]\n", argv[0]);
            printf("\nOptions:\n");
            printf("  --single-pp          Single PP test (crop OR scale)\n");
            printf("  --multi-pp           Multi PP test (crop AND scale)\n");
            printf("  --crop-only          Crop-only test\n");
            printf("  --crop x,y,w,h       Crop region (e.g., 100,100,800,600)\n");
            printf("  --scale WxH          Scale resolution (e.g., 1280x720)\n");
            printf("  --codec CODEC        Codec (h264, h265/hevc, mjpeg, default: h264)\n");
            printf("  --save-frames N      Save N frames (0=disable, -1=all)\n");
            printf("  --max-frames N       Maximum frames to process (-1=all)\n");
            printf("  --threads N          Number of threads (default: 2)\n");
            printf("  --display            Enable display output\n");
            printf("  --output FILE        Output file path\n");
            printf("  --auto-test          Run all test cases automatically\n");
            printf("  --all-tests          Alias for --auto-test\n");
            printf("\nExamples:\n");
            printf("  %s video.mp4 --single-pp --crop 100,100,800,600\n", argv[0]);
            printf("  %s video.mp4 --single-pp --scale 1280x720\n", argv[0]);
            printf("  %s video.mp4 --multi-pp --crop 100,100,800,600 --scale 1280x720\n", argv[0]);
            printf("  %s video.mp4 --crop-only --crop 100,100,800,600\n", argv[0]);
            printf("  %s video.mp4 --auto-test --codec h264\n", argv[0]);
            return false;
        } else if (argv[i][0] != '-') {
            config.video_file = argv[i];
        }
    }
    
    if (config.video_file.empty()) {
        fprintf(stderr, "Error: Video file not specified\n");
        return false;
    }
    
    // 验证测试模式（如果启用了自动测试，跳过模式验证）
    if (!config.auto_test) {
        int mode_count = (config.single_pp ? 1 : 0) + 
                         (config.multi_pp ? 1 : 0) + 
                         (config.crop_only ? 1 : 0);
        if (mode_count == 0) {
            fprintf(stderr, "Error: Must specify one of --single-pp, --multi-pp, --crop-only, or --auto-test\n");
            return false;
        }
        if (mode_count > 1) {
            fprintf(stderr, "Error: Can only specify one of --single-pp, --multi-pp, or --crop-only\n");
            return false;
        }
    }
    
    // 验证参数
    if (config.single_pp) {
        // 单PP测试允许不指定crop或scale（测试基本解码功能）
        if (config.has_crop() && config.has_scale()) {
            fprintf(stderr, "Error: --single-pp can only use --crop OR --scale, not both\n");
            fprintf(stderr, "Use --multi-pp if you want to test both crop and scale\n");
            return false;
        }
    }
    
    if (config.multi_pp) {
        if (!config.has_crop() || !config.has_scale()) {
            fprintf(stderr, "Error: --multi-pp requires both --crop and --scale\n");
            fprintf(stderr, "\nExample:\n");
            fprintf(stderr, "  %s %s --multi-pp --crop 100,100,800,600 --scale 1280x720\n", 
                    program_name, config.video_file.c_str());
            return false;
        }
    }
    
    if (config.crop_only) {
        if (!config.has_crop()) {
            fprintf(stderr, "Error: --crop-only requires --crop\n");
            fprintf(stderr, "\nExample:\n");
            fprintf(stderr, "  %s %s --crop-only --crop 100,100,800,600\n", 
                    program_name, config.video_file.c_str());
            return false;
        }
    }
    
    return true;
}

/**
 * @brief 将测试用例格式字符串映射到硬件解码器支持的格式
 * @param test_format 测试用例中的格式字符串
 * @return 映射后的格式字符串，如果格式不支持则返回空字符串
 * 
 * 支持的硬件解码器格式：
 * - argb888, abgr888, bgra888, rgba888 (8-bit ARGB/ABGR/BGRA/RGBA)
 * - rgb888, bgr888 (8-bit RGB/BGR)
 * - xrgb888, xbgr888 (8-bit 0RGB/0BGR)
 * - r16g16b16, b16g16r16 (16-bit RGB/BGR)
 */
std::string map_format_to_hardware(const std::string& test_format) {
    if (test_format.empty()) {
        return "argb888";  // 默认格式
    }
    
    // 直接支持的格式
    if (test_format == "rgb888" || test_format == "rgb888_planar") {
        return "rgb888";
    }
    if (test_format == "argb8888") {
        return "argb888";
    }
    if (test_format == "bgr888") {
        return "bgr888";
    }
    if (test_format == "abgr8888") {
        return "abgr888";
    }
    if (test_format == "bgra8888") {
        return "bgra888";
    }
    if (test_format == "rgba8888") {
        return "rgba888";
    }
    if (test_format == "xrgb8888") {
        return "xrgb888";
    }
    if (test_format == "xbgr8888") {
        return "xbgr888";
    }
    
    // 16-bit格式映射
    if (test_format == "rgb161616" || test_format == "rgb16") {
        return "r16g16b16";
    }
    if (test_format == "bgr161616" || test_format == "bgr16") {
        return "b16g16r16";
    }
    
    // 不支持的格式（YUV格式、10-bit格式等）
    // 这些格式硬件解码器不支持作为ch1_rgb_format
    if (test_format == "nv12" || test_format == "nv21" || 
        test_format == "p010" || test_format == "i010" || 
        test_format == "yuv400" || test_format == "nv21_p010" ||
        test_format == "argb2101010" || test_format == "yuv420") {
        return "";  // 返回空字符串表示不支持
    }
    
    // 未知格式，尝试直接使用（可能失败）
    return test_format;
}

/**
 * @brief 检查格式是否支持
 */
bool is_format_supported(const std::string& test_format) {
    std::string mapped = map_format_to_hardware(test_format);
    return !mapped.empty();
}

/**
 * @brief 执行PP测试
 */
bool run_pp_test(const TestConfig& config) {
    LOG_INFO("");
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  Post-Processing (PP) Test                            ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO("");
    
    // 显示配置信息
    LOG_INFO_FMT("Video file: %s", config.video_file.c_str());
    LOG_INFO_FMT("Codec: %s", config.codec.c_str());
    LOG_INFO_FMT("Threads: %d", config.threads);
    
    if (config.single_pp) {
        LOG_INFO("Test mode: Single PP");
        if (config.has_crop()) {
            LOG_INFO_FMT("  Crop: (%d, %d) %dx%d", 
                        config.crop_x, config.crop_y, 
                        config.crop_width, config.crop_height);
        } else if (config.has_scale()) {
            LOG_INFO_FMT("  Scale: %dx%d", config.scale_width, config.scale_height);
        } else {
            LOG_INFO("  No PP operations (basic decode test)");
        }
    } else if (config.multi_pp) {
        LOG_INFO("Test mode: Multi PP");
        LOG_INFO_FMT("  Crop: (%d, %d) %dx%d", 
                    config.crop_x, config.crop_y, 
                    config.crop_width, config.crop_height);
        LOG_INFO_FMT("  Scale: %dx%d", config.scale_width, config.scale_height);
    } else if (config.crop_only) {
        LOG_INFO("Test mode: Crop Only");
        LOG_INFO_FMT("  Crop: (%d, %d) %dx%d", 
                    config.crop_x, config.crop_y, 
                    config.crop_width, config.crop_height);
    }
    
    LOG_INFO("");
    
    // 构建Worker配置
    WorkerConfig worker_config;
    
    // 对于MJPEG，如果没有硬件解码器，使用软件解码器
    // 对于H264/H265，使用TACO硬件解码器（支持PP功能）
    bool use_software_decoder = (config.codec == "mjpeg");
    
    if (use_software_decoder) {
        // MJPEG使用软件解码器（不支持PP功能）
        LOG_INFO("Using software decoder for MJPEG (PP features not available)");
        worker_config = WorkerConfigBuilder()
            .setFileConfig(
                FileConfigBuilder()
                    .setFilePath(config.video_file)
                    .setEndFrame(config.max_frames >= 0 ? config.max_frames : -1)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useSoftware()
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
            .build();
        
        if (config.has_crop() || config.has_scale()) {
            LOG_WARN("⚠️  Warning: Software decoder does not support PP features (crop/scale)");
            LOG_WARN("   PP parameters will be ignored");
        }
    } else {
        // H264/H265使用TACO硬件解码器（支持PP功能）
        TacoConfigBuilder taco_builder;
        taco_builder.setChannels(true, true);  // 启用ch0和ch1
        
        // 配置PP格式（如果指定了多PP测试格式）
        if (config.multi_pp && !config.pp1_format.empty()) {
            // 多PP测试：根据pp1_format配置输出格式
            std::string format = map_format_to_hardware(config.pp1_format);
            
            if (format.empty()) {
                // 格式不支持，记录警告并跳过此测试
                LOG_WARN_FMT("⚠️  Format '%s' is not supported by hardware decoder", 
                            config.pp1_format.c_str());
                LOG_WARN("   Supported formats: argb888, abgr888, bgra888, rgba888,");
                LOG_WARN("                      rgb888, bgr888, xrgb888, xbgr888,");
                LOG_WARN("                      r16g16b16, b16g16r16");
                LOG_WARN("   Skipping this test case...");
                return false;  // 测试失败（格式不支持）
            }
            
            LOG_INFO_FMT("Format mapping: '%s' -> '%s'", 
                        config.pp1_format.c_str(), format.c_str());
            taco_builder.setRgbConfig(true, format, "bt601");  // ch1输出指定格式
        } else {
            taco_builder.setRgbConfig(true, "argb888", "bt601");  // ch1输出ARGB（默认）
        }
        
        if (config.has_crop()) {
            taco_builder.setCropRegion(config.crop_x, config.crop_y, 
                                      config.crop_width, config.crop_height);
        }
        
        if (config.has_scale()) {
            taco_builder.setDecoderOutputResolution(config.scale_width, config.scale_height);
        }
        
        auto taco_config = taco_builder.build();
        
        worker_config = WorkerConfigBuilder()
            .setFileConfig(
                FileConfigBuilder()
                    .setFilePath(config.video_file)
                    .setEndFrame(config.max_frames >= 0 ? config.max_frames : -1)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco(config.codec, taco_config)
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
            .build();
    }
    
    // 创建VideoProductionLine
    VideoProductionLine producer(false, config.threads, false);
    
    // 启动解码
    LOG_INFO("[Step 1/5] Starting decode...");
    LOG_INFO_FMT("Using decoder: %s_taco", config.codec.c_str());
    if (!producer.start(worker_config)) {
        LOG_ERROR("❌ Failed to start producer");
        LOG_ERROR("Possible causes:");
        LOG_ERROR("  1. Wrong codec specified (file might be MJPEG but using h264 decoder)");
        LOG_ERROR_FMT("  2. Try: ./test_pp %s --single-pp --codec mjpeg", 
                     config.video_file.c_str());
        return false;
    }
    LOG_INFO("✅ Producer started");
    
    // 获取BufferPool
    LOG_INFO("[Step 2/5] Getting BufferPool...");
    uint64_t pool_id = producer.getWorkingBufferPoolId();
    if (pool_id == 0) {
        LOG_ERROR("❌ No working BufferPool ID available");
        producer.stop();
        return false;
    }
    
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool_sptr = pool_weak.lock();
    if (!pool_sptr) {
        LOG_ERROR("❌ BufferPool not found or destroyed");
        producer.stop();
        return false;
    }
    
    LOG_INFO_FMT("✅ BufferPool: '%s' (ID: %lu)", 
                pool_sptr->getName().c_str(), pool_id);
    
    // 打开输出文件（如果需要）
    FILE* output_file = nullptr;
    char output_path[256];
    const char* output_file_path = nullptr;
    
    if (config.save_frames != 0) {
        LOG_INFO("[Step 3/5] Opening output file...");
        if (config.output_file.empty()) {
            time_t now = time(nullptr);
            snprintf(output_path, sizeof(output_path), 
                    "/tmp/pp_output_%ld.rgb", now);
            output_file_path = output_path;
        } else {
            output_file_path = config.output_file.c_str();
        }
        output_file = fopen(output_file_path, "wb");
        if (!output_file) {
            LOG_ERROR_FMT("❌ Failed to open output file: %s", output_file_path);
            producer.stop();
            return false;
        }
        LOG_INFO_FMT("✅ Output file: %s", output_file_path);
    } else {
        LOG_INFO("[Step 3/5] Output file disabled");
    }
    
    // 初始化显示设备（如果需要）
    std::unique_ptr<LinuxFramebufferDevice> display;
    bool has_display = false;
    
    if (config.enable_display) {
        LOG_INFO("[Step 4/5] Initializing display...");
        display = std::make_unique<LinuxFramebufferDevice>();
        has_display = display->initialize(0);
        if (has_display) {
            LOG_INFO_FMT("✅ Display initialized: %dx%d @ %d bpp",
                        display->getWidth(), display->getHeight(), display->getBitsPerPixel());
        } else {
            LOG_WARN("⚠️  Display not available, continuing without display");
        }
    } else {
        LOG_INFO("[Step 4/5] Display disabled");
    }
    
    // 消费者循环
    LOG_INFO("[Step 5/5] Processing frames...");
    LOG_INFO("Press Ctrl+C to stop early");
    LOG_INFO("");
    
    int frame_count = 0;
    int save_count = 0;
    int display_count = 0;
    auto start_time = std::chrono::steady_clock::now();
    
    // 确定输出分辨率（用于保存）
    int output_width = config.has_scale() ? config.scale_width : 
                      (config.has_crop() ? config.crop_width : 0);
    int output_height = config.has_scale() ? config.scale_height : 
                       (config.has_crop() ? config.crop_height : 0);
    
    // 如果都没有，需要从Buffer中获取
    bool need_detect_resolution = (output_width == 0 || output_height == 0);
    
    while (g_running && (config.max_frames < 0 || frame_count < config.max_frames)) {
        Buffer* buffer = pool_sptr->acquireFilled(true, 100);
        
        if (!buffer) {
            if (!producer.isRunning()) {
                LOG_INFO("Producer stopped, exiting consumer loop");
                break;
            }
            continue;
        }
        
        // 检测分辨率（如果需要）
        if (need_detect_resolution) {
            AVFrame* frame = buffer->getAVFrame();
            if (frame) {
                output_width = frame->width;
                output_height = frame->height;
                
                // 如果width/height为0，尝试从linesize和format推断
                if ((output_width == 0 || output_height == 0)) {
                    // 记录调试信息（仅第一帧）
                    if (frame_count == 0) {
                        LOG_INFO_FMT("[DEBUG] AVFrame: format=%d, linesize[0]=%d, linesize[1]=%d, linesize[2]=%d",
                                    frame->format, frame->linesize[0], 
                                    frame->linesize[1], frame->linesize[2]);
                    }
                    
                    // 尝试从linesize推断（根据不同的像素格式）
                    if (frame->linesize[0] > 0) {
                        // AV_PIX_FMT_ARGB = 28 (libavutil/pixfmt.h)
                        // AV_PIX_FMT_YUV420P = 0
                        // AV_PIX_FMT_NV12 = 23
                        
                        if (frame->format == 28 || frame->format == 2) {  // ARGB or RGB
                            // ARGB格式：linesize[0] = width * 4
                            int inferred_width = frame->linesize[0] / 4;
                            if (inferred_width > 0 && inferred_width <= 7680) {
                                // 尝试常见分辨率
                                if (inferred_width == 640) {
                                    output_width = 640;
                                    output_height = 480;
                                } else if (inferred_width == 1280) {
                                    output_width = 1280;
                                    output_height = 720;
                                } else if (inferred_width == 1920) {
                                    output_width = 1920;
                                    output_height = 1080;
                                } else {
                                    // 使用linesize推断的width，height需要从其他地方获取
                                    output_width = inferred_width;
                                    // 尝试从linesize[1]推断height（如果有）
                                    if (frame->linesize[1] > 0) {
                                        output_height = frame->linesize[1] / 4;
                                    }
                                }
                            }
                        } else if (frame->format == 0) {  // YUV420P
                            // YUV420P格式：linesize[0] = width (可能对齐)
                            // 尝试常见分辨率
                            int inferred_width = frame->linesize[0];
                            if (inferred_width == 640 || inferred_width >= 640) {
                                // 640可能是对齐后的值，尝试640x480
                                if (inferred_width == 640) {
                                    output_width = 640;
                                    output_height = 480;
                                } else if (inferred_width == 1280) {
                                    output_width = 1280;
                                    output_height = 720;
                                } else if (inferred_width == 1920) {
                                    output_width = 1920;
                                    output_height = 1080;
                                } else {
                                    output_width = inferred_width;
                                    // 尝试从linesize[1]推断（U plane）
                                    if (frame->linesize[1] > 0) {
                                        output_height = frame->linesize[1] * 2;  // U plane是width/2
                                    }
                                }
                            }
                        } else if (frame->format == 23) {  // NV12
                            // NV12格式：linesize[0] = width (可能对齐)
                            int inferred_width = frame->linesize[0];
                            if (inferred_width == 640) {
                                output_width = 640;
                                output_height = 480;
                            } else if (inferred_width == 1280) {
                                output_width = 1280;
                                output_height = 720;
                            } else if (inferred_width == 1920) {
                                output_width = 1920;
                                output_height = 1080;
                            } else if (inferred_width > 0) {
                                output_width = inferred_width;
                                // NV12的UV plane在linesize[1]，height可以从那里推断
                                if (frame->linesize[1] > 0) {
                                    output_height = frame->linesize[1] * 2;
                                }
                            }
                        }
                        
                        // 如果成功推断
                        if (output_width > 0 && output_height > 0) {
                            LOG_INFO_FMT("Detected output resolution from linesize: %dx%d (format=%d)", 
                                       output_width, output_height, frame->format);
                            need_detect_resolution = false;
                        }
                    }
                }
                
                // 如果成功获取分辨率
                if (output_width > 0 && output_height > 0) {
                    if (frame_count == 0 || need_detect_resolution) {
                        LOG_INFO_FMT("Detected output resolution: %dx%d", output_width, output_height);
                        need_detect_resolution = false;
                    }
                } else if (frame_count < 5) {
                    // 前5帧继续尝试
                    if (frame_count == 0) {
                        LOG_WARN_FMT("⚠️  Cannot detect resolution from frame %d (width=%d, height=%d, format=%d, linesize[0]=%d), will retry", 
                                    frame_count, output_width, output_height,
                                    frame->format, frame->linesize[0]);
                    }
                } else {
                    // 5帧后仍无法检测，使用默认值或跳过
                    LOG_WARN("⚠️  Cannot detect resolution after 5 frames, resolution-dependent features may not work");
                    LOG_WARN("   Hint: Use --crop or --scale to specify resolution explicitly");
                    need_detect_resolution = false;  // 停止尝试
                }
            } else {
                if (frame_count == 0) {
                    LOG_WARN_FMT("⚠️  AVFrame is null for frame %d", frame_count);
                }
            }
        }
        
        // 显示到屏幕（如果需要）
        if (has_display && display && display->displayFilledFramebuffer(buffer)) {
            display_count++;
        }
        
        // 保存到文件（如果需要）
        if (output_file && 
            (config.save_frames < 0 || save_count < config.save_frames)) {
            if (output_width > 0 && output_height > 0) {
                if (save_argb_frame(output_file, buffer, output_width, output_height)) {
                    save_count++;
                }
            } else {
                // 分辨率未检测到，跳过保存
                if (frame_count == 1) {
                    LOG_WARN("⚠️  Cannot save frames: resolution not detected (width=0 or height=0)");
                }
            }
        }
        
        pool_sptr->releaseFilled(buffer);
        frame_count++;
        
        // 显示进度
        if (frame_count % 30 == 0 || frame_count == config.max_frames) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start_time).count();
            double fps = elapsed > 0 ? (frame_count * 1000.0 / elapsed) : 0.0;
            LOG_INFO_FMT("Progress: %d frames | FPS: %.1f | Display: %d | Saved: %d",
                        frame_count, fps, display_count, save_count);
        }
    }
    
    // 清理
    if (output_file) {
        fclose(output_file);
        LOG_INFO_FMT("✅ Output file closed: %s", output_file_path);
    }
    
    producer.stop();
    
    auto end_time = std::chrono::steady_clock::now();
    auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();
    double avg_fps = total_time > 0 ? (frame_count * 1000.0 / total_time) : 0.0;
    
    // 输出报告
    LOG_INFO("");
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  Test Report                                          ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO_FMT("Total frames processed: %d", frame_count);
    LOG_INFO_FMT("Frames displayed: %d", display_count);
    LOG_INFO_FMT("Frames saved: %d", save_count);
    LOG_INFO_FMT("Total time: %.2f seconds", total_time / 1000.0);
    LOG_INFO_FMT("Average FPS: %.2f", avg_fps);
    LOG_INFO("");
    
    if (output_file && save_count > 0 && output_file_path) {
        LOG_INFO_FMT("📁 Saved data: %s", output_file_path);
        LOG_INFO_FMT("   Resolution: %dx%d", output_width, output_height);
        LOG_INFO("   You can verify with FFmpeg:");
        LOG_INFO_FMT("   ffplay -f rawvideo -pix_fmt argb -s %dx%d %s",
                    output_width, output_height, output_file_path);
        LOG_INFO("");
    }
    
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  ✅ PP Test Completed                                ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    
    return true;
}

/**
 * @brief 主函数
 */
/**
 * @brief 自动执行所有测试用例
 */
bool run_auto_tests(const TestConfig& base_config) {
    LOG_INFO("");
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  Auto Test Suite - Running All Test Cases             ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO("");
    LOG_INFO_FMT("Video file: %s", base_config.video_file.c_str());
    LOG_INFO_FMT("Codec: %s", base_config.codec.c_str());
    LOG_INFO_FMT("Threads: %d", base_config.threads);
    LOG_INFO("");
    
    int total_tests = NUM_MULTI_PP_TESTS + NUM_MULTI_PP_CROP_TESTS;
    int passed_tests = 0;
    int failed_tests = 0;
    int skipped_tests = 0;
    
    // 执行多PP测试用例
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  Multi-PP Tests (Format Conversion)                   ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO("");
    
    for (int i = 0; i < NUM_MULTI_PP_TESTS; i++) {
        const TestCase& test_case = MULTI_PP_TESTS[i];
        LOG_INFO("─────────────────────────────────────────────────────────");
        LOG_INFO_FMT("Test Case %s: %s", test_case.name, test_case.description);
        LOG_INFO("─────────────────────────────────────────────────────────");
        
        // 检查格式是否支持
        if (!test_case.pp1_format.empty() && !is_format_supported(test_case.pp1_format)) {
            skipped_tests++;
            LOG_WARN_FMT("⚠️  Test %s SKIPPED: Format '%s' is not supported", 
                        test_case.name, test_case.pp1_format.c_str());
            LOG_WARN("   This format cannot be used as ch1_rgb_format");
            LOG_WARN("   Supported formats: argb888, abgr888, bgra888, rgba888,");
            LOG_WARN("                      rgb888, bgr888, xrgb888, xbgr888,");
            LOG_WARN("                      r16g16b16, b16g16r16");
            LOG_INFO("");
            continue;  // 跳过不支持的测试
        }
        
        TestConfig test_config = base_config;
        test_config.multi_pp = true;
        test_config.pp0_format = test_case.pp0_format;
        test_config.pp1_format = test_case.pp1_format;
        test_config.max_frames = base_config.max_frames >= 0 ? base_config.max_frames : 30;  // 限制测试帧数
        
        bool success = run_pp_test(test_config);
        if (success) {
            passed_tests++;
            LOG_INFO_FMT("✅ Test %s PASSED", test_case.name);
        } else {
            failed_tests++;
            LOG_ERROR_FMT("❌ Test %s FAILED", test_case.name);
        }
        LOG_INFO("");
        
        // 短暂延迟，避免资源竞争
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // 执行多PP+裁剪测试用例
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  Multi-PP + Crop Tests                                 ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO("");
    
    for (int i = 0; i < NUM_MULTI_PP_CROP_TESTS; i++) {
        const TestCase& test_case = MULTI_PP_CROP_TESTS[i];
        LOG_INFO("─────────────────────────────────────────────────────────");
        LOG_INFO_FMT("Test Case %s: %s", test_case.name, test_case.description);
        LOG_INFO("─────────────────────────────────────────────────────────");
        
        TestConfig test_config = base_config;
        if (test_case.is_crop_test) {
            test_config.crop_only = true;
            test_config.crop_x = test_case.crop_x;
            test_config.crop_y = test_case.crop_y;
            test_config.crop_width = test_case.crop_w;
            test_config.crop_height = test_case.crop_h;
        } else {
            test_config.single_pp = true;
            test_config.scale_width = test_case.scale_w;
            test_config.scale_height = test_case.scale_h;
        }
        test_config.max_frames = base_config.max_frames >= 0 ? base_config.max_frames : 30;  // 限制测试帧数
        
        bool success = run_pp_test(test_config);
        if (success) {
            passed_tests++;
            LOG_INFO_FMT("✅ Test %s PASSED", test_case.name);
        } else {
            failed_tests++;
            LOG_ERROR_FMT("❌ Test %s FAILED", test_case.name);
        }
        LOG_INFO("");
        
        // 短暂延迟，避免资源竞争
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // 输出测试总结
    LOG_INFO("");
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  Test Summary                                          ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO_FMT("Total tests: %d", total_tests);
    LOG_INFO_FMT("Passed: %d ✅", passed_tests);
    LOG_INFO_FMT("Failed: %d ❌", failed_tests);
    if (skipped_tests > 0) {
        LOG_INFO_FMT("Skipped: %d ⏭️  (unsupported formats)", skipped_tests);
    }
    int executed_tests = total_tests - skipped_tests;
    LOG_INFO_FMT("Success rate: %.1f%% (%d/%d executed)", 
                executed_tests > 0 ? (passed_tests * 100.0 / executed_tests) : 0.0,
                passed_tests, executed_tests);
    LOG_INFO("");
    
    return failed_tests == 0;
}

int main(int argc, char* argv[]) {
    // 初始化日志系统
    INIT_LOGGER();
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 解析命令行参数
    TestConfig config;
    if (!parse_arguments(argc, argv, config, argv[0])) {
        return 1;
    }
    
    // 检查视频文件
    struct stat st;
    if (stat(config.video_file.c_str(), &st) != 0) {
        fprintf(stderr, "Error: Video file not found: %s\n", config.video_file.c_str());
        return 1;
    }
    
    // 运行测试
    bool success = false;
    if (config.auto_test) {
        // 自动执行所有测试用例
        success = run_auto_tests(config);
    } else {
        // 执行单个测试
        success = run_pp_test(config);
    }
    
    return success ? 0 : 1;
}

