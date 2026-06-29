#include "OpencvPlugin.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "../common/ExecuteMode.hpp"
#include "consumptionline/core/BufferConsumerService.hpp"
#include "consumptionline/config/ConsumerTypeConfigBuilder.hpp"
#include "../common/third_party/CLI11.hpp"

#include <iostream>
#include <getopt.h>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <limits>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace opencv {

// 为了兼容现有代码，定义别名
using OpencvTestSuite = OpencvPlugin;

// ========================================
// 预定义测试参数表
// ========================================

const std::map<std::string, OpencvTestParams>& OpencvPlugin::getPredefinedTests() {
    static std::map<std::string, OpencvTestParams> tests;
    return tests;
}

void OpencvPlugin::listTests() const {
    std::cout << "\nAvailable OpenCV tests:\n"
              << "────────────────────────────────────────────────────────\n";
    for (const auto& pair : getPredefinedTests()) {
        std::cout << "  " << pair.first << "\n";
    }
    std::cout << "────────────────────────────────────────────────────────\n"
              << "Total: " << getPredefinedTests().size() << " predefined tests\n"
              << std::endl;
}

// ========================================
// IOptionPlugin 接口实现
// ========================================

void OpencvPlugin::registerOptions(CLI::App& app) {
    app.add_flag("-l,--list", show_list_, "列出所有预定义测试");
    app.add_flag("-v,--verbose", verbose_, "得分详细输出");
    app.add_option("-f,--file", input_path_, "视频文件路径");
    app.add_option("-c,--case", case_str_, "OpenCV 操作类型（resize_wh / resize_xy / ...）")->required();
    app.add_option("-m,--max-frames", max_frames_, "最大帧数");
    app.add_option("--min-fps", min_fps_, "性能最低帧率");
    app.add_option("--dst-fmt", dst_fmt_, "cvtColor目标格式");
    app.add_flag("--progressive", jpeg_progressive_, "JPEG_PROGRESSIVE");
    app.add_option("--quality", quality_, "JPEG_QUALITY")->check(CLI::Range(0,100));
    app.add_flag("--sw", use_software, "use_software");
    app.add_option("--src-fmt", src_pix_fmt, "source data pixel format");

    // 连体婴
    auto* src_w_opt = app.add_option("--src_w", src_width_, "image original 宽度")->check(CLI::Range(0, 10000));
    auto* src_h_opt = app.add_option("--src_h", src_height_, "image original 高度")->check(CLI::Range(0, 10000));
    src_w_opt->needs(src_h_opt);
    src_h_opt->needs(src_w_opt);

    // 连体婴
    auto* dst_w_opt = app.add_option("--dst_w", dst_width_, "resize_wh/crop 目标宽度")->check(CLI::Range(-100, 10000));
    auto* dst_h_opt = app.add_option("--dst_h", dst_height_, "resize_wh/crop 目标高度")->check(CLI::Range(-100, 10000));
    dst_w_opt->needs(dst_h_opt);
    dst_h_opt->needs(dst_w_opt);

    app.add_option("--interpolation", interpolation, "插值算法")->check(CLI::Range(0,1));

    // 连体婴
    auto* fx_opt = app.add_option("--fx", resize_fx_, "resize_xy 水平缩放比例")->check(CLI::Range(0.0, 1.0));
    auto* fy_opt = app.add_option("--fy", resize_fy_, "resize_xy 垂直缩放比例")->check(CLI::Range(0.0, 1.0));
    fx_opt->needs(fy_opt);
    fy_opt->needs(fx_opt);

    // 连体婴
    auto* x_opt = app.add_option("--x", crop_x_, "crop 起始 x 坐标")->check(CLI::Range(-100, 10000));
    auto* y_opt = app.add_option("--y", crop_y_, "crop 起始 y 坐标")->check(CLI::Range(-100, 10000));
    x_opt->needs(y_opt);
    y_opt->needs(x_opt);

    // 断言模式互斥组
    auto* assert_group = app.add_option_group("assert_mode", "断言模式（互斥，只能选择一个）");
    auto* pix_opt = assert_group->add_flag("--compare", enable_pix_compare, "启用 PSNR/SSIM 像素比较");
    auto* api_opt = assert_group->add_flag("--error", enable_api_exception, "启用 API 异常验证");
    auto* perf_opt = assert_group->add_flag("--perf", enable_perf, "启用性能计时");

    pix_opt->excludes(api_opt);
    pix_opt->excludes(perf_opt);
    api_opt->excludes(perf_opt);
}

void OpencvPlugin::applyTo(WorkerConfig& config) const {
    ;
}

int OpencvPlugin::handlePreActions() {
    return -1;
}

std::string probeDecoder(const std::string& filename, bool hw) {
    // 1. 将文件名转为小写方便比较
    std::string filename_lower = filename;
    std::transform(filename_lower.begin(), filename_lower.end(), 
                   filename_lower.begin(), ::tolower);
    
    // 2. 检查是否为JPEG图片
    if (filename_lower.find(".jpg") != std::string::npos ||
        filename_lower.find(".jpeg") != std::string::npos) {
        if (hw ==true) return "jpeg_taco";  // JPEG图片使用MJPEG解码器
        else return "mjpeg";
    }
    
    // 3. 如果是MP4视频，需要进一步判断编码格式
    // 这里简化处理，实际应该用FFmpeg探测
    if (filename_lower.find(".mp4") != std::string::npos ||
        filename_lower.find(".m4v") != std::string::npos) {
        
        // 实际项目中应该调用FFmpeg探测编码格式
        // 这里返回一个占位符，表示需要进一步探测
        
        // 可以根据文件名猜测编码
        if (filename_lower.find("hevc") != std::string::npos ||
            filename_lower.find("h265") != std::string::npos) {
            if (hw ==true) return "hevc_taco";  // JPEG图片使用MJPEG解码器
            else return "hevc";
        } else if (filename_lower.find("h264") != std::string::npos ||
                   filename_lower.find("avc") != std::string::npos) {
            if (hw ==true) return "h264_taco";  // JPEG图片使用MJPEG解码器
            else return "h264";
        } else if (filename_lower.find("mjpeg") != std::string::npos) {
            if (hw ==true) return "jpeg_taco";  // JPEG图片使用MJPEG解码器
            else return "mjpeg";
        } else {
            return "";
        }
    }
    
    // 4. 不支持的文件格式
    return "unknown";
}

static bool endsWith(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin());
}

std::vector<WorkerConfig> OpencvPlugin::buildPipelineConfigs(const WorkerConfig& shared_config) {
    using OpType = WorkerConfig::ConsumerTypeConfig::OpencvType::OpType;

    auto parseOpType = [](const std::string& name) -> OpType {
        if      (name == "resize_wh")   return OpType::RESIZE_WH;
        else if (name == "resize_xy")   return OpType::RESIZE_XY;
        else if (name == "crop")        return OpType::CROP;
        else if (name == "cvtcolor")    return OpType::CVTCOLOR;
        else if (name == "imwrite") return OpType::IMWRITE;
        else if (name == "imread") return OpType::IMREAD;
        return OpType::NONE;
    };

    OpType op = case_str_.empty() ? OpType::NONE : parseOpType(case_str_);

    auto buildFfmpegConfig = [&](bool use_hw) -> WorkerConfig {
        const std::string decoder = use_hw ? "h264" : "software";
        auto config = common::WorkerConfigFactory::createDecode(input_path_, decoder);
        config.decoder.name = probeDecoder(input_path_,use_hw);
        return config;
    };

    auto buildOpencvConfig = [&](bool use_hw, bool use_mock) -> WorkerConfig {
        auto config = WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(input_path_)
                    .setBufferCount(128)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useSoftware()
                    .build()
            )
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::OPENCV).build())
            .build();
        config.decoder.enable_hardware = ! use_software;
        config.decoder.use_mock = use_mock;
        config.decoder.mock_src_width = src_width_;
        config.decoder.mock_src_height = src_height_;
        if (src_pix_fmt == "bgr888") config.decoder.pix_fmt = AV_PIX_FMT_BGR24;
        else if (src_pix_fmt == "rgb888") config.decoder.pix_fmt = AV_PIX_FMT_RGB24;
        else if (src_pix_fmt == "nv12") config.decoder.pix_fmt = AV_PIX_FMT_NV12;
        else if (src_pix_fmt == "nv21") config.decoder.pix_fmt = AV_PIX_FMT_NV21;
        else config.decoder.pix_fmt = AV_PIX_FMT_NONE;
        return config;
    };

    auto buildConsumerConfig = [&](WorkerConfig config) -> WorkerConfig {
        // 设置断言模式
        using AssertMode = WorkerConfig::ConsumerTypeConfig::OpencvType::AssertMode;
        if (enable_pix_compare) {
            config.consumer_type.compare.enable_psnr = true;
            config.consumer_type.compare.min_psnr = 38;
            config.consumer_type.compare.enable_ssim = true;
            config.consumer_type.compare.min_ssim = 0.95;
            config.consumer_type.opencv.assert_mode = AssertMode::PIX_COMPARE;
        } else if (enable_api_exception) {
            config.consumer_type.opencv.assert_mode = AssertMode::API_EXCEPTION;
        } else if (enable_perf) {
            config.consumer_type.opencv.assert_mode = AssertMode::PERFORMANCE;  // PERF 也需要比较
            config.consumer_type.opencv.min_fps = min_fps_;
            config.consumer_type.compare.enable_psnr = true;
            config.consumer_type.compare.min_psnr = 38;
            config.consumer_type.compare.enable_ssim = true;
            config.consumer_type.compare.min_ssim = 0.95;
        } else {
            config.consumer_type.opencv.assert_mode = AssertMode::API_EXCEPTION;  // 默认
        }

        config.consumer_type.compare.verbose = verbose_;
        config.consumer_type.max_frames = max_frames_;
        config.data_source.max_frames = max_frames_;
        config.consumer_type.opencv.enable = true;

        auto& opencv = config.consumer_type.opencv;

        opencv.op_type = op;

        if (op == OpType::RESIZE_WH) {
            opencv.resize.dst_width = dst_width_;
            opencv.resize.dst_height = dst_height_;
            opencv.resize.fx = 0.0;
            opencv.resize.fy = 0.0;
            opencv.resize.interpolation = interpolation;
        } else if (op == OpType::RESIZE_XY) {
            opencv.resize.dst_width = 0;
            opencv.resize.dst_height = 0;
            opencv.resize.fx = resize_fx_;
            opencv.resize.fy = resize_fy_;
            opencv.resize.interpolation = interpolation;
        } else if (op == OpType::CROP) {
            opencv.crop.x = crop_x_;
            opencv.crop.y = crop_y_;
            opencv.crop.width = dst_width_;
            opencv.crop.height = dst_height_;
        } else if (op == OpType::CVTCOLOR) {
            opencv.cvtcolor.dst_fmt = dst_fmt_;
        } else if (op == OpType::IMWRITE) {
            opencv.imwrite.jpeg_progressive = jpeg_progressive_;
            opencv.imwrite.jpeg_quality = quality_;
        } else {
            // 其他操作使用默认值
        }

        return config;
    };

    // SINGLE mode
    if (input_path_.empty()) return {buildConsumerConfig(buildOpencvConfig(true,true))};
    else if (endsWith(input_path_,".jpg") || endsWith(input_path_,".jpeg")) return {buildConsumerConfig(buildOpencvConfig(true,false))};
    else if (endsWith(input_path_,".mp4")) return {buildConsumerConfig(buildFfmpegConfig(true))};
    else return {buildConsumerConfig(buildOpencvConfig(true,false))};
}

std::string OpencvPlugin::getTestName() const {
    std::string filename = input_path_;
    size_t pos = filename.find_last_of("/\\");
    if (pos != std::string::npos) filename = filename.substr(pos + 1);
    return "OpenCV: " + filename;
}

}
}
