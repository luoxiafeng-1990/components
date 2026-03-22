/**
 * @file RecordPlugin.cpp
 * @brief RecordPlugin 实现
 * 
 * 重构为 IOptionPlugin 插件架构，使用 CLI11 解析选项
 */

#include "RecordPlugin.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "consumptionline/BufferConsumerService.hpp"
#include "../common/third_party/CLI11.hpp"

#include <iostream>
#include <sstream>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace record {

// 模块级日志实例
static log4cplus::Logger& getLogger() {
    static log4cplus::Logger logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.test.RecordSuite"));
    return logger;
}

// ========================================
// 预定义测试参数表
// ========================================

const std::map<std::string, RecordTestParams>& RecordPlugin::getPredefinedTests() {
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

// ========================================
// 支持的录制格式列表
// ========================================

const std::vector<std::string>& RecordPlugin::getAllFormats() {
    static std::vector<std::string> formats = {
        "mp4", "mkv", "mov", "ts", "flv", "avi", "3gp"
    };
    return formats;
}

// ========================================
// IOptionPlugin 接口实现
// ========================================

void RecordPlugin::registerOptions(CLI::App& app) {
    app.add_flag("-l,--list", show_list_, "列出所有预定义测试");
    app.add_option("-r,--rtsp", input_path_, "RTSP URL");
    app.add_option("-f,--file", input_path_, "输入文件路径");
    app.add_option("-i,--input", input_path_, "输入路径");
    app.add_option("-o,--output", output_path_, "输出文件路径");
    app.add_option("-F,--format", params_.format, "输出格式 (mp4|mkv|mov|ts|flv|avi|3gp)");
    app.add_option("--duration", params_.duration, "录制时长（秒，-1=无限制）");
    app.add_flag("-a,--all-formats", all_formats_, "测试所有输出格式");
    app.add_flag("-v,--verbose", verbose_, "详细日志");
    app.add_option("positional", positional_args_, "测试名或输入源路径");

    app.footer(
        "Examples:\n"
        "  qa_cases record --rtsp rtsp://192.168.1.100/stream\n"
        "  qa_cases record --rtsp rtsp://... --output /tmp/out.mp4\n"
        "  qa_cases record --file input.mkv --output /tmp/out.mp4\n"
        "  qa_cases record --all-formats --duration 5 --output /tmp/record_all\n"
    );
}

void RecordPlugin::applyTo(WorkerConfig& config) const {
    config.data_source = DataSourceConfigBuilder(config.data_source)
        .setPathIfNonEmpty(input_path_)
        .build();
    if (verbose_)
        config.consumer_type.verbose = true;
}

int RecordPlugin::handlePreActions() {
    // Process positional args
    for (const auto& a : positional_args_) {
        const auto& tests = getPredefinedTests();
        auto it = tests.find(a);
        if (it != tests.end()) { params_ = it->second; continue; }
        if (input_path_.empty()) { input_path_ = a; continue; }
        if (output_path_.empty()) { output_path_ = a; }
    }

    if (show_list_) { listTests(); return 0; }
    if (input_path_.empty()) {
        LOG4CPLUS_ERROR(getLogger(), "No input source specified");
        return 1;
    }
    return -1;
}

std::string RecordPlugin::getTestName() const {
    if (all_formats_) {
        std::ostringstream name;
        name << "Record All " << getAllFormats().size() << " formats";
        if (params_.duration > 0) name << " (" << params_.duration << "s each)";
        return name.str();
    }
    std::ostringstream name;
    name << "Record to " << params_.format;
    if (params_.duration > 0) name << " (" << params_.duration << "s)";
    return name.str();
}

std::vector<WorkerConfig> RecordPlugin::buildPipelineConfigs(const WorkerConfig& shared_config) {
    if (input_path_.empty()) return {};

    auto buildOne = [&](const std::string& fmt) -> WorkerConfig {
        std::string output = output_path_;
        if (output.empty()) {
            output = "/tmp/qa_record_" + std::to_string(time(nullptr));
        }
        if (all_formats_) {
            output += "_" + fmt + "." + fmt;
        } else if (output.find('.') == std::string::npos) {
            output += "." + fmt;
        }

        auto config = common::WorkerConfigFactory::createRtspRecord(shared_config.data_source.path);
        config.consumer_type.save_encoded.output_path = output;
        config.consumer_type.save_encoded.enable = true;
        config.consumer_type.max_duration_seconds = params_.duration;
        return config;
    };

    if (all_formats_) {
        std::vector<WorkerConfig> configs;
        for (const auto& fmt : getAllFormats()) {
            configs.push_back(buildOne(fmt));
        }
        return configs;
    }

    return {buildOne(params_.format)};
}

void RecordPlugin::listTests() const {
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

} // namespace record
} // namespace test
