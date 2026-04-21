#pragma once

#include "common/IOptionPlugin.hpp"
#include <string>
#include <vector>
#include <log4cplus/logger.h>

namespace test::memleak {

/**
 * @brief 内存泄漏检测工具插件
 *
 * 分类：UTILITY（不走消费策略，由 main 直接调用 run()）
 *
 * 原理：
 * - 以 valgrind --tool=massif 或 --tool=memcheck 方式
 *   包裹一个子 qa_cases 进程
 * - 收集输出并生成泄漏报告（JSON + 可选 WebUI 展示）
 *
 * 命令行用法：
 *   ./qa_cases memleak --target "vdec --input xxx.h264"
 *   ./qa_cases memleak --target "vdec --input xxx.h264" --tool massif
 *   ./qa_cases memleak --target "vdec --input xxx.h264" --duration 60
 *   ./qa_cases memleak --report /tmp/memleak_report.json
 */
class MemleakPlugin : public IOptionPlugin {
public:
    std::string getName() const override { return "memleak"; }
    std::string getDescription() const override {
        return "内存泄漏检测工具 (valgrind wrapper)";
    }

    PluginCategory getCategory() const override { return PluginCategory::UTILITY; }

    void registerOptions(CLI::App& app) override;
    void applyTo(WorkerConfig& /*config*/) const override {}

    int run() override;

private:
    int runMemcheck();
    int runMassif();
    bool isValgrindAvailable() const;
    std::string buildChildCommand() const;
    int parseMemcheckOutput(const std::string& log_path);
    int parseMassifOutput(const std::string& output_path);

    std::string target_cmd_;
    std::string tool_ = "memcheck";
    int duration_sec_ = 0;
    std::string report_path_ = "/tmp/memleak_report.json";
    bool verbose_ = false;

    log4cplus::Logger logger_ =
        log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("qa_cases.memleak"));
};

} // namespace test::memleak
