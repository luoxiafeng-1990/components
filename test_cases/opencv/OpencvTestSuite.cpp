#include "OpencvTestSuite.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "productionline/io/BufferConsumerService.hpp"

#include <iostream>
#include <getopt.h>
#include <cstring>
#include <sstream>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace opencv {

// ========================================
// 辅助函数：从 WorkerConfig 构建消费标志
// ========================================
uint32_t OpencvTestSuite::buildConsumeFlags(const WorkerConfig& config) {
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

const std::map<std::string, OpencvTestParams>& OpencvTestSuite::getPredefinedTests() {
    static std::map<std::string, OpencvTestParams> tests = {
        // ========================================
        // H.264 测试（9 个分辨率/帧率组合）
        // ========================================
        {"h264_128x128_30",   {"h264", 128,  128,  30.0, true}},
        {"h264_320x240_30",   {"h264", 320,  240,  30.0, true}},
        {"h264_640x480_30",   {"h264", 640,  480,  30.0, true}},
        {"h264_640x480_60",   {"h264", 640,  480,  60.0, true}},
        {"h264_1280x720_30",  {"h264", 1280, 720,  30.0, true}},
        {"h264_1920x1080_30", {"h264", 1920, 1080, 30.0, true}},
        {"h264_1920x1080_60", {"h264", 1920, 1080, 60.0, true}},
        {"h264_2560x1440_30", {"h264", 2560, 1440, 30.0, true}},
        {"h264_3840x2160_30", {"h264", 3840, 2160, 30.0, true}},

        // ========================================
        // H.265/HEVC 测试（9 个分辨率/帧率组合）
        // ========================================
        {"h265_128x128_30",   {"h265", 128,  128,  30.0, true}},
        {"h265_320x240_30",   {"h265", 320,  240,  30.0, true}},
        {"h265_640x480_30",   {"h265", 640,  480,  30.0, true}},
        {"h265_640x480_60",   {"h265", 640,  480,  60.0, true}},
        {"h265_1280x720_30",  {"h265", 1280, 720,  30.0, true}},
        {"h265_1920x1080_30", {"h265", 1920, 1080, 30.0, true}},
        {"h265_1920x1080_60", {"h265", 1920, 1080, 60.0, true}},
        {"h265_2560x1440_30", {"h265", 2560, 1440, 30.0, true}},
        {"h265_3840x2160_30", {"h265", 3840, 2160, 30.0, true}},

        // ========================================
        // MJPEG 测试（9 个分辨率/帧率组合）
        // ========================================
        {"mjpeg_128x128_30",   {"mjpeg", 128,  128,  30.0, true}},
        {"mjpeg_320x240_30",   {"mjpeg", 320,  240,  30.0, true}},
        {"mjpeg_640x480_30",   {"mjpeg", 640,  480,  30.0, true}},
        {"mjpeg_640x480_60",   {"mjpeg", 640,  480,  60.0, true}},
        {"mjpeg_1280x720_30",  {"mjpeg", 1280, 720,  30.0, true}},
        {"mjpeg_1920x1080_30", {"mjpeg", 1920, 1080, 30.0, true}},
        {"mjpeg_1920x1080_60", {"mjpeg", 1920, 1080, 60.0, true}},
        {"mjpeg_2560x1440_30", {"mjpeg", 2560, 1440, 30.0, true}},
        {"mjpeg_3840x2160_30", {"mjpeg", 3840, 2160, 30.0, true}},

        // ========================================
        // 软件解码测试
        // ========================================
        {"sw_h264_1920x1080_30", {"h264", 1920, 1080, 30.0, false}},
        {"sw_h265_1920x1080_30", {"h265", 1920, 1080, 30.0, false}},
    };
    return tests;
}

std::vector<std::string> OpencvTestSuite::getTestNames() const {
    std::vector<std::string> names;
    for (const auto& pair : getPredefinedTests()) {
        names.push_back(pair.first);
    }
    return names;
}

// ========================================
// ITestModule 接口实现
// ========================================

int OpencvTestSuite::run(int argc, char* argv[]) {
    WorkerConfig config;
    OpencvTestParams params;

    if (!parseArgs(argc, argv, config, params)) {
        return 1;
    }

    // 生成测试名称
    std::ostringstream test_name;
    if (params.isPredefined()) {
        test_name << params.predefined_name << " ("
                  << params.codec << " " << params.width << "x" << params.height
                  << " " << static_cast<int>(params.fps) << "fps)";
    } else {
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
    // COMPARE 模式：PSNR/SSIM 质量验证（HW opencv vs SW opencv）
    // ========================================
    if (config.consumer_type.compare.enable_psnr || config.consumer_type.compare.enable_ssim) {
        auto hw_config = common::WorkerConfigFactory::createHardwareCv(
            config.data_source.path, params.codec, params.width, params.height);
        hw_config.consumer_type = config.consumer_type;
        hw_config.consumer_type.performance.target_fps = params.fps;
        hw_config.data_source.max_frames = config.data_source.max_frames;

        auto sw_config = common::WorkerConfigFactory::createSoftwareCv(
            config.data_source.path, params.width, params.height);
        sw_config.consumer_type.performance.target_fps = params.fps;
        sw_config.data_source.max_frames = config.data_source.max_frames;
        
        /*
        auto sw_config = common::WorkerConfigFactory::createHardwareCv(
            config.data_source.path, params.codec, params.width, params.height);
        sw_config.consumer_type = config.consumer_type;
        sw_config.consumer_type.performance.target_fps = params.fps;
        sw_config.data_source.max_frames = config.data_source.max_frames;
        */

        uint32_t compare_flags = 0;
        if (config.consumer_type.display.enable) {
            compare_flags |= consumer::CONSUME_DISPLAY;
        }
        if (config.consumer_type.save_raw.enable) {
            compare_flags |= consumer::CONSUME_SAVE_RAW;
        }

        auto result = runCompare({hw_config, sw_config}, compare_flags,
                                 test_name.str() + " (COMPARE)");
        consumer::BufferConsumerService::printResult(test_name.str(), result);
        return result.success ? 0 : 1;
    }

    // ========================================
    // SINGLE 模式：默认单路消费
    // ========================================
    WorkerConfig full_config;
    if (params.use_hardware) {
        full_config = common::WorkerConfigFactory::createHardwareCv(
            config.data_source.path, params.codec, params.width, params.height);
    } else {
        full_config = common::WorkerConfigFactory::createSoftwareCv(
            config.data_source.path, params.width, params.height);
    }
    full_config.consumer_type = config.consumer_type;
    full_config.consumer_type.performance.target_fps = params.fps;
    full_config.data_source.max_frames = config.data_source.max_frames;

    uint32_t flags = buildConsumeFlags(full_config);
    auto result = runSingle(full_config, flags, test_name.str());
    consumer::BufferConsumerService::printResult(test_name.str(), result);

    return result.success ? 0 : 1;
}

bool OpencvTestSuite::parseArgs(int argc, char* argv[], WorkerConfig& config, OpencvTestParams& params) {
    optind = 1;

    static struct option long_options[] = {
        {"help",         no_argument,       0, 'h'},
        {"list",         no_argument,       0, 'l'},
        {"file",         required_argument, 0, 'f'},
        {"rtsp",         required_argument, 0, 'r'},
        {"codec",        required_argument, 0, 'c'},
        {"decoder",      required_argument, 0, 'D'},
        {"width",        required_argument, 0, 'W'},
        {"height",       required_argument, 0, 'H'},
        {"resolution",   required_argument, 0, 'R'},
        {"fps",          required_argument, 0, 'F'},
        {"max-frames",   required_argument, 0, 'm'},
        {"save",         required_argument, 0, 's'},
        {"output",       required_argument, 0, 'o'},
        {"display",      no_argument,       0, 'd'},
        {"display-mode", required_argument, 0, 'X'},
        {"display-fps",  required_argument, 0, 'Z'},
        {"osd",          no_argument,       0,  1001},
        {"osd-fps",      required_argument, 0,  1002},
        {"psnr",         no_argument,       0, 'p'},
        {"ssim",         no_argument,       0, 'S'},
        {"min-psnr",     required_argument, 0, 'P'},
        {"min-ssim",     required_argument, 0, 'M'},
        {"verbose",      no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };

    std::string input_path;

    int opt;
    while ((opt = getopt_long(argc, argv, "hlf:r:c:D:W:H:R:F:m:s:o:dpSP:M:vX:Z:",
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
                std::string res = optarg;
                size_t pos = res.find('x');
                if (pos != std::string::npos) {
                    params.width  = std::stoi(res.substr(0, pos));
                    params.height = std::stoi(res.substr(pos + 1));
                }
                break;
            }

            case 'F':
                params.fps = std::stod(optarg);
                break;

            case 'm':
                config.data_source.max_frames = std::stoi(optarg);
                break;

            case 's':
                config.consumer_type.save_raw.max_frames_per_channel = {std::stoi(optarg)};
                break;

            case 'o':
                config.consumer_type.save_raw.enable = true;
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

            default:
                printHelp();
                return false;
        }
    }

    // 处理剩余位置参数
    for (int i = optind; i < argc; i++) {
        std::string arg = argv[i];

        // 检查是否是预定义测试名称
        const auto& tests = getPredefinedTests();
        auto it = tests.find(arg);
        if (it != tests.end()) {
            params = it->second;
            params.predefined_name = arg;
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

    // 验证：-s 需配合 -o 使用
    if (!config.consumer_type.save_raw.max_frames_per_channel.empty() &&
        config.consumer_type.save_raw.max_frames_per_channel[0] != 0 &&
        !config.consumer_type.save_raw.enable) {
        std::cerr << "Error: -s/--save requires -o/--output to specify output file path\n" << std::endl;
        std::cerr << "Example: qa_cases opencv -s 100 -o /tmp/output.yuv video.mp4\n" << std::endl;
        return false;
    }

    config.data_source.path = input_path;
    return true;
}

int OpencvTestSuite::runPredefinedTest(const std::string& test_name, const std::string& path) {
    const auto& tests = getPredefinedTests();
    auto it = tests.find(test_name);
    if (it == tests.end()) {
        LOG4CPLUS_ERROR_FMT(getLogger(), "Unknown test '%s'", test_name.c_str());
        return 1;
    }

    const auto& params = it->second;
    WorkerConfig config;
    if (params.use_hardware) {
        config = common::WorkerConfigFactory::createHardwareCv(
            path, params.codec, params.width, params.height);
    } else {
        config = common::WorkerConfigFactory::createSoftwareCv(
            path, params.width, params.height);
    }
    
    config.consumer_type.performance.target_fps = params.fps;

    auto result = runSingle(config, consumer::CONSUME_COUNT, test_name);
    consumer::BufferConsumerService::printResult(test_name, result);
    return result.success ? 0 : 1;
}

void OpencvTestSuite::printHelp() const {
    std::cout << "\n"
              << "TaOpenCV Module - OpenCV 解码测试\n"
              << "\n"
              << "Usage:\n"
              << "  qa_cases opencv [options] <video_path>\n"
              << "  qa_cases opencv [options] <test_name> <video_path>\n"
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
              << "  -s, --save <n>          保存帧数 (0=不保存, -1=全部)，需配合 -o 使用\n"
              << "  -o, --output <path>     输出文件路径，启用保存功能\n"
              << "  -d, --display           启用显示输出\n"
              << "  --display-mode <mode>   显示模式: fb(默认), vo(taco-vo), shared_fb\n"
              << "  --display-fps <n>       显示刷新帧率\n"
              << "  --osd                   启用 OSD 叠加\n"
              << "  --osd-fps <n>           OSD 刷新频率\n"
              << "  -p, --psnr              启用 PSNR 验证 (ExecuteMode::COMPARE)\n"
              << "  -S, --ssim              启用 SSIM 验证 (ExecuteMode::COMPARE)\n"
              << "  -P, --min-psnr <n>      PSNR 阈值 (默认: 30.0 dB)\n"
              << "  -M, --min-ssim <n>      SSIM 阈值 (默认: 0.95)\n"
              << "  -v, --verbose           详细日志\n"
              << "\n"
              << "ExecuteMode Mapping:\n"
              << "  SINGLE   - 默认单路 OpenCV 解码，支持 --decoder hw/sw\n"
              << "  COMPARE  - --psnr/--ssim 启用时，HW opencv vs SW opencv 对比\n"
              << "\n"
              << "Examples:\n"
              << "  qa_cases opencv video.mp4                        # SINGLE (hw)\n"
              << "  qa_cases opencv --decoder sw video.mp4           # SINGLE (sw)\n"
              << "  qa_cases opencv --display video.mp4              # SINGLE + DISPLAY\n"
              << "  qa_cases opencv -s 100 -o /tmp/out.yuv video.mp4 # SINGLE + SAVE\n"
              << "  qa_cases opencv --psnr video.mp4                 # COMPARE (HW vs SW)\n"
              << "  qa_cases opencv h264_1920x1080_30 video.mp4      # 预定义测试\n"
              << std::endl;
}

void OpencvTestSuite::listTests() const {
    std::cout << "\n"
              << "Available TaOpenCV tests:\n"
              << "────────────────────────────────────────────────────────\n"
              << "\n"
              << "H.264 Tests (9) - ExecuteMode::SINGLE (hw):\n"
              << "  h264_128x128_30         H.264 128x128 30fps\n"
              << "  h264_320x240_30         H.264 320x240 30fps\n"
              << "  h264_640x480_30         H.264 640x480 30fps\n"
              << "  h264_640x480_60         H.264 640x480 60fps\n"
              << "  h264_1280x720_30        H.264 720p 30fps\n"
              << "  h264_1920x1080_30       H.264 1080p 30fps\n"
              << "  h264_1920x1080_60       H.264 1080p 60fps\n"
              << "  h264_2560x1440_30       H.264 1440p 30fps\n"
              << "  h264_3840x2160_30       H.264 4K 30fps\n"
              << "\n"
              << "H.265/HEVC Tests (9) - ExecuteMode::SINGLE (hw):\n"
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
              << "MJPEG Tests (9) - ExecuteMode::SINGLE (hw):\n"
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
              << "Software Decode Tests (2) - ExecuteMode::SINGLE (sw):\n"
              << "  sw_h264_1920x1080_30    Software H.264 1080p 30fps\n"
              << "  sw_h265_1920x1080_30    Software H.265 1080p 30fps\n"
              << "\n"
              << "────────────────────────────────────────────────────────\n"
              << "Total: 29 predefined tests\n"
              << std::endl;
}

}  // namespace opencv
}  // namespace test
