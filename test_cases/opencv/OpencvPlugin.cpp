#include "OpencvPlugin.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "../common/ExecuteMode.hpp"
#include "consumptionline/BufferConsumerService.hpp"
#include "../common/third_party/CLI11.hpp"

#include <iostream>
#include <getopt.h>
#include <cstring>
#include <sstream>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace opencv {

// 为了兼容现有代码，定义别名
using OpencvTestSuite = OpencvPlugin;

// ========================================
// 辅助函数：从 WorkerConfig 构建消费标志
// ========================================
uint32_t OpencvPlugin::buildConsumeFlags(const WorkerConfig& config) {
    uint32_t flags = consumer::CONSUME_COUNT;  // 默认计数

    if (config.consumer_type.opencv.enable) {
        flags |= consumer::CONSUME_OPENCV;
    }

    return flags;
}

// ========================================
// 预定义测试参数表
// ========================================

const std::map<std::string, OpencvTestParams>& OpencvPlugin::getPredefinedTests() {
    static std::map<std::string, OpencvTestParams> tests = []() {
        std::map<std::string, OpencvTestParams> m;

        auto add = [&](const std::string& name,
                       OpencvTestParams::OpType op,
                       bool hw,
                       const std::string& params_str) {
            OpencvTestParams p;
            p.opencv_op    = op;
            p.use_hardware = hw;
            p.params_str   = params_str;
            m[name] = p;
        };

        using OpType = OpencvTestParams::OpType;

        // ---- OpenCV resize 预定义测试 ----
        add("cv_resize_h264_1080p_to_720p_30",    OpType::RESIZE, true,  "resize_1280_720");
        add("cv_resize_h264_1080p_to_480p_30",    OpType::RESIZE, true,  "resize_640_480");
        add("cv_resize_h265_1080p_to_720p_30",    OpType::RESIZE, true,  "resize_1280_720");
        add("cv_resize_sw_h264_1080p_to_720p_30", OpType::RESIZE, false, "resize_1280_720");

        // ---- OpenCV crop 预定义测试 ----
        add("cv_crop_h264_1080p_topleft_30",    OpType::CROP, true,  "crop_960_540");
        add("cv_crop_h264_1080p_center_30",     OpType::CROP, true,  "crop_960_540");
        add("cv_crop_h265_1080p_topleft_30",    OpType::CROP, true,  "crop_960_540");
        add("cv_crop_sw_h264_1080p_topleft_30", OpType::CROP, false, "crop_960_540");

        // ---- OpenCV erode 预定义测试 ----
        add("cv_erode_h264_1080p_k3_30",     OpType::ERODE,       true,  "erode_3_1");
        add("cv_erode_h264_1080p_k5_30",     OpType::ERODE,       true,  "erode_5_1");
        add("cv_erode_sw_h264_1080p_k3_30",  OpType::ERODE,       false, "erode_3_1");

        // ---- OpenCV dilate 预定义测试 ----
        add("cv_dilate_h264_1080p_k3_30",    OpType::DILATE,      true,  "dilate_3_1");
        add("cv_dilate_h264_1080p_k5_30",    OpType::DILATE,      true,  "dilate_5_1");
        add("cv_dilate_sw_h264_1080p_k3_30", OpType::DILATE,      false, "dilate_3_1");

        // ---- OpenCV 开运算（先腐蚀后膨胀）预定义测试 ----
        add("cv_open_h264_1080p_k3_30",      OpType::MORPH_OPEN,  true,  "open_3_1");
        add("cv_open_h264_1080p_k5_30",      OpType::MORPH_OPEN,  true,  "open_5_1");

        // ---- OpenCV 闭运算（先膨胀后腐蚀）预定义测试 ----
        add("cv_close_h264_1080p_k3_30",     OpType::MORPH_CLOSE, true,  "close_3_1");
        add("cv_close_h264_1080p_k5_30",     OpType::MORPH_CLOSE, true,  "close_5_1");

        // ---- OpenCV Sobel 边缘检测 ----
        add("cv_sobel_h264_1080p_dx_30",     OpType::SOBEL,        true, "sobel_1_0_3");
        add("cv_sobel_h264_1080p_dy_30",     OpType::SOBEL,        true, "sobel_0_1_3");

        // ---- OpenCV Canny 边缘检测 ----
        add("cv_canny_h264_1080p_30",        OpType::CANNY,        true, "canny_100_200");

        // ---- OpenCV Laplacian 拉普拉斯边缘 ----
        add("cv_laplacian_h264_1080p_30",    OpType::LAPLACIAN,    true, "laplacian_1");

        // ---- WarpAffine 平移 ----
        add("cv_translate_h264_1080p_30",    OpType::TRANSLATE,    true, "translate_100_50");

        // ---- WarpAffine 旋转 ----
        add("cv_rotate_h264_1080p_30",       OpType::ROTATE,       true, "rotate_45_1");

        // ---- WarpPerspective 透视变换 ----
        add("cv_perspective_h264_1080p_30",  OpType::PERSPECTIVE,  true, "perspective_50");

        // ---- cv::line 画线 ----
        add("cv_line_h264_1080p_30",         OpType::DRAW_LINE,    true, "line_0_0_1919_1079");

        // ---- cv::rectangle 画矩形 ----
        add("cv_rectangle_h264_1080p_30",    OpType::DRAW_RECT,    true, "rectangle_100_100_300_300");

        // ---- cv::putText 绘文字 ----
        add("cv_puttext_h264_1080p_30",      OpType::PUT_TEXT,     true, "puttext_50_100");

        // ---- cv::GaussianBlur 高斯模糊 ----
        add("cv_blur_h264_1080p_k5_30",      OpType::GAUSSIAN_BLUR,true, "blur_5_0");

        // ---- cv::threshold 二值化 ----
        add("cv_threshold_h264_1080p_30",    OpType::THRESHOLD,    true, "threshold_128_255");

        // ---- cv::split 通道分离测试 ----
        add("cv_split_h264_1080p_30",        OpType::SPLIT,        true, "split_3");
        add("cv_split_h265_1080p_30",        OpType::SPLIT,        true, "split_3");
        add("cv_split_sw_h264_1080p_30",     OpType::SPLIT,        false, "split_3");

        // ---- cv::merge 通道合并测试 ----
        add("cv_merge_h264_1080p_30",        OpType::MERGE,        true, "merge_3");
        add("cv_merge_h265_1080p_30",        OpType::MERGE,        true, "merge_3");
        add("cv_merge_sw_h264_1080p_30",     OpType::MERGE,        false, "merge_3");

        // ---- cv::cvtColor 颜色空间转换测试 ----
        // code=40: CV_YUV2BGR_NV12, code=91: CV_YUV2GRAY_NV12, code=116: CV_YUV2RGB_NV12
        add("cv_cvtcolor_h264_1080p_30",     OpType::CVTCOLOR,     true, "cvtcolor_40");
        add("cv_cvtcolor_h265_1080p_30",     OpType::CVTCOLOR,     true, "cvtcolor_40");
        add("cv_cvtcolor_sw_h264_1080p_30",  OpType::CVTCOLOR,     false, "cvtcolor_40");

        // ---- OpenCV ADD 算术运算测试（多生产者对比）----
        // 注：ADD 操作在 COMPARE 模式中执行，对比 hw_decoder 和 sw_decoder 的输出
        // hw_mat + sw_mat 的结果，然后计算与原始 buffer 的 PSNR/SSIM
        add("cv_add_h264_hw_vs_sw_psnr_30",  OpType::ADD,   true,  "add_psnr");
        add("cv_add_h264_hw_vs_sw_ssim_30",  OpType::ADD,   true,  "add_ssim");
        add("cv_add_h265_hw_vs_sw_psnr_30",  OpType::ADD,   true,  "add_psnr");

        // ---- OpenCV ABSDIFF 算术运算测试（多生产者对比）----
        // 注：ABSDIFF 计算 hw 和 sw 解码输出的绝对差，用于检测两者的差异
        // absdiff(hw_mat, sw_mat) 的结果，然后计算与原始 buffer 的 PSNR/SSIM
        add("cv_absdiff_h264_hw_vs_sw_psnr_30",  OpType::ABSDIFF,   true,  "absdiff_psnr");
        add("cv_absdiff_h264_hw_vs_sw_ssim_30",  OpType::ABSDIFF,   true,  "absdiff_ssim");
        add("cv_absdiff_h265_hw_vs_sw_psnr_30",  OpType::ABSDIFF,   true,  "absdiff_psnr");

        // ---- OpenCV ADD_WEIGHTED 加权求和测试（多生产者对比）----
        // 注：addWeighted(hw, 0.5, sw, 0.5, 0) 计算两个解码器输出的加权平均
        add("cv_addweighted_h264_hw_vs_sw_psnr_30",  OpType::ADD_WEIGHTED,  true,  "addweighted_psnr");
        add("cv_addweighted_h264_hw_vs_sw_ssim_30",  OpType::ADD_WEIGHTED,  true,  "addweighted_ssim");
        add("cv_addweighted_h265_hw_vs_sw_psnr_30",  OpType::ADD_WEIGHTED,  true,  "addweighted_psnr");

        // ---- OpenCV BITWISE_AND 按位与测试（多生产者对比）----
        // 注：bitwise_and(hw, sw) 计算两个解码器输出的按位与
        add("cv_bitwiseand_h264_hw_vs_sw_psnr_30",  OpType::BITWISE_AND,  true,  "bitwiseand_psnr");
        add("cv_bitwiseand_h264_hw_vs_sw_ssim_30",  OpType::BITWISE_AND,  true,  "bitwiseand_ssim");

        // ---- OpenCV BITWISE_OR 按位或测试（多生产者对比）----
        // 注：bitwise_or(hw, sw) 计算两个解码器输出的按位或
        add("cv_bitwiseor_h264_hw_vs_sw_psnr_30",  OpType::BITWISE_OR,  true,  "bitwiseor_psnr");
        add("cv_bitwiseor_h264_hw_vs_sw_ssim_30",  OpType::BITWISE_OR,  true,  "bitwiseor_ssim");

        // ---- OpenCV BITWISE_XOR 按位异或测试（多生产者对比）----
        // 注：bitwise_xor(hw, sw) 计算两个解码器输出的按位异或
        add("cv_bitwisexor_h264_hw_vs_sw_psnr_30",  OpType::BITWISE_XOR,  true,  "bitwisexor_psnr");
        add("cv_bitwisexor_h264_hw_vs_sw_ssim_30",  OpType::BITWISE_XOR,  true,  "bitwisexor_ssim");

        // ---- OpenCV BITWISE_NOT 按位非测试（多生产者对比）----
        // 注：bitwise_not(hw) 计算硬件解码器输出的按位非
        add("cv_bitwisenot_h264_hw_vs_sw_psnr_30",  OpType::BITWISE_NOT,  true,  "bitwisenot_psnr");
        add("cv_bitwisenot_h264_hw_vs_sw_ssim_30",  OpType::BITWISE_NOT,  true,  "bitwisenot_ssim");

        // ---- OpenCV SAVE_LOAD_IMG 图片保存和读取测试（单生产者I/O测试）----
        // 注：SAVE_LOAD_IMG 操作在 SINGLE 模式中执行，测试硬件解码器输出的图片保存/读取流程
        // 硬件 Mat -> cv::imwrite -> cv::imread -> 与原始 Mat 比较
        add("cv_saveloadimg_h264_hw_psnr_30",  OpType::SAVE_LOAD_IMG,  true,  "saveloadimg_psnr");
        add("cv_saveloadimg_h264_hw_ssim_30",  OpType::SAVE_LOAD_IMG,  true,  "saveloadimg_ssim");
        add("cv_saveloadimg_h265_hw_psnr_30",  OpType::SAVE_LOAD_IMG,  true,  "saveloadimg_psnr");

        return m;
    }();
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
    app.add_option("-f,--file", input_path_, "视频文件路径");
    app.add_option("-c,--case", case_str_, "OpenCV 操作类型");
    app.add_option("-p,--params", params_str_, "操作参数");
    app.add_option("-m,--max-frames", max_frames_, "最大帧数");
    app.add_flag("-D,--decoder", use_hardware_, "使用硬件解码 (默认: true)");
    app.add_flag("-p,--psnr", enable_psnr_, "启用 PSNR 验证");
    app.add_flag("-S,--ssim", enable_ssim_, "启用 SSIM 验证");
    app.add_flag("-v,--verbose", verbose_, "详细日志");
    app.add_option("positional", positional_args_, "测试名或输入文件路径");
}

void OpencvPlugin::applyTo(WorkerConfig& config) const {
    config.data_source = DataSourceConfigBuilder(config.data_source)
        .setPathIfNonEmpty(input_path_)
        .setMaxFrames(max_frames_)
        .build();
    config.consumer_type.max_frames = max_frames_;
    config.consumer_type.compare.enable_psnr = enable_psnr_;
    config.consumer_type.compare.enable_ssim = enable_ssim_;
    config.consumer_type.verbose = verbose_;
}

int OpencvPlugin::handlePreActions() {
    for (const auto& arg : positional_args_) {
        const auto& tests = getPredefinedTests();
        auto it = tests.find(arg);
        if (it != tests.end()) {
            params_ = it->second;
            continue;
        }
        if (input_path_.empty()) {
            input_path_ = arg;
        }
    }

    if (show_list_) { listTests(); return 0; }
    if (input_path_.empty()) {
        std::cerr << "Error: No input file specified\n" << std::endl;
        return 1;
    }
    return -1;
}

std::vector<WorkerConfig> OpencvPlugin::buildPipelineConfigs(const WorkerConfig& shared_config) {
    if (input_path_.empty()) return {};

    bool is_arithmetic_op = (params_.opencv_op == OpencvTestParams::OpType::ADD ||
                             params_.opencv_op == OpencvTestParams::OpType::ABSDIFF ||
                             params_.opencv_op == OpencvTestParams::OpType::ADD_WEIGHTED ||
                             params_.opencv_op == OpencvTestParams::OpType::BITWISE_AND ||
                             params_.opencv_op == OpencvTestParams::OpType::BITWISE_OR ||
                             params_.opencv_op == OpencvTestParams::OpType::BITWISE_XOR ||
                             params_.opencv_op == OpencvTestParams::OpType::BITWISE_NOT);

    const bool compare_enabled = shared_config.consumer_type.compare.enable_psnr
                              || shared_config.consumer_type.compare.enable_ssim;

    // COMPARE 模式：hw vs sw
    if ((is_arithmetic_op || compare_enabled) && params_.use_hardware) {
        auto hw_config = common::WorkerConfigFactory::buildOpencvConfig(
            shared_config.data_source.path, params_.params_str, true);
        auto sw_config = common::WorkerConfigFactory::buildOpencvConfig(
            shared_config.data_source.path, params_.params_str, false);

        hw_config.consumer_type = shared_config.consumer_type;
        hw_config.data_source = DataSourceConfigBuilder(hw_config.data_source)
            .setMaxFrames(shared_config.data_source.max_frames)
            .build();

        sw_config.consumer_type = shared_config.consumer_type;
        sw_config.data_source = DataSourceConfigBuilder(sw_config.data_source)
            .setMaxFrames(shared_config.data_source.max_frames)
            .build();

        return {hw_config, sw_config};
    }

    // SINGLE 模式
    auto config = common::WorkerConfigFactory::buildOpencvConfig(
        shared_config.data_source.path, params_.params_str, params_.use_hardware);
    config.consumer_type = shared_config.consumer_type;
    config.data_source = DataSourceConfigBuilder(config.data_source)
        .setMaxFrames(shared_config.data_source.max_frames)
        .build();

    return {config};
}

std::string OpencvPlugin::getTestName() const {
    std::string filename = input_path_;
    size_t pos = filename.find_last_of("/\\");
    if (pos != std::string::npos) filename = filename.substr(pos + 1);
    return "OpenCV: " + filename;
}

}
}
