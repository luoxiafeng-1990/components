#include "OpencvTestSuite.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "productionline/io/BufferConsumerService.hpp"

#include <iostream>
#include <getopt.h>
#include <cstring>
#include <sstream>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace opencv {

// ========================================
// 辅助函数：从 WorkerConfig 构建消费标志
// ========================================
uint32_t OpencvTestSuite::buildConsumeFlags(const WorkerConfig& config) {
    uint32_t flags = consumer::CONSUME_COUNT;  // 默认计数

    if (config.consumer_type.opencv.enable) {
        flags |= consumer::CONSUME_OPENCV;
    }

    return flags;
}

// ========================================
// 预定义测试参数表
// ========================================

const std::map<std::string, OpencvTestParams>& OpencvTestSuite::getPredefinedTests() {
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

std::vector<std::string> OpencvTestSuite::getTestNames() const {
    std::vector<std::string> names;
    for (const auto& pair : getPredefinedTests()) {
        names.push_back(pair.first);
    }
    return names;
}

// ========================================
// ITestModule 接口实现
// ========================================

int OpencvTestSuite::run(int argc, char* argv[]) {
    WorkerConfig config;
    OpencvTestParams params;

    if (!parseArgs(argc, argv, config, params)) {
        return 1;
    }

    // 检测是否为多生产者算术/逻辑运算（需要 COMPARE 模式）
    bool is_arithmetic_op = (params.opencv_op == OpencvTestParams::OpType::ADD ||
                             params.opencv_op == OpencvTestParams::OpType::ABSDIFF ||
                             params.opencv_op == OpencvTestParams::OpType::ADD_WEIGHTED ||
                             params.opencv_op == OpencvTestParams::OpType::BITWISE_AND ||
                             params.opencv_op == OpencvTestParams::OpType::BITWISE_OR ||
                             params.opencv_op == OpencvTestParams::OpType::BITWISE_XOR ||
                             params.opencv_op == OpencvTestParams::OpType::BITWISE_NOT);

    std::string filename = config.data_source.path;
    size_t pos = filename.find_last_of("/\\");
    if (pos != std::string::npos) {
        filename = filename.substr(pos + 1);
    }
    const std::string test_name = "Custom: " + filename;

    if (is_arithmetic_op) {
        // ⭐ ADD/ABSDIFF 需要两个解码器（hw 和 sw）进行对比
        auto hw_config = common::WorkerConfigFactory::buildOpencvConfig(
            config.data_source.path, params.params_str, true);  // hardware=true
        auto sw_config = common::WorkerConfigFactory::buildOpencvConfig(
            config.data_source.path, params.params_str, false); // hardware=false

        // 合并附加设置
        hw_config.consumer_type.verbose  = config.consumer_type.verbose;
        hw_config.data_source.max_frames = config.data_source.max_frames;
        sw_config.consumer_type.verbose  = config.consumer_type.verbose;
        sw_config.data_source.max_frames = config.data_source.max_frames;

        uint32_t flags = consumer::CONSUME_COUNT;
        if (hw_config.consumer_type.opencv.enable) {
            flags |= consumer::CONSUME_OPENCV;
        }

        auto result = runCompare({hw_config, sw_config}, flags, test_name);
        consumer::BufferConsumerService::printResult(test_name, result);
        return result.success ? 0 : 1;
    } else {
        // ⭐ 其他操作使用 SINGLE 模式
        auto hw_config = common::WorkerConfigFactory::buildOpencvConfig(
            config.data_source.path, params.params_str, params.use_hardware);

        // 合并附加设置
        hw_config.consumer_type.verbose  = config.consumer_type.verbose;
        hw_config.data_source.max_frames = config.data_source.max_frames;

        uint32_t flags = buildConsumeFlags(hw_config);

        auto result = runSingle(hw_config, flags, test_name);
        consumer::BufferConsumerService::printResult(test_name, result);

        return result.success ? 0 : 1;
    }
}

bool OpencvTestSuite::parseArgs(int argc, char* argv[], WorkerConfig& config, OpencvTestParams& params) {
    optind = 1;

    static struct option long_options[] = {
        {"help",       no_argument,       0, 'h'},
        {"list",       no_argument,       0, 'l'},
        {"input",      required_argument, 0, 'i'},
        {"max_frames", required_argument, 0, 'm'},
        {"verbose",    no_argument,       0, 'v'},
        {"case",       required_argument, 0, 1001},
        {"params",     required_argument, 0, 1002},
        {0, 0, 0, 0}
    };

    std::string input_path;
    std::string case_str;
    std::string params_arg;

    int opt;
    while ((opt = getopt_long(argc, argv, "hli:m:v", long_options, nullptr)) != -1) {
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

            case 'm':
                config.data_source.max_frames = std::stoi(optarg);
                break;

            case 'v':
                config.consumer_type.verbose = true;
                break;

            case 1001:  // --case <resize|crop>
                case_str = optarg;
                break;

            case 1002:  // --params <string>
                params_arg = optarg;
                break;

            default:
                printHelp();
                return false;
        }
    }

    // 处理剩余位置参数作为输入路径
    for (int i = optind; i < argc; i++) {
        if (input_path.empty()) {
            input_path = argv[i];
        }
    }

    if (input_path.empty()) {
        std::cerr << "Error: No input file specified\n" << std::endl;
        printHelp();
        return false;
    }

    if (case_str.empty()) {
        std::cerr << "Error: --case not specified (use 'resize' or 'crop')\n" << std::endl;
        printHelp();
        return false;
    }

    if (case_str == "resize") {
        params.opencv_op = OpencvTestParams::OpType::RESIZE;
    } else if (case_str == "crop") {
        params.opencv_op = OpencvTestParams::OpType::CROP;
    } else if (case_str == "erode") {
        params.opencv_op = OpencvTestParams::OpType::ERODE;
    } else if (case_str == "dilate") {
        params.opencv_op = OpencvTestParams::OpType::DILATE;
    } else if (case_str == "open") {
        params.opencv_op = OpencvTestParams::OpType::MORPH_OPEN;
    } else if (case_str == "close") {
        params.opencv_op = OpencvTestParams::OpType::MORPH_CLOSE;
    } else if (case_str == "sobel") {
        params.opencv_op = OpencvTestParams::OpType::SOBEL;
    } else if (case_str == "canny") {
        params.opencv_op = OpencvTestParams::OpType::CANNY;
    } else if (case_str == "laplacian") {
        params.opencv_op = OpencvTestParams::OpType::LAPLACIAN;
    } else if (case_str == "translate") {
        params.opencv_op = OpencvTestParams::OpType::TRANSLATE;
    } else if (case_str == "rotate") {
        params.opencv_op = OpencvTestParams::OpType::ROTATE;
    } else if (case_str == "perspective") {
        params.opencv_op = OpencvTestParams::OpType::PERSPECTIVE;
    } else if (case_str == "line") {
        params.opencv_op = OpencvTestParams::OpType::DRAW_LINE;
    } else if (case_str == "rectangle") {
        params.opencv_op = OpencvTestParams::OpType::DRAW_RECT;
    } else if (case_str == "puttext") {
        params.opencv_op = OpencvTestParams::OpType::PUT_TEXT;
    } else if (case_str == "blur") {
        params.opencv_op = OpencvTestParams::OpType::GAUSSIAN_BLUR;
    } else if (case_str == "threshold") {
        params.opencv_op = OpencvTestParams::OpType::THRESHOLD;
    } else if (case_str == "split") {
        params.opencv_op = OpencvTestParams::OpType::SPLIT;
    } else if (case_str == "merge") {
        params.opencv_op = OpencvTestParams::OpType::MERGE;
    } else if (case_str == "cvtcolor") {
        params.opencv_op = OpencvTestParams::OpType::CVTCOLOR;
    } else if (case_str == "add") {
        params.opencv_op = OpencvTestParams::OpType::ADD;
    } else if (case_str == "absdiff") {
        params.opencv_op = OpencvTestParams::OpType::ABSDIFF;
    } else if (case_str == "addweighted") {
        params.opencv_op = OpencvTestParams::OpType::ADD_WEIGHTED;
    } else if (case_str == "bitwiseand") {
        params.opencv_op = OpencvTestParams::OpType::BITWISE_AND;
    } else if (case_str == "bitwiseor") {
        params.opencv_op = OpencvTestParams::OpType::BITWISE_OR;
    } else if (case_str == "bitwisexor") {
        params.opencv_op = OpencvTestParams::OpType::BITWISE_XOR;
    } else if (case_str == "bitwisenot") {
        params.opencv_op = OpencvTestParams::OpType::BITWISE_NOT;
    } else if (case_str == "saveloadimg") {
        params.opencv_op = OpencvTestParams::OpType::SAVE_LOAD_IMG;
    } else {
        std::cerr << "Error: unknown --case '" << case_str
                  << "'. Valid: resize/crop/erode/dilate/open/close/"
                     "sobel/canny/laplacian/translate/rotate/perspective/"
                     "line/rectangle/puttext/blur/threshold/split/merge/cvtcolor/"
                     "add/absdiff/addweighted/bitwiseand/bitwiseor/bitwisexor/bitwisenot/saveloadimg\n" << std::endl;
        return false;
    }

    // 将 case 和 params 用 _ 拼接
    params.params_str = case_str;
    if (!params_arg.empty()) {
        params.params_str += "_" + params_arg;
    }

    config.data_source.path = input_path;
    return true;
}

int OpencvTestSuite::runPredefinedTest(const std::string& test_name, const std::string& path) {
    const auto& tests = getPredefinedTests();
    auto it = tests.find(test_name);
    if (it == tests.end()) {
        LOG4CPLUS_ERROR_FMT(getLogger(), "Unknown test '%s'", test_name.c_str());
        return 1;
    }

    const auto& params = it->second;

    // 检测是否为多生产者算术/逻辑运算（需要 COMPARE 模式）
    bool is_arithmetic_op = (params.opencv_op == OpencvTestParams::OpType::ADD ||
                             params.opencv_op == OpencvTestParams::OpType::ABSDIFF ||
                             params.opencv_op == OpencvTestParams::OpType::ADD_WEIGHTED ||
                             params.opencv_op == OpencvTestParams::OpType::BITWISE_AND ||
                             params.opencv_op == OpencvTestParams::OpType::BITWISE_OR ||
                             params.opencv_op == OpencvTestParams::OpType::BITWISE_XOR ||
                             params.opencv_op == OpencvTestParams::OpType::BITWISE_NOT);

    if (is_arithmetic_op) {
        // ⭐ ADD/ABSDIFF 需要两个解码器（hw 和 sw）进行对比
        auto hw_config = common::WorkerConfigFactory::buildOpencvConfig(
            path, params.params_str, true);   // hardware=true
        auto sw_config = common::WorkerConfigFactory::buildOpencvConfig(
            path, params.params_str, false);  // hardware=false

        uint32_t flags = consumer::CONSUME_COUNT;
        if (hw_config.consumer_type.opencv.enable) {
            flags |= consumer::CONSUME_OPENCV;
        }

        auto result = runCompare({hw_config, sw_config}, flags, test_name);
        consumer::BufferConsumerService::printResult(test_name, result);
        return result.success ? 0 : 1;
    } else {
        // ⭐ 其他操作使用 SINGLE 模式
        auto config = common::WorkerConfigFactory::buildOpencvConfig(
            path, params.params_str, params.use_hardware);

        uint32_t flags = buildConsumeFlags(config);

        auto result = runSingle(config, flags, test_name);
        consumer::BufferConsumerService::printResult(test_name, result);
        return result.success ? 0 : 1;
    }
}

void OpencvTestSuite::printHelp() const {
    std::cout << "empty\n"
              << std::endl;
}

void OpencvTestSuite::listTests() const {
    std::cout << "empty\n"
              << std::endl;
}

}
}
