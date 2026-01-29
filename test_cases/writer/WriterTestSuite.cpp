/**
 * @file WriterTestSuite.cpp
 * @brief WriterTestSuite 实现
 * 
 * 重构为 ExecuteMode 风格，与 BufferConsumerService 架构对齐
 */

#include "WriterTestSuite.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "productionline/io/BufferConsumerService.hpp"

#include <iostream>
#include <getopt.h>
#include <cstring>
#include <sstream>
#include <filesystem>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace writer {

// 获取模块级日志实例
log4cplus::Logger& WriterTestSuite::getLogger() {
    static log4cplus::Logger logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.test.WriterSuite"));
    return logger;
}

// ========================================
// 格式列表
// ========================================

const std::vector<std::pair<OutputFormat, std::string>>& WriterTestSuite::getRgbFormats() {
    static std::vector<std::pair<OutputFormat, std::string>> formats = {
        {OutputFormat::RGB_ARGB888,       "ARGB8888"},
        {OutputFormat::RGB_ABGR888,       "ABGR8888"},
        {OutputFormat::RGB_RGBA888,       "RGBA8888"},
        {OutputFormat::RGB_BGRA888,       "BGRA8888"},
        {OutputFormat::RGB_RGB888,        "RGB888"},
        {OutputFormat::RGB_BGR888,        "BGR888"},
        {OutputFormat::RGB_XRGB888,       "XRGB8888 (0RGB)"},
        {OutputFormat::RGB_XBGR888,       "XBGR8888 (0BGR)"},
        {OutputFormat::RGB_RGBX888,       "RGBX8888 (RGB0)"},
        {OutputFormat::RGB_BGRX888,       "BGRX8888 (BGR0)"},
        {OutputFormat::RGB_R16G16B16,     "RGB161616 (RGB48)"},
        {OutputFormat::RGB_B16G16R16,     "BGR161616 (BGR48)"},
    };
    return formats;
}

const std::vector<std::pair<OutputFormat, std::string>>& WriterTestSuite::getYuvFormats() {
    static std::vector<std::pair<OutputFormat, std::string>> formats = {
        {OutputFormat::YUV_NV12,    "YUV420 NV12"},
        {OutputFormat::YUV_NV21,    "YUV420 NV21"},
        {OutputFormat::YUV_I420,    "YUV420 I420"},
        {OutputFormat::YUV_YV12,    "YUV420 YV12"},
        {OutputFormat::YUV_P010,    "YUV420 P010 (10-bit)"},
        {OutputFormat::YUV_NV16,    "YUV422 NV16"},
        {OutputFormat::YUV_NV61,    "YUV422 NV61"},
        {OutputFormat::YUV_I422,    "YUV422 I422"},
        {OutputFormat::YUV_NV24,    "YUV444 NV24"},
        {OutputFormat::YUV_I444,    "YUV444 I444"},
    };
    return formats;
}

// ========================================
// 预定义测试参数表
// ========================================

const std::map<std::string, WriterTestParams>& WriterTestSuite::getPredefinedTests() {
    static std::map<std::string, WriterTestParams> tests = {
        // ========================================
        // RGB 格式（12 个）
        // ========================================
        {"rgb_argb888",     {OutputFormat::RGB_ARGB888,     "ARGB8888"}},
        {"rgb_abgr888",     {OutputFormat::RGB_ABGR888,     "ABGR8888"}},
        {"rgb_rgba888",     {OutputFormat::RGB_RGBA888,     "RGBA8888"}},
        {"rgb_bgra888",     {OutputFormat::RGB_BGRA888,     "BGRA8888"}},
        {"rgb_rgb888",      {OutputFormat::RGB_RGB888,      "RGB888"}},
        {"rgb_bgr888",      {OutputFormat::RGB_BGR888,      "BGR888"}},
        {"rgb_xrgb888",     {OutputFormat::RGB_XRGB888,     "XRGB8888 (0RGB)"}},
        {"rgb_xbgr888",     {OutputFormat::RGB_XBGR888,     "XBGR8888 (0BGR)"}},
        {"rgb_rgbx888",     {OutputFormat::RGB_RGBX888,     "RGBX8888 (RGB0)"}},
        {"rgb_bgrx888",     {OutputFormat::RGB_BGRX888,     "BGRX8888 (BGR0)"}},
        {"rgb_r16g16b16",   {OutputFormat::RGB_R16G16B16,   "RGB161616 (RGB48)"}},
        {"rgb_b16g16r16",   {OutputFormat::RGB_B16G16R16,   "BGR161616 (BGR48)"}},
        
        // ========================================
        // YUV 格式（10 个）
        // ========================================
        {"yuv_nv12",        {OutputFormat::YUV_NV12,  "YUV420 NV12"}},
        {"yuv_nv21",        {OutputFormat::YUV_NV21,  "YUV420 NV21"}},
        {"yuv_i420",        {OutputFormat::YUV_I420,  "YUV420 I420"}},
        {"yuv_yv12",        {OutputFormat::YUV_YV12,  "YUV420 YV12"}},
        {"yuv_p010",        {OutputFormat::YUV_P010,  "YUV420 P010 (10-bit)"}},
        {"yuv_nv16",        {OutputFormat::YUV_NV16,  "YUV422 NV16"}},
        {"yuv_nv61",        {OutputFormat::YUV_NV61,  "YUV422 NV61"}},
        {"yuv_i422",        {OutputFormat::YUV_I422,  "YUV422 I422"}},
        {"yuv_nv24",        {OutputFormat::YUV_NV24,  "YUV444 NV24"}},
        {"yuv_i444",        {OutputFormat::YUV_I444,  "YUV444 I444"}},
        
        // ========================================
        // 批量测试
        // ========================================
        {"all_rgb",         {OutputFormat::RGB_RGB888, "All 12 RGB formats"}},
        {"all_yuv",         {OutputFormat::YUV_NV12,   "All 10 YUV formats"}},
    };
    return tests;
}

std::vector<std::string> WriterTestSuite::getTestNames() const {
    std::vector<std::string> names;
    for (const auto& pair : getPredefinedTests()) {
        names.push_back(pair.first);
    }
    return names;
}

// ========================================
// ITestModule 接口实现
// ========================================

int WriterTestSuite::run(int argc, char* argv[]) {
    WorkerConfig config;
    WriterTestParams params;
    std::string output_path;
    
    if (!parseArgs(argc, argv, config, params, output_path)) {
        return 1;
    }
    
    TestResult result;
    
    // 检查是否是批量测试
    if (params.description == "All 12 RGB formats") {
        // 批量测试所有 RGB 格式
        result.success = true;
        const auto& formats = getRgbFormats();
        int passed = 0;
        
        std::cout << "\n";
        std::cout << "═══════════════════════════════════════════════════════\n";
        std::cout << "  Writer Test: All 12 RGB Formats (ExecuteMode::SINGLE)\n";
        std::cout << "═══════════════════════════════════════════════════════\n\n";
        
        for (const auto& [format, desc] : formats) {
            WriterTestParams p(format, desc);
            p.save_frames = 5;
            std::string out_file = (output_path.empty() ? "/tmp" : output_path) + 
                                   "/rgb_" + TacoConfigBuilder::mapFormatEnumToName(format).data() + ".raw";
            
            std::cout << "Testing " << desc << "... ";
            std::cout.flush();
            
            auto r = runSingle(config.data_source.path, p, out_file);
            if (r.success) {
                std::cout << "PASSED (" << r.frames_consumed << " frames)\n";
                passed++;
            } else {
                std::cout << "FAILED: " << r.error_message << "\n";
                result.success = false;
            }
            result.frames_consumed += r.frames_consumed;
        }
        
        std::cout << "\n  RGB Format Summary: " << passed << "/" << formats.size() << " passed\n";
    } else if (params.description == "All 10 YUV formats") {
        // 批量测试所有 YUV 格式
        result.success = true;
        const auto& formats = getYuvFormats();
        int passed = 0;
        
        std::cout << "\n";
        std::cout << "═══════════════════════════════════════════════════════\n";
        std::cout << "  Writer Test: All 10 YUV Formats (ExecuteMode::SINGLE)\n";
        std::cout << "═══════════════════════════════════════════════════════\n\n";
        
        for (const auto& [format, desc] : formats) {
            WriterTestParams p(format, desc);
            p.save_frames = 5;
            std::string out_file = (output_path.empty() ? "/tmp" : output_path) + 
                                   "/yuv_" + TacoConfigBuilder::mapFormatEnumToName(format).data() + ".raw";
            
            std::cout << "Testing " << desc << "... ";
            std::cout.flush();
            
            auto r = runSingle(config.data_source.path, p, out_file);
            if (r.success) {
                std::cout << "PASSED (" << r.frames_consumed << " frames)\n";
                passed++;
            } else {
                std::cout << "FAILED: " << r.error_message << "\n";
                result.success = false;
            }
            result.frames_consumed += r.frames_consumed;
        }
        
        std::cout << "\n  YUV Format Summary: " << passed << "/" << formats.size() << " passed\n";
    } else {
        // 单格式测试
        result = runSingle(config.data_source.path, params, output_path);
    }
    
    consumer::BufferConsumerService::printResult("Writer " + params.description, result);
    
    return result.success ? 0 : 1;
}

bool WriterTestSuite::parseArgs(int argc, char* argv[], WorkerConfig& config,
                                 WriterTestParams& params, std::string& output_path) {
    optind = 1;
    
    static struct option long_options[] = {
        {"help",      no_argument,       0, 'h'},
        {"list",      no_argument,       0, 'l'},
        {"input",     required_argument, 0, 'i'},
        {"decoder",   required_argument, 0, 'D'},
        {"output",    required_argument, 0, 'o'},
        {"format",    required_argument, 0, 'f'},
        {"frames",    required_argument, 0, 'n'},
        {"all-rgb",   no_argument,       0, 'R'},
        {"all-yuv",   no_argument,       0, 'Y'},
        {"verbose",   no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };
    
    std::string input_path;
    std::string format_str = "nv12";
    bool all_rgb = false;
    bool all_yuv = false;
    
    int opt;
    while ((opt = getopt_long(argc, argv, "hli:D:o:f:n:RYv",
                              long_options, nullptr)) != -1) {
        switch (opt) {
            case 'h':
                printHelp();
                return false;
            
            case 'l':
                listTests();
                return false;
            
            case 'i':
                input_path = optarg;
                break;
            
            case 'D': {
                std::string decoder_type = optarg;
                if (decoder_type == "hw" || decoder_type == "hardware") {
                    params.use_hardware = true;
                } else if (decoder_type == "sw" || decoder_type == "software") {
                    params.use_hardware = false;
                } else {
                    std::cerr << "Invalid decoder type: " << optarg << ", use 'hw' or 'sw'\n";
                    return false;
                }
                break;
            }
            
            case 'o':
                output_path = optarg;
                break;
            
            case 'f':
                format_str = optarg;
                break;
            
            case 'n':
                params.save_frames = std::stoi(optarg);
                break;
            
            case 'R':
                all_rgb = true;
                break;
            
            case 'Y':
                all_yuv = true;
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
        
        // 检查是否是预定义测试
        const auto& tests = getPredefinedTests();
        if (tests.find(arg) != tests.end()) {
            params = tests.at(arg);
            continue;
        }
        
        // 否则作为输入路径
        if (input_path.empty()) {
            input_path = arg;
        }
    }
    
    // 处理批量测试
    if (all_rgb) {
        params.description = "All 12 RGB formats";
    } else if (all_yuv) {
        params.description = "All 10 YUV formats";
    }
    
    // 检查输入
    if (input_path.empty()) {
        std::cerr << "Error: No input file specified\n";
        printHelp();
        return false;
    }
    
    config.data_source.path = input_path;
    
    return true;
}

void WriterTestSuite::printHelp() const {
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  Writer Test Suite - BufferWriter Format Tests\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "\n";
    std::cout << "ExecuteMode: SINGLE + CONSUME_SAVE_RAW\n";
    std::cout << "\n";
    std::cout << "Usage: qa_cases writer [options] [test_name|input_file]\n";
    std::cout << "\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help           Show this help message\n";
    std::cout << "  -l, --list           List all predefined tests\n";
    std::cout << "  -i, --input <file>   Input video file\n";
    std::cout << "  -D, --decoder <type> Decoder type (hw|hardware|sw|software, default: hw)\n";
    std::cout << "  -o, --output <path>  Output file/directory\n";
    std::cout << "  -f, --format <fmt>   Output format (nv12, rgb888, etc.)\n";
    std::cout << "  -n, --frames <n>     Number of frames to save (default: 10)\n";
    std::cout << "  -R, --all-rgb        Test all 12 RGB formats\n";
    std::cout << "  -Y, --all-yuv        Test all 10 YUV formats\n";
    std::cout << "  -v, --verbose        Verbose output\n";
    std::cout << "\n";
    std::cout << "Examples:\n";
    std::cout << "  qa_cases writer -i video.mp4 -f rgb888 -o output.rgb\n";
    std::cout << "  qa_cases writer -i video.mp4 --all-rgb -o /tmp/rgb_test\n";
    std::cout << "  qa_cases writer -i video.mp4 --all-yuv -o /tmp/yuv_test\n";
    std::cout << "  qa_cases writer rgb_argb888 -i video.mp4\n";
    std::cout << "\n";
}

void WriterTestSuite::listTests() const {
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  Available Writer Tests (ExecuteMode::SINGLE)\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    
    std::cout << "\nRGB Format Tests (12):\n";
    std::cout << "  rgb_argb888         ARGB8888 format\n";
    std::cout << "  rgb_abgr888         ABGR8888 format\n";
    std::cout << "  rgb_rgba888         RGBA8888 format\n";
    std::cout << "  rgb_bgra888         BGRA8888 format\n";
    std::cout << "  rgb_rgb888          RGB888 format\n";
    std::cout << "  rgb_bgr888          BGR888 format\n";
    std::cout << "  rgb_xrgb888         XRGB8888 (0RGB) format\n";
    std::cout << "  rgb_xbgr888         XBGR8888 (0BGR) format\n";
    std::cout << "  rgb_rgbx888         RGBX8888 (RGB0) format\n";
    std::cout << "  rgb_bgrx888         BGRX8888 (BGR0) format\n";
    std::cout << "  rgb_r16g16b16       RGB161616 (RGB48) format\n";
    std::cout << "  rgb_b16g16r16       BGR161616 (BGR48) format\n";
    
    std::cout << "\nYUV Format Tests (10):\n";
    std::cout << "  yuv_nv12            YUV420 NV12 format\n";
    std::cout << "  yuv_nv21            YUV420 NV21 format\n";
    std::cout << "  yuv_i420            YUV420 I420 format\n";
    std::cout << "  yuv_yv12            YUV420 YV12 format\n";
    std::cout << "  yuv_p010            YUV420 P010 (10-bit) format\n";
    std::cout << "  yuv_nv16            YUV422 NV16 format\n";
    std::cout << "  yuv_nv61            YUV422 NV61 format\n";
    std::cout << "  yuv_i422            YUV422 I422 format\n";
    std::cout << "  yuv_nv24            YUV444 NV24 format\n";
    std::cout << "  yuv_i444            YUV444 I444 format\n";
    
    std::cout << "\nBatch Tests:\n";
    std::cout << "  all_rgb             Test all 12 RGB formats\n";
    std::cout << "  all_yuv             Test all 10 YUV formats\n";
    
    std::cout << "────────────────────────────────────────────────────────\n";
    std::cout << "Total: 24 predefined tests\n";
    std::cout << "\n";
}

// ========================================
// 核心测试方法实现（与 ExecuteMode 对齐）
// ========================================

TestResult WriterTestSuite::runSingle(
    const std::string& input_path,
    const WriterTestParams& params,
    const std::string& output_path
) {
    auto& logger = getLogger();
    
    // 创建配置 - 根据解码方式和格式选择合适的工厂方法
    WorkerConfig config;
    
    if (!params.use_hardware) {
        // 软件解码模式
        config = common::WorkerConfigFactory::createSoftwareDecode(
            input_path, params.width, params.height);
        LOG4CPLUS_WARN(logger, 
            "Software decode mode: Hardware PP features are not available");
    } else if (static_cast<int>(params.format) >= 1000) {
        // 硬件解码 + RGB 格式 - 使用 PP1 通道
        config = common::WorkerConfigFactory::createPP1RgbConfig(
            input_path,
            params.format,
            params.width,
            params.height
        );
    } else {
        // 硬件解码 + YUV 格式 - 使用 PP0 通道
        config = common::WorkerConfigFactory::createPP0YuvConfig(
            input_path,
            params.format,
            params.width,
            params.height
        );
    }
    
    config.consumer.save_frames = params.save_frames;
    config.consumer.output_path = output_path.empty() ? 
        "/tmp/writer_" + params.description + ".raw" : output_path;
    config.consumer.verbose = true;
    
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  Writer Test: " << params.description << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  Mode:    ExecuteMode::SINGLE + CONSUME_SAVE_RAW\n";
    std::cout << "  Input:   " << input_path << "\n";
    std::cout << "  Output:  " << config.consumer.output_path << "\n";
    std::cout << "  Format:  " << params.description << "\n";
    std::cout << "  Frames:  " << params.save_frames << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    
    LOG4CPLUS_DEBUG_FMT(logger, "runSingle: mode=SINGLE, flags=CONSUME_COUNT|CONSUME_SAVE_RAW, format=%s", 
                        params.description.c_str());
    
    // 使用 BufferConsumerService，ExecuteMode::SINGLE + CONSUME_SAVE_RAW
    consumer::BufferConsumerService service;
    return service.start({config}, consumer::ExecuteMode::SINGLE, 
                        consumer::CONSUME_COUNT | consumer::CONSUME_SAVE_RAW);
}

} // namespace writer
} // namespace test
