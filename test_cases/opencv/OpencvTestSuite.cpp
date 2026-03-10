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
        add("cv_resize_h264_1080p_to_720p_30",    OpType::RESIZE, true,  "resize+1280+720");
        add("cv_resize_h264_1080p_to_480p_30",    OpType::RESIZE, true,  "resize+640+480");
        add("cv_resize_h265_1080p_to_720p_30",    OpType::RESIZE, true,  "resize+1280+720");
        add("cv_resize_sw_h264_1080p_to_720p_30", OpType::RESIZE, false, "resize+1280+720");

        // ---- OpenCV crop 预定义测试 ----
        add("cv_crop_h264_1080p_topleft_30",    OpType::CROP, true,  "crop+960+540");
        add("cv_crop_h264_1080p_center_30",     OpType::CROP, true,  "crop+960+540");
        add("cv_crop_h265_1080p_topleft_30",    OpType::CROP, true,  "crop+960+540");
        add("cv_crop_sw_h264_1080p_topleft_30", OpType::CROP, false, "crop+960+540");

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

    auto sw_config = common::WorkerConfigFactory::buildOpencvConfig(
        config.data_source.path, params.params_str, false);

    // 合并附加设置
    sw_config.consumer_type.verbose  = config.consumer_type.verbose;
    sw_config.data_source.max_frames = config.data_source.max_frames;

    uint32_t flags = buildConsumeFlags(hw_config);

    std::string filename = config.data_source.path;
    size_t pos = filename.find_last_of("/\\");
    if (pos != std::string::npos) {
        filename = filename.substr(pos + 1);
    }
    const std::string test_name = "Custom: " + filename;

    auto result = runCompare({hw_config,sw_config}, flags, test_name);
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
    } else {
        std::cerr << "Error: --case must be 'resize' or 'crop', got '" << case_str << "'\n" << std::endl;
        return false;
    }

    // 将 case 和 params 用 + 拼接
    params.params_str = case_str;
    if (!params_arg.empty()) {
        params.params_str += "+" + params_arg;
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
