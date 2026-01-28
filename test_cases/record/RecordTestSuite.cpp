/**
 * @file RecordTestSuite.cpp
 * @brief RecordTestSuite 实现
 */

#include "RecordTestSuite.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "../common/TestExecutor.hpp"

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
        {"rtsp_to_mp4",     {"mp4", 10.0, -1}},
        {"rtsp_to_mkv",     {"mkv", 10.0, -1}},
        {"rtsp_to_mov",     {"mov", 10.0, -1}},
        {"rtsp_to_ts",      {"ts",  10.0, -1}},
        {"rtsp_to_flv",     {"flv", 10.0, -1}},
        {"rtsp_to_avi",     {"avi", 10.0, -1}},
        {"rtsp_to_3gp",     {"3gp", 10.0, -1}},
        
        // 长时间录制测试
        {"rtsp_long_mp4",   {"mp4", 60.0, -1}},
        {"rtsp_long_mkv",   {"mkv", 60.0, -1}},
        
        // 文件重封装测试
        {"file_to_mp4",     {"mp4", -1, -1}},
        {"file_to_mkv",     {"mkv", -1, -1}},
        {"file_to_ts",      {"ts",  -1, -1}},
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
    
    common::TestResult result;
    
    if (common::WorkerConfigFactory::isRtspUrl(config.data_source.path)) {
        result = runRtspRecord(config.data_source.path, output_path, params);
    } else {
        result = runFileRecord(config.data_source.path, output_path, params);
    }
    
    std::string test_name = "Record to " + params.format;
    common::TestExecutor::printResult(test_name, result);
    
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
        {"all-formats", no_argument,     0, 'a'},
        {"verbose",   no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };
    
    std::string input_path;
    bool all_formats = false;
    
    int opt;
    while ((opt = getopt_long(argc, argv, "hlr:f:i:o:F:d:av",
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
            
            case 'a':
                all_formats = true;
                break;
            
            case 'v':
                config.consumer.verbose = true;
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
    
    // 如果测试所有格式
    if (all_formats) {
        auto result = runAllFormatsRecord(input_path, "/tmp");
        common::TestExecutor::printResult("All Formats Record", result);
        return false; // 已经执行完毕
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
    std::cout << "  -a, --all-formats       测试所有格式\n";
    std::cout << "  -v, --verbose           详细日志\n";
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
    std::cout << "  qa_cases record --all-formats rtsp://192.168.1.100/stream\n";
    std::cout << "\n";
}

void RecordTestSuite::listTests() const {
    std::cout << "\nAvailable RECORD tests:\n";
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
// 核心测试方法实现
// ========================================

common::TestResult RecordTestSuite::runRtspRecord(
    const std::string& rtsp_url,
    const std::string& output_path,
    const RecordTestParams& params
) {
    auto config = common::WorkerConfigFactory::createRtspRecord(rtsp_url);
    
    std::ostringstream test_name;
    test_name << "RTSP Record to " << params.format;
    
    common::TestExecutor::printHeader(test_name.str(), config);
    return common::TestExecutor::runRecord(config, output_path, params.duration);
}

common::TestResult RecordTestSuite::runFileRecord(
    const std::string& input_path,
    const std::string& output_path,
    const RecordTestParams& params
) {
    auto config = common::WorkerConfigFactory::createRtspRecord(input_path);
    
    std::ostringstream test_name;
    test_name << "File Remux to " << params.format;
    
    common::TestExecutor::printHeader(test_name.str(), config);
    return common::TestExecutor::runRecord(config, output_path, params.duration);
}

common::TestResult RecordTestSuite::runAllFormatsRecord(
    const std::string& input_source,
    const std::string& output_dir
) {
    common::TestResult total_result;
    total_result.success = true;
    
    std::vector<std::string> formats = {"mp4", "mkv", "mov", "ts", "flv", "avi", "3gp"};
    
    for (const auto& fmt : formats) {
        std::string output_path = output_dir + "/qa_record_" + 
                                  std::to_string(time(nullptr)) + "." + fmt;
        
        RecordTestParams params(fmt, 5.0); // 每个格式录 5 秒
        
        common::TestResult result;
        if (common::WorkerConfigFactory::isRtspUrl(input_source)) {
            result = runRtspRecord(input_source, output_path, params);
        } else {
            result = runFileRecord(input_source, output_path, params);
        }
        
        std::cout << "  " << fmt << ": " 
                  << (result.success ? "\033[32mPASS\033[0m" : "\033[31mFAIL\033[0m")
                  << " (" << result.packets_recorded << " packets)\n";
        
        if (!result.success) {
            total_result.success = false;
            total_result.error_message += fmt + " failed; ";
        }
        
        total_result.packets_recorded += result.packets_recorded;
        total_result.bytes_recorded += result.bytes_recorded;
    }
    
    return total_result;
}

} // namespace record
} // namespace test
