#include "TaOpencvTestSuite.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "productionline/io/BufferConsumerService.hpp"

#include <iostream>
#include <getopt.h>
#include <cstring>
#include <sstream>
#include <log4cplus/loggingmacros.h>
#include <chrono>

#include "opencv2/opencv.hpp"
#include "opencv2/core/tacv.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/VideoProductionLine.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "buffer/bufferpool/Buffer.hpp"

namespace test {
namespace taopencv {

// 获取模块级日志实例

// ========================================
// 辅助函数：从 WorkerConfig 构建消费标志
// ========================================
uint32_t TaOpencvTestSuite::buildConsumeFlags(const WorkerConfig& config) {
    uint32_t flags = consumer::CONSUME_COUNT;  // 默认计数
    
    if (config.consumer_type.display.enable) {
        flags |= consumer::CONSUME_DISPLAY;
    }
    if (config.consumer_type.save_raw.enable) {
        flags |= consumer::CONSUME_SAVE_RAW;
    }
    
    return flags;
}

const std::map<std::string, TaOpencvTestParams>& TaOpencvTestSuite::getPredefinedTests() {
    static std::map<std::string, TaOpencvTestParams> tests = {
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
        {"h264_3840x2160_30",     {"h264", 3840, 2160, 30.0, "high"}}
    };
    return tests;
}

std::vector<std::string> TaOpencvTestSuite::getTestNames() const {
    std::vector<std::string> names;
    for (const auto& pair : getPredefinedTests()) {
        names.push_back(pair.first);
    }
    return names;
}

// ========================================
// ITestModule 接口实现
// ========================================

int TaOpencvTestSuite::run(int argc, char* argv[]) {
    WorkerConfig config;
    TaOpencvTestParams params;
    
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
    
    // if (config.consumer_type.compare.enable_psnr || config.consumer_type.compare.enable_ssim)
    auto hw_config = common::WorkerConfigFactory::createHardwareCv(
        config.data_source.path, params.codec, params.width, params.height);
    hw_config.consumer_type = config.consumer_type;
    hw_config.consumer_type.performance.target_fps = params.fps;
    hw_config.data_source.max_frames = config.data_source.max_frames;  // v2.23: 传递帧数限制
    
    auto sw_config = common::WorkerConfigFactory::createSoftwareCv(
        config.data_source.path, params.width, params.height);
    sw_config.consumer_type.performance.target_fps = params.fps;
    sw_config.data_source.max_frames = config.data_source.max_frames;  // v2.23: 传递帧数限制
    
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

bool TaOpencvTestSuite::parseArgs(int argc, char* argv[], WorkerConfig& config, TaOpencvTestParams& params) {
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
        {"display-mode", required_argument, 0, 'X'},
        {"display-fps",  required_argument, 0, 'Z'},
        {"osd",          no_argument,       0,  1001},
        {"osd-fps",      required_argument, 0,  1002},
        {"psnr",       no_argument,       0, 'p'},
        {"ssim",       no_argument,       0, 'S'},
        {"min-psnr",   required_argument, 0, 'P'},
        {"min-ssim",   required_argument, 0, 'M'},
        {"verbose",    no_argument,       0, 'v'},
        {"threads",    required_argument, 0, 't'},  // 并发路数（PARALLEL 模式）
        {0, 0, 0, 0}
    };
    
    std::string input_path;
    
    int opt;
    while ((opt = getopt_long(argc, argv, "hlf:r:c:D:W:H:R:F:m:s:o:dpSP:M:vt:X:Z:", 
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
                // v2.23：从数据源层面限制帧数，而不是消费层面
                config.data_source.max_frames = std::stoi(optarg);
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

            case 'X': {
                std::string mode_str = optarg;
                if (mode_str == "vo" || mode_str == "taco-vo") {
                    config.consumer_type.display.mode =
                        WorkerConfig::ConsumerTypeConfig::DisplayType::TACO_VO;
                } else if (mode_str == "shared_fb" || mode_str == "shared-fb") {
                    config.consumer_type.display.mode =
                        WorkerConfig::ConsumerTypeConfig::DisplayType::SHARED_FB;
                } else if (mode_str == "fb" || mode_str == "framebuffer") {
                    config.consumer_type.display.mode =
                        WorkerConfig::ConsumerTypeConfig::DisplayType::FRAMEBUFFER;
                } else {
                    LOG4CPLUS_ERROR_FMT(getLogger(),
                        "Invalid display mode '%s', use 'vo', 'shared_fb' or 'fb'", optarg);
                    return false;
                }
                break;
            }

            case 'Z':
                config.consumer_type.display.taco_vo.target_fps = std::stoi(optarg);
                break;

            case 1001:
                config.consumer_type.display.taco_vo.osd_enable = true;
                break;

            case 1002:
                config.consumer_type.display.taco_vo.osd_fps = std::stoi(optarg);
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
            
            case 't': {
                int thread_count = std::stoi(optarg);
                if (thread_count < 1) {
                    LOG4CPLUS_ERROR_FMT(getLogger(), 
                        "Invalid thread count '%s', must be >= 1", optarg);
                    return false;
                }
                // 设置 profile 为 parallel_N 格式，触发 PARALLEL 模式
                // params.profile = "parallel_" + std::to_string(thread_count);
                break;
            }
            
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
    
    // 验证：如果指定了 -s 保存帧数，必须同时指定 -o 输出路径
    if (!config.consumer_type.save_raw.max_frames_per_channel.empty() && 
        config.consumer_type.save_raw.max_frames_per_channel[0] != 0 &&
        !config.consumer_type.save_raw.enable) {
        std::cerr << "Error: -s/--save requires -o/--output to specify output file path\n" << std::endl;
        std::cerr << "Example: qa_cases taopencv -s 100 -o /tmp/output.yuv video.mp4\n" << std::endl;
        return false;
    }
    
    config.data_source.path = input_path;
    
    return true;
}

int TaOpencvTestSuite::runPredefinedTest(const std::string& test_name, const std::string& path) {
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

void TaOpencvTestSuite::printHelp() const {
    std::cout << "Empty\n"
              << std::endl;
}

void TaOpencvTestSuite::listTests() const {
    std::cout << "Empty\n"
              << std::endl;
}


}
}