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

    auto hw_config = common::WorkerConfigFactory::buildOpencvConfig(
        config.data_source.path, params.params_str, params.use_hardware);

    // 合并附加设置
    hw_config.consumer_type.verbose  = config.consumer_type.verbose;
    hw_config.data_source.max_frames = config.data_source.max_frames;

    uint32_t flags = buildConsumeFlags(hw_config);

    std::string filename = config.data_source.path;
    size_t pos = filename.find_last_of("/\\");
    if (pos != std::string::npos) {
        filename = filename.substr(pos + 1);
    }
    const std::string test_name = "Custom: " + filename;

    auto result = runSingle(hw_config, flags, test_name);
    consumer::BufferConsumerService::printResult(test_name, result);

    return result.success ? 0 : 1;
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
    } else {
        std::cerr << "Error: unknown --case '" << case_str
                  << "'. Valid: resize/crop/erode/dilate/open/close/"
                     "sobel/canny/laplacian/translate/rotate/perspective/"
                     "line/rectangle/puttext/blur/threshold\n" << std::endl;
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
    auto config = common::WorkerConfigFactory::buildOpencvConfig(
        path, params.params_str, params.use_hardware);

    uint32_t flags = buildConsumeFlags(config);

    auto result = runSingle(config, flags, test_name);
    consumer::BufferConsumerService::printResult(test_name, result);
    return result.success ? 0 : 1;
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
