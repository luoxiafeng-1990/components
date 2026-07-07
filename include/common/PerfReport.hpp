#ifndef COMMON_PERF_REPORT_HPP
#define COMMON_PERF_REPORT_HPP

#include "common/StageTimer.hpp"
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace perf {

/// 统一性能报告数据模型
struct PerfReport {
    // ── 元数据 ──
    struct Metadata {
        std::string components_version;
        std::string platform_arch;
        std::string platform_os;
        std::string board_model;
        int         cpu_cores     = 0;
        int64_t     cpu_freq_mhz  = 0;
        std::string cpu_model;
        std::string timestamp;
        std::string test_command;
        std::string test_name;
    } metadata;

    // ── 端到端指标 ──
    struct EndToEnd {
        int    frames_consumed  = 0;
        int    frames_displayed = 0;
        int    frames_saved     = 0;
        double duration_seconds = 0;
        double average_fps      = 0;
        bool   fps_passed       = true;
        double target_fps       = 0;
    } end_to_end;

    // ── 阶段级计时 ──
    std::vector<StageTiming> stages;

    // ── 模块级扩展指标 ──
    struct ModuleMetrics {
        std::string module;
        std::map<std::string, double>      numeric;
        std::map<std::string, std::string> text;
        bool passed = true;
    };
    std::vector<ModuleMetrics> modules;

    // ── 质量指标 ──
    struct QualityMetrics {
        int    frames_compared = 0;
        double psnr_average    = 0;
        double ssim_average    = 0;
        bool   psnr_passed     = true;
        bool   ssim_passed     = true;
        bool   compare_passed  = true;
    };
    std::optional<QualityMetrics> quality;

    // ── 系统资源快照 ──
    struct SystemSnapshot {
        double cpu_usage_percent  = 0;
        double memory_used_mb    = 0;
        double memory_total_mb   = 0;
        double npu_usage_percent = -1;
    };
    std::optional<SystemSnapshot> system;

    // ── 并行模式子结果 ──
    std::vector<PerfReport> worker_reports;

    // ── 总体判定 ──
    bool overall_passed = true;

    // ── 序列化 ──
    std::string toJson(bool pretty = true) const;
    bool saveToFile(const std::string& path) const;

    // ── 基线对比 ──
    struct BaselineComparison {
        struct Diff {
            std::string metric_name;
            double baseline_value;
            double current_value;
            double change_percent;
            bool   is_regression;
        };
        std::vector<Diff> diffs;
        bool has_regression = false;
        std::string summary;
    };

    static BaselineComparison compareWithBaseline(
        const PerfReport& current,
        const PerfReport& baseline,
        double regression_threshold_percent = 5.0);

    static PerfReport loadFromJson(const std::string& path);
};

}  // namespace perf

#endif  // COMMON_PERF_REPORT_HPP
