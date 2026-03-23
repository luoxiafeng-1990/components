/**
 * @file WriterPlugin.cpp
 * @brief WriterPlugin 实现
 * 
 * 重构为 IOptionPlugin 插件架构，使用 CLI11 解析命令行
 */

#include "WriterPlugin.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "../common/third_party/CLI11.hpp"
#include "consumptionline/BufferConsumerService.hpp"

#include <iostream>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace writer {

// 模块级日志实例
static log4cplus::Logger& getLogger() {
    static log4cplus::Logger logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.test.WriterSuite"));
    return logger;
}

// ========================================
// 格式列表
// ========================================

const std::vector<std::pair<OutputFormat, std::string>>& WriterPlugin::getRgbFormats() {
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

const std::vector<std::pair<OutputFormat, std::string>>& WriterPlugin::getYuvFormats() {
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

const std::map<std::string, WriterTestParams>& WriterPlugin::getPredefinedTests() {
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

// ========================================
// IOptionPlugin 接口实现
// ========================================

void WriterPlugin::registerOptions(CLI::App& app) {
    app.add_flag("-l,--list", show_list_, "列出所有预定义测试");
    app.add_option("-i,--input", input_path_, "输入视频文件");
    app.add_option("-D,--decoder", decoder_str_, "解码方式 (hw|sw, 默认: hw)");
    app.add_option("-o,--output", output_path_, "输出文件/目录");
    app.add_option("-f,--format", format_str_, "输出格式 (nv12, rgb888, etc.)");
    app.add_option("-n,--frames", params_.save_frames, "保存帧数 (默认: 10)");
    app.add_flag("-R,--all-rgb", all_rgb_, "测试所有 12 种 RGB 格式");
    app.add_flag("-Y,--all-yuv", all_yuv_, "测试所有 10 种 YUV 格式");
    app.add_flag("-v,--verbose", verbose_, "详细日志");
    app.add_option("positional", positional_args_, "测试名或输入文件路径");

    app.footer(
        "Examples:\n"
        "  qa_cases writer -i video.mp4 -f rgb888 -o output.rgb\n"
        "  qa_cases writer -i video.mp4 --all-rgb -o /tmp/rgb_test\n"
        "  qa_cases writer -i video.mp4 --all-yuv -o /tmp/yuv_test\n"
        "  qa_cases writer rgb_argb888 -i video.mp4\n"
    );
}

void WriterPlugin::applyTo(WorkerConfig& config) const {
    config.data_source = DataSourceConfigBuilder(config.data_source)
        .setPathIfNonEmpty(input_path_)
        .build();
    if (verbose_)
        config.consumer_type.verbose = true;
}

int WriterPlugin::handlePreActions() {
    // Process decoder string
    if (!decoder_str_.empty()) {
        if (decoder_str_ == "sw" || decoder_str_ == "software")
            params_.use_hardware = false;
    }
    // Process format string
    if (!format_str_.empty() && format_str_ != "nv12") {
        format_specified_ = true;
    }
    // Process positional args
    for (const auto& a : positional_args_) {
        const auto& tests = getPredefinedTests();
        if (tests.find(a) != tests.end()) { params_ = tests.at(a); continue; }
        if (input_path_.empty()) { input_path_ = a; }
    }
    
    if (show_list_) { listTests(); return 0; }
    if (input_path_.empty()) {
        LOG4CPLUS_ERROR(getLogger(), "No input file specified");
        return 1;
    }
    return -1;
}

std::string WriterPlugin::getTestName() const {
    WriterTestParams params = params_;
    if (all_rgb_) params.description = "All 12 RGB formats";
    else if (all_yuv_) params.description = "All 10 YUV formats";
    else if (format_specified_) params.description = format_str_;
    return "Writer " + params.description;
}

std::vector<WorkerConfig> WriterPlugin::buildPipelineConfigs(const WorkerConfig& shared_config) {
    if (input_path_.empty()) return {};

    WriterTestParams params = params_;
    if (format_specified_) {
        params.format = TacoConfigBuilder::mapFormatNameToEnum(format_str_);
    }

    auto buildOne = [&](OutputFormat fmt, const std::string& desc, const std::string& out_path) -> WorkerConfig {
        WriterTestParams p(fmt, desc);
        p.save_frames = (all_rgb_ || all_yuv_) ? 5 : params.save_frames;
        p.use_hardware = params.use_hardware;
        p.width = params.width;
        p.height = params.height;

        WorkerConfig config;
        if (!p.use_hardware) {
            config = common::WorkerConfigFactory::createSoftwareDecode(
                shared_config.data_source.path);
        } else if (static_cast<int>(fmt) >= 1000) {
            config = common::WorkerConfigFactory::createPP1RgbConfig(
                shared_config.data_source.path, fmt, p.width, p.height);
        } else {
            config = common::WorkerConfigFactory::createPP0YuvConfig(
                shared_config.data_source.path, fmt, p.width, p.height);
        }
        config.consumer_type.save_raw.enable = true;
        config.consumer_type.save_raw.max_frames_per_channel = {p.save_frames};
        config.consumer_type.save_raw.setOutputPath(out_path.empty()
            ? "/tmp/writer_" + desc + ".raw" : out_path);
        config.consumer_type.verbose = shared_config.consumer_type.verbose;
        return config;
    };

    std::vector<WorkerConfig> configs;

    if (all_rgb_) {
        for (const auto& [fmt, desc] : getRgbFormats()) {
            std::string out = (output_path_.empty() ? "/tmp" : output_path_)
                + "/rgb_" + TacoConfigBuilder::mapFormatEnumToName(fmt).data() + ".raw";
            configs.push_back(buildOne(fmt, desc, out));
        }
    } else if (all_yuv_) {
        for (const auto& [fmt, desc] : getYuvFormats()) {
            std::string out = (output_path_.empty() ? "/tmp" : output_path_)
                + "/yuv_" + TacoConfigBuilder::mapFormatEnumToName(fmt).data() + ".raw";
            configs.push_back(buildOne(fmt, desc, out));
        }
    } else {
        configs.push_back(buildOne(params.format, params.description, output_path_));
    }

    return configs;
}

void WriterPlugin::listTests() const {
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

} // namespace writer
} // namespace test
