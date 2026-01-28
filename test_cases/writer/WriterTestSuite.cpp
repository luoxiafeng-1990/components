/**
 * @file WriterTestSuite.cpp
 * @brief WriterTestSuite 实现
 */

#include "WriterTestSuite.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "../common/TestExecutor.hpp"

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
        // YUV 格式（15 个，包含 YUV400 变体）
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
        {"all_yuv",         {OutputFormat::YUV_NV12,   "All 15 YUV formats"}},
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
    
    common::TestResult result;
    
    // 检查是否是批量测试
    if (params.description == "All 12 RGB formats") {
        result = runAllRgbFormats(config.data_source.path, 
                                   output_path.empty() ? "/tmp" : output_path);
    } else if (params.description == "All 15 YUV formats") {
        result = runAllYuvFormats(config.data_source.path,
                                   output_path.empty() ? "/tmp" : output_path);
    } else {
        result = runWriterTest(config.data_source.path, params, output_path);
    }
    
    common::TestExecutor::printResult("Writer " + params.description, result);
    
    return result.success ? 0 : 1;
}

bool WriterTestSuite::parseArgs(int argc, char* argv[], WorkerConfig& config,
                                 WriterTestParams& params, std::string& output_path) {
    optind = 1;
    
    static struct option long_options[] = {
        {"help",      no_argument,       0, 'h'},
        {"list",      no_argument,       0, 'l'},
        {"input",     required_argument, 0, 'i'},
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
    while ((opt = getopt_long(argc, argv, "hli:o:f:n:RYv",
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
        params.description = "All 15 YUV formats";
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
    std::cout << "Usage: qa_cases writer [options] [test_name|input_file]\n";
    std::cout << "\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help           Show this help message\n";
    std::cout << "  -l, --list           List all predefined tests\n";
    std::cout << "  -i, --input <file>   Input video file\n";
    std::cout << "  -o, --output <path>  Output file/directory\n";
    std::cout << "  -f, --format <fmt>   Output format (nv12, rgb888, etc.)\n";
    std::cout << "  -n, --frames <n>     Number of frames to save (default: 10)\n";
    std::cout << "  -R, --all-rgb        Test all 12 RGB formats\n";
    std::cout << "  -Y, --all-yuv        Test all 15 YUV formats\n";
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
    std::cout << "  Available Writer Tests\n";
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
    std::cout << "  all_yuv             Test all 15 YUV formats\n";
    
    std::cout << "────────────────────────────────────────────────────────\n";
    std::cout << "Total: 24 predefined tests\n";
    std::cout << "\n";
}

// ========================================
// 核心测试方法实现
// ========================================

common::TestResult WriterTestSuite::runWriterTest(
    const std::string& input_path,
    const WriterTestParams& params,
    const std::string& output_path
) {
    // 创建配置 - 根据格式选择合适的工厂方法
    // RGB 格式值 >= 1000，YUV 格式值 < 1000
    WorkerConfig config;
    
    if (static_cast<int>(params.format) >= 1000) {
        // RGB 格式 - 使用 PP1 通道
        config = common::WorkerConfigFactory::createPP1RgbConfig(
            input_path,
            params.format,
            params.width,
            params.height
        );
    } else {
        // YUV 格式 - 使用 PP0 通道
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
    std::cout << "  Input:   " << input_path << "\n";
    std::cout << "  Output:  " << config.consumer.output_path << "\n";
    std::cout << "  Format:  " << params.description << "\n";
    std::cout << "  Frames:  " << params.save_frames << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    
    return common::TestExecutor::runDecode(config);
}

common::TestResult WriterTestSuite::runAllRgbFormats(
    const std::string& input_path,
    const std::string& output_dir
) {
    common::TestResult total_result;
    total_result.success = true;
    
    const auto& formats = getRgbFormats();
    int passed = 0;
    int failed = 0;
    
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  Writer Test: All 12 RGB Formats\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  Input:  " << input_path << "\n";
    std::cout << "  Output: " << output_dir << "/\n";
    std::cout << "═══════════════════════════════════════════════════════\n\n";
    
    for (const auto& [format, desc] : formats) {
        WriterTestParams params(format, desc);
        params.save_frames = 5;  // 批量测试减少帧数
        
        std::string output_file = output_dir + "/rgb_" + 
            TacoConfigBuilder::mapFormatEnumToName(format).data() + ".raw";
        
        std::cout << "Testing " << desc << "... ";
        std::cout.flush();
        
        auto result = runWriterTest(input_path, params, output_file);
        
        if (result.success) {
            std::cout << "PASSED (" << result.frames_decoded << " frames)\n";
            passed++;
        } else {
            std::cout << "FAILED: " << result.error_message << "\n";
            failed++;
            total_result.success = false;
        }
        
        total_result.frames_decoded += result.frames_decoded;
        total_result.duration_seconds += result.duration_seconds;
    }
    
    std::cout << "\n";
    std::cout << "────────────────────────────────────────────────────────\n";
    std::cout << "  RGB Format Summary: " << passed << "/" << formats.size() << " passed\n";
    std::cout << "────────────────────────────────────────────────────────\n";
    
    return total_result;
}

common::TestResult WriterTestSuite::runAllYuvFormats(
    const std::string& input_path,
    const std::string& output_dir
) {
    common::TestResult total_result;
    total_result.success = true;
    
    const auto& formats = getYuvFormats();
    int passed = 0;
    int failed = 0;
    
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  Writer Test: All 15 YUV Formats\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  Input:  " << input_path << "\n";
    std::cout << "  Output: " << output_dir << "/\n";
    std::cout << "═══════════════════════════════════════════════════════\n\n";
    
    for (const auto& [format, desc] : formats) {
        WriterTestParams params(format, desc);
        params.save_frames = 5;  // 批量测试减少帧数
        
        std::string output_file = output_dir + "/yuv_" + 
            TacoConfigBuilder::mapFormatEnumToName(format).data() + ".raw";
        
        std::cout << "Testing " << desc << "... ";
        std::cout.flush();
        
        auto result = runWriterTest(input_path, params, output_file);
        
        if (result.success) {
            std::cout << "PASSED (" << result.frames_decoded << " frames)\n";
            passed++;
        } else {
            std::cout << "FAILED: " << result.error_message << "\n";
            failed++;
            total_result.success = false;
        }
        
        total_result.frames_decoded += result.frames_decoded;
        total_result.duration_seconds += result.duration_seconds;
    }
    
    std::cout << "\n";
    std::cout << "────────────────────────────────────────────────────────\n";
    std::cout << "  YUV Format Summary: " << passed << "/" << formats.size() << " passed\n";
    std::cout << "────────────────────────────────────────────────────────\n";
    
    return total_result;
}

} // namespace writer
} // namespace test
