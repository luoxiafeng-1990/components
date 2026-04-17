/**
 * @file SavePlugin.cpp
 * @brief SavePlugin 实现（合并原 RecordPlugin + WriterPlugin）
 */

#include "SavePlugin.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "../common/third_party/CLI11.hpp"
#include "consumptionline/BufferConsumerService.hpp"
#include "consumptionline/config/ConsumerTypeConfigBuilder.hpp"

#include <iostream>
#include <sstream>
#include <set>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace save {

static log4cplus::Logger& getLogger() {
    static log4cplus::Logger logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.test.SaveSuite"));
    return logger;
}

// ========================================
// 格式列表
// ========================================

const std::vector<std::string>& SavePlugin::getContainerFormats() {
    static std::vector<std::string> formats = {
        "mp4", "mkv", "mov", "ts", "flv", "avi", "3gp"
    };
    return formats;
}

bool SavePlugin::isContainerFormat(const std::string& fmt) {
    static const std::set<std::string> containers = {
        "mp4", "mkv", "mov", "ts", "flv", "avi", "3gp"
    };
    return containers.count(fmt) > 0;
}

const std::vector<std::pair<OutputFormat, std::string>>& SavePlugin::getRgbFormats() {
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
        {OutputFormat::RGB_A2R10G10B10,   "ARGB2101010"},
        {OutputFormat::RGB_A2B10G10R10,   "ABGR2101010"},
        {OutputFormat::RGB_R10G10B10A2,   "RGBA2101010"},
        {OutputFormat::RGB_B10G10R10A2,   "BGRA2101010"},
    };
    return formats;
}

const std::vector<std::pair<OutputFormat, std::string>>& SavePlugin::getYuvFormats() {
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
// 预定义测试
// ========================================

const std::map<std::string, SavePlugin::StreamTestParams>& SavePlugin::getStreamTests() {
    static std::map<std::string, StreamTestParams> tests = {
        // 基本录制
        {"rtsp_to_mp4",     {"mp4", 10.0}},
        {"rtsp_to_mkv",     {"mkv", 10.0}},
        {"rtsp_to_mov",     {"mov", 10.0}},
        {"rtsp_to_ts",      {"ts",  10.0}},
        {"rtsp_to_flv",     {"flv", 10.0}},
        {"rtsp_to_avi",     {"avi", 10.0}},
        {"rtsp_to_3gp",     {"3gp", 10.0}},
        // 长时间录制
        {"rtsp_long_mp4",   {"mp4", 60.0}},
        {"rtsp_long_mkv",   {"mkv", 60.0}},
        // 文件重封装
        {"file_to_mp4",     {"mp4", -1}},
        {"file_to_mkv",     {"mkv", -1}},
        {"file_to_ts",      {"ts",  -1}},
    };
    return tests;
}

const std::map<std::string, SavePlugin::FrameTestParams>& SavePlugin::getFrameTests() {
    static std::map<std::string, FrameTestParams> tests = {
        // RGB 格式（12 个）
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
        {"rgb_argb2101010", {OutputFormat::RGB_A2R10G10B10,  "ARGB2101010"}},
        {"rgb_abgr2101010", {OutputFormat::RGB_A2B10G10R10,  "ABGR2101010"}},
        {"rgb_rgba2101010", {OutputFormat::RGB_R10G10B10A2,  "RGBA2101010"}},
        {"rgb_bgra2101010", {OutputFormat::RGB_B10G10R10A2,  "BGRA2101010"}},
        // YUV 格式（10 个）
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
        // 批量测试
        {"all_rgb",         {OutputFormat::RGB_RGB888, "All 12 RGB formats"}},
        {"all_yuv",         {OutputFormat::YUV_NV12,   "All 10 YUV formats"}},
    };
    return tests;
}

// ========================================
// IOptionPlugin 接口实现
// ========================================

void SavePlugin::registerOptions(CLI::App& app) {
    app.add_flag("-l,--list", show_list_, "列出所有预定义测试");
    app.add_option("-i,--input", input_path_, "输入路径");
    rtsp_opt_ = app.add_option("-r,--rtsp", input_path_, "RTSP URL");
    app.add_option("-o,--output", output_path_, "输出文件路径");
    app.add_option("-f,--format", format_str_,
        "输出格式（容器: mp4|mkv|mov|ts|flv|avi|3gp; 像素: nv12|rgb888|...）");

    // Stream-specific
    duration_opt_ = app.add_option("--duration", duration_, "录制时长（秒，-1=无限制）");
    app.add_flag("-a,--all-formats", all_container_formats_, "测试所有容器格式");

    // Frame-specific
    decoder_opt_ = app.add_option("-D,--decoder", decoder_str_, "解码方式 (hw|sw, 默认: hw)");
    frames_opt_ = app.add_option("-n,--frames", save_frames_, "保存帧数 (默认: 10)");
    app.add_flag("-R,--all-rgb", all_rgb_, "测试所有 12 种 RGB 格式");
    app.add_flag("-Y,--all-yuv", all_yuv_, "测试所有 10 种 YUV 格式");

    app.add_flag("-v,--verbose", verbose_, "详细日志");
    ds_opts_.registerTo(app);
    app.add_option("positional", positional_args_, "测试名或输入源路径");

    app.footer(
        "Examples:\n"
        "  # 流录制（stream 模式 — 自动识别容器格式）\n"
        "  qa_cases save -r rtsp://192.168.1.100/stream -o /tmp/out.mp4\n"
        "  qa_cases save -i input.mkv -f mkv -o /tmp/remux.mkv\n"
        "  qa_cases save -a --duration 5 -i rtsp://... -o /tmp/all\n"
        "\n"
        "  # 帧导出（frame 模式 — 自动识别像素格式）\n"
        "  qa_cases save -i video.mp4 -f rgb888 -o output.rgb\n"
        "  qa_cases save -i video.mp4 --all-rgb -o /tmp/rgb_test\n"
        "  qa_cases save -i video.mp4 --all-yuv -o /tmp/yuv_test\n"
        "  qa_cases save rgb_argb888 -i video.mp4\n"
    );
}

void SavePlugin::applyTo(WorkerConfig& config) const {
    ds_opts_.applyTo(config);
    config.data_source = DataSourceConfigBuilder(config.data_source)
        .setPathIfNonEmpty(input_path_)
        .build();
    if (verbose_)
        config.consumer_type = ConsumerTypeConfigBuilder(config.consumer_type)
            .setVerbose(true)
            .build();
}

int SavePlugin::handlePreActions() {
    if (!decoder_str_.empty()) {
        if (decoder_str_ == "sw" || decoder_str_ == "software")
            use_hardware_ = false;
    }

    bool matched_stream = false, matched_frame = false;
    for (const auto& a : positional_args_) {
        auto& st = getStreamTests();
        auto sit = st.find(a);
        if (sit != st.end()) {
            container_format_ = sit->second.format;
            duration_ = sit->second.duration;
            matched_stream = true;
            continue;
        }
        auto& ft = getFrameTests();
        auto fit = ft.find(a);
        if (fit != ft.end()) {
            pixel_format_ = fit->second.format;
            pixel_desc_ = fit->second.description;
            save_frames_ = fit->second.save_frames;
            use_hardware_ = fit->second.use_hardware;
            width_ = fit->second.width;
            height_ = fit->second.height;
            matched_frame = true;
            continue;
        }
        if (input_path_.empty()) { input_path_ = a; continue; }
        if (output_path_.empty()) { output_path_ = a; }
    }

    if (show_list_) { listTests(); return 0; }
    if (input_path_.empty()) {
        LOG4CPLUS_ERROR(getLogger(), "No input source specified");
        return 1;
    }

    // 模式推断（优先级：预定义测试 > 独有选项 > format 值 > 默认 FRAME）
    if (matched_stream) {
        resolved_mode_ = SaveMode::STREAM;
    } else if (matched_frame) {
        resolved_mode_ = SaveMode::FRAME;
    } else if (all_container_formats_
               || (rtsp_opt_ && rtsp_opt_->count() > 0)
               || (duration_opt_ && duration_opt_->count() > 0)) {
        resolved_mode_ = SaveMode::STREAM;
    } else if (all_rgb_ || all_yuv_
               || (decoder_opt_ && decoder_opt_->count() > 0)
               || (frames_opt_ && frames_opt_->count() > 0)) {
        resolved_mode_ = SaveMode::FRAME;
    } else if (!format_str_.empty()) {
        resolved_mode_ = isContainerFormat(format_str_) ? SaveMode::STREAM : SaveMode::FRAME;
    } else {
        resolved_mode_ = SaveMode::FRAME;
    }

    // 将 format_str_ 应用到对应模式的参数
    if (!format_str_.empty()) {
        if (resolved_mode_ == SaveMode::STREAM) {
            container_format_ = format_str_;
        } else {
            if (format_str_ != "nv12")
                format_specified_ = true;
        }
    }

    return -1;
}

std::string SavePlugin::getTestName() const {
    if (resolved_mode_ == SaveMode::STREAM) {
        if (all_container_formats_) {
            std::ostringstream name;
            name << "Save Stream All " << getContainerFormats().size() << " formats";
            if (duration_ > 0) name << " (" << duration_ << "s each)";
            return name.str();
        }
        std::ostringstream name;
        name << "Save Stream to " << container_format_;
        if (duration_ > 0) name << " (" << duration_ << "s)";
        return name.str();
    }

    std::string desc = pixel_desc_;
    if (all_rgb_) desc = "All 12 RGB formats";
    else if (all_yuv_) desc = "All 10 YUV formats";
    else if (format_specified_) desc = format_str_;
    return "Save Frame " + desc;
}

std::vector<WorkerConfig> SavePlugin::buildPipelineConfigs(const WorkerConfig& shared_config) {
    if (resolved_mode_ == SaveMode::STREAM)
        return buildStreamPipeline(shared_config);
    return buildFramePipeline(shared_config);
}

// ========================================
// Stream pipeline（原 RecordPlugin 逻辑）
// ========================================

std::vector<WorkerConfig> SavePlugin::buildStreamPipeline(const WorkerConfig& shared_config) {
    if (input_path_.empty()) return {};

    auto buildOne = [&](const std::string& fmt) -> WorkerConfig {
        std::string output = output_path_;
        if (output.empty())
            output = "/tmp/qa_record_" + std::to_string(time(nullptr));
        if (all_container_formats_)
            output += "_" + fmt + "." + fmt;
        else if (output.find('.') == std::string::npos)
            output += "." + fmt;

        auto config = common::WorkerConfigFactory::createRtspRecord(shared_config.data_source.path);
        config.consumer_type = ConsumerTypeConfigBuilder(config.consumer_type)
            .setSaveEncodedConfig(SaveEncodedConfigBuilder()
                .setEnable(true)
                .setOutputPath(output)
                .build())
            .setMaxDurationSeconds(duration_)
            .build();
        return config;
    };

    if (all_container_formats_) {
        std::vector<WorkerConfig> configs;
        for (const auto& fmt : getContainerFormats())
            configs.push_back(buildOne(fmt));
        return configs;
    }
    return {buildOne(container_format_)};
}

// ========================================
// Frame pipeline（原 WriterPlugin 逻辑）
// ========================================

std::vector<WorkerConfig> SavePlugin::buildFramePipeline(const WorkerConfig& shared_config) {
    if (input_path_.empty()) return {};

    OutputFormat fmt = pixel_format_;
    if (format_specified_)
        fmt = TacoConfigBuilder::mapFormatNameToEnum(format_str_);

    auto buildOne = [&](OutputFormat f, const std::string& desc, const std::string& out_path) -> WorkerConfig {
        int frames = (all_rgb_ || all_yuv_) ? 5 : save_frames_;

        WorkerConfig config;
        if (!use_hardware_) {
            config = common::WorkerConfigFactory::createSoftwareDecode(
                shared_config.data_source.path);
        } else if (static_cast<int>(f) >= 1000) {
            config = common::WorkerConfigFactory::createPP1RgbConfig(
                shared_config.data_source.path, f, width_, height_);
        } else {
            config = common::WorkerConfigFactory::createPP0YuvConfig(
                shared_config.data_source.path, f, width_, height_);
        }
        config.consumer_type = ConsumerTypeConfigBuilder(config.consumer_type)
            .setSaveRawConfig(SaveRawConfigBuilder()
                .setEnable(true)
                .setMaxFramesPerChannel({frames})
                .setOutputPaths({out_path.empty()
                    ? "/tmp/save_frame_" + desc + ".raw" : out_path})
                .build())
            .setVerbose(shared_config.consumer_type.verbose)
            .build();
        return config;
    };

    std::vector<WorkerConfig> configs;
    if (all_rgb_) {
        for (const auto& [f, desc] : getRgbFormats()) {
            std::string out = (output_path_.empty() ? "/tmp" : output_path_)
                + "/rgb_" + TacoConfigBuilder::mapFormatEnumToName(f).data() + ".raw";
            configs.push_back(buildOne(f, desc, out));
        }
    } else if (all_yuv_) {
        for (const auto& [f, desc] : getYuvFormats()) {
            std::string out = (output_path_.empty() ? "/tmp" : output_path_)
                + "/yuv_" + TacoConfigBuilder::mapFormatEnumToName(f).data() + ".raw";
            configs.push_back(buildOne(f, desc, out));
        }
    } else {
        configs.push_back(buildOne(fmt, format_specified_ ? format_str_ : pixel_desc_, output_path_));
    }
    return configs;
}

// ========================================
// listTests
// ========================================

void SavePlugin::listTests() const {
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  Available Save Tests\n";
    std::cout << "═══════════════════════════════════════════════════════\n";

    std::cout << "\n── Stream Mode（流录制 → 容器文件）──\n";
    std::cout << "\nRTSP Recording:\n";
    std::cout << "  rtsp_to_mp4         RTSP to MP4 (10s)\n";
    std::cout << "  rtsp_to_mkv         RTSP to MKV (10s)\n";
    std::cout << "  rtsp_to_mov         RTSP to MOV (10s)\n";
    std::cout << "  rtsp_to_ts          RTSP to TS (10s)\n";
    std::cout << "  rtsp_to_flv         RTSP to FLV (10s)\n";
    std::cout << "  rtsp_to_avi         RTSP to AVI (10s)\n";
    std::cout << "  rtsp_to_3gp         RTSP to 3GP (10s)\n";
    std::cout << "\nLong Recording:\n";
    std::cout << "  rtsp_long_mp4       RTSP to MP4 (60s)\n";
    std::cout << "  rtsp_long_mkv       RTSP to MKV (60s)\n";
    std::cout << "\nFile Remux:\n";
    std::cout << "  file_to_mp4         File remux to MP4\n";
    std::cout << "  file_to_mkv         File remux to MKV\n";
    std::cout << "  file_to_ts          File remux to TS\n";

    std::cout << "\n── Frame Mode（帧导出 → 原始像素文件）──\n";
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
    std::cout << "Total: " << (getStreamTests().size() + getFrameTests().size()) << " predefined tests\n";
    std::cout << "\n";
}

} // namespace save
} // namespace test
