/**
 * @file PPTestSuite.cpp
 * @brief PPTestSuite 实现
 * 
 * 重构为 ExecuteMode 风格，与 BufferConsumerService 架构对齐
 */

#include "PPTestSuite.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "productionline/io/BufferConsumerService.hpp"
#include "productionline/io/BufferConsumerStrategies.hpp"
#include "productionline/VideoProductionLine.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"

#include <iostream>
#include <getopt.h>
#include <cstring>
#include <sstream>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace pp {

// ========================================
// 辅助函数：解析逗号分隔的列表
// ========================================

/// 解析逗号分隔的字符串列表
static std::vector<std::string> parseStringList(const std::string& str) {
    std::vector<std::string> result;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
        // 去除首尾空格
        size_t start = item.find_first_not_of(" \t");
        size_t end = item.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos) {
            result.push_back(item.substr(start, end - start + 1));
        } else if (!item.empty()) {
            result.push_back(item);
        }
    }
    return result;
}

/// 解析通道参数（支持 "0", "1", "0,1" 等格式）
/// v2.27: 移除旧格式 "pp0", "pp1", "multi"，统一使用数字格式
static std::vector<int> parseChannels(const std::string& str) {
    std::vector<int> result;
    for (const auto& s : parseStringList(str)) {
        try {
            result.push_back(std::stoi(s));
        } catch (...) {
            // 忽略无效的数字
        }
    }
    return result.empty() ? std::vector<int>{0} : result;  // 默认通道 0
}

/// 解析格式参数（支持 "nv12", "nv12,rgb888" 等格式）
static std::vector<OutputFormat> parseFormats(const std::string& str) {
    std::vector<OutputFormat> result;
    for (const auto& s : parseStringList(str)) {
        result.push_back(TacoConfigBuilder::mapFormatNameToEnum(s));
    }
    return result.empty() ? std::vector<OutputFormat>{OutputFormat::YUV_NV12} : result;
}

/// 解析整数列表（支持 "200", "100,200" 等格式）
static std::vector<int> parseIntList(const std::string& str) {
    std::vector<int> result;
    for (const auto& s : parseStringList(str)) {
        try {
            result.push_back(std::stoi(s));
        } catch (...) {
            // 忽略无效的数字
        }
    }
    return result;
}

// 获取模块级日志实例

// ========================================
// 辅助函数：从 WorkerConfig 构建消费标志
// ========================================
uint32_t PPTestSuite::buildConsumeFlags(const WorkerConfig& config) {
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
    
    // 生成测试名称
    std::ostringstream test_name;
    test_name << params.getChannelString();
    for (size_t i = 0; i < params.formats.size(); ++i) {
        test_name << (i == 0 ? " " : ",") 
                  << TacoConfigBuilder::mapFormatEnumToName(params.formats[i]);
    }
    test_name << " (" << params.width << "x" << params.height << ")";
    
    // ========================================
    // 根据配置字段判断执行模式
    // ========================================
    
    // COMPARE 模式：PSNR/SSIM 质量验证
    if (config.consumer_type.compare.enable_psnr || config.consumer_type.compare.enable_ssim) {
        
        // ⭐ v2.27: 多通道 + PSNR/SSIM → 通道比较模式（pp0 vs pp1）
        if (params.channels.size() >= 2) {
            LOG4CPLUS_INFO_FMT(getLogger(), 
                "Channel Compare Mode: ch%d vs ch%d", 
                params.channels[0], params.channels[1]);
            
            // 构建多通道配置
            WorkerConfig full_config = buildConfig(config.data_source.path, params);
            full_config.consumer_type = config.consumer_type;
            
            // 设置通道比较参数
            full_config.consumer_type.compare.enable_channel_compare = true;
            full_config.consumer_type.compare.reference_channel = params.channels[0];
            full_config.consumer_type.compare.compare_channel = params.channels[1];
            
            // 启动生产线
            VideoProductionLine producer;
            if (!producer.start(full_config)) {
                LOG4CPLUS_ERROR(getLogger(), "Failed to start production line");
                return 1;
            }
            
            // 获取 BufferPool
            auto pool_id = producer.getWorkingBufferPoolId();
            auto pool = BufferPoolRegistry::getInstance().getPool(pool_id).lock();
            if (!pool) {
                LOG4CPLUS_ERROR(getLogger(), "Failed to get BufferPool");
                producer.stop();
                return 1;
            }
            
            // 创建通道比较消费者
            auto consumer = std::make_shared<consumer::ChannelCompareConsumer>(
                pool, full_config.consumer_type.compare);
            
            // 运行比较
            int max_frames = full_config.consumer_type.max_frames;
            consumer->run(max_frames);
            
            // 停止生产线
            producer.stop();
            
            // 输出结果
            LOG4CPLUS_INFO_FMT(getLogger(), "Channel Compare Result: %s", 
                consumer->getStats().c_str());
            
            std::cout << "\n" << test_name.str() << " (CHANNEL_COMPARE): "
                      << (consumer->isPassed() ? "PASSED" : "FAILED") << "\n"
                      << "  Compared: " << consumer->getComparedCount() << " frames\n"
                      << "  Avg PSNR: " << consumer->getAveragePsnr() << " dB\n"
                      << "  Avg SSIM: " << consumer->getAverageSsim() << "\n"
                      << "  Mismatch: " << consumer->getMismatchCount() << "\n";
            
            return consumer->isPassed() ? 0 : 1;
        }
        
        // 单通道 + PSNR/SSIM → HW vs SW 比较模式
        // 构建 HW PP 配置
        auto hw_config = buildConfig(config.data_source.path, params);
        hw_config.consumer_type = config.consumer_type;
        
        // 构建 SW 配置（软件解码作为参考）
        auto sw_config = common::WorkerConfigFactory::createSoftwareDecode(
            config.data_source.path, params.width, params.height);
        sw_config.consumer_type = config.consumer_type;
        
        // COMPARE 模式也支持叠加其他消费类型（display、save）
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
    
    // SINGLE 模式：默认单路消费
    WorkerConfig full_config = buildConfig(config.data_source.path, params);
    full_config.consumer_type = config.consumer_type;
    
    uint32_t flags = buildConsumeFlags(full_config);
    
    auto result = runSingle(full_config, flags, test_name.str());
    consumer::BufferConsumerService::printResult(test_name.str(), result);
    
    return result.success ? 0 : 1;
}

bool PPTestSuite::parseArgs(int argc, char* argv[], WorkerConfig& config, PPTestParams& params) {
    optind = 1;
    
    static struct option long_options[] = {
        {"help",       no_argument,       0, 'h'},
        {"list",       no_argument,       0, 'l'},
        {"input",      required_argument, 0, 'i'},
        {"decoder",    required_argument, 0, 'D'},
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
        {"psnr",       no_argument,       0, 'p'},
        {"ssim",       no_argument,       0, 'S'},
        {"min-psnr",   required_argument, 0, 'P'},
        {"min-ssim",   required_argument, 0, 'M'},
        {"verbose",    no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };
    
    std::string input_path;
    std::string format_str = "nv12";
    std::string channel_str;        // 通道参数（命令行）
    std::string color_std_str = "bt601";
    
    int opt;
    while ((opt = getopt_long(argc, argv, "hli:D:f:c:W:H:R:C:s:o:n:dm:pSP:M:v",
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
                    LOG4CPLUS_ERROR_FMT(getLogger(), 
                        "Invalid decoder type '%s', use 'hw' or 'sw'", optarg);
                    return false;
                }
                break;
            }
            
            case 'f':
                format_str = optarg;
                params.formats = parseFormats(optarg);
                break;
            
            case 'c':
                channel_str = optarg;
                params.channels = parseChannels(optarg);
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
                config.consumer_type.save_raw.enable = true;  // 指定路径即启用保存
                config.consumer_type.save_raw.output_paths = parseStringList(optarg);
                break;
            
            case 'n':
                config.consumer_type.save_raw.max_frames_per_channel = parseIntList(optarg);
                break;
            
            case 'd':
                config.consumer_type.display.enable = true;
                break;
            
            case 'm':
                config.consumer_type.max_frames = std::stoi(optarg);
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
        } else {
            LOG4CPLUS_WARN_FMT(getLogger(), 
                "Extra positional argument ignored: '%s' (input already set to: '%s')",
                arg.c_str(), input_path.c_str());
        }
    }
    
    if (input_path.empty()) {
        LOG4CPLUS_ERROR(getLogger(), "No input file specified");
        printHelp();
        return false;
    }
    
    config.data_source.path = input_path;
    
    // 只有当没有使用预定义测试时才解析 channel、format 和 color_std 参数
    // 预定义测试通过 channels 非空来标识
    if (params.channels.empty()) {
        // 没有使用预定义测试，从命令行参数解析
        if (channel_str.empty()) {
            params.channels = {0};  // 默认通道 0
        }
        if (params.formats.empty()) {
            params.formats = parseFormats(format_str);
        }
        params.color_std = TacoConfigBuilder::mapColorStdNameToEnum(color_std_str);
        
        // 提示用户使用了默认配置
        std::string channels_info;
        for (size_t i = 0; i < params.channels.size(); ++i) {
            if (i > 0) channels_info += ",";
            channels_info += std::to_string(params.channels[i]);
        }
        std::string formats_info;
        for (size_t i = 0; i < params.formats.size(); ++i) {
            if (i > 0) formats_info += ",";
            formats_info += std::string(TacoConfigBuilder::mapFormatEnumToName(params.formats[i]));
        }
        LOG4CPLUS_INFO_FMT(getLogger(), 
            "Using PP configuration: channels=[%s], formats=[%s] (use -c and -f to customize)",
            channels_info.c_str(), formats_info.c_str());
    }
    // 如果使用了预定义测试，params 已经被正确设置，不需要覆盖
    
    return true;
}

const std::map<std::string, PPTestParams>& PPTestSuite::getPredefinedTests() {
    static std::map<std::string, PPTestParams> tests = {
        // ════════════════════════════════════════════════════════════════════
        // PP0 YUV 格式（15 种，对应 lfl 分支 test_decode.cpp 定义）
        // 注：YUV400 系列使用相同的底层格式，只是语义不同
        // ════════════════════════════════════════════════════════════════════
        // YUV400 系列 (灰度 10-bit)
        {"pp0_yuv400_p010",       {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv400_i010",       {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv400_l010",       {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv400_pack10",     {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV400 8-bit (灰度 8-bit)
        {"pp0_yuv400_8bit",       {"0", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT601}},
        // YUV420 NV12 10-bit 系列
        {"pp0_yuv420_nv12_p010",  {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv420_nv12_i010",  {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv420_nv12_l010",  {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv420_nv12_pack10",{"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 8-bit NV12
        {"pp0_yuv420_8bit_nv12",  {"0", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT601}},
        // YUV420 NV21 10-bit 系列
        {"pp0_yuv420_nv21_p010_tiled", {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv420_nv21_i011",  {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv420_nv21_l010",  {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 P010
        {"pp0_yuv420_p010",       {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 8-bit NV21
        {"pp0_yuv420_8bit_nv21",  {"0", OutputFormat::YUV_NV21, 1920, 1080, ColorStandard::BT601}},
        
        // 便捷别名（兼容旧测试名）
        {"pp0_nv12",              {"0", OutputFormat::YUV_NV12, 1920, 1080}},
        {"pp0_nv21",              {"0", OutputFormat::YUV_NV21, 1920, 1080}},
        {"pp0_i420",              {"0", OutputFormat::YUV_I420, 1920, 1080}},
        {"pp0_yv12",              {"0", OutputFormat::YUV_YV12, 1920, 1080}},
        {"pp0_p010",              {"0", OutputFormat::YUV_P010, 1920, 1080}},
        {"pp0_nv16",              {"0", OutputFormat::YUV_NV16, 1920, 1080}},
        {"pp0_nv61",              {"0", OutputFormat::YUV_NV61, 1920, 1080}},
        {"pp0_i422",              {"0", OutputFormat::YUV_I422, 1920, 1080}},
        {"pp0_nv24",              {"0", OutputFormat::YUV_NV24, 1920, 1080}},
        {"pp0_i444",              {"0", OutputFormat::YUV_I444, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // PP1 RGB 格式（18 种，对应 lfl 分支 test_decode.cpp 定义）
        // ════════════════════════════════════════════════════════════════════
        // RGB 10-bit 系列（使用 16-bit 格式实现）
        {"pp1_argb2101010",       {"1", OutputFormat::RGB_ARGB888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_abgr2101010",       {"1", OutputFormat::RGB_ABGR888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgra2101010",       {"1", OutputFormat::RGB_BGRA888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_rgba2101010",       {"1", OutputFormat::RGB_RGBA888, 1920, 1080, ColorStandard::BT601}},
        // RGB 8-bit 系列 - packed
        {"pp1_abgr8888",          {"1", OutputFormat::RGB_ABGR888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_argb8888",          {"1", OutputFormat::RGB_ARGB888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgr888",            {"1", OutputFormat::RGB_BGR888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgra8888",          {"1", OutputFormat::RGB_BGRA888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgrx8888",          {"1", OutputFormat::RGB_BGRX888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_rgb888",            {"1", OutputFormat::RGB_RGB888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_rgba8888",          {"1", OutputFormat::RGB_RGBA888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_rgbx8888",          {"1", OutputFormat::RGB_RGBX888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_xrgb8888",          {"1", OutputFormat::RGB_XRGB888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_xbgr8888",          {"1", OutputFormat::RGB_XBGR888, 1920, 1080, ColorStandard::BT601}},
        // RGB 8-bit 系列 - planar
        {"pp1_rgb888_planar",     {"1", OutputFormat::RGB_RGB888_PLANAR, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgr888_planar",     {"1", OutputFormat::RGB_BGR888_PLANAR, 1920, 1080, ColorStandard::BT601}},
        // RGB 16-bit 系列
        {"pp1_rgb161616",         {"1", OutputFormat::RGB_R16G16B16, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgr161616",         {"1", OutputFormat::RGB_B16G16R16, 1920, 1080, ColorStandard::BT601}},
        {"pp1_rgb161616_planar",  {"1", OutputFormat::RGB_GBRP, 1920, 1080, ColorStandard::BT601}},
        
        // 便捷别名（兼容旧测试名）
        {"pp1_argb888",           {"1", OutputFormat::RGB_ARGB888, 1920, 1080}},
        {"pp1_abgr888",           {"1", OutputFormat::RGB_ABGR888, 1920, 1080}},
        {"pp1_rgba888",           {"1", OutputFormat::RGB_RGBA888, 1920, 1080}},
        {"pp1_bgra888",           {"1", OutputFormat::RGB_BGRA888, 1920, 1080}},
        {"pp1_r16g16b16",         {"1", OutputFormat::RGB_R16G16B16, 1920, 1080}},
        {"pp1_b16g16r16",         {"1", OutputFormat::RGB_B16G16R16, 1920, 1080}},
        {"pp1_gbrp",              {"1", OutputFormat::RGB_GBRP, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // PP1 YUV 格式（16 种，对应 lfl 分支 test_decode.cpp 定义）
        // ════════════════════════════════════════════════════════════════════
        // YUV400 系列 (灰度 10-bit)
        {"pp1_yuv400_p010",       {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv400_i010",       {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv400_l010",       {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv400_pack10",     {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV400 8-bit (灰度 8-bit)
        {"pp1_yuv400_8bit",       {"1", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT601}},
        // YUV420 NV12 10-bit 系列
        {"pp1_yuv420_nv12_p010",  {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv420_nv12_i010",  {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv420_nv12_l010",  {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv420_nv12_pack10",{"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 8-bit NV12
        {"pp1_yuv420_8bit_nv12",  {"1", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT601}},
        // YUV420 NV21 10-bit 系列
        {"pp1_yuv420_nv21_p010_tiled", {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv420_nv21_i011",  {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv420_nv21_l010",  {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 P010
        {"pp1_yuv420_p010",       {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 8-bit NV21
        {"pp1_yuv420_8bit_nv21",  {"1", OutputFormat::YUV_NV21, 1920, 1080, ColorStandard::BT601}},
        // YUV420 NV21 P010（第16种，PP1比PP0多一种）
        {"pp1_yuv420_nv21_p010",  {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        
        // 便捷别名（兼容旧测试名）
        {"pp1_nv12",              {"1", OutputFormat::YUV_NV12, 1920, 1080}},
        {"pp1_nv21",              {"1", OutputFormat::YUV_NV21, 1920, 1080}},
        {"pp1_i420",              {"1", OutputFormat::YUV_I420, 1920, 1080}},
        {"pp1_yv12",              {"1", OutputFormat::YUV_YV12, 1920, 1080}},
        {"pp1_p010",              {"1", OutputFormat::YUV_P010, 1920, 1080}},
        {"pp1_nv16",              {"1", OutputFormat::YUV_NV16, 1920, 1080}},
        {"pp1_nv61",              {"1", OutputFormat::YUV_NV61, 1920, 1080}},
        {"pp1_i422",              {"1", OutputFormat::YUV_I422, 1920, 1080}},
        {"pp1_nv24",              {"1", OutputFormat::YUV_NV24, 1920, 1080}},
        {"pp1_i444",              {"1", OutputFormat::YUV_I444, 1920, 1080}},
        
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
        // Multi-PP Crop/Scale 测试（8 个，对应 lfl 分支 test_decode.cpp）
        // ========================================
        // Crop 测试（4 个）
        // Crop1: PP0 crop 4096x2160 -> 1920x1080
        {"multi_pp_crop1",  {"0", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT709, 0, 0, 4096, 2160}},
        // Crop2: PP0 crop 32768x32768 -> 1280x720
        {"multi_pp_crop2",  {"0", OutputFormat::YUV_NV12, 1280, 720, ColorStandard::BT709, 0, 0, 32768, 32768}},
        // Crop3: PP1 crop 4096x2160 -> 1920x1080
        {"multi_pp_crop3",  {"1", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT709, 0, 0, 4096, 2160}},
        // Crop4: PP1 crop 32768x32768 -> 1280x720
        {"multi_pp_crop4",  {"1", OutputFormat::YUV_NV12, 1280, 720, ColorStandard::BT709, 0, 0, 32768, 32768}},
        
        // Scale 测试（4 个）
        // Scale1: PP0 down-scale 32768x32768 -> 256x256
        {"multi_pp_scale1", {"0", OutputFormat::YUV_NV12, 256, 256, ColorStandard::BT709}},
        // Scale2: PP1 down-scale 4096x2160 -> 128x128
        {"multi_pp_scale2", {"1", OutputFormat::YUV_NV12, 128, 128, ColorStandard::BT709}},
        // Scale3: PP0+PP1 双通道 down-scale 32768x32768 -> 256x256
        {"multi_pp_scale3", {OutputFormat::YUV_NV12, OutputFormat::YUV_NV12, 256, 256, ColorStandard::BT709}},
        // Scale4: PP0+PP1 双通道 down-scale 4096x2160 -> 128x128
        {"multi_pp_scale4", {OutputFormat::YUV_NV12, OutputFormat::YUV_NV12, 128, 128, ColorStandard::BT709}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - PP Crop 测试（带分辨率）
        // ════════════════════════════════════════════════════════════════════
        // PP0 Crop
        {"pp0_720p_crop",           {"0", OutputFormat::YUV_NV12, 1280, 720, ColorStandard::BT709, 0, 0, 1280, 720}},
        {"pp0_1080p_crop",          {"0", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT709, 0, 0, 1920, 1080}},
        // PP1 RGB Crop
        {"pp1_720p_rgb_crop",       {"1", OutputFormat::RGB_RGB888, 1280, 720, ColorStandard::BT709, 0, 0, 1280, 720}},
        {"pp1_1080p_rgb_crop",      {"1", OutputFormat::RGB_RGB888, 1920, 1080, ColorStandard::BT709, 0, 0, 1920, 1080}},
        // PP1 YUV Crop
        {"pp1_720p_yuv_crop",       {"1", OutputFormat::YUV_NV12, 1280, 720, ColorStandard::BT709, 0, 0, 1280, 720}},
        {"pp1_1080p_yuv_crop",      {"1", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT709, 0, 0, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - Multi-PP 扩展测试
        // ════════════════════════════════════════════════════════════════════
        {"multi_pp",                {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 1920, 1080}},
        {"multi_pp_crop",           {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 1920, 1080, ColorStandard::BT709}},
        {"multi_pp_scale",          {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 960, 540, ColorStandard::BT709}},
        {"multi_pp_crop_scale",     {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 960, 540, ColorStandard::BT709}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - PP0 带分辨率的格式测试
        // ════════════════════════════════════════════════════════════════════
        {"pp0_720p_nv12",           {"0", OutputFormat::YUV_NV12, 1280, 720}},
        {"pp0_720p_p010",           {"0", OutputFormat::YUV_P010, 1280, 720}},
        {"pp0_1080p_nv21",          {"0", OutputFormat::YUV_NV21, 1920, 1080}},
        {"pp0_4k_nv12",             {"0", OutputFormat::YUV_NV12, 3840, 2160}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - PP1 带分辨率的格式测试
        // ════════════════════════════════════════════════════════════════════
        {"pp1_720p_argb8888",       {"1", OutputFormat::RGB_ARGB888, 1280, 720}},
        {"pp1_4k_argb8888",         {"1", OutputFormat::RGB_ARGB888, 3840, 2160}},
        {"pp1_720p_rgb888",         {"1", OutputFormat::RGB_RGB888, 1280, 720}},
        {"pp1_1080p_argb8888",      {"1", OutputFormat::RGB_ARGB888, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - Crop 带输出分辨率
        // ════════════════════════════════════════════════════════════════════
        {"crop_720p_1024x576",      {"0", OutputFormat::YUV_NV12, 1024, 576, ColorStandard::BT709, 0, 0, 1280, 720}},
        {"crop_1080p_1600x900",     {"0", OutputFormat::YUV_NV12, 1600, 900, ColorStandard::BT709, 0, 0, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - Scale 带输出分辨率
        // ════════════════════════════════════════════════════════════════════
        {"scale_720p_512x288",      {"0", OutputFormat::YUV_NV12, 512, 288, ColorStandard::BT709}},
        {"scale_1080p_800x450",     {"0", OutputFormat::YUV_NV12, 800, 450, ColorStandard::BT709}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - H265 PP0/PP1 格式测试
        // ════════════════════════════════════════════════════════════════════
        {"pp0_h265_720p_nv12",      {"0", OutputFormat::YUV_NV12, 1280, 720}},
        {"pp0_h265_1080p_p010",     {"0", OutputFormat::YUV_P010, 1920, 1080}},
        {"pp0_h265_4k_nv12",        {"0", OutputFormat::YUV_NV12, 3840, 2160}},
        {"pp1_h265_720p_rgb888",    {"1", OutputFormat::RGB_RGB888, 1280, 720}},
        {"pp1_h265_1080p_argb8888", {"1", OutputFormat::RGB_ARGB888, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - H265 Crop+Scale
        // ════════════════════════════════════════════════════════════════════
        {"h265_1080p_crop_scale",   {"0", OutputFormat::YUV_NV12, 960, 540, ColorStandard::BT709, 0, 0, 1920, 1080}},
    };
    return tests;
}

int PPTestSuite::runPredefinedTest(const std::string& test_name, const std::string& path) {
    const auto& tests = getPredefinedTests();
    auto it = tests.find(test_name);
    if (it == tests.end()) {
        LOG4CPLUS_ERROR_FMT(getLogger(), "Unknown test '%s'", test_name.c_str());
        return 1;
    }
    
    // 构建配置
    WorkerConfig config = buildConfig(path, it->second);
    config.consumer_type.save_raw.enable = true;
    config.consumer_type.save_raw.max_frames_per_channel = {10};  // 默认保存前10帧验证
    
    uint32_t flags = consumer::CONSUME_COUNT | consumer::CONSUME_SAVE_RAW;
    
    // 使用基类 runSingle（ExecuteMode::SINGLE）
    auto result = runSingle(config, flags, test_name);
    consumer::BufferConsumerService::printResult(test_name, result);
    return result.success ? 0 : 1;
}


// ========================================
// 核心测试方法实现（与 ExecuteMode 对齐）
// ========================================

WorkerConfig PPTestSuite::buildConfig(const std::string& path, const PPTestParams& params) {
    WorkerConfig config;
    
    // 软件解码模式：不使用硬件 PP，直接解码
    if (!params.use_hardware) {
        config = common::WorkerConfigFactory::createSoftwareDecode(
            path, params.width, params.height);
        LOG4CPLUS_WARN(getLogger(), 
            "Software decode mode: Hardware PP features are not available");
        return config;
    }
    
    // 硬件解码模式：使用 TACO PP
    // 根据 channels 列表决定使用哪种配置
    if (params.channels.empty() || (params.channels.size() == 1 && params.channels[0] == 0)) {
        // 单通道 PP0
        config = common::WorkerConfigFactory::createPP0YuvConfig(
            path, params.getFormat(0), params.width, params.height, params.color_std);
    } else if (params.channels.size() == 1 && params.channels[0] == 1) {
        // 单通道 PP1
        OutputFormat fmt = params.getFormat(0);
        int fmt_val = static_cast<int>(fmt);
        if (fmt_val >= 1000) {
            config = common::WorkerConfigFactory::createPP1RgbConfig(
                path, fmt, params.width, params.height, params.color_std);
        } else {
            config = common::WorkerConfigFactory::createPP1YuvConfig(
                path, fmt, params.width, params.height, params.color_std);
        }
    } else if (params.isMultiChannel()) {
        // 多通道模式
        OutputFormat pp0_fmt = params.getFormat(0);
        OutputFormat pp1_fmt = params.getFormat(1);
        config = common::WorkerConfigFactory::createMultiPPConfig(
            path, pp0_fmt, pp1_fmt, 
            params.width, params.height, params.color_std);
    }
    
    // 应用裁剪参数（如果有）
    if (params.crop_w > 0 && params.crop_h > 0) {
        config = common::WorkerConfigFactory::createCropConfig(
            path, params.crop_x, params.crop_y, params.crop_w, params.crop_h,
            params.width, params.height);
    }
    
    return config;
}

void PPTestSuite::printHelp() const {
    std::cout << "\n"
              << "PP Module - 后处理格式测试\n"
              << "\n"
              << "Usage:\n"
              << "  qa_cases pp [options] <video_path>\n"
              << "  qa_cases pp [options] <test_name> <video_path>\n"
              << "\n"
              << "Options:\n"
              << "  -h, --help              显示帮助信息\n"
              << "  -l, --list              列出所有预定义测试\n"
              << "  -i, --input <path>      输入视频路径\n"
              << "  -D, --decoder <type>    解码方式 (hw|hardware|sw|software，默认: hw)\n"
              << "  -f, --format <fmt>      输出格式 (nv12|argb888|...), 多通道用逗号分隔\n"
              << "  -c, --channel <ch>      通道选择 (0|1|0,1), 多通道用逗号分隔\n"
              << "  -W, --width <n>         输出宽度\n"
              << "  -H, --height <n>        输出高度\n"
              << "  -R, --resolution <WxH>  分辨率 (如 1920x1080)\n"
              << "  -C, --crop <x,y,w,h>    裁剪区域\n"
              << "  -s, --color-std <s>     颜色标准 (bt601|bt709|bt2020)\n"
              << "  -o, --output <path>     输出文件路径\n"
              << "  -n, --save <n>          保存帧数 (0=不保存, -1=全部)\n"
              << "  -d, --display           启用显示输出 (CONSUME_DISPLAY)\n"
              << "  -m, --max-frames <n>    最大帧数 (-1=无限制)\n"
              << "  -p, --psnr              启用 PSNR 验证 (ExecuteMode::COMPARE)\n"
              << "  -S, --ssim              启用 SSIM 验证 (ExecuteMode::COMPARE)\n"
              << "  -P, --min-psnr <n>      PSNR 阈值 (默认: 30.0 dB)\n"
              << "  -M, --min-ssim <n>      SSIM 阈值 (默认: 0.95)\n"
              << "  -v, --verbose           详细日志\n"
              << "\n"
              << "ExecuteMode Mapping:\n"
              << "  SINGLE          - 默认单路 PP 处理\n"
              << "  COMPARE (HW/SW) - 单通道 + --psnr/--ssim 时，HW PP vs SW 对比\n"
              << "  COMPARE (CH)    - 多通道 + --psnr/--ssim 时，通道间对比 (如 ch0 vs ch1)\n"
              << "\n"
              << "Supported formats:\n"
              << "  PP0 (YUV):  nv12, nv21, i420, yv12, p010, nv16, nv61, i422, nv24, i444\n"
              << "  PP1 (RGB):  argb888, abgr888, rgba888, bgra888, rgb888, bgr888\n"
              << "              xrgb888, xbgr888, rgbx888, bgrx888\n"
              << "              rgb888_planar, bgr888_planar, r16g16b16, b16g16r16, gbrp\n"
              << "  PP1 (YUV):  同 PP0\n"
              << "\n"
              << "Examples:\n"
              << "  qa_cases pp video.mp4                                      # SINGLE (ch0)\n"
              << "  qa_cases pp --channel 0 --format nv12 video.mp4            # SINGLE ch0\n"
              << "  qa_cases pp --channel 1 --format argb888 video.mp4         # SINGLE ch1\n"
              << "  qa_cases pp --display video.mp4                            # SINGLE + DISPLAY\n"
              << "  qa_cases pp --psnr video.mp4                               # HW vs SW 比较\n"
              << "  qa_cases pp --channel 0,1 --psnr video.mp4                 # 通道比较 ch0 vs ch1\n"
              << "  qa_cases pp --channel 0,1 --format nv12,argb888 --psnr video.mp4\n"
              << "  qa_cases pp pp1_argb888 video.mp4                          # 预定义测试\n"
              << "  qa_cases pp --crop 0,0,1280,720 --resolution 1280x720 video.mp4\n"
              << std::endl;
}

void PPTestSuite::listTests() const {
    std::cout << "\nAvailable PP tests:\n";
    std::cout << "════════════════════════════════════════════════════════\n";
    
    // PP0 YUV 格式（15 种，对应 lfl 分支定义）
    std::cout << "\nPP0 YUV Format Tests (15 种 + 10 别名):\n";
    std::cout << "  YUV400 系列 (灰度):\n";
    std::cout << "    pp0_yuv400_p010       YUV400 P010 (10-bit grayscale)\n";
    std::cout << "    pp0_yuv400_i010       YUV400 I010 (10-bit grayscale)\n";
    std::cout << "    pp0_yuv400_l010       YUV400 L010 (10-bit grayscale)\n";
    std::cout << "    pp0_yuv400_pack10     YUV400 Pack10 (10-bit grayscale)\n";
    std::cout << "    pp0_yuv400_8bit       YUV400 8-bit (8-bit grayscale)\n";
    std::cout << "  YUV420 NV12 系列:\n";
    std::cout << "    pp0_yuv420_nv12_p010  YUV420 NV12 P010 (10-bit)\n";
    std::cout << "    pp0_yuv420_nv12_i010  YUV420 NV12 I010 (10-bit)\n";
    std::cout << "    pp0_yuv420_nv12_l010  YUV420 NV12 L010 (10-bit)\n";
    std::cout << "    pp0_yuv420_nv12_pack10 YUV420 NV12 Pack10 (10-bit)\n";
    std::cout << "    pp0_yuv420_8bit_nv12  YUV420 8-bit NV12\n";
    std::cout << "  YUV420 NV21 系列:\n";
    std::cout << "    pp0_yuv420_nv21_p010_tiled YUV420 NV21 P010 Tiled-4x4\n";
    std::cout << "    pp0_yuv420_nv21_i011  YUV420 NV21 I011\n";
    std::cout << "    pp0_yuv420_nv21_l010  YUV420 NV21 L010\n";
    std::cout << "    pp0_yuv420_p010       YUV420 P010 (10-bit)\n";
    std::cout << "    pp0_yuv420_8bit_nv21  YUV420 8-bit NV21\n";
    std::cout << "  便捷别名:\n";
    std::cout << "    pp0_nv12, pp0_nv21, pp0_i420, pp0_yv12, pp0_p010\n";
    std::cout << "    pp0_nv16, pp0_nv61, pp0_i422, pp0_nv24, pp0_i444\n";
    
    // PP1 RGB 格式（18 种）
    std::cout << "\nPP1 RGB Format Tests (18 种 + 7 别名):\n";
    std::cout << "  RGB 10-bit 系列:\n";
    std::cout << "    pp1_argb2101010       ARGB2101010 (10-bit per channel)\n";
    std::cout << "    pp1_abgr2101010       ABGR2101010 (10-bit per channel)\n";
    std::cout << "    pp1_bgra2101010       BGRA2101010 (10-bit per channel)\n";
    std::cout << "    pp1_rgba2101010       RGBA2101010 (10-bit per channel)\n";
    std::cout << "  RGB 8-bit packed:\n";
    std::cout << "    pp1_abgr8888          ABGR8888 packed\n";
    std::cout << "    pp1_argb8888          ARGB8888 packed\n";
    std::cout << "    pp1_bgr888            BGR888 packed\n";
    std::cout << "    pp1_bgra8888          BGRA8888 packed\n";
    std::cout << "    pp1_bgrx8888          BGRX8888 packed\n";
    std::cout << "    pp1_rgb888            RGB888 packed\n";
    std::cout << "    pp1_rgba8888          RGBA8888 packed\n";
    std::cout << "    pp1_rgbx8888          RGBX8888 packed\n";
    std::cout << "    pp1_xrgb8888          XRGB8888 packed\n";
    std::cout << "    pp1_xbgr8888          XBGR8888 packed\n";
    std::cout << "  RGB 8-bit planar:\n";
    std::cout << "    pp1_rgb888_planar     RGB888 planar\n";
    std::cout << "    pp1_bgr888_planar     BGR888 planar\n";
    std::cout << "  RGB 16-bit:\n";
    std::cout << "    pp1_rgb161616         RGB 16-bit per channel\n";
    std::cout << "    pp1_bgr161616         BGR 16-bit per channel\n";
    std::cout << "    pp1_rgb161616_planar  RGB 16-bit planar\n";
    std::cout << "  便捷别名:\n";
    std::cout << "    pp1_argb888, pp1_abgr888, pp1_rgba888, pp1_bgra888\n";
    std::cout << "    pp1_r16g16b16, pp1_b16g16r16, pp1_gbrp\n";
    
    // PP1 YUV 格式（16 种）
    std::cout << "\nPP1 YUV Format Tests (16 种 + 10 别名):\n";
    std::cout << "  YUV400 系列 (灰度):\n";
    std::cout << "    pp1_yuv400_p010       YUV400 P010 (10-bit grayscale)\n";
    std::cout << "    pp1_yuv400_i010       YUV400 I010 (10-bit grayscale)\n";
    std::cout << "    pp1_yuv400_l010       YUV400 L010 (10-bit grayscale)\n";
    std::cout << "    pp1_yuv400_pack10     YUV400 Pack10 (10-bit grayscale)\n";
    std::cout << "    pp1_yuv400_8bit       YUV400 8-bit (8-bit grayscale)\n";
    std::cout << "  YUV420 NV12 系列:\n";
    std::cout << "    pp1_yuv420_nv12_p010  YUV420 NV12 P010 (10-bit)\n";
    std::cout << "    pp1_yuv420_nv12_i010  YUV420 NV12 I010 (10-bit)\n";
    std::cout << "    pp1_yuv420_nv12_l010  YUV420 NV12 L010 (10-bit)\n";
    std::cout << "    pp1_yuv420_nv12_pack10 YUV420 NV12 Pack10 (10-bit)\n";
    std::cout << "    pp1_yuv420_8bit_nv12  YUV420 8-bit NV12\n";
    std::cout << "  YUV420 NV21 系列:\n";
    std::cout << "    pp1_yuv420_nv21_p010_tiled YUV420 NV21 P010 Tiled-4x4\n";
    std::cout << "    pp1_yuv420_nv21_i011  YUV420 NV21 I011\n";
    std::cout << "    pp1_yuv420_nv21_l010  YUV420 NV21 L010\n";
    std::cout << "    pp1_yuv420_p010       YUV420 P010 (10-bit)\n";
    std::cout << "    pp1_yuv420_8bit_nv21  YUV420 8-bit NV21\n";
    std::cout << "    pp1_yuv420_nv21_p010  YUV420 NV21 P010 (PP1专属)\n";
    std::cout << "  便捷别名:\n";
    std::cout << "    pp1_nv12, pp1_nv21, pp1_i420, pp1_yv12, pp1_p010\n";
    std::cout << "    pp1_nv16, pp1_nv61, pp1_i422, pp1_nv24, pp1_i444\n";
    
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
    
    std::cout << "\nCrop Tests (4):\n";
    std::cout << "  multi_pp_crop1      PP0 crop 4096x2160 -> 1920x1080\n";
    std::cout << "  multi_pp_crop2      PP0 crop 32768x32768 -> 1280x720\n";
    std::cout << "  multi_pp_crop3      PP1 crop 4096x2160 -> 1920x1080\n";
    std::cout << "  multi_pp_crop4      PP1 crop 32768x32768 -> 1280x720\n";
    
    std::cout << "\nScale Tests (4):\n";
    std::cout << "  multi_pp_scale1     PP0 scale 32768x32768 -> 256x256\n";
    std::cout << "  multi_pp_scale2     PP1 scale 4096x2160 -> 128x128\n";
    std::cout << "  multi_pp_scale3     PP0+PP1 dual scale 32768x32768 -> 256x256\n";
    std::cout << "  multi_pp_scale4     PP0+PP1 dual scale 4096x2160 -> 128x128\n";
    
    std::cout << "════════════════════════════════════════════════════════\n";
    std::cout << "PP 格式测试总计: 49 种（15 PP0 YUV + 18 PP1 RGB + 16 PP1 YUV）\n";
    std::cout << "Multi-PP 测试: 10 种\n";
    std::cout << "Crop/Scale 测试: 8 种\n";
    std::cout << "便捷别名: 27 个\n";
    std::cout << "────────────────────────────────────────────────────────\n";
    std::cout << "Total: 94 个预定义测试项（含别名）\n";
    std::cout << "\n";
}


} // namespace pp
} // namespace test
