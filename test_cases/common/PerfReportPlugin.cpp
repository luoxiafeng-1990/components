#include "common/PerfReportPlugin.hpp"
#include "common/Logger.hpp"
#include "common/third_party/CLI11.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <ctime>
#include <sys/utsname.h>
#include <map>
#include <numeric>
#include <algorithm>

namespace test {

void PerfReportController::registerGlobalOptions(CLI::App& app) {
    app.add_flag("--perf", console_enabled_,
        "在测试结束后打印控制台性能统计摘要");
    app.add_flag("--perf-only", perf_only_,
        "静默所有日志，仅输出性能统计（适配 watch 命令）");
    app.add_option("--perf-file", perf_file_path_,
        "运行中周期性写入性能快照的文件路径（双窗口模式，覆盖写入）");
    app.add_option("--json", json_output_path_,
        "输出性能报告 JSON 文件路径（同时隐含 --perf）");
    app.add_option("--baseline", baseline_path_,
        "基线 JSON 文件路径，用于性能回归对比");
    app.add_option("--regression-threshold", regression_threshold_,
        "回归检测阈值百分比（默认 5.0）")
        ->default_val(5.0);
}

void PerfReportController::beginCapture(int argc, char** argv) {
    std::ostringstream cmd;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) cmd << " ";
        cmd << argv[i];
    }
    command_line_ = cmd.str();
    sys_collector_.begin();
}

// ── 格式化性能表格为纯文本（复用于控制台和文件输出） ──
std::string PerfReportController::formatTable(
    const std::string& test_name,
    bool   overall_passed,
    double fps,
    double duration_seconds,
    int    frames_consumed,
    const std::vector<perf::StageTiming>& stages,
    const perf::PerfReport::SystemSnapshot* system)
{
    const char* SEP1 = "======================================================================";
    const char* SEP2 = "----------------------------------------------------------------------";

    std::ostringstream os;
    os << SEP1 << "\n";
    os << "                  PERFORMANCE LATENCY REPORT SUMMARY\n";
    os << SEP1 << "\n";
    os << "  Test Name:  " << test_name << "\n";
    os << "  Status:     " << (overall_passed ? "PASSED" : "FAILED") << "\n";
    os << "  FPS:        " << std::fixed << std::setprecision(2) << fps << " fps\n";
    os << "  Duration:   " << std::fixed << std::setprecision(2) << duration_seconds << " s\n";
    os << "  Frames:     " << frames_consumed << "\n";
    os << SEP2 << "\n";

    // 阶段延迟表头
    os << "  " << std::left
       << std::setw(18) << "Stage"
       << std::setw(7)  << "Count"
       << std::setw(11) << "Avg(ms)"
       << std::setw(11) << "p50(ms)"
       << std::setw(11) << "p95(ms)"
       << std::setw(11) << "Max(ms)" << "\n";

    for (const auto& s : stages) {
        if (s.count == 0) continue;  // 跳过未采集数据的阶段
        os << "  " << std::left
           << std::setw(18) << s.name
           << std::setw(7)  << s.count
           << std::setw(11) << std::fixed << std::setprecision(3) << s.avg_ms
           << std::setw(11) << s.p50_ms
           << std::setw(11) << s.p95_ms
           << std::setw(11) << s.max_ms << "\n";
    }

    os << SEP2 << "\n";

    if (system) {
        os << "  CPU Usage:  " << std::fixed << std::setprecision(1)
           << system->cpu_usage_percent << " %\n";
        os << "  Memory:     " << std::fixed << std::setprecision(1)
           << system->memory_used_mb << " MB / "
           << system->memory_total_mb << " MB\n";
        if (system->npu_usage_percent >= 0) {
            os << "  NPU Usage:  " << std::fixed << std::setprecision(1)
               << system->npu_usage_percent << " %\n";
        } else {
            os << "  NPU Usage:  N/A\n";
        }
    }

    os << SEP1 << "\n";
    return os.str();
}

// ── 覆盖写入快照文件 ──
bool PerfReportController::writeSnapshotToFile(
    const std::string& path, const std::string& content)
{
    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) return false;
    ofs << content;
    ofs.flush();
    return true;
}

perf::PerfReport PerfReportController::buildReport(
    const consumer::ConsumeResult& result,
    const std::string& test_name,
    const std::string& module_name) const
{
    perf::PerfReport report;

    report.metadata = detectPlatformMetadata();
    report.metadata.test_command = command_line_;
    report.metadata.test_name = test_name;

    auto now = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", std::localtime(&now));
    report.metadata.timestamp = buf;

    report.end_to_end.frames_consumed  = result.frames_consumed;
    report.end_to_end.frames_displayed = result.frames_displayed;
    report.end_to_end.frames_saved     = result.frames_saved;
    report.end_to_end.duration_seconds = result.duration_seconds;
    report.end_to_end.average_fps      = result.average_fps;
    report.end_to_end.fps_passed       = result.fps_passed;
    report.end_to_end.target_fps       = result.target_fps;

    report.stages = result.stage_timings;

    perf::PerfReport::ModuleMetrics mm;
    mm.module = module_name;
    mm.numeric["fps"] = result.average_fps;
    mm.numeric["frames"] = static_cast<double>(result.frames_consumed);
    mm.numeric["duration_sec"] = result.duration_seconds;
    mm.passed = result.getOverallResult();
    report.modules.push_back(mm);

    if (result.frames_compared > 0) {
        perf::PerfReport::QualityMetrics qm;
        qm.frames_compared = result.frames_compared;
        qm.psnr_average    = result.psnr_average;
        qm.ssim_average    = result.ssim_average;
        qm.psnr_passed     = result.psnr_passed;
        qm.ssim_passed     = result.ssim_passed;
        qm.compare_passed  = result.compare_passed;
        report.quality = qm;
    }

    report.system = sys_collector_.end();

    for (const auto& wr : result.worker_results) {
        report.worker_reports.push_back(
            buildReport(wr, test_name + "_worker", module_name));
    }

    report.overall_passed = result.getOverallResult();
    return report;
}

int PerfReportController::finalize(const perf::PerfReport& report) const {
    int exit_code = report.overall_passed ? 0 : 1;

    // ── 汇总并行模式下的 stages ──
    std::vector<perf::StageTiming> display_stages = report.stages;
    if (display_stages.empty() && !report.worker_reports.empty()) {
        std::map<std::string, std::vector<double>> stage_avgs;
        std::map<std::string, std::vector<double>> stage_p50s;
        std::map<std::string, std::vector<double>> stage_p95s;
        std::map<std::string, std::vector<double>> stage_maxs;
        std::map<std::string, int64_t> stage_counts;
        for (const auto& wr : report.worker_reports) {
            for (const auto& s : wr.stages) {
                if (s.count > 0) {
                    stage_avgs[s.name].push_back(s.avg_ms);
                    stage_p50s[s.name].push_back(s.p50_ms);
                    stage_p95s[s.name].push_back(s.p95_ms);
                    stage_maxs[s.name].push_back(s.max_ms);
                    stage_counts[s.name] += s.count;
                }
            }
        }
        for (const auto& [name, avgs] : stage_avgs) {
            perf::StageTiming aggregated;
            aggregated.name = name + " (avg)";
            aggregated.count = stage_counts[name];
            aggregated.avg_ms = std::accumulate(avgs.begin(), avgs.end(), 0.0) / avgs.size();
            
            auto& p50s = stage_p50s[name];
            aggregated.p50_ms = std::accumulate(p50s.begin(), p50s.end(), 0.0) / p50s.size();
            
            auto& p95s = stage_p95s[name];
            aggregated.p95_ms = std::accumulate(p95s.begin(), p95s.end(), 0.0) / p95s.size();
            
            auto& maxs = stage_maxs[name];
            aggregated.max_ms = *std::max_element(maxs.begin(), maxs.end());
            display_stages.push_back(aggregated);
        }
    }

    // 控制台性能摘要（--perf、--perf-only 或 --json 时打印）
    bool print_console = console_enabled_ || perf_only_ || !json_output_path_.empty();
    if (print_console) {
        const perf::PerfReport::SystemSnapshot* sys_ptr =
            report.system.has_value() ? &report.system.value() : nullptr;
        std::string table = formatTable(
            report.metadata.test_name,
            report.overall_passed,
            report.end_to_end.average_fps,
            report.end_to_end.duration_seconds,
            report.end_to_end.frames_consumed,
            display_stages,
            sys_ptr);
        std::cout << "\n" << table << "\n";
        std::cout.flush();
    }

    // --perf-file 最终写入（覆盖运行中的快照为最终结果）
    if (!perf_file_path_.empty()) {
        const perf::PerfReport::SystemSnapshot* sys_ptr =
            report.system.has_value() ? &report.system.value() : nullptr;
        std::string table = formatTable(
            report.metadata.test_name,
            report.overall_passed,
            report.end_to_end.average_fps,
            report.end_to_end.duration_seconds,
            report.end_to_end.frames_consumed,
            display_stages,
            sys_ptr);
        writeSnapshotToFile(perf_file_path_, table);
    }

    if (!json_output_path_.empty()) {
        if (report.saveToFile(json_output_path_)) {
            LOG_INFO_FMT("Performance report saved to: %s", json_output_path_.c_str());
        } else {
            LOG_ERROR_FMT("Failed to save performance report to: %s", json_output_path_.c_str());
        }
    }

    if (!baseline_path_.empty()) {
        try {
            auto baseline = perf::PerfReport::loadFromJson(baseline_path_);
            auto cmp = perf::PerfReport::compareWithBaseline(
                report, baseline, regression_threshold_);

            LOG_INFO_FMT("Baseline comparison: %s", cmp.summary.c_str());

            for (const auto& d : cmp.diffs) {
                const char* arrow = d.change_percent >= 0 ? "+" : "-";
                const char* flag  = d.is_regression ? " [REGRESSION]" : "";
                LOG_INFO_FMT("  %s: %.2f -> %.2f (%s%.1f%%)%s",
                    d.metric_name.c_str(),
                    d.baseline_value, d.current_value,
                    arrow, std::abs(d.change_percent), flag);
            }

            if (cmp.has_regression) {
                exit_code = 2;
            }
        } catch (const std::exception& e) {
            LOG_WARN_FMT("Failed to load baseline: %s", e.what());
        }
    }

    return exit_code;
}

perf::PerfReport::Metadata PerfReportController::detectPlatformMetadata() {
    perf::PerfReport::Metadata meta;

    struct utsname uts;
    if (uname(&uts) == 0) {
        meta.platform_arch = uts.machine;
        meta.platform_os = std::string(uts.sysname) + " " + uts.release;
    }

    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    int cores = 0;
    while (std::getline(cpuinfo, line)) {
        if (line.find("processor") == 0) cores++;
        if (line.find("model name") != std::string::npos ||
            line.find("isa") != std::string::npos) {
            auto pos = line.find(':');
            if (pos != std::string::npos && pos + 2 < line.size()) {
                meta.cpu_model = line.substr(pos + 2);
            }
        }
    }
    meta.cpu_cores = cores;

    std::ifstream freq_f("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    long freq_khz = 0;
    if (freq_f >> freq_khz) {
        meta.cpu_freq_mhz = freq_khz / 1000;
    }

#ifdef COMPONENTS_VERSION
    meta.components_version = COMPONENTS_VERSION;
#else
    meta.components_version = "unknown";
#endif

    return meta;
}

}  // namespace test
