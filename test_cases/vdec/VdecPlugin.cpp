/**
 * @file VdecPlugin.cpp
 * @brief VdecPlugin 实现
 * 
 * IOptionPlugin 插件架构，使用 ExecuteMode 静态方法执行测试
 */

#include "VdecPlugin.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "consumptionline/BufferConsumerService.hpp"

#include "../common/third_party/CLI11.hpp"

#include <iostream>
#include <sstream>

namespace test {
namespace vdec {

// ========================================
// 预定义测试参数表
// ========================================

const std::map<std::string, DecodeTestParams>& VdecPlugin::getPredefinedTests() {
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
        {"sw_h264_1920x1080_30",  {"h264", 1920, 1080, 30.0, "", false}},
        {"sw_h265_1920x1080_30",  {"h265", 1920, 1080, 30.0, "", false}},
        
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
    };
    return tests;
}

// ========================================
// IOptionPlugin: registerOptions (CLI11)
// ========================================

void VdecPlugin::registerOptions(CLI::App& app) {
    app.add_flag("-l,--list", show_list_, "列出所有预定义测试");
    app.add_option("-f,--file", input_path_, "视频文件路径");
    app.add_option("-r,--rtsp", input_path_, "RTSP URL");
    app.add_option("-c,--codec", params_.codec, "编解码格式 (h264|h265|mjpeg)");
    app.add_option("-D,--decoder", decoder_str_, "解码方式 (hw|sw, 默认: hw)");
    app.add_option("-W,--width", params_.width, "分辨率宽度");
    app.add_option("-H,--height", params_.height, "分辨率高度");
    app.add_option("-R,--resolution", resolution_str_, "分辨率 (如 1920x1080)");
    app.add_option("-F,--fps", params_.fps, "目标帧率");
    app.add_option("-m,--max-frames", max_frames_, "最大帧数（数据源读取与消费循环共用，-1=无限制）");
    app.add_flag("-p,--psnr", enable_psnr_, "启用 PSNR 验证");
    app.add_flag("-S,--ssim", enable_ssim_, "启用 SSIM 验证");
    app.add_option("-P,--min-psnr", min_psnr_, "PSNR 阈值 (默认: 30.0 dB)");
    app.add_option("-M,--min-ssim", min_ssim_, "SSIM 阈值 (默认: 0.95)");
    app.add_flag("-v,--verbose", verbose_, "详细日志");
    app.add_option("-t,--threads", threads_, "并发路数 (启用 PARALLEL 模式)");
    app.add_flag("--loop", loop_, "循环播放");
    app.add_option("positional", positional_args_, "测试名或输入文件路径");

    app.footer(
        "ExecuteMode Mapping:\n"
        "  SINGLE   - 默认单路解码\n"
        "  COMPARE  - --psnr/--ssim 启用时，HW vs SW 对比\n"
        "  PARALLEL - --threads N，支持 --decoder hw/sw\n"
        "\n"
        "Examples:\n"
        "  qa_cases vdec video.mp4\n"
        "  qa_cases vdec --psnr video.mp4\n"
        "  qa_cases vdec --threads 4 video.mp4\n"
        "  qa_cases vdec h264_1920x1080_30 video.mp4\n"
        "  qa_cases vdec video.mp4 display --mode vo\n"
        "  qa_cases vdec video.mp4 display npu --model m.nb\n"
    );
}

// ========================================
// IOptionPlugin: applyTo
// ========================================

void VdecPlugin::applyTo(WorkerConfig& config) const {
    config.data_source = DataSourceConfigBuilder(config.data_source)
        .setPathIfNonEmpty(input_path_)
        .setMaxFrames(max_frames_)
        .setLoop(loop_)
        .build();
    // 与数据源读帧上限一致：消费循环使用同一上限（-1=无限制）
    config.consumer_type.max_frames = max_frames_;
    
    // compare 设置
    config.consumer_type.compare.enable_psnr = enable_psnr_;
    config.consumer_type.compare.enable_ssim = enable_ssim_;
    if (min_psnr_ > 0.0) {
        config.consumer_type.compare.min_psnr = min_psnr_;
    }
    if (min_ssim_ > 0.0) {
        config.consumer_type.compare.min_ssim = min_ssim_;
    }
    
    config.consumer_type.verbose = verbose_;
}

// ========================================
// IOptionPlugin: handlePreActions
// ========================================

int VdecPlugin::handlePreActions() {
    if (!decoder_str_.empty()) {
        if (decoder_str_ == "sw" || decoder_str_ == "software")
            params_.use_hardware = false;
    }

    if (!resolution_str_.empty()) {
        size_t pos = resolution_str_.find('x');
        if (pos != std::string::npos) {
            params_.width = std::stoi(resolution_str_.substr(0, pos));
            params_.height = std::stoi(resolution_str_.substr(pos + 1));
        }
    }

    for (const auto& arg : positional_args_) {
        const auto& tests = getPredefinedTests();
        auto it = tests.find(arg);
        if (it != tests.end()) {
            params_ = it->second;
            params_.predefined_name = arg;
            continue;
        }
        if (input_path_.empty()) {
            input_path_ = arg;
        }
    }

    if (show_list_) { listTests(); return 0; }
    if (input_path_.empty()) {
        std::cerr << "Error: No input file or RTSP URL specified\n" << std::endl;
        return 1;
    }
    return -1;
}

// ========================================
// IOptionPlugin: getTestName
// ========================================

std::string VdecPlugin::getTestName() const {
    auto params = resolveParams();

    std::ostringstream name;
    if (params.isPredefined()) {
        name << params.predefined_name << " ("
             << params.codec << " " << params.width << "x" << params.height
             << " " << static_cast<int>(params.fps) << "fps)";
    } else {
        std::string filename = input_path_;
        size_t pos = filename.find_last_of("/\\");
        if (pos != std::string::npos) filename = filename.substr(pos + 1);
        name << "Custom: " << filename << " ("
             << params.codec << " " << params.width << "x" << params.height
             << " " << static_cast<int>(params.fps) << "fps)";
    }
    return name.str();
}

// ========================================
// IOptionPlugin: buildPipelineConfigs
// ========================================

std::vector<WorkerConfig> VdecPlugin::buildPipelineConfigs(const WorkerConfig& shared_config) {
    if (input_path_.empty()) return {};

    auto params = resolveParams();

    // COMPARE 模式：hw + sw 两组 config
    if (shared_config.consumer_type.compare.enable_psnr || shared_config.consumer_type.compare.enable_ssim) {
        auto hw_config = common::WorkerConfigFactory::createDecode(
            shared_config.data_source.path, params.codec, params.width, params.height);
        hw_config.consumer_type = shared_config.consumer_type;
        hw_config.consumer_type.performance.target_fps = params.fps;
        hw_config.data_source = DataSourceConfigBuilder(hw_config.data_source)
            .setMaxFrames(shared_config.data_source.max_frames)
            .setLoop(shared_config.data_source.loop)
            .build();

        auto sw_config = common::WorkerConfigFactory::createSoftwareDecode(
            shared_config.data_source.path, params.width, params.height);
        sw_config.consumer_type.performance.target_fps = params.fps;
        sw_config.consumer_type.max_frames = shared_config.consumer_type.max_frames;
        sw_config.data_source = DataSourceConfigBuilder(sw_config.data_source)
            .setMaxFrames(shared_config.data_source.max_frames)
            .setLoop(shared_config.data_source.loop)
            .build();

        return {hw_config, sw_config};
    }

    // PARALLEL 模式：N 组 config
    if (params.profile == "parallel" || params.profile.find("parallel_") == 0) {
        int thread_count = 2;
        if (params.profile.find("parallel_") == 0) {
            thread_count = std::stoi(params.profile.substr(9));
        }

        WorkerConfig base = shared_config;
        if (base.consumer_type.display.enable) {
            int mc = base.consumer_type.display.taco_vo.max_channels;
            if (mc > thread_count)
                thread_count = mc;
            base.consumer_type.display.taco_vo.max_channels = thread_count;
        }

        std::vector<WorkerConfig> configs;
        for (int i = 0; i < thread_count; i++) {
            WorkerConfig cfg;
            if (params.use_hardware) {
                cfg = common::WorkerConfigFactory::createDecode(
                    base.data_source.path, params.codec, params.width, params.height);
            } else {
                cfg = common::WorkerConfigFactory::createSoftwareDecode(
                    base.data_source.path, params.width, params.height);
            }
            cfg.consumer_type = base.consumer_type;
            cfg.consumer_type.performance.target_fps = params.fps;
            cfg.data_source = DataSourceConfigBuilder(cfg.data_source)
                .setMaxFrames(base.data_source.max_frames)
                .setLoop(base.data_source.loop)
                .build();
            configs.push_back(cfg);
        }
        return configs;
    }

    // SINGLE 模式：1 组 config
    WorkerConfig full_config;
    if (params.use_hardware) {
        full_config = common::WorkerConfigFactory::createDecode(
            shared_config.data_source.path, params.codec, params.width, params.height);
    } else {
        full_config = common::WorkerConfigFactory::createSoftwareDecode(
            shared_config.data_source.path, params.width, params.height);
    }
    full_config.consumer_type = shared_config.consumer_type;
    full_config.consumer_type.performance.target_fps = params.fps;
    full_config.data_source = DataSourceConfigBuilder(full_config.data_source)
        .setMaxFrames(shared_config.data_source.max_frames)
        .setLoop(shared_config.data_source.loop)
        .build();

    // 显示多宫格：每路画面需要独立解码 worker（各注册一个 display channel）。仅 max_channels>1 时展开。
    if (full_config.consumer_type.display.enable
        && full_config.consumer_type.display.taco_vo.max_channels > 1) {
        const int n = full_config.consumer_type.display.taco_vo.max_channels;
        std::vector<WorkerConfig> configs;
        for (int i = 0; i < n; i++) {
            WorkerConfig cfg;
            if (params.use_hardware) {
                cfg = common::WorkerConfigFactory::createDecode(
                    shared_config.data_source.path, params.codec, params.width, params.height);
            } else {
                cfg = common::WorkerConfigFactory::createSoftwareDecode(
                    shared_config.data_source.path, params.width, params.height);
            }
            cfg.consumer_type = full_config.consumer_type;
            cfg.consumer_type.performance.target_fps = params.fps;
            cfg.data_source = DataSourceConfigBuilder(cfg.data_source)
                .setMaxFrames(shared_config.data_source.max_frames)
                .setLoop(shared_config.data_source.loop)
                .build();
            configs.push_back(std::move(cfg));
        }
        return configs;
    }

    return {full_config};
}

// ========================================
// listTests
// ========================================

void VdecPlugin::listTests() const {
    std::cout << "\n"
              << "Available VDEC tests:\n"
              << "────────────────────────────────────────────────────────\n"
              << "\n"
              << "H.264 Tests (9) - ExecuteMode::SINGLE:\n"
              << "  h264_128x128_30         H.264 128x128 30fps main\n"
              << "  h264_320x240_30         H.264 320x240 30fps high\n"
              << "  h264_640x480_30         H.264 640x480 30fps main\n"
              << "  h264_640x480_60         H.264 640x480 60fps high\n"
              << "  h264_1280x720_30        H.264 720p 30fps high\n"
              << "  h264_1920x1080_30       H.264 1080p 30fps high\n"
              << "  h264_1920x1080_60       H.264 1080p 60fps high\n"
              << "  h264_2560x1440_30       H.264 1440p 30fps high\n"
              << "  h264_3840x2160_30       H.264 4K 30fps high\n"
              << "\n"
              << "H.265/HEVC Tests (9) - ExecuteMode::SINGLE:\n"
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
              << "MJPEG Tests (9) - ExecuteMode::SINGLE:\n"
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
              << "Software Decode Tests (2) - ExecuteMode::SINGLE:\n"
              << "  sw_h264_1920x1080_30    Software H.264 1080p 30fps\n"
              << "  sw_h265_1920x1080_30    Software H.265 1080p 30fps\n"
              << "\n"
              << "RTSP H.264 Tests (6) - ExecuteMode::SINGLE:\n"
              << "  rtsp_h264_1280x720_30_cbr   RTSP H.264 720p CBR\n"
              << "  rtsp_h264_1280x720_30_vbr   RTSP H.264 720p VBR\n"
              << "  rtsp_h264_1920x1080_30_cbr  RTSP H.264 1080p CBR\n"
              << "  rtsp_h264_1920x1080_30_vbr  RTSP H.264 1080p VBR\n"
              << "  rtsp_h264_3840x2160_30_cbr  RTSP H.264 4K CBR\n"
              << "  rtsp_h264_3840x2160_30_vbr  RTSP H.264 4K VBR\n"
              << "\n"
              << "RTSP H.265 Tests (6) - ExecuteMode::SINGLE:\n"
              << "  rtsp_h265_1280x720_30_cbr   RTSP H.265 720p CBR\n"
              << "  rtsp_h265_1280x720_30_vbr   RTSP H.265 720p VBR\n"
              << "  rtsp_h265_1920x1080_30_cbr  RTSP H.265 1080p CBR\n"
              << "  rtsp_h265_1920x1080_30_vbr  RTSP H.265 1080p VBR\n"
              << "  rtsp_h265_3840x2160_30_cbr  RTSP H.265 4K CBR\n"
              << "  rtsp_h265_3840x2160_30_vbr  RTSP H.265 4K VBR\n"
              << "\n"
              << "RTSP MJPEG Tests (1) - ExecuteMode::SINGLE:\n"
              << "  rtsp_mjpeg_32768x18432_30   RTSP MJPEG Ultra-High\n"
              << "\n"
              << "Multi-Worker Tests (2) - ExecuteMode::PARALLEL:\n"
              << "  multi_worker               HW+SW concurrent decode\n"
              << "  multi_worker_4k            HW+SW concurrent 4K decode\n"
              << "\n"
              << "Multi-Thread Tests (3) - ExecuteMode::PARALLEL:\n"
              << "  multithread_2              2-thread decode\n"
              << "  multithread_4              4-thread decode\n"
              << "  multithread_8              8-thread decode\n"
              << "\n"
              << "────────────────────────────────────────────────────────\n"
              << "Total: 47 predefined tests\n"
              << std::endl;
}


} // namespace vdec
} // namespace test
