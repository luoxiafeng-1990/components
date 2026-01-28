/**
 * @file VdecTestSuite.cpp
 * @brief VdecTestSuite 实现
 */

#include "VdecTestSuite.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "../common/TestExecutor.hpp"

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
        // 多 Worker 测试（对应原始 multi_worker）
        // ========================================
        {"multi_worker",        {"h264", 1920, 1080, 30.0, "multi"}},
        {"multi_worker_4k",     {"h264", 3840, 2160, 30.0, "multi"}},
        
        // ========================================
        // 多线程解码测试（对应原始 ffmpeg_multithread）
        // ========================================
        {"multithread_2",       {"h264", 1920, 1080, 30.0, "mt2"}},
        {"multithread_4",       {"h264", 1920, 1080, 30.0, "mt4"}},
        {"multithread_8",       {"h264", 1920, 1080, 30.0, "mt8"}},
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
    
    // 构建完整的配置
    auto full_config = common::WorkerConfigFactory::createDecode(
        config.data_source.path, params.codec, params.width, params.height);
    
    // 合并命令行传入的 consumer 配置
    full_config.consumer = config.consumer;
    full_config.consumer.target_fps = params.fps;
    
    // 生成测试名称
    std::ostringstream test_name;
    test_name << params.codec << " " << params.width << "x" << params.height 
              << " " << static_cast<int>(params.fps) << "fps";
    
    // 执行测试
    common::TestExecutor::printHeader(test_name.str(), full_config);
    auto result = common::TestExecutor::runDecode(full_config);
    
    common::TestExecutor::printResult(test_name.str(), result);
    
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
        std::cerr << "Error: No input file or RTSP URL specified\n";
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
        std::cerr << "Error: Unknown test '" << test_name << "'\n";
        return 1;
    }
    
    auto result = runDecodeTest(path, it->second);
    common::TestExecutor::printResult(test_name, result);
    return result.success ? 0 : 1;
}

void VdecTestSuite::printHelp() const {
    std::cout << "\n";
    std::cout << "VDEC Module - 视频解码测试\n";
    std::cout << "\n";
    std::cout << "Usage:\n";
    std::cout << "  qa_cases vdec [options] <video_path>\n";
    std::cout << "  qa_cases vdec [options] <test_name> <video_path>\n";
    std::cout << "\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help              显示帮助信息\n";
    std::cout << "  -l, --list              列出所有预定义测试\n";
    std::cout << "  -f, --file <path>       视频文件路径\n";
    std::cout << "  -r, --rtsp <url>        RTSP URL\n";
    std::cout << "  -c, --codec <name>      编解码器 (h264|h265|mjpeg|software)\n";
    std::cout << "  -W, --width <n>         分辨率宽度\n";
    std::cout << "  -H, --height <n>        分辨率高度\n";
    std::cout << "  -R, --resolution <WxH>  分辨率 (如 1920x1080)\n";
    std::cout << "  -F, --fps <n>           目标帧率\n";
    std::cout << "  -m, --max-frames <n>    最大帧数 (-1=无限制)\n";
    std::cout << "  -s, --save <n>          保存帧数 (0=不保存, -1=全部)\n";
    std::cout << "  -o, --output <path>     输出文件路径\n";
    std::cout << "  -d, --display           启用显示输出 (输出到 framebuffer)\n";
    std::cout << "  -p, --psnr              启用 PSNR 验证\n";
    std::cout << "  -S, --ssim              启用 SSIM 验证\n";
    std::cout << "  -P, --min-psnr <n>      PSNR 阈值 (默认: 30.0 dB)\n";
    std::cout << "  -M, --min-ssim <n>      SSIM 阈值 (默认: 0.95)\n";
    std::cout << "  -e, --reference <path>  参考文件路径\n";
    std::cout << "  -v, --verbose           详细日志\n";
    std::cout << "\n";
    std::cout << "Examples:\n";
    std::cout << "  qa_cases vdec video.mp4\n";
    std::cout << "  qa_cases vdec --codec h264 --resolution 1920x1080 video.mp4\n";
    std::cout << "  qa_cases vdec --rtsp rtsp://192.168.1.100/stream\n";
    std::cout << "  qa_cases vdec h264_1920x1080_30 video.mp4\n";
    std::cout << "  qa_cases vdec --psnr --reference ref.yuv video.mp4\n";
    std::cout << "\n";
}

void VdecTestSuite::listTests() const {
    std::cout << "\nAvailable VDEC tests:\n";
    std::cout << "────────────────────────────────────────────────────────\n";
    
    std::cout << "\nH.264 Tests (9):\n";
    std::cout << "  h264_128x128_30         H.264 128x128 30fps main\n";
    std::cout << "  h264_320x240_30         H.264 320x240 30fps high\n";
    std::cout << "  h264_640x480_30         H.264 640x480 30fps main\n";
    std::cout << "  h264_640x480_60         H.264 640x480 60fps high\n";
    std::cout << "  h264_1280x720_30        H.264 720p 30fps high\n";
    std::cout << "  h264_1920x1080_30       H.264 1080p 30fps high\n";
    std::cout << "  h264_1920x1080_60       H.264 1080p 60fps high\n";
    std::cout << "  h264_2560x1440_30       H.264 1440p 30fps high\n";
    std::cout << "  h264_3840x2160_30       H.264 4K 30fps high\n";
    
    std::cout << "\nH.265/HEVC Tests (9):\n";
    std::cout << "  h265_128x128_30         H.265 128x128 30fps\n";
    std::cout << "  h265_320x240_30         H.265 320x240 30fps\n";
    std::cout << "  h265_640x480_30         H.265 640x480 30fps\n";
    std::cout << "  h265_640x480_60         H.265 640x480 60fps\n";
    std::cout << "  h265_1280x720_30        H.265 720p 30fps\n";
    std::cout << "  h265_1920x1080_30       H.265 1080p 30fps\n";
    std::cout << "  h265_1920x1080_60       H.265 1080p 60fps\n";
    std::cout << "  h265_2560x1440_30       H.265 1440p 30fps\n";
    std::cout << "  h265_3840x2160_30       H.265 4K 30fps\n";
    
    std::cout << "\nMJPEG Tests (9):\n";
    std::cout << "  mjpeg_128x128_30        MJPEG 128x128 30fps\n";
    std::cout << "  mjpeg_320x240_30        MJPEG 320x240 30fps\n";
    std::cout << "  mjpeg_640x480_30        MJPEG 640x480 30fps\n";
    std::cout << "  mjpeg_640x480_60        MJPEG 640x480 60fps\n";
    std::cout << "  mjpeg_1280x720_30       MJPEG 720p 30fps\n";
    std::cout << "  mjpeg_1920x1080_30      MJPEG 1080p 30fps\n";
    std::cout << "  mjpeg_1920x1080_60      MJPEG 1080p 60fps\n";
    std::cout << "  mjpeg_2560x1440_30      MJPEG 1440p 30fps\n";
    std::cout << "  mjpeg_3840x2160_30      MJPEG 4K 30fps\n";
    
    std::cout << "\nSoftware Decode Tests (2):\n";
    std::cout << "  sw_h264_1920x1080_30    Software H.264 1080p 30fps\n";
    std::cout << "  sw_h265_1920x1080_30    Software H.265 1080p 30fps\n";
    
    std::cout << "\nRTSP H.264 Tests (6):\n";
    std::cout << "  rtsp_h264_1280x720_30_cbr   RTSP H.264 720p CBR\n";
    std::cout << "  rtsp_h264_1280x720_30_vbr   RTSP H.264 720p VBR\n";
    std::cout << "  rtsp_h264_1920x1080_30_cbr  RTSP H.264 1080p CBR\n";
    std::cout << "  rtsp_h264_1920x1080_30_vbr  RTSP H.264 1080p VBR\n";
    std::cout << "  rtsp_h264_3840x2160_30_cbr  RTSP H.264 4K CBR\n";
    std::cout << "  rtsp_h264_3840x2160_30_vbr  RTSP H.264 4K VBR\n";
    
    std::cout << "\nRTSP H.265 Tests (6):\n";
    std::cout << "  rtsp_h265_1280x720_30_cbr   RTSP H.265 720p CBR\n";
    std::cout << "  rtsp_h265_1280x720_30_vbr   RTSP H.265 720p VBR\n";
    std::cout << "  rtsp_h265_1920x1080_30_cbr  RTSP H.265 1080p CBR\n";
    std::cout << "  rtsp_h265_1920x1080_30_vbr  RTSP H.265 1080p VBR\n";
    std::cout << "  rtsp_h265_3840x2160_30_cbr  RTSP H.265 4K CBR\n";
    std::cout << "  rtsp_h265_3840x2160_30_vbr  RTSP H.265 4K VBR\n";
    
    std::cout << "\nRTSP MJPEG Tests (1):\n";
    std::cout << "  rtsp_mjpeg_32768x18432_30   RTSP MJPEG Ultra-High\n";
    
    std::cout << "\nMulti-Worker Tests (2):\n";
    std::cout << "  multi_worker               HW+SW concurrent decode\n";
    std::cout << "  multi_worker_4k            HW+SW concurrent 4K decode\n";
    
    std::cout << "\nMulti-Thread Tests (3):\n";
    std::cout << "  multithread_2              2-thread decode\n";
    std::cout << "  multithread_4              4-thread decode\n";
    std::cout << "  multithread_8              8-thread decode\n";
    
    std::cout << "────────────────────────────────────────────────────────\n";
    std::cout << "Total: 47 predefined tests\n";
    std::cout << "\n";
}

// ========================================
// 核心测试方法实现
// ========================================

common::TestResult VdecTestSuite::runDecodeTest(
    const std::string& path,
    const DecodeTestParams& params
) {
    // 创建配置
    auto config = common::WorkerConfigFactory::createDecode(
        path, params.codec, params.width, params.height);
    config.consumer.target_fps = params.fps;
    
    // 生成测试名称
    std::ostringstream test_name;
    test_name << params.codec << " " << params.width << "x" << params.height 
              << " " << static_cast<int>(params.fps) << "fps";
    
    common::TestExecutor::printHeader(test_name.str(), config);
    return common::TestExecutor::runDecode(config);
}

common::TestResult VdecTestSuite::runDecodeTest(
    const std::string& path,
    const std::string& codec,
    int width,
    int height,
    double target_fps
) {
    return runDecodeTest(path, DecodeTestParams(codec, width, height, target_fps));
}

common::TestResult VdecTestSuite::runQualityTest(
    const std::string& path,
    const DecodeTestParams& params,
    const std::string& reference_path,
    bool enable_psnr,
    bool enable_ssim
) {
    (void)reference_path;  // 不再需要外部参考文件，使用软件解码作为参考
    
    // 创建硬件解码配置
    auto hw_config = common::WorkerConfigFactory::createDecode(
        path, params.codec, params.width, params.height);
    hw_config.consumer.target_fps = params.fps;
    hw_config.consumer.enable_psnr = enable_psnr;
    hw_config.consumer.enable_ssim = enable_ssim;
    hw_config.consumer.verbose = true;
    
    // 创建软件解码配置（作为参考）
    auto sw_config = common::WorkerConfigFactory::createSoftwareDecode(
        path, params.width, params.height);
    sw_config.consumer.target_fps = params.fps;
    
    std::ostringstream test_name;
    test_name << "Quality Test: " << params.codec << " " << params.width << "x" << params.height;
    test_name << " (HW vs SW)";
    
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  " << test_name.str() << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  Input:      " << path << "\n";
    std::cout << "  HW Decoder: " << params.codec << "_taco\n";
    std::cout << "  SW Decoder: FFmpeg (libavcodec)\n";
    std::cout << "  PSNR:       " << (enable_psnr ? "enabled" : "disabled") << "\n";
    std::cout << "  SSIM:       " << (enable_ssim ? "enabled" : "disabled") << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    
    // 使用 MultiWorkerProductionLine + 帧同步进行 PSNR/SSIM 验证
    return common::TestExecutor::runPsnrValidation(hw_config, sw_config);
}

common::TestResult VdecTestSuite::runMultiWorkerTest(
    const std::string& path,
    const DecodeTestParams& params
) {
    // 创建硬件解码配置
    auto hw_config = common::WorkerConfigFactory::createDecode(
        path, params.codec, params.width, params.height);
    hw_config.consumer.target_fps = params.fps;
    
    // 创建软件解码配置
    auto sw_config = common::WorkerConfigFactory::createSoftwareDecode(
        path, params.width, params.height);
    sw_config.consumer.target_fps = params.fps;
    
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  Multi-Worker Test\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  Input:      " << path << "\n";
    std::cout << "  Worker 1:   Hardware Decoder (" << params.codec << ")\n";
    std::cout << "  Worker 2:   Software Decoder (FFmpeg)\n";
    std::cout << "  Mode:       Concurrent decode from same source\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    
    // 使用 TestExecutor 的 runMultiWorker 方法
    std::vector<WorkerConfig> configs = {hw_config, sw_config};
    return common::TestExecutor::runMultiWorker(configs);
}

common::TestResult VdecTestSuite::runMultithreadTest(
    const std::string& path,
    const DecodeTestParams& params,
    int thread_count
) {
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  Multi-thread Decode Test\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  Input:      " << path << "\n";
    std::cout << "  Threads:    " << thread_count << "\n";
    std::cout << "  Codec:      " << params.codec << "\n";
    std::cout << "  Resolution: " << params.width << "x" << params.height << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    
    // 创建多个配置，每个线程一个
    std::vector<WorkerConfig> configs;
    for (int i = 0; i < thread_count; i++) {
        auto config = common::WorkerConfigFactory::createDecode(
            path, params.codec, params.width, params.height);
        config.consumer.target_fps = params.fps;
        configs.push_back(config);
    }
    
    return common::TestExecutor::runMultiWorker(configs);
}

} // namespace vdec
} // namespace test
