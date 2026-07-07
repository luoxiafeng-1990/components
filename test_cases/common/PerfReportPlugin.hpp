#ifndef TEST_PERF_REPORT_PLUGIN_HPP
#define TEST_PERF_REPORT_PLUGIN_HPP

#include "common/PerfReport.hpp"
#include "common/SystemSnapshot.hpp"
#include "consumptionline/core/BufferConsumerService.hpp"
#include <string>

namespace CLI { class App; }

namespace test {

class PerfReportController {
public:
    void registerGlobalOptions(CLI::App& app);

    /// 任意一项输出（控制台、JSON、文件）被开启时为 true
    bool isEnabled() const {
        return console_enabled_ || perf_only_ ||
               !json_output_path_.empty() || !perf_file_path_.empty();
    }
    bool isConsoleEnabled() const { return console_enabled_; }
    bool isPerfOnly() const { return perf_only_; }
    const std::string& getPerfFilePath() const { return perf_file_path_; }

    void beginCapture(int argc, char** argv);

    perf::PerfReport buildReport(
        const consumer::ConsumeResult& result,
        const std::string& test_name,
        const std::string& module_name) const;

    int finalize(const perf::PerfReport& report) const;

    /// 将性能表格格式化为纯文本字符串（供控制台和文件共用）
    static std::string formatTable(
        const std::string& test_name,
        bool   overall_passed,
        double fps,
        double duration_seconds,
        int    frames_consumed,
        const std::vector<perf::StageTiming>& stages,
        const perf::PerfReport::SystemSnapshot* system = nullptr);

    /// 将运行中快照写入文件（覆盖模式）
    static bool writeSnapshotToFile(
        const std::string& path,
        const std::string& content);

private:
    bool        console_enabled_       = false;
    bool        perf_only_             = false;
    std::string perf_file_path_;
    std::string json_output_path_;
    std::string baseline_path_;
    double      regression_threshold_  = 5.0;
    mutable perf::SystemSnapshotCollector sys_collector_;
    std::string command_line_;

    static perf::PerfReport::Metadata detectPlatformMetadata();
};

}  // namespace test

#endif  // TEST_PERF_REPORT_PLUGIN_HPP
