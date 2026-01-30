/**
 * @file RecordTestSuite.cpp
 * @brief RecordTestSuite 实现
 * 
 * 重构为 ExecuteMode 风格，与 BufferConsumerService 架构对齐
 */

#include "RecordTestSuite.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "productionline/io/BufferConsumerService.hpp"

#include <iostream>
#include <getopt.h>
#include <cstring>
#include <sstream>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace record {

// 获取模块级日志实例
log4cplus::Logger& RecordTestSuite::getLogger() {
    static log4cplus::Logger logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.test.RecordSuite"));
    return logger;
}

// ========================================
// 预定义测试参数表
// ========================================

const std::map<std::string, RecordTestParams>& RecordTestSuite::getPredefinedTests() {
    static std::map<std::string, RecordTestParams> tests = {
        // 基本录制测试
        {"rtsp_to_mp4",     {"mp4", 10.0}},
        {"rtsp_to_mkv",     {"mkv", 10.0}},
        {"rtsp_to_mov",     {"mov", 10.0}},
        {"rtsp_to_ts",      {"ts",  10.0}},
        {"rtsp_to_flv",     {"flv", 10.0}},
        {"rtsp_to_avi",     {"avi", 10.0}},
        {"rtsp_to_3gp",     {"3gp", 10.0}},
        
        // 长时间录制测试
        {"rtsp_long_mp4",   {"mp4", 60.0}},
        {"rtsp_long_mkv",   {"mkv", 60.0}},
        
        // 文件重封装测试
        {"file_to_mp4",     {"mp4", -1}},
        {"file_to_mkv",     {"mkv", -1}},
        {"file_to_ts",      {"ts",  -1}},
    };
    return tests;
}

std::vector<std::string> RecordTestSuite::getTestNames() const {
    std::vector<std::string> names;
    for (const auto& pair : getPredefinedTests()) {
        names.push_back(pair.first);
    }
    return names;
}

// ========================================
// ITestModule 接口实现
// ========================================

int RecordTestSuite::run(int argc, char* argv[]) {
    WorkerConfig config;
    std::string output_path;
    RecordTestParams params;
    
    if (!parseArgs(argc, argv, config, output_path, params)) {
        return 1;
    }
    
    // 使用 runSingle（ExecuteMode::SINGLE + CONSUME_SAVE_ENCODED）
    auto result = runSingle(config.data_source.path, output_path, params);
    
    std::string test_name = "Record to " + params.format;
    consumer::BufferConsumerService::printResult(test_name, result);
    
    return result.success ? 0 : 1;
}

bool RecordTestSuite::parseArgs(int argc, char* argv[], WorkerConfig& config,
                                 std::string& output_path, RecordTestParams& params) {
    optind = 1;
    
    static struct option long_options[] = {
        {"help",      no_argument,       0, 'h'},
        {"list",      no_argument,       0, 'l'},
        {"rtsp",      required_argument, 0, 'r'},
        {"file",      required_argument, 0, 'f'},
        {"input",     required_argument, 0, 'i'},
        {"output",    required_argument, 0, 'o'},
        {"format",    required_argument, 0, 'F'},
        {"duration",  required_argument, 0, 'd'},
        {"verbose",   no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };
    
    std::string input_path;
    
    int opt;
    while ((opt = getopt_long(argc, argv, "hlr:f:i:o:F:d:v",
                              long_options, nullptr)) != -1) {
        switch (opt) {
            case 'h':
                printHelp();
                return false;
            
            case 'l':
                listTests();
                return false;
            
            case 'r':
            case 'f':
            case 'i':
                input_path = optarg;
                break;
            
            case 'o':
                output_path = optarg;
                break;
            
            case 'F':
                params.format = optarg;
                break;
            
            case 'd':
                params.duration = std::stod(optarg);
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
        } else if (output_path.empty()) {
            output_path = arg;
        }
    }
    
    if (input_path.empty()) {
        std::cerr << "Error: No input source specified\n";
        printHelp();
        return false;
    }
    
    config.data_source.path = input_path;
    
    // 如果没有指定输出路径，自动生成
    if (output_path.empty()) {
        output_path = "/tmp/qa_record_" + std::to_string(time(nullptr)) + "." + params.format;
    }
    
    return true;
}

void RecordTestSuite::printHelp() const {
    std::cout << "\n";
    std::cout << "RECORD Module - 录制测试\n";
    std::cout << "\n";
    std::cout << "Usage:\n";
    std::cout << "  qa_cases record [options] <input_source> [output_path]\n";
    std::cout << "  qa_cases record [options] <test_name> <input_source>\n";
    std::cout << "\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help              显示帮助信息\n";
    std::cout << "  -l, --list              列出所有预定义测试\n";
    std::cout << "  -r, --rtsp <url>        RTSP URL\n";
    std::cout << "  -f, --file <path>       输入文件路径\n";
    std::cout << "  -i, --input <path>      输入路径（自动检测类型）\n";
    std::cout << "  -o, --output <path>     输出文件路径\n";
    std::cout << "  -F, --format <fmt>      输出格式 (mp4|mkv|mov|ts|flv|avi|3gp)\n";
    std::cout << "  -d, --duration <sec>    录制时长（秒，-1=无限制）\n";
    std::cout << "  -v, --verbose           详细日志\n";
    std::cout << "\n";
    std::cout << "ExecuteMode: SINGLE + CONSUME_SAVE_ENCODED\n";
    std::cout << "\n";
    std::cout << "Supported formats:\n";
    std::cout << "  mp4   - MPEG-4 Part 14 (最常用)\n";
    std::cout << "  mkv   - Matroska Video\n";
    std::cout << "  mov   - QuickTime Movie\n";
    std::cout << "  ts    - MPEG Transport Stream\n";
    std::cout << "  flv   - Flash Video\n";
    std::cout << "  avi   - Audio Video Interleave\n";
    std::cout << "  3gp   - 3GPP (移动设备)\n";
    std::cout << "\n";
    std::cout << "Examples:\n";
    std::cout << "  qa_cases record --rtsp rtsp://192.168.1.100/stream\n";
    std::cout << "  qa_cases record --rtsp rtsp://... --output /tmp/out.mp4\n";
    std::cout << "  qa_cases record --file input.mkv --output /tmp/out.mp4\n";
    std::cout << "  qa_cases record rtsp_to_mp4 rtsp://192.168.1.100/stream\n";
    std::cout << "\n";
}

void RecordTestSuite::listTests() const {
    std::cout << "\nAvailable RECORD tests (ExecuteMode::SINGLE + CONSUME_SAVE_ENCODED):\n";
    std::cout << "────────────────────────────────────────────────────────\n";
    
    std::cout << "\nRTSP Recording Tests:\n";
    std::cout << "  rtsp_to_mp4         RTSP to MP4 (10s)\n";
    std::cout << "  rtsp_to_mkv         RTSP to MKV (10s)\n";
    std::cout << "  rtsp_to_mov         RTSP to MOV (10s)\n";
    std::cout << "  rtsp_to_ts          RTSP to TS (10s)\n";
    std::cout << "  rtsp_to_flv         RTSP to FLV (10s)\n";
    std::cout << "  rtsp_to_avi         RTSP to AVI (10s)\n";
    std::cout << "  rtsp_to_3gp         RTSP to 3GP (10s)\n";
    
    std::cout << "\nLong Recording Tests:\n";
    std::cout << "  rtsp_long_mp4       RTSP to MP4 (60s)\n";
    std::cout << "  rtsp_long_mkv       RTSP to MKV (60s)\n";
    
    std::cout << "\nFile Remux Tests:\n";
    std::cout << "  file_to_mp4         File remux to MP4\n";
    std::cout << "  file_to_mkv         File remux to MKV\n";
    std::cout << "  file_to_ts          File remux to TS\n";
    
    std::cout << "────────────────────────────────────────────────────────\n";
    std::cout << "\n";
}

// ========================================
// 核心测试方法实现（与 ExecuteMode 对齐）
// ========================================

TestResult RecordTestSuite::runSingle(
    const std::string& input_source,
    const std::string& output_path,
    const RecordTestParams& params
) {
    auto& logger = getLogger();
    
    // 构建配置
    auto config = common::WorkerConfigFactory::createRtspRecord(input_source);
    config.consumer_type.save_encoded.output_path = output_path;
    config.consumer_type.max_duration_seconds = params.duration;
    
    // 生成测试名称
    std::ostringstream test_name;
    test_name << "Record to " << params.format;
    if (params.duration > 0) {
        test_name << " (" << params.duration << "s)";
    }
    
    consumer::BufferConsumerService::printHeader(test_name.str(), config);
    
    LOG4CPLUS_DEBUG_FMT(logger, "runSingle: mode=SINGLE, flags=CONSUME_SAVE_ENCODED, output=%s", 
                        output_path.c_str());
    
    // 使用 BufferConsumerService，ExecuteMode::SINGLE + CONSUME_SAVE_ENCODED
    consumer::BufferConsumerService service;
    return service.start({config}, consumer::ExecuteMode::SINGLE, consumer::CONSUME_SAVE_ENCODED);
}

} // namespace record
} // namespace test
