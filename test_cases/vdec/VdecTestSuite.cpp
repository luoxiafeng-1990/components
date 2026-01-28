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
log4cplus::Logger& VdecTestSuite::getLogger() {
    static log4cplus::Logger logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.test.VdecSuite"));
    return logger;
}

// ========================================
// 辅助函数：从 WorkerConfig 构建消费标志
// ========================================
uint32_t VdecTestSuite::buildConsumeFlags(const WorkerConfig& config) {
    uint32_t flags = consumer::CONSUME_COUNT;  // 默认计数
    
    if (config.consumer.enable_display) {
        flags |= consumer::CONSUME_DISPLAY;
    }
    if (config.consumer.save_frames != 0) {
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
        {"sw_h264_1920x1080_30",  {"software", 1920, 1080, 30.0, ""}},
        {"sw_h265_1920x1080_30",  {"software", 1920, 1080, 30.0, ""}},
        
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
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - 解码 + PP 组合测试的便捷入口
        // 注：完整的 PP 参数配置请使用 qa_cases pp 命令
        // ════════════════════════════════════════════════════════════════════
        // H264 解码 + PP 基础组合
        {"h264_pp0",            {"h264", 1920, 1080, 30.0, "pp0"}},
        {"h264_pp1",            {"h264", 1920, 1080, 30.0, "pp1"}},
        {"h264_multi_pp",       {"h264", 1920, 1080, 30.0, "multi_pp"}},
        {"h264_720p_pp0",       {"h264", 1280, 720, 30.0, "pp0"}},
        {"h264_720p_pp1",       {"h264", 1280, 720, 30.0, "pp1"}},
        {"h264_4k_pp0",         {"h264", 3840, 2160, 30.0, "pp0"}},
        {"h264_4k_pp1",         {"h264", 3840, 2160, 30.0, "pp1"}},
        // H265 解码 + PP 基础组合
        {"h265_pp0",            {"h265", 1920, 1080, 30.0, "pp0"}},
        {"h265_pp1",            {"h265", 1920, 1080, 30.0, "pp1"}},
        {"h265_multi_pp",       {"h265", 1920, 1080, 30.0, "multi_pp"}},
        {"h265_720p_pp0",       {"h265", 1280, 720, 30.0, "pp0"}},
        {"h265_720p_pp1",       {"h265", 1280, 720, 30.0, "pp1"}},
        {"h265_4k_pp0",         {"h265", 3840, 2160, 30.0, "pp0"}},
        {"h265_4k_pp1",         {"h265", 3840, 2160, 30.0, "pp1"}},
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
    std::ostringstream test_name;
    test_name << params.codec << " " << params.width << "x" << params.height 
              << " " << static_cast<int>(params.fps) << "fps";
    
    // ========================================
    // 根据 profile 字段判断执行模式
    // ========================================
    
    // COMPARE 模式：PSNR/SSIM 质量验证
    if (config.consumer.enable_psnr || config.consumer.enable_ssim) {
        // 创建 HW + SW 配置
        auto hw_config = common::WorkerConfigFactory::createDecode(
            config.data_source.path, params.codec, params.width, params.height);
        hw_config.consumer = config.consumer;
        hw_config.consumer.target_fps = params.fps;
        
        auto sw_config = common::WorkerConfigFactory::createSoftwareDecode(
            config.data_source.path, params.width, params.height);
        sw_config.consumer.target_fps = params.fps;
        
        auto result = runCompare({hw_config, sw_config}, test_name.str() + " (COMPARE)");
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
            auto cfg = common::WorkerConfigFactory::createDecode(
                config.data_source.path, params.codec, params.width, params.height);
            cfg.consumer = config.consumer;
            cfg.consumer.target_fps = params.fps;
            configs.push_back(cfg);
        }
        
        auto result = runParallel(configs, consumer::CONSUME_COUNT, 
                                   test_name.str() + " (PARALLEL x" + std::to_string(thread_count) + ")");
        consumer::BufferConsumerService::printResult(test_name.str(), result);
        return result.success ? 0 : 1;
    }
    
    // SINGLE 模式：默认单路消费
    auto full_config = common::WorkerConfigFactory::createDecode(
        config.data_source.path, params.codec, params.width, params.height);
    full_config.consumer = config.consumer;
    full_config.consumer.target_fps = params.fps;
    
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
        {"reference",  required_argument, 0, 'e'},
        {"verbose",    no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };
    
    std::string input_path;
    
    int opt;
    while ((opt = getopt_long(argc, argv, "hlf:r:c:W:H:R:F:m:s:o:dpSP:M:e:v", 
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
                config.consumer.max_frames = std::stoi(optarg);
                break;
            
            case 's':
                config.consumer.save_frames = std::stoi(optarg);
                break;
            
            case 'o':
                config.consumer.output_path = optarg;
                break;
            
            case 'd':
                config.consumer.enable_display = true;
                break;
            
            case 'p':
                config.consumer.enable_psnr = true;
                break;
            
            case 'S':
                config.consumer.enable_ssim = true;
                break;
            
            case 'P':
                config.consumer.min_psnr = std::stod(optarg);
                break;
            
            case 'M':
                config.consumer.min_ssim = std::stod(optarg);
                break;
            
            case 'e':
                config.consumer.reference_path = optarg;
                break;
            
            case 'v':
                config.consumer.verbose = true;
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
            // 下一个参数作为路径
            if (i + 1 < argc) {
                input_path = argv[++i];
            }
            continue;
        }
        
        // 否则作为输入路径
        if (input_path.empty()) {
            input_path = arg;
        }
    }
    
    if (input_path.empty()) {
        LOG4CPLUS_ERROR(getLogger(), "No input file or RTSP URL specified");
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
    config.consumer.target_fps = params.fps;
    
    auto result = runSingle(config, consumer::CONSUME_COUNT, test_name);
    consumer::BufferConsumerService::printResult(test_name, result);
    return result.success ? 0 : 1;
}

void VdecTestSuite::printHelp() const {
    auto& logger = getLogger();
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "VDEC Module - 视频解码测试");
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "Usage:");
    LOG4CPLUS_INFO(logger, "  qa_cases vdec [options] <video_path>");
    LOG4CPLUS_INFO(logger, "  qa_cases vdec [options] <test_name> <video_path>");
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "Options:");
    LOG4CPLUS_INFO(logger, "  -h, --help              显示帮助信息");
    LOG4CPLUS_INFO(logger, "  -l, --list              列出所有预定义测试");
    LOG4CPLUS_INFO(logger, "  -f, --file <path>       视频文件路径");
    LOG4CPLUS_INFO(logger, "  -r, --rtsp <url>        RTSP URL");
    LOG4CPLUS_INFO(logger, "  -c, --codec <name>      编解码器 (h264|h265|mjpeg|software)");
    LOG4CPLUS_INFO(logger, "  -W, --width <n>         分辨率宽度");
    LOG4CPLUS_INFO(logger, "  -H, --height <n>        分辨率高度");
    LOG4CPLUS_INFO(logger, "  -R, --resolution <WxH>  分辨率 (如 1920x1080)");
    LOG4CPLUS_INFO(logger, "  -F, --fps <n>           目标帧率");
    LOG4CPLUS_INFO(logger, "  -m, --max-frames <n>    最大帧数 (-1=无限制)");
    LOG4CPLUS_INFO(logger, "  -s, --save <n>          保存帧数 (0=不保存, -1=全部)");
    LOG4CPLUS_INFO(logger, "  -o, --output <path>     输出文件路径");
    LOG4CPLUS_INFO(logger, "  -d, --display           启用显示输出 (CONSUME_DISPLAY)");
    LOG4CPLUS_INFO(logger, "  -p, --psnr              启用 PSNR 验证 (ExecuteMode::COMPARE)");
    LOG4CPLUS_INFO(logger, "  -S, --ssim              启用 SSIM 验证 (ExecuteMode::COMPARE)");
    LOG4CPLUS_INFO(logger, "  -P, --min-psnr <n>      PSNR 阈值 (默认: 30.0 dB)");
    LOG4CPLUS_INFO(logger, "  -M, --min-ssim <n>      SSIM 阈值 (默认: 0.95)");
    LOG4CPLUS_INFO(logger, "  -e, --reference <path>  参考文件路径");
    LOG4CPLUS_INFO(logger, "  -v, --verbose           详细日志");
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "ExecuteMode Mapping:");
    LOG4CPLUS_INFO(logger, "  SINGLE   - 默认单路解码");
    LOG4CPLUS_INFO(logger, "  COMPARE  - --psnr/--ssim 启用时，HW vs SW 对比");
    LOG4CPLUS_INFO(logger, "  PARALLEL - multi_worker/multithread_N 测试");
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "Examples:");
    LOG4CPLUS_INFO(logger, "  qa_cases vdec video.mp4                           # SINGLE");
    LOG4CPLUS_INFO(logger, "  qa_cases vdec --display video.mp4                 # SINGLE + DISPLAY");
    LOG4CPLUS_INFO(logger, "  qa_cases vdec --psnr video.mp4                    # COMPARE (HW vs SW)");
    LOG4CPLUS_INFO(logger, "  qa_cases vdec multithread_4 video.mp4             # PARALLEL x4");
    LOG4CPLUS_INFO(logger, "");
}

void VdecTestSuite::listTests() const {
    auto& logger = getLogger();
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "Available VDEC tests:");
    LOG4CPLUS_INFO(logger, "────────────────────────────────────────────────────────");
    
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "H.264 Tests (9) - ExecuteMode::SINGLE:");
    LOG4CPLUS_INFO(logger, "  h264_128x128_30         H.264 128x128 30fps main");
    LOG4CPLUS_INFO(logger, "  h264_320x240_30         H.264 320x240 30fps high");
    LOG4CPLUS_INFO(logger, "  h264_640x480_30         H.264 640x480 30fps main");
    LOG4CPLUS_INFO(logger, "  h264_640x480_60         H.264 640x480 60fps high");
    LOG4CPLUS_INFO(logger, "  h264_1280x720_30        H.264 720p 30fps high");
    LOG4CPLUS_INFO(logger, "  h264_1920x1080_30       H.264 1080p 30fps high");
    LOG4CPLUS_INFO(logger, "  h264_1920x1080_60       H.264 1080p 60fps high");
    LOG4CPLUS_INFO(logger, "  h264_2560x1440_30       H.264 1440p 30fps high");
    LOG4CPLUS_INFO(logger, "  h264_3840x2160_30       H.264 4K 30fps high");
    
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "H.265/HEVC Tests (9) - ExecuteMode::SINGLE:");
    LOG4CPLUS_INFO(logger, "  h265_128x128_30         H.265 128x128 30fps");
    LOG4CPLUS_INFO(logger, "  h265_320x240_30         H.265 320x240 30fps");
    LOG4CPLUS_INFO(logger, "  h265_640x480_30         H.265 640x480 30fps");
    LOG4CPLUS_INFO(logger, "  h265_640x480_60         H.265 640x480 60fps");
    LOG4CPLUS_INFO(logger, "  h265_1280x720_30        H.265 720p 30fps");
    LOG4CPLUS_INFO(logger, "  h265_1920x1080_30       H.265 1080p 30fps");
    LOG4CPLUS_INFO(logger, "  h265_1920x1080_60       H.265 1080p 60fps");
    LOG4CPLUS_INFO(logger, "  h265_2560x1440_30       H.265 1440p 30fps");
    LOG4CPLUS_INFO(logger, "  h265_3840x2160_30       H.265 4K 30fps");
    
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "MJPEG Tests (9) - ExecuteMode::SINGLE:");
    LOG4CPLUS_INFO(logger, "  mjpeg_128x128_30        MJPEG 128x128 30fps");
    LOG4CPLUS_INFO(logger, "  mjpeg_320x240_30        MJPEG 320x240 30fps");
    LOG4CPLUS_INFO(logger, "  mjpeg_640x480_30        MJPEG 640x480 30fps");
    LOG4CPLUS_INFO(logger, "  mjpeg_640x480_60        MJPEG 640x480 60fps");
    LOG4CPLUS_INFO(logger, "  mjpeg_1280x720_30       MJPEG 720p 30fps");
    LOG4CPLUS_INFO(logger, "  mjpeg_1920x1080_30      MJPEG 1080p 30fps");
    LOG4CPLUS_INFO(logger, "  mjpeg_1920x1080_60      MJPEG 1080p 60fps");
    LOG4CPLUS_INFO(logger, "  mjpeg_2560x1440_30      MJPEG 1440p 30fps");
    LOG4CPLUS_INFO(logger, "  mjpeg_3840x2160_30      MJPEG 4K 30fps");
    
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "Software Decode Tests (2) - ExecuteMode::SINGLE:");
    LOG4CPLUS_INFO(logger, "  sw_h264_1920x1080_30    Software H.264 1080p 30fps");
    LOG4CPLUS_INFO(logger, "  sw_h265_1920x1080_30    Software H.265 1080p 30fps");
    
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "RTSP H.264 Tests (6) - ExecuteMode::SINGLE:");
    LOG4CPLUS_INFO(logger, "  rtsp_h264_1280x720_30_cbr   RTSP H.264 720p CBR");
    LOG4CPLUS_INFO(logger, "  rtsp_h264_1280x720_30_vbr   RTSP H.264 720p VBR");
    LOG4CPLUS_INFO(logger, "  rtsp_h264_1920x1080_30_cbr  RTSP H.264 1080p CBR");
    LOG4CPLUS_INFO(logger, "  rtsp_h264_1920x1080_30_vbr  RTSP H.264 1080p VBR");
    LOG4CPLUS_INFO(logger, "  rtsp_h264_3840x2160_30_cbr  RTSP H.264 4K CBR");
    LOG4CPLUS_INFO(logger, "  rtsp_h264_3840x2160_30_vbr  RTSP H.264 4K VBR");
    
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "RTSP H.265 Tests (6) - ExecuteMode::SINGLE:");
    LOG4CPLUS_INFO(logger, "  rtsp_h265_1280x720_30_cbr   RTSP H.265 720p CBR");
    LOG4CPLUS_INFO(logger, "  rtsp_h265_1280x720_30_vbr   RTSP H.265 720p VBR");
    LOG4CPLUS_INFO(logger, "  rtsp_h265_1920x1080_30_cbr  RTSP H.265 1080p CBR");
    LOG4CPLUS_INFO(logger, "  rtsp_h265_1920x1080_30_vbr  RTSP H.265 1080p VBR");
    LOG4CPLUS_INFO(logger, "  rtsp_h265_3840x2160_30_cbr  RTSP H.265 4K CBR");
    LOG4CPLUS_INFO(logger, "  rtsp_h265_3840x2160_30_vbr  RTSP H.265 4K VBR");
    
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "RTSP MJPEG Tests (1) - ExecuteMode::SINGLE:");
    LOG4CPLUS_INFO(logger, "  rtsp_mjpeg_32768x18432_30   RTSP MJPEG Ultra-High");
    
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "Multi-Worker Tests (2) - ExecuteMode::PARALLEL:");
    LOG4CPLUS_INFO(logger, "  multi_worker               HW+SW concurrent decode");
    LOG4CPLUS_INFO(logger, "  multi_worker_4k            HW+SW concurrent 4K decode");
    
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, "Multi-Thread Tests (3) - ExecuteMode::PARALLEL:");
    LOG4CPLUS_INFO(logger, "  multithread_2              2-thread decode");
    LOG4CPLUS_INFO(logger, "  multithread_4              4-thread decode");
    LOG4CPLUS_INFO(logger, "  multithread_8              8-thread decode");
    
    LOG4CPLUS_INFO(logger, "────────────────────────────────────────────────────────");
    LOG4CPLUS_INFO(logger, "Total: 47 predefined tests");
    LOG4CPLUS_INFO(logger, "");
}

// ========================================
// 核心测试方法实现（与 ExecuteMode 对齐）
// ========================================

TestResult VdecTestSuite::runSingle(
    const WorkerConfig& config,
    uint32_t flags,
    const std::string& test_name
) {
    auto& logger = getLogger();
    
    if (!test_name.empty()) {
        consumer::BufferConsumerService::printHeader(test_name, config);
    }
    
    LOG4CPLUS_DEBUG_FMT(logger, "runSingle: mode=SINGLE, flags=0x%X", flags);
    
    consumer::BufferConsumerService service;
    return service.start({config}, consumer::ExecuteMode::SINGLE, flags);
}

TestResult VdecTestSuite::runCompare(
    const std::vector<WorkerConfig>& configs,
    const std::string& test_name
) {
    auto& logger = getLogger();
    
    if (!test_name.empty()) {
        LOG4CPLUS_INFO(logger, "");
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
        LOG4CPLUS_INFO_FMT(logger, "  %s", test_name.c_str());
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
        LOG4CPLUS_INFO_FMT(logger, "  Mode:       ExecuteMode::COMPARE");
        LOG4CPLUS_INFO_FMT(logger, "  Workers:    %zu", configs.size());
        if (!configs.empty()) {
            LOG4CPLUS_INFO_FMT(logger, "  Input:      %s", configs[0].data_source.path.c_str());
            if (configs[0].consumer.enable_psnr) {
                LOG4CPLUS_INFO_FMT(logger, "  PSNR:       enabled (min: %.1f dB)", configs[0].consumer.min_psnr);
            }
            if (configs[0].consumer.enable_ssim) {
                LOG4CPLUS_INFO_FMT(logger, "  SSIM:       enabled (min: %.2f)", configs[0].consumer.min_ssim);
            }
        }
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
    }
    
    LOG4CPLUS_DEBUG_FMT(logger, "runCompare: mode=COMPARE, workers=%zu", configs.size());
    
    consumer::BufferConsumerService service;
    return service.start(configs, consumer::ExecuteMode::COMPARE, 0);
}

TestResult VdecTestSuite::runParallel(
    const std::vector<WorkerConfig>& configs,
    uint32_t flags,
    const std::string& test_name
) {
    auto& logger = getLogger();
    
    if (!test_name.empty()) {
        LOG4CPLUS_INFO(logger, "");
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
        LOG4CPLUS_INFO_FMT(logger, "  %s", test_name.c_str());
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
        LOG4CPLUS_INFO_FMT(logger, "  Mode:       ExecuteMode::PARALLEL");
        LOG4CPLUS_INFO_FMT(logger, "  Workers:    %zu", configs.size());
        LOG4CPLUS_INFO_FMT(logger, "  Flags:      0x%X", flags);
        if (!configs.empty()) {
            LOG4CPLUS_INFO_FMT(logger, "  Input:      %s", configs[0].data_source.path.c_str());
        }
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
    }
    
    LOG4CPLUS_DEBUG_FMT(logger, "runParallel: mode=PARALLEL, workers=%zu, flags=0x%X", 
                        configs.size(), flags);
    
    consumer::BufferConsumerService service;
    return service.start(configs, consumer::ExecuteMode::PARALLEL, flags);
}

} // namespace vdec
} // namespace test
