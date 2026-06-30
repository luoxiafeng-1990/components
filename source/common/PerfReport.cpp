#include "common/PerfReport.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <cmath>
#include <ctime>

namespace perf {

using json = nlohmann::json;

std::string PerfReport::toJson(bool pretty) const {
    json j;

    j["metadata"] = {
        {"components_version", metadata.components_version},
        {"platform_arch",      metadata.platform_arch},
        {"platform_os",        metadata.platform_os},
        {"board_model",        metadata.board_model},
        {"cpu_cores",          metadata.cpu_cores},
        {"cpu_freq_mhz",       metadata.cpu_freq_mhz},
        {"cpu_model",          metadata.cpu_model},
        {"timestamp",          metadata.timestamp},
        {"test_command",       metadata.test_command},
        {"test_name",          metadata.test_name}
    };

    j["end_to_end"] = {
        {"frames_consumed",  end_to_end.frames_consumed},
        {"frames_displayed", end_to_end.frames_displayed},
        {"frames_saved",     end_to_end.frames_saved},
        {"duration_seconds", end_to_end.duration_seconds},
        {"average_fps",      end_to_end.average_fps},
        {"fps_passed",       end_to_end.fps_passed},
        {"target_fps",       end_to_end.target_fps}
    };

    j["stages"] = json::array();
    for (const auto& s : stages) {
        j["stages"].push_back({
            {"name",     s.name},
            {"count",    s.count},
            {"avg_ms",   s.avg_ms},
            {"min_ms",   s.min_ms},
            {"max_ms",   s.max_ms},
            {"p50_ms",   s.p50_ms},
            {"p95_ms",   s.p95_ms},
            {"p99_ms",   s.p99_ms},
            {"total_ms", s.total_ms}
        });
    }

    j["modules"] = json::array();
    for (const auto& m : modules) {
        json mj;
        mj["module"]  = m.module;
        mj["passed"]  = m.passed;
        mj["numeric"] = m.numeric;
        mj["text"]    = m.text;
        j["modules"].push_back(mj);
    }

    if (quality.has_value()) {
        j["quality"] = {
            {"frames_compared", quality->frames_compared},
            {"psnr_average",    quality->psnr_average},
            {"ssim_average",    quality->ssim_average},
            {"psnr_passed",     quality->psnr_passed},
            {"ssim_passed",     quality->ssim_passed},
            {"compare_passed",  quality->compare_passed}
        };
    }

    if (system.has_value()) {
        j["system"] = {
            {"cpu_usage_percent",  system->cpu_usage_percent},
            {"memory_used_mb",     system->memory_used_mb},
            {"memory_total_mb",    system->memory_total_mb},
            {"npu_usage_percent",  system->npu_usage_percent}
        };
    }

    if (!worker_reports.empty()) {
        j["worker_reports"] = json::array();
        for (const auto& wr : worker_reports) {
            j["worker_reports"].push_back(json::parse(wr.toJson(false)));
        }
    }

    j["overall_passed"] = overall_passed;

    return pretty ? j.dump(2) : j.dump();
}

bool PerfReport::saveToFile(const std::string& path) const {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << toJson(true);
    return f.good();
}

PerfReport PerfReport::loadFromJson(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open baseline file: " + path);
    }
    json j = json::parse(f);

    PerfReport report;
    if (j.contains("metadata")) {
        auto& m = j["metadata"];
        report.metadata.components_version = m.value("components_version", "");
        report.metadata.platform_arch      = m.value("platform_arch", "");
        report.metadata.platform_os        = m.value("platform_os", "");
        report.metadata.board_model        = m.value("board_model", "");
        report.metadata.cpu_cores          = m.value("cpu_cores", 0);
        report.metadata.cpu_freq_mhz       = m.value("cpu_freq_mhz", int64_t(0));
        report.metadata.cpu_model          = m.value("cpu_model", "");
        report.metadata.timestamp          = m.value("timestamp", "");
        report.metadata.test_command       = m.value("test_command", "");
        report.metadata.test_name          = m.value("test_name", "");
    }
    if (j.contains("end_to_end")) {
        auto& e = j["end_to_end"];
        report.end_to_end.frames_consumed  = e.value("frames_consumed", 0);
        report.end_to_end.frames_displayed = e.value("frames_displayed", 0);
        report.end_to_end.frames_saved     = e.value("frames_saved", 0);
        report.end_to_end.duration_seconds = e.value("duration_seconds", 0.0);
        report.end_to_end.average_fps      = e.value("average_fps", 0.0);
        report.end_to_end.fps_passed       = e.value("fps_passed", true);
        report.end_to_end.target_fps       = e.value("target_fps", 0.0);
    }
    if (j.contains("stages")) {
        for (auto& s : j["stages"]) {
            StageTiming st;
            st.name     = s.value("name", "");
            st.count    = s.value("count", int64_t(0));
            st.avg_ms   = s.value("avg_ms", 0.0);
            st.min_ms   = s.value("min_ms", 0.0);
            st.max_ms   = s.value("max_ms", 0.0);
            st.p50_ms   = s.value("p50_ms", 0.0);
            st.p95_ms   = s.value("p95_ms", 0.0);
            st.p99_ms   = s.value("p99_ms", 0.0);
            st.total_ms = s.value("total_ms", 0.0);
            report.stages.push_back(st);
        }
    }
    if (j.contains("modules")) {
        for (auto& mj : j["modules"]) {
            PerfReport::ModuleMetrics mm;
            mm.module = mj.value("module", "");
            mm.passed = mj.value("passed", true);
            if (mj.contains("numeric")) {
                for (auto& [k, v] : mj["numeric"].items()) {
                    mm.numeric[k] = v.get<double>();
                }
            }
            if (mj.contains("text")) {
                for (auto& [k, v] : mj["text"].items()) {
                    mm.text[k] = v.get<std::string>();
                }
            }
            report.modules.push_back(mm);
        }
    }
    if (j.contains("quality")) {
        auto& q = j["quality"];
        PerfReport::QualityMetrics qm;
        qm.frames_compared = q.value("frames_compared", 0);
        qm.psnr_average    = q.value("psnr_average", 0.0);
        qm.ssim_average    = q.value("ssim_average", 0.0);
        qm.psnr_passed     = q.value("psnr_passed", true);
        qm.ssim_passed     = q.value("ssim_passed", true);
        qm.compare_passed  = q.value("compare_passed", true);
        report.quality = qm;
    }
    if (j.contains("system")) {
        auto& sys = j["system"];
        PerfReport::SystemSnapshot ss;
        ss.cpu_usage_percent = sys.value("cpu_usage_percent", 0.0);
        ss.memory_used_mb    = sys.value("memory_used_mb", 0.0);
        ss.memory_total_mb   = sys.value("memory_total_mb", 0.0);
        ss.npu_usage_percent = sys.value("npu_usage_percent", -1.0);
        report.system = ss;
    }
    report.overall_passed = j.value("overall_passed", true);
    return report;
}

PerfReport::BaselineComparison PerfReport::compareWithBaseline(
    const PerfReport& current,
    const PerfReport& baseline,
    double regression_threshold_percent)
{
    BaselineComparison cmp;
    std::ostringstream summary;

    if (baseline.end_to_end.average_fps > 0) {
        double change = ((current.end_to_end.average_fps - baseline.end_to_end.average_fps)
                        / baseline.end_to_end.average_fps) * 100.0;
        bool regressed = change < -regression_threshold_percent;
        cmp.diffs.push_back({
            "end_to_end.average_fps",
            baseline.end_to_end.average_fps,
            current.end_to_end.average_fps,
            change, regressed
        });
        if (regressed) cmp.has_regression = true;
    }

    for (const auto& cur_stage : current.stages) {
        for (const auto& base_stage : baseline.stages) {
            if (cur_stage.name == base_stage.name && base_stage.avg_ms > 0) {
                double change = ((base_stage.avg_ms - cur_stage.avg_ms)
                                / base_stage.avg_ms) * 100.0;
                bool regressed = change < -regression_threshold_percent;
                cmp.diffs.push_back({
                    "stage." + cur_stage.name + ".avg_ms",
                    base_stage.avg_ms,
                    cur_stage.avg_ms,
                    change, regressed
                });
                if (regressed) cmp.has_regression = true;
            }
        }
    }

    for (const auto& cur_mod : current.modules) {
        for (const auto& base_mod : baseline.modules) {
            if (cur_mod.module == base_mod.module) {
                for (const auto& [key, cur_val] : cur_mod.numeric) {
                    auto it = base_mod.numeric.find(key);
                    if (it != base_mod.numeric.end() && it->second != 0) {
                        double change = ((cur_val - it->second) / std::abs(it->second)) * 100.0;
                        bool regressed = change < -regression_threshold_percent;
                        cmp.diffs.push_back({
                            cur_mod.module + "." + key,
                            it->second, cur_val, change, regressed
                        });
                        if (regressed) cmp.has_regression = true;
                    }
                }
            }
        }
    }

    int regressions = 0, improvements = 0;
    for (const auto& d : cmp.diffs) {
        if (d.is_regression) regressions++;
        else if (d.change_percent > regression_threshold_percent) improvements++;
    }
    summary << "Compared " << cmp.diffs.size() << " metrics: "
            << regressions << " regressions, "
            << improvements << " improvements";
    if (cmp.has_regression) {
        summary << " [REGRESSION DETECTED]";
    }
    cmp.summary = summary.str();
    return cmp;
}

}  // namespace perf
