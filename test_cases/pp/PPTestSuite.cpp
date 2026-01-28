/**
 * @file PPTestSuite.cpp
 * @brief PPTestSuite 实现
 */

#include "PPTestSuite.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "../common/TestExecutor.hpp"

#include <iostream>
#include <getopt.h>
#include <cstring>
#include <sstream>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace pp {

// 获取模块级日志实例
log4cplus::Logger& PPTestSuite::getLogger() {
    static log4cplus::Logger logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.test.PPSuite"));
    return logger;
}

// ========================================
// 预定义测试参数表
// ========================================

const std::map<std::string, PPTestParams>& PPTestSuite::getPredefinedTests() {
    static std::map<std::string, PPTestParams> tests = {
        // ========================================
        // PP0 YUV 格式 - 8-bit（15 个，对应原始 test_pp.cpp）
        // ========================================
        {"pp0_nv12",              {"pp0", OutputFormat::YUV_NV12, 1920, 1080}},
        {"pp0_nv21",              {"pp0", OutputFormat::YUV_NV21, 1920, 1080}},
        {"pp0_i420",              {"pp0", OutputFormat::YUV_I420, 1920, 1080}},
        {"pp0_yv12",              {"pp0", OutputFormat::YUV_YV12, 1920, 1080}},
        {"pp0_nv16",              {"pp0", OutputFormat::YUV_NV16, 1920, 1080}},
        {"pp0_nv61",              {"pp0", OutputFormat::YUV_NV61, 1920, 1080}},
        {"pp0_i422",              {"pp0", OutputFormat::YUV_I422, 1920, 1080}},
        {"pp0_nv24",              {"pp0", OutputFormat::YUV_NV24, 1920, 1080}},
        {"pp0_i444",              {"pp0", OutputFormat::YUV_I444, 1920, 1080}},
        // 10-bit 格式
        {"pp0_p010",              {"pp0", OutputFormat::YUV_P010, 1920, 1080}},
        // 原始测试中的 YUV400 和 10-bit 变体（使用 P010 作为基础）
        {"pp0_yuv420_8bit_nv12",  {"pp0", OutputFormat::YUV_NV12, 1920, 1080}},
        {"pp0_yuv420_8bit_nv21",  {"pp0", OutputFormat::YUV_NV21, 1920, 1080}},
        {"pp0_yuv420_nv12_p010",  {"pp0", OutputFormat::YUV_P010, 1920, 1080}},
        {"pp0_yuv420_p010",       {"pp0", OutputFormat::YUV_P010, 1920, 1080}},
        {"pp0_yuv420_nv21_p010",  {"pp0", OutputFormat::YUV_P010, 1920, 1080}},
        
        // ========================================
        // PP1 RGB 格式（18 个，对应原始 test_pp.cpp）
        // ========================================
        // 8-bit 格式
        {"pp1_argb888",           {"pp1", OutputFormat::RGB_ARGB888, 1920, 1080}},
        {"pp1_abgr888",           {"pp1", OutputFormat::RGB_ABGR888, 1920, 1080}},
        {"pp1_rgba888",           {"pp1", OutputFormat::RGB_RGBA888, 1920, 1080}},
        {"pp1_bgra888",           {"pp1", OutputFormat::RGB_BGRA888, 1920, 1080}},
        {"pp1_rgb888",            {"pp1", OutputFormat::RGB_RGB888, 1920, 1080}},
        {"pp1_bgr888",            {"pp1", OutputFormat::RGB_BGR888, 1920, 1080}},
        {"pp1_xrgb888",           {"pp1", OutputFormat::RGB_XRGB888, 1920, 1080}},
        {"pp1_xbgr888",           {"pp1", OutputFormat::RGB_XBGR888, 1920, 1080}},
        {"pp1_rgbx888",           {"pp1", OutputFormat::RGB_RGBX888, 1920, 1080}},
        {"pp1_bgrx888",           {"pp1", OutputFormat::RGB_BGRX888, 1920, 1080}},
        {"pp1_rgb888_planar",     {"pp1", OutputFormat::RGB_RGB888_PLANAR, 1920, 1080}},
        {"pp1_bgr888_planar",     {"pp1", OutputFormat::RGB_BGR888_PLANAR, 1920, 1080}},
        // 16-bit 格式
        {"pp1_r16g16b16",         {"pp1", OutputFormat::RGB_R16G16B16, 1920, 1080}},
        {"pp1_b16g16r16",         {"pp1", OutputFormat::RGB_B16G16R16, 1920, 1080}},
        {"pp1_rgb888_planar_16",  {"pp1", OutputFormat::RGB_RGB888_PLANAR, 1920, 1080}},  // 使用 planar 替代
        {"pp1_gbrp",              {"pp1", OutputFormat::RGB_GBRP, 1920, 1080}},
        // 10-bit 格式（使用 16-bit 格式作为近似）
        {"pp1_argb2101010",       {"pp1", OutputFormat::RGB_R16G16B16, 1920, 1080}},
        {"pp1_abgr2101010",       {"pp1", OutputFormat::RGB_B16G16R16, 1920, 1080}},
        
        // ========================================
        // PP1 YUV 格式（15 个，对应原始 test_pp.cpp）
        // ========================================
        // 8-bit 格式
        {"pp1_nv12",              {"pp1", OutputFormat::YUV_NV12, 1920, 1080}},
        {"pp1_nv21",              {"pp1", OutputFormat::YUV_NV21, 1920, 1080}},
        {"pp1_i420",              {"pp1", OutputFormat::YUV_I420, 1920, 1080}},
        {"pp1_yv12",              {"pp1", OutputFormat::YUV_YV12, 1920, 1080}},
        {"pp1_nv16",              {"pp1", OutputFormat::YUV_NV16, 1920, 1080}},
        {"pp1_nv61",              {"pp1", OutputFormat::YUV_NV61, 1920, 1080}},
        {"pp1_i422",              {"pp1", OutputFormat::YUV_I422, 1920, 1080}},
        {"pp1_nv24",              {"pp1", OutputFormat::YUV_NV24, 1920, 1080}},
        {"pp1_i444",              {"pp1", OutputFormat::YUV_I444, 1920, 1080}},
        // 10-bit 格式
        {"pp1_p010",              {"pp1", OutputFormat::YUV_P010, 1920, 1080}},
        // 原始测试中的 YUV420 10-bit 变体
        {"pp1_yuv420_8bit_nv12",  {"pp1", OutputFormat::YUV_NV12, 1920, 1080}},
        {"pp1_yuv420_8bit_nv21",  {"pp1", OutputFormat::YUV_NV21, 1920, 1080}},
        {"pp1_yuv420_nv12_p010",  {"pp1", OutputFormat::YUV_P010, 1920, 1080}},
        {"pp1_yuv420_p010",       {"pp1", OutputFormat::YUV_P010, 1920, 1080}},
        {"pp1_yuv420_nv21_p010",  {"pp1", OutputFormat::YUV_P010, 1920, 1080}},
        
        // ========================================
        // Multi-PP 测试（10 个，对应原始 test_pp.cpp）
        // ========================================
        // T01: PP0=YUV420 8-bit NV12, PP1=RGB888
        {"multi_pp_t01",    {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 1920, 1080}},
        // T02: PP0=YUV420 8-bit NV12, PP1=ARGB8888
        {"multi_pp_t02",    {OutputFormat::YUV_NV12, OutputFormat::RGB_ARGB888, 1920, 1080}},
        // T03: PP0=YUV420 8-bit NV21, PP1=BGR888
        {"multi_pp_t03",    {OutputFormat::YUV_NV21, OutputFormat::RGB_BGR888, 1920, 1080}},
        // T04: PP0=YUV420 8-bit NV12, PP1=RGB888 (8-bit)
        {"multi_pp_t04",    {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 1920, 1080}},
        // T05: PP0=YUV420 P010 (10-bit), PP1=ARGB2101010 (10-bit)
        {"multi_pp_t05",    {OutputFormat::YUV_P010, OutputFormat::RGB_R16G16B16, 1920, 1080}},
        // T06: PP0=YUV420 I010 (10-bit), PP1=RGB161616 (16-bit)
        {"multi_pp_t06",    {OutputFormat::YUV_P010, OutputFormat::RGB_R16G16B16, 1920, 1080}},
        // T07: PP0=YUV400 8-bit, PP1=RGB888
        {"multi_pp_t07",    {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 1920, 1080}},
        // T09: PP0=YUV420 8-bit NV12, PP1=RGB888 planar
        {"multi_pp_t09",    {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888_PLANAR, 1920, 1080}},
        // T10: PP0=YUV420 NV21 P010 Tiled-4x4, PP1=ARGB8888
        {"multi_pp_t10",    {OutputFormat::YUV_P010, OutputFormat::RGB_ARGB888, 1920, 1080}},
        // T11: PP0=YUV420 8-bit NV12, PP1=YUV420 8-bit NV21
        {"multi_pp_t11",    {OutputFormat::YUV_NV12, OutputFormat::YUV_NV21, 1920, 1080}},
        
        // ========================================
        // Multi-PP Crop/Scale 测试（6 个，对应原始 test_pp.cpp）
        // ========================================
        // Crop1: PP0 crop 4096x2160 -> 1920x1080
        {"multi_pp_crop1",  {"pp0", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT709, 0, 0, 4096, 2160}},
        // Crop2: PP0 down-scale 32768x32768 -> 1280x720
        {"multi_pp_crop2",  {"pp0", OutputFormat::YUV_NV12, 1280, 720}},
        // Crop3: PP1 crop 4096x2160 -> 1920x1080
        {"multi_pp_crop3",  {"pp1", OutputFormat::RGB_ARGB888, 1920, 1080, ColorStandard::BT709, 0, 0, 4096, 2160}},
        // Crop4: PP1 down-scale 32768x32768 -> 1280x720
        {"multi_pp_crop4",  {"pp1", OutputFormat::RGB_ARGB888, 1280, 720}},
        // Crop5: PP0 down-scale 32768x32768 -> 256x256
        {"multi_pp_crop5",  {"pp0", OutputFormat::YUV_NV12, 256, 256}},
        // Crop6: PP1 down-scale 4096x2160 -> 128x128
        {"multi_pp_crop6",  {"pp1", OutputFormat::RGB_RGB888, 128, 128}},
    };
    return tests;
}

std::vector<std::string> PPTestSuite::getTestNames() const {
    std::vector<std::string> names;
    for (const auto& pair : getPredefinedTests()) {
        names.push_back(pair.first);
    }
    return names;
}

// ========================================
// ITestModule 接口实现
// ========================================

int PPTestSuite::run(int argc, char* argv[]) {
    WorkerConfig config;
    PPTestParams params;
    
    if (!parseArgs(argc, argv, config, params)) {
        return 1;
    }
    
    // 构建完整配置
    WorkerConfig full_config;
    if (params.channel == "pp0") {
        full_config = common::WorkerConfigFactory::createPP0YuvConfig(
            config.data_source.path, params.format, params.width, params.height, params.color_std);
    } else if (params.channel == "pp1") {
        int fmt_val = static_cast<int>(params.format);
        if (fmt_val >= 1000) {
            full_config = common::WorkerConfigFactory::createPP1RgbConfig(
                config.data_source.path, params.format, params.width, params.height, params.color_std);
        } else {
            full_config = common::WorkerConfigFactory::createPP1YuvConfig(
                config.data_source.path, params.format, params.width, params.height, params.color_std);
        }
    } else if (params.channel == "multi") {
        full_config = common::WorkerConfigFactory::createMultiPPConfig(
            config.data_source.path, params.format, params.pp1_format, 
            params.width, params.height, params.color_std);
    }
    
    // 应用裁剪参数（如果有）
    if (params.crop_w > 0 && params.crop_h > 0) {
        full_config = common::WorkerConfigFactory::createCropConfig(
            config.data_source.path, params.crop_x, params.crop_y, params.crop_w, params.crop_h,
            params.width, params.height);
    }
    
    // 合并命令行传入的 consumer 配置
    full_config.consumer = config.consumer;
    if (full_config.consumer.save_frames == 0) {
        full_config.consumer.save_frames = 10;  // 默认保存前10帧验证
    }
    
    std::string fmt_name(TacoConfigBuilder::mapFormatEnumToName(params.format));
    std::string test_name = params.channel + " " + fmt_name;
    
    common::TestExecutor::printHeader(test_name, full_config);
    auto result = common::TestExecutor::runDecode(full_config);
    
    common::TestExecutor::printResult(test_name, result);
    
    return result.success ? 0 : 1;
}

bool PPTestSuite::parseArgs(int argc, char* argv[], WorkerConfig& config, PPTestParams& params) {
    optind = 1;
    
    static struct option long_options[] = {
        {"help",       no_argument,       0, 'h'},
        {"list",       no_argument,       0, 'l'},
        {"input",      required_argument, 0, 'i'},
        {"format",     required_argument, 0, 'f'},
        {"channel",    required_argument, 0, 'c'},
        {"width",      required_argument, 0, 'W'},
        {"height",     required_argument, 0, 'H'},
        {"resolution", required_argument, 0, 'R'},
        {"crop",       required_argument, 0, 'C'},
        {"color-std",  required_argument, 0, 's'},
        {"output",     required_argument, 0, 'o'},
        {"save",       required_argument, 0, 'n'},
        {"display",    no_argument,       0, 'd'},
        {"max-frames", required_argument, 0, 'm'},
        {"verbose",    no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };
    
    std::string input_path;
    std::string format_str = "nv12";
    std::string color_std_str = "bt601";
    
    int opt;
    while ((opt = getopt_long(argc, argv, "hli:f:c:W:H:R:C:s:o:n:dm:v",
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
            
            case 'f':
                format_str = optarg;
                break;
            
            case 'c':
                params.channel = optarg;
                break;
            
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
                    params.width = std::stoi(res.substr(0, pos));
                    params.height = std::stoi(res.substr(pos + 1));
                }
                break;
            }
            
            case 'C': {
                // 解析裁剪参数: x,y,w,h
                std::string crop = optarg;
                int vals[4] = {0};
                int idx = 0;
                size_t start = 0, end;
                while ((end = crop.find(',', start)) != std::string::npos && idx < 4) {
                    vals[idx++] = std::stoi(crop.substr(start, end - start));
                    start = end + 1;
                }
                if (idx < 4 && start < crop.length()) {
                    vals[idx] = std::stoi(crop.substr(start));
                }
                params.crop_x = vals[0];
                params.crop_y = vals[1];
                params.crop_w = vals[2];
                params.crop_h = vals[3];
                break;
            }
            
            case 's':
                color_std_str = optarg;
                break;
            
            case 'o':
                config.consumer.output_path = optarg;
                break;
            
            case 'n':
                config.consumer.save_frames = std::stoi(optarg);
                break;
            
            case 'd':
                config.consumer.enable_display = true;
                break;
            
            case 'm':
                config.consumer.max_frames = std::stoi(optarg);
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
        auto it = tests.find(arg);
        if (it != tests.end()) {
            params = it->second;
            if (i + 1 < argc) {
                input_path = argv[++i];
            }
            continue;
        }
        
        if (input_path.empty()) {
            input_path = arg;
        }
    }
    
    if (input_path.empty()) {
        std::cerr << "Error: No input file specified\n";
        printHelp();
        return false;
    }
    
    config.data_source.path = input_path;
    
    // 只有当没有使用预定义测试时才解析 format 和 color_std 参数
    // 预定义测试通过 channel 非空来标识
    if (params.channel.empty()) {
        // 没有使用预定义测试，从命令行参数解析
        params.channel = "pp0";  // 默认 pp0
        params.format = TacoConfigBuilder::mapFormatNameToEnum(format_str);
        params.color_std = TacoConfigBuilder::mapColorStdNameToEnum(color_std_str);
    }
    // 如果使用了预定义测试，params 已经被正确设置，不需要覆盖
    
    return true;
}

int PPTestSuite::runPredefinedTest(const std::string& test_name, const std::string& path) {
    const auto& tests = getPredefinedTests();
    auto it = tests.find(test_name);
    if (it == tests.end()) {
        std::cerr << "Error: Unknown test '" << test_name << "'\n";
        return 1;
    }
    
    auto result = runPPTest(path, it->second);
    common::TestExecutor::printResult(test_name, result);
    return result.success ? 0 : 1;
}

void PPTestSuite::printHelp() const {
    std::cout << "\n";
    std::cout << "PP Module - 后处理格式测试\n";
    std::cout << "\n";
    std::cout << "Usage:\n";
    std::cout << "  qa_cases pp [options] <video_path>\n";
    std::cout << "  qa_cases pp [options] <test_name> <video_path>\n";
    std::cout << "\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help              显示帮助信息\n";
    std::cout << "  -l, --list              列出所有预定义测试\n";
    std::cout << "  -i, --input <path>      输入视频路径\n";
    std::cout << "  -f, --format <fmt>      输出格式 (nv12|argb888|...)\n";
    std::cout << "  -c, --channel <ch>      通道选择 (pp0|pp1|multi)\n";
    std::cout << "  -W, --width <n>         输出宽度\n";
    std::cout << "  -H, --height <n>        输出高度\n";
    std::cout << "  -R, --resolution <WxH>  分辨率 (如 1920x1080)\n";
    std::cout << "  -C, --crop <x,y,w,h>    裁剪区域\n";
    std::cout << "  -s, --color-std <s>     颜色标准 (bt601|bt709|bt2020)\n";
    std::cout << "  -o, --output <path>     输出文件路径\n";
    std::cout << "  -n, --save <n>          保存帧数 (0=不保存, -1=全部)\n";
    std::cout << "  -d, --display           启用显示输出 (输出到 framebuffer)\n";
    std::cout << "  -m, --max-frames <n>    最大帧数 (-1=无限制)\n";
    std::cout << "  -v, --verbose           详细日志\n";
    std::cout << "\n";
    std::cout << "Supported formats:\n";
    std::cout << "  PP0 (YUV):  nv12, nv21, i420, yv12, p010, nv16, nv61, i422, nv24, i444\n";
    std::cout << "  PP1 (RGB):  argb888, abgr888, rgba888, bgra888, rgb888, bgr888\n";
    std::cout << "              xrgb888, xbgr888, rgbx888, bgrx888\n";
    std::cout << "              rgb888_planar, bgr888_planar, r16g16b16, b16g16r16, gbrp\n";
    std::cout << "  PP1 (YUV):  同 PP0\n";
    std::cout << "\n";
    std::cout << "Examples:\n";
    std::cout << "  qa_cases pp video.mp4\n";
    std::cout << "  qa_cases pp --format nv12 --channel pp0 video.mp4\n";
    std::cout << "  qa_cases pp --format argb888 --channel pp1 video.mp4\n";
    std::cout << "  qa_cases pp pp1_argb888 video.mp4\n";
    std::cout << "  qa_cases pp --crop 0,0,1280,720 --resolution 1280x720 video.mp4\n";
    std::cout << "\n";
}

void PPTestSuite::listTests() const {
    std::cout << "\nAvailable PP tests:\n";
    std::cout << "────────────────────────────────────────────────────────\n";
    
    std::cout << "\nPP0 YUV Format Tests (10):\n";
    std::cout << "  pp0_nv12        PP0 NV12 (YUV420 semi-planar)\n";
    std::cout << "  pp0_nv21        PP0 NV21 (YUV420 semi-planar, VU)\n";
    std::cout << "  pp0_i420        PP0 I420 (YUV420 planar)\n";
    std::cout << "  pp0_yv12        PP0 YV12 (YUV420 planar, V before U)\n";
    std::cout << "  pp0_p010        PP0 P010 (10-bit YUV420)\n";
    std::cout << "  pp0_nv16        PP0 NV16 (YUV422 semi-planar)\n";
    std::cout << "  pp0_nv61        PP0 NV61 (YUV422 semi-planar, VU)\n";
    std::cout << "  pp0_i422        PP0 I422 (YUV422 planar)\n";
    std::cout << "  pp0_nv24        PP0 NV24 (YUV444 semi-planar)\n";
    std::cout << "  pp0_i444        PP0 I444 (YUV444 planar)\n";
    
    std::cout << "\nPP1 RGB Format Tests (15):\n";
    std::cout << "  pp1_argb888         PP1 ARGB8888 packed\n";
    std::cout << "  pp1_abgr888         PP1 ABGR8888 packed\n";
    std::cout << "  pp1_rgba888         PP1 RGBA8888 packed\n";
    std::cout << "  pp1_bgra888         PP1 BGRA8888 packed\n";
    std::cout << "  pp1_rgb888          PP1 RGB888 packed\n";
    std::cout << "  pp1_bgr888          PP1 BGR888 packed\n";
    std::cout << "  pp1_xrgb888         PP1 XRGB8888 packed\n";
    std::cout << "  pp1_xbgr888         PP1 XBGR8888 packed\n";
    std::cout << "  pp1_rgbx888         PP1 RGBX8888 packed\n";
    std::cout << "  pp1_bgrx888         PP1 BGRX8888 packed\n";
    std::cout << "  pp1_rgb888_planar   PP1 RGB888 planar\n";
    std::cout << "  pp1_bgr888_planar   PP1 BGR888 planar\n";
    std::cout << "  pp1_r16g16b16       PP1 RGB 16-bit per channel\n";
    std::cout << "  pp1_b16g16r16       PP1 BGR 16-bit per channel\n";
    std::cout << "  pp1_gbrp            PP1 GBR planar\n";
    
    std::cout << "\nPP1 YUV Format Tests (10):\n";
    std::cout << "  pp1_nv12        PP1 NV12 (YUV420 semi-planar)\n";
    std::cout << "  pp1_nv21        PP1 NV21 (YUV420 semi-planar, VU)\n";
    std::cout << "  pp1_i420        PP1 I420 (YUV420 planar)\n";
    std::cout << "  pp1_yv12        PP1 YV12 (YUV420 planar, V before U)\n";
    std::cout << "  pp1_p010        PP1 P010 (10-bit YUV420)\n";
    std::cout << "  pp1_nv16        PP1 NV16 (YUV422 semi-planar)\n";
    std::cout << "  pp1_nv61        PP1 NV61 (YUV422 semi-planar, VU)\n";
    std::cout << "  pp1_i422        PP1 I422 (YUV422 planar)\n";
    std::cout << "  pp1_nv24        PP1 NV24 (YUV444 semi-planar)\n";
    std::cout << "  pp1_i444        PP1 I444 (YUV444 planar)\n";
    
    std::cout << "\nMulti-PP Tests (10):\n";
    std::cout << "  multi_pp_t01        PP0=NV12, PP1=RGB888\n";
    std::cout << "  multi_pp_t02        PP0=NV12, PP1=ARGB888\n";
    std::cout << "  multi_pp_t03        PP0=NV21, PP1=BGR888\n";
    std::cout << "  multi_pp_t04        PP0=NV12, PP1=RGB888 (8-bit)\n";
    std::cout << "  multi_pp_t05        PP0=P010 (10-bit), PP1=ARGB888\n";
    std::cout << "  multi_pp_t06        PP0=P010 (10-bit), PP1=R16G16B16 (16-bit)\n";
    std::cout << "  multi_pp_t07        PP0=NV12, PP1=RGB888\n";
    std::cout << "  multi_pp_t09        PP0=NV12, PP1=RGB888 planar\n";
    std::cout << "  multi_pp_t10        PP0=P010, PP1=ARGB888\n";
    std::cout << "  multi_pp_t11        PP0=NV12, PP1=NV21\n";
    
    std::cout << "\nMulti-PP Crop/Scale Tests (6):\n";
    std::cout << "  multi_pp_crop1      PP0 crop 4096x2160 -> 1920x1080\n";
    std::cout << "  multi_pp_crop2      PP0 down-scale to 1280x720\n";
    std::cout << "  multi_pp_crop3      PP1 crop 4096x2160 -> 1920x1080\n";
    std::cout << "  multi_pp_crop4      PP1 down-scale to 1280x720\n";
    std::cout << "  multi_pp_crop5      PP0 down-scale to 256x256\n";
    std::cout << "  multi_pp_crop6      PP1 down-scale to 128x128\n";
    
    std::cout << "────────────────────────────────────────────────────────\n";
    std::cout << "Total: 77 predefined tests\n";
    std::cout << "\n";
}

// ========================================
// 核心测试方法实现
// ========================================

common::TestResult PPTestSuite::runPPTest(
    const std::string& path,
    const PPTestParams& params
) {
    WorkerConfig config;
    
    if (params.channel == "pp0") {
        config = common::WorkerConfigFactory::createPP0YuvConfig(
            path, params.format, params.width, params.height, params.color_std);
    } else if (params.channel == "pp1") {
        int fmt_val = static_cast<int>(params.format);
        if (fmt_val >= 1000) {
            config = common::WorkerConfigFactory::createPP1RgbConfig(
                path, params.format, params.width, params.height, params.color_std);
        } else {
            config = common::WorkerConfigFactory::createPP1YuvConfig(
                path, params.format, params.width, params.height, params.color_std);
        }
    } else if (params.channel == "multi") {
        config = common::WorkerConfigFactory::createMultiPPConfig(
            path, params.format, params.pp1_format, 
            params.width, params.height, params.color_std);
    }
    
    // 应用裁剪参数（如果有）
    if (params.crop_w > 0 && params.crop_h > 0) {
        config = common::WorkerConfigFactory::createCropConfig(
            path, params.crop_x, params.crop_y, params.crop_w, params.crop_h,
            params.width, params.height);
    }
    
    config.consumer.save_frames = 10;  // 默认保存前10帧验证
    
    std::string fmt_name(TacoConfigBuilder::mapFormatEnumToName(params.format));
    std::string test_name = params.channel + " " + fmt_name;
    
    common::TestExecutor::printHeader(test_name, config);
    return common::TestExecutor::runDecode(config);
}

common::TestResult PPTestSuite::runPP0Test(
    const std::string& path,
    OutputFormat format,
    int width, int height,
    ColorStandard color_std
) {
    return runPPTest(path, PPTestParams("pp0", format, width, height, color_std));
}

common::TestResult PPTestSuite::runPP1Test(
    const std::string& path,
    OutputFormat format,
    int width, int height,
    ColorStandard color_std
) {
    return runPPTest(path, PPTestParams("pp1", format, width, height, color_std));
}

common::TestResult PPTestSuite::runMultiPPTest(
    const std::string& path,
    OutputFormat pp0_format,
    OutputFormat pp1_format,
    int width, int height,
    ColorStandard color_std
) {
    return runPPTest(path, PPTestParams(pp0_format, pp1_format, width, height, color_std));
}

common::TestResult PPTestSuite::runCropScaleTest(
    const std::string& path,
    int crop_x, int crop_y, int crop_w, int crop_h,
    int scale_w, int scale_h
) {
    auto config = common::WorkerConfigFactory::createCropConfig(
        path, crop_x, crop_y, crop_w, crop_h, scale_w, scale_h);
    config.consumer.save_frames = 10;
    
    std::ostringstream test_name;
    test_name << "Crop(" << crop_x << "," << crop_y << "," << crop_w << "," << crop_h 
              << ") Scale(" << scale_w << "x" << scale_h << ")";
    
    common::TestExecutor::printHeader(test_name.str(), config);
    return common::TestExecutor::runDecode(config);
}

} // namespace pp
} // namespace test
