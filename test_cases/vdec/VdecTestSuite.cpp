/**
 * @file VdecTestSuite.cpp
 * @brief VdecTestSuite 实现
 * 
 * 重构为 ExecuteMode 风格，与 BufferConsumerService 架构对齐
 */

#include "VdecTestSuite.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "productionline/io/BufferConsumerService.hpp"

#include <iostream>
#include <getopt.h>
#include <cstring>
#include <sstream>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace vdec {

// 获取模块级日志实例

// ========================================
// 辅助函数：从 WorkerConfig 构建消费标志
// ========================================
uint32_t VdecTestSuite::buildConsumeFlags(const WorkerConfig& config) {
    uint32_t flags = consumer::CONSUME_COUNT;  // 默认计数
    
    if (config.consumer_type.display.enable) {
        flags |= consumer::CONSUME_DISPLAY;
    }
    if (config.consumer_type.save_raw.enable) {
        flags |= consumer::CONSUME_SAVE_RAW;
    }
    
    return flags;
}

// ========================================
// 预定义测试参数表
// ========================================

const std::map<std::string, DecodeTestParams>& VdecTestSuite::getPredefinedTests() {
    static std::map<std::string, DecodeTestParams> tests = {
        // ========================================
        // H.264 测试（9 个分辨率/帧率组合）
        // ========================================
        {"h264_128x128_30",       {"h264", 128, 128, 30.0, "main"}},
        {"h264_320x240_30",       {"h264", 320, 240, 30.0, "high"}},
        {"h264_640x480_30",       {"h264", 640, 480, 30.0, "main"}},
        {"h264_640x480_60",       {"h264", 640, 480, 60.0, "high"}},
        {"h264_1280x720_30",      {"h264", 1280, 720, 30.0, "high"}},
        {"h264_1920x1080_30",     {"h264", 1920, 1080, 30.0, "high"}},
        {"h264_1920x1080_60",     {"h264", 1920, 1080, 60.0, "high"}},
        {"h264_2560x1440_30",     {"h264", 2560, 1440, 30.0, "high"}},
        {"h264_3840x2160_30",     {"h264", 3840, 2160, 30.0, "high"}},
        
        // ========================================
        // H.265/HEVC 测试（9 个分辨率/帧率组合）
        // ========================================
        {"h265_128x128_30",       {"h265", 128, 128, 30.0, "main"}},
        {"h265_320x240_30",       {"h265", 320, 240, 30.0, "main"}},
        {"h265_640x480_30",       {"h265", 640, 480, 30.0, "main"}},
        {"h265_640x480_60",       {"h265", 640, 480, 60.0, "main"}},
        {"h265_1280x720_30",      {"h265", 1280, 720, 30.0, "main"}},
        {"h265_1920x1080_30",     {"h265", 1920, 1080, 30.0, "main"}},
        {"h265_1920x1080_60",     {"h265", 1920, 1080, 60.0, "main"}},
        {"h265_2560x1440_30",     {"h265", 2560, 1440, 30.0, "main"}},
        {"h265_3840x2160_30",     {"h265", 3840, 2160, 30.0, "main"}},
        
        // ========================================
        // MJPEG 测试（9 个分辨率/帧率组合）
        // ========================================
        {"mjpeg_128x128_30",      {"mjpeg", 128, 128, 30.0, ""}},
        {"mjpeg_320x240_30",      {"mjpeg", 320, 240, 30.0, ""}},
        {"mjpeg_640x480_30",      {"mjpeg", 640, 480, 30.0, ""}},
        {"mjpeg_640x480_60",      {"mjpeg", 640, 480, 60.0, ""}},
        {"mjpeg_1280x720_30",     {"mjpeg", 1280, 720, 30.0, ""}},
        {"mjpeg_1920x1080_30",    {"mjpeg", 1920, 1080, 30.0, ""}},
        {"mjpeg_1920x1080_60",    {"mjpeg", 1920, 1080, 60.0, ""}},
        {"mjpeg_2560x1440_30",    {"mjpeg", 2560, 1440, 30.0, ""}},
        {"mjpeg_3840x2160_30",    {"mjpeg", 3840, 2160, 30.0, ""}},
        
        // ========================================
        // 软件解码测试
        // ========================================
        {"sw_h264_1920x1080_30",  {"h264", 1920, 1080, 30.0, "", false}},
        {"sw_h265_1920x1080_30",  {"h265", 1920, 1080, 30.0, "", false}},
        
        // ========================================
        // RTSP H.264 测试（CBR/VBR）
        // ========================================
        {"rtsp_h264_1280x720_30_cbr",  {"h264", 1280, 720, 30.0, "cbr"}},
        {"rtsp_h264_1280x720_30_vbr",  {"h264", 1280, 720, 30.0, "vbr"}},
        {"rtsp_h264_1920x1080_30_cbr", {"h264", 1920, 1080, 30.0, "cbr"}},
        {"rtsp_h264_1920x1080_30_vbr", {"h264", 1920, 1080, 30.0, "vbr"}},
        {"rtsp_h264_3840x2160_30_cbr", {"h264", 3840, 2160, 30.0, "cbr"}},
        {"rtsp_h264_3840x2160_30_vbr", {"h264", 3840, 2160, 30.0, "vbr"}},
        
        // ========================================
        // RTSP H.265 测试（CBR/VBR）
        // ========================================
        {"rtsp_h265_1280x720_30_cbr",  {"h265", 1280, 720, 30.0, "cbr"}},
        {"rtsp_h265_1280x720_30_vbr",  {"h265", 1280, 720, 30.0, "vbr"}},
        {"rtsp_h265_1920x1080_30_cbr", {"h265", 1920, 1080, 30.0, "cbr"}},
        {"rtsp_h265_1920x1080_30_vbr", {"h265", 1920, 1080, 30.0, "vbr"}},
        {"rtsp_h265_3840x2160_30_cbr", {"h265", 3840, 2160, 30.0, "cbr"}},
        {"rtsp_h265_3840x2160_30_vbr", {"h265", 3840, 2160, 30.0, "vbr"}},
        
        // ========================================
        // RTSP MJPEG 测试
        // ========================================
        {"rtsp_mjpeg_32768x18432_30", {"mjpeg", 32768, 18432, 30.0, ""}},
        
        // ========================================
        // 多 Worker 测试（对应原始 multi_worker）- PARALLEL 模式
        // ========================================
        {"multi_worker",        {"h264", 1920, 1080, 30.0, "parallel"}},
        {"multi_worker_4k",     {"h264", 3840, 2160, 30.0, "parallel"}},
        
        // ========================================
        // 多线程解码测试（对应原始 ffmpeg_multithread）- PARALLEL 模式
        // ========================================
        {"multithread_2",       {"h264", 1920, 1080, 30.0, "parallel_2"}},
        {"multithread_4",       {"h264", 1920, 1080, 30.0, "parallel_4"}},
        {"multithread_8",       {"h264", 1920, 1080, 30.0, "parallel_8"}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - MP4 解码基础测试（配合 PP 使用）
        // ════════════════════════════════════════════════════════════════════
        // H264 MP4 解码
        {"mp4_h264",            {"h264", 1920, 1080, 30.0, "high"}},
        {"mp4_h264_720p",       {"h264", 1280, 720, 30.0, "high"}},
        {"mp4_h264_1080p",      {"h264", 1920, 1080, 30.0, "high"}},
        {"mp4_h264_4k",         {"h264", 3840, 2160, 30.0, "high"}},
        // H265 MP4 解码
        {"mp4_h265",            {"h265", 1920, 1080, 30.0, "main"}},
        {"mp4_h265_720p",       {"h265", 1280, 720, 30.0, "main"}},
        {"mp4_h265_1080p",      {"h265", 1920, 1080, 30.0, "main"}},
        {"mp4_h265_4k",         {"h265", 3840, 2160, 30.0, "main"}},
        // MJPEG MP4 解码
        {"mp4_mjpeg",           {"mjpeg", 1920, 1080, 30.0, ""}},
        {"mp4_mjpeg_720p",      {"mjpeg", 1280, 720, 30.0, ""}},
        {"mp4_mjpeg_1080p",     {"mjpeg", 1920, 1080, 30.0, ""}},
        {"mp4_mjpeg_4k",        {"mjpeg", 3840, 2160, 30.0, ""}},
    };
    return tests;
}

std::vector<std::string> VdecTestSuite::getTestNames() const {
    std::vector<std::string> names;
    for (const auto& pair : getPredefinedTests()) {
        names.push_back(pair.first);
    }
    return names;
}

// ========================================
// ITestModule 接口实现
// ========================================

int VdecTestSuite::run(int argc, char* argv[]) {
    WorkerConfig config;
    DecodeTestParams params;
    
    if (!parseArgs(argc, argv, config, params)) {
        return 1;
    }
    
    // 生成测试名称
    // 方案 A：使用预定义测试名称（如果匹配）
    // 方案 B：使用文件路径 + 实际配置（如果未匹配预定义测试）
    std::ostringstream test_name;
    if (params.isPredefined()) {
        // 使用预定义测试名称 + 参数信息
        test_name << params.predefined_name << " (" 
                  << params.codec << " " << params.width << "x" << params.height 
                  << " " << static_cast<int>(params.fps) << "fps)";
    } else {
        // 未匹配预定义测试，使用文件名 + 实际配置
        // 提取文件名（去掉路径）
        std::string filename = config.data_source.path;
        size_t pos = filename.find_last_of("/\\");
        if (pos != std::string::npos) {
            filename = filename.substr(pos + 1);
        }
        test_name << "Custom: " << filename << " (" 
                  << params.codec << " " << params.width << "x" << params.height 
                  << " " << static_cast<int>(params.fps) << "fps)";
    }
    
    // ========================================
    // 根据 profile 字段判断执行模式
    // ========================================
    
    // COMPARE 模式：PSNR/SSIM 质量验证
    if (config.consumer_type.compare.enable_psnr || config.consumer_type.compare.enable_ssim) {
        // 创建 HW + SW 配置
        auto hw_config = common::WorkerConfigFactory::createDecode(
            config.data_source.path, params.codec, params.width, params.height);
        hw_config.consumer_type = config.consumer_type;
        hw_config.consumer_type.performance.target_fps = params.fps;
        
        auto sw_config = common::WorkerConfigFactory::createSoftwareDecode(
            config.data_source.path, params.width, params.height);
        sw_config.consumer_type.performance.target_fps = params.fps;
        
        // COMPARE 模式也支持叠加其他消费类型（display、save）
        uint32_t compare_flags = 0;
        if (config.consumer_type.display.enable) {
            compare_flags |= consumer::CONSUME_DISPLAY;
        }
        if (config.consumer_type.save_raw.enable) {
            compare_flags |= consumer::CONSUME_SAVE_RAW;
        }
        
        auto result = runCompare({hw_config, sw_config}, compare_flags, test_name.str() + " (COMPARE)");
        consumer::BufferConsumerService::printResult(test_name.str(), result);
        return result.success ? 0 : 1;
    }
    
    // PARALLEL 模式：多 Worker 或多线程
    if (params.profile == "parallel" || params.profile.find("parallel_") == 0) {
        int thread_count = 2;  // 默认 2 个
        if (params.profile.find("parallel_") == 0) {
            thread_count = std::stoi(params.profile.substr(9));
        }
        
        std::vector<WorkerConfig> configs;
        for (int i = 0; i < thread_count; i++) {
            WorkerConfig cfg;
            if (params.use_hardware) {
                cfg = common::WorkerConfigFactory::createDecode(
                    config.data_source.path, params.codec, params.width, params.height);
            } else {
                cfg = common::WorkerConfigFactory::createSoftwareDecode(
                    config.data_source.path, params.width, params.height);
            }
            cfg.consumer_type = config.consumer_type;
            cfg.consumer_type.performance.target_fps = params.fps;
            configs.push_back(cfg);
        }
        
        uint32_t parallel_flags = buildConsumeFlags(config);
        auto result = runParallel(configs, parallel_flags, 
                                   test_name.str() + " (PARALLEL x" + std::to_string(thread_count) + ")");
        consumer::BufferConsumerService::printResult(test_name.str(), result);
        return result.success ? 0 : 1;
    }
    
    // SINGLE 模式：默认单路消费
    WorkerConfig full_config;
    if (params.use_hardware) {
        full_config = common::WorkerConfigFactory::createDecode(
            config.data_source.path, params.codec, params.width, params.height);
    } else {
        full_config = common::WorkerConfigFactory::createSoftwareDecode(
            config.data_source.path, params.width, params.height);
    }
    full_config.consumer_type = config.consumer_type;
    full_config.consumer_type.performance.target_fps = params.fps;
    
    uint32_t flags = buildConsumeFlags(full_config);
    auto result = runSingle(full_config, flags, test_name.str());
    consumer::BufferConsumerService::printResult(test_name.str(), result);
    
    return result.success ? 0 : 1;
}

bool VdecTestSuite::parseArgs(int argc, char* argv[], WorkerConfig& config, DecodeTestParams& params) {
    optind = 1;
    
    static struct option long_options[] = {
        {"help",       no_argument,       0, 'h'},
        {"list",       no_argument,       0, 'l'},
        {"file",       required_argument, 0, 'f'},
        {"rtsp",       required_argument, 0, 'r'},
        {"codec",      required_argument, 0, 'c'},
        {"decoder",    required_argument, 0, 'D'},
        {"width",      required_argument, 0, 'W'},
        {"height",     required_argument, 0, 'H'},
        {"resolution", required_argument, 0, 'R'},
        {"fps",        required_argument, 0, 'F'},
        {"max-frames", required_argument, 0, 'm'},
        {"save",       required_argument, 0, 's'},
        {"output",     required_argument, 0, 'o'},
        {"display",    no_argument,       0, 'd'},
        {"psnr",       no_argument,       0, 'p'},
        {"ssim",       no_argument,       0, 'S'},
        {"min-psnr",   required_argument, 0, 'P'},
        {"min-ssim",   required_argument, 0, 'M'},
        {"verbose",    no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };
    
    std::string input_path;
    
    int opt;
    while ((opt = getopt_long(argc, argv, "hlf:r:c:D:W:H:R:F:m:s:o:dpSP:M:v", 
                              long_options, nullptr)) != -1) {
        switch (opt) {
            case 'h':
                printHelp();
                return false;
            
            case 'l':
                listTests();
                return false;
            
            case 'f':
            case 'r':
                input_path = optarg;
                break;
            
            case 'c':
                params.codec = optarg;
                break;
            
            case 'D': {
                std::string decoder_type = optarg;
                if (decoder_type == "hw" || decoder_type == "hardware") {
                    params.use_hardware = true;
                } else if (decoder_type == "sw" || decoder_type == "software") {
                    params.use_hardware = false;
                } else {
                    LOG4CPLUS_ERROR_FMT(getLogger(), 
                        "Invalid decoder type '%s', use 'hw' or 'sw'", optarg);
                    return false;
                }
                break;
            }
            
            case 'W':
                params.width = std::stoi(optarg);
                break;
            
            case 'H':
                params.height = std::stoi(optarg);
                break;
            
            case 'R': {
                // 解析 WxH 格式
                std::string res = optarg;
                size_t pos = res.find('x');
                if (pos != std::string::npos) {
                    params.width = std::stoi(res.substr(0, pos));
                    params.height = std::stoi(res.substr(pos + 1));
                }
                break;
            }
            
            case 'F':
                params.fps = std::stod(optarg);
                break;
            
            case 'm':
                config.consumer_type.max_frames = std::stoi(optarg);
                break;
            
            case 's':
                config.consumer_type.save_raw.max_frames_per_channel = {std::stoi(optarg)};  // 只设置帧数
                break;
            
            case 'o':
                config.consumer_type.save_raw.enable = true;  // 指定路径即启用保存
                config.consumer_type.save_raw.setOutputPath(optarg);
                break;
            
            case 'd':
                config.consumer_type.display.enable = true;
                break;
            
            case 'p':
                config.consumer_type.compare.enable_psnr = true;
                break;
            
            case 'S':
                config.consumer_type.compare.enable_ssim = true;
                break;
            
            case 'P':
                config.consumer_type.compare.min_psnr = std::stod(optarg);
                break;
            
            case 'M':
                config.consumer_type.compare.min_ssim = std::stod(optarg);
                break;
            
            case 'v':
                config.consumer_type.verbose = true;
                break;
            
            default:
                printHelp();
                return false;
        }
    }
    
    // 处理剩余参数
    for (int i = optind; i < argc; i++) {
        std::string arg = argv[i];
        
        // 检查是否是预定义测试名称
        const auto& tests = getPredefinedTests();
        auto it = tests.find(arg);
        if (it != tests.end()) {
            params = it->second;
            params.predefined_name = arg;  // 记录匹配的预定义测试名称
            // 下一个参数作为路径
            if (i + 1 < argc) {
                input_path = argv[++i];
            }
            continue;
        }
        
        // 否则作为输入路径
        if (input_path.empty()) {
            input_path = arg;
        } else {
            LOG4CPLUS_WARN_FMT(getLogger(), 
                "Extra positional argument ignored: '%s' (input already set to: '%s')",
                arg.c_str(), input_path.c_str());
        }
    }
    
    if (input_path.empty()) {
        std::cerr << "Error: No input file or RTSP URL specified\n" << std::endl;
        printHelp();
        return false;
    }
    
    config.data_source.path = input_path;
    
    return true;
}

int VdecTestSuite::runPredefinedTest(const std::string& test_name, const std::string& path) {
    const auto& tests = getPredefinedTests();
    auto it = tests.find(test_name);
    if (it == tests.end()) {
        LOG4CPLUS_ERROR_FMT(getLogger(), "Unknown test '%s'", test_name.c_str());
        return 1;
    }
    
    const auto& params = it->second;
    auto config = common::WorkerConfigFactory::createDecode(
        path, params.codec, params.width, params.height);
    config.consumer_type.performance.target_fps = params.fps;
    
    auto result = runSingle(config, consumer::CONSUME_COUNT, test_name);
    consumer::BufferConsumerService::printResult(test_name, result);
    return result.success ? 0 : 1;
}

void VdecTestSuite::printHelp() const {
    std::cout << "\n"
              << "VDEC Module - 视频解码测试\n"
              << "\n"
              << "Usage:\n"
              << "  qa_cases vdec [options] <video_path>\n"
              << "  qa_cases vdec [options] <test_name> <video_path>\n"
              << "\n"
              << "Options:\n"
              << "  -h, --help              显示帮助信息\n"
              << "  -l, --list              列出所有预定义测试\n"
              << "  -f, --file <path>       视频文件路径\n"
              << "  -r, --rtsp <url>        RTSP URL\n"
              << "  -c, --codec <name>      编解码格式 (h264|h265|mjpeg)\n"
              << "  -D, --decoder <type>    解码方式 (hw|hardware|sw|software，默认: hw)\n"
              << "  -W, --width <n>         分辨率宽度\n"
              << "  -H, --height <n>        分辨率高度\n"
              << "  -R, --resolution <WxH>  分辨率 (如 1920x1080)\n"
              << "  -F, --fps <n>           目标帧率\n"
              << "  -m, --max-frames <n>    最大帧数 (-1=无限制)\n"
              << "  -s, --save <n>          保存帧数 (0=不保存, -1=全部)\n"
              << "  -o, --output <path>     输出文件路径\n"
              << "  -d, --display           启用显示输出 (CONSUME_DISPLAY)\n"
              << "  -p, --psnr              启用 PSNR 验证 (ExecuteMode::COMPARE)\n"
              << "  -S, --ssim              启用 SSIM 验证 (ExecuteMode::COMPARE)\n"
              << "  -P, --min-psnr <n>      PSNR 阈值 (默认: 30.0 dB)\n"
              << "  -M, --min-ssim <n>      SSIM 阈值 (默认: 0.95)\n"
              << "  -v, --verbose           详细日志\n"
              << "\n"
              << "ExecuteMode Mapping:\n"
              << "  SINGLE   - 默认单路解码，支持 --decoder hw/sw\n"
              << "  COMPARE  - --psnr/--ssim 启用时，HW vs SW 对比\n"
              << "  PARALLEL - multi_worker/multithread_N 测试，支持 --decoder hw/sw\n"
              << "\n"
              << "Examples:\n"
              << "  qa_cases vdec video.mp4                           # SINGLE\n"
              << "  qa_cases vdec --display video.mp4                 # SINGLE + DISPLAY\n"
              << "  qa_cases vdec --psnr video.mp4                    # COMPARE (HW vs SW)\n"
              << "  qa_cases vdec multithread_4 video.mp4             # PARALLEL x4\n"
              << std::endl;
}

void VdecTestSuite::listTests() const {
    std::cout << "\n"
              << "Available VDEC tests:\n"
              << "────────────────────────────────────────────────────────\n"
              << "\n"
              << "H.264 Tests (9) - ExecuteMode::SINGLE:\n"
              << "  h264_128x128_30         H.264 128x128 30fps main\n"
              << "  h264_320x240_30         H.264 320x240 30fps high\n"
              << "  h264_640x480_30         H.264 640x480 30fps main\n"
              << "  h264_640x480_60         H.264 640x480 60fps high\n"
              << "  h264_1280x720_30        H.264 720p 30fps high\n"
              << "  h264_1920x1080_30       H.264 1080p 30fps high\n"
              << "  h264_1920x1080_60       H.264 1080p 60fps high\n"
              << "  h264_2560x1440_30       H.264 1440p 30fps high\n"
              << "  h264_3840x2160_30       H.264 4K 30fps high\n"
              << "\n"
              << "H.265/HEVC Tests (9) - ExecuteMode::SINGLE:\n"
              << "  h265_128x128_30         H.265 128x128 30fps\n"
              << "  h265_320x240_30         H.265 320x240 30fps\n"
              << "  h265_640x480_30         H.265 640x480 30fps\n"
              << "  h265_640x480_60         H.265 640x480 60fps\n"
              << "  h265_1280x720_30        H.265 720p 30fps\n"
              << "  h265_1920x1080_30       H.265 1080p 30fps\n"
              << "  h265_1920x1080_60       H.265 1080p 60fps\n"
              << "  h265_2560x1440_30       H.265 1440p 30fps\n"
              << "  h265_3840x2160_30       H.265 4K 30fps\n"
              << "\n"
              << "MJPEG Tests (9) - ExecuteMode::SINGLE:\n"
              << "  mjpeg_128x128_30        MJPEG 128x128 30fps\n"
              << "  mjpeg_320x240_30        MJPEG 320x240 30fps\n"
              << "  mjpeg_640x480_30        MJPEG 640x480 30fps\n"
              << "  mjpeg_640x480_60        MJPEG 640x480 60fps\n"
              << "  mjpeg_1280x720_30       MJPEG 720p 30fps\n"
              << "  mjpeg_1920x1080_30      MJPEG 1080p 30fps\n"
              << "  mjpeg_1920x1080_60      MJPEG 1080p 60fps\n"
              << "  mjpeg_2560x1440_30      MJPEG 1440p 30fps\n"
              << "  mjpeg_3840x2160_30      MJPEG 4K 30fps\n"
              << "\n"
              << "Software Decode Tests (2) - ExecuteMode::SINGLE:\n"
              << "  sw_h264_1920x1080_30    Software H.264 1080p 30fps\n"
              << "  sw_h265_1920x1080_30    Software H.265 1080p 30fps\n"
              << "\n"
              << "RTSP H.264 Tests (6) - ExecuteMode::SINGLE:\n"
              << "  rtsp_h264_1280x720_30_cbr   RTSP H.264 720p CBR\n"
              << "  rtsp_h264_1280x720_30_vbr   RTSP H.264 720p VBR\n"
              << "  rtsp_h264_1920x1080_30_cbr  RTSP H.264 1080p CBR\n"
              << "  rtsp_h264_1920x1080_30_vbr  RTSP H.264 1080p VBR\n"
              << "  rtsp_h264_3840x2160_30_cbr  RTSP H.264 4K CBR\n"
              << "  rtsp_h264_3840x2160_30_vbr  RTSP H.264 4K VBR\n"
              << "\n"
              << "RTSP H.265 Tests (6) - ExecuteMode::SINGLE:\n"
              << "  rtsp_h265_1280x720_30_cbr   RTSP H.265 720p CBR\n"
              << "  rtsp_h265_1280x720_30_vbr   RTSP H.265 720p VBR\n"
              << "  rtsp_h265_1920x1080_30_cbr  RTSP H.265 1080p CBR\n"
              << "  rtsp_h265_1920x1080_30_vbr  RTSP H.265 1080p VBR\n"
              << "  rtsp_h265_3840x2160_30_cbr  RTSP H.265 4K CBR\n"
              << "  rtsp_h265_3840x2160_30_vbr  RTSP H.265 4K VBR\n"
              << "\n"
              << "RTSP MJPEG Tests (1) - ExecuteMode::SINGLE:\n"
              << "  rtsp_mjpeg_32768x18432_30   RTSP MJPEG Ultra-High\n"
              << "\n"
              << "Multi-Worker Tests (2) - ExecuteMode::PARALLEL:\n"
              << "  multi_worker               HW+SW concurrent decode\n"
              << "  multi_worker_4k            HW+SW concurrent 4K decode\n"
              << "\n"
              << "Multi-Thread Tests (3) - ExecuteMode::PARALLEL:\n"
              << "  multithread_2              2-thread decode\n"
              << "  multithread_4              4-thread decode\n"
              << "  multithread_8              8-thread decode\n"
              << "\n"
              << "────────────────────────────────────────────────────────\n"
              << "Total: 47 predefined tests\n"
              << std::endl;
}


} // namespace vdec
} // namespace test
