#ifndef COMMON_PERF_FILE_WRITER_HPP
#define COMMON_PERF_FILE_WRITER_HPP

#include "common/StageTimer.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace perf {

/// 轻量级性能快照文件写入器（header-only，供 library 和 test 共用）
class PerfFileWriter {
public:
    /// 将阶段计时格式化为 ASCII 表格
    static std::string formatSnapshot(
        const std::string& label,
        double fps,
        double elapsed_seconds,
        int frames,
        const std::vector<StageTiming>& stages)
    {
        const char* SEP1 = "======================================================================";
        const char* SEP2 = "----------------------------------------------------------------------";

        std::ostringstream os;
        os << SEP1 << "\n";
        os << "                  PERFORMANCE LATENCY REPORT SUMMARY\n";
        os << SEP1 << "\n";
        os << "  Test Name:  " << label << "\n";
        os << "  Status:     RUNNING\n";
        os << "  FPS:        " << std::fixed << std::setprecision(2) << fps << " fps\n";
        os << "  Duration:   " << std::fixed << std::setprecision(2) << elapsed_seconds << " s\n";
        os << "  Frames:     " << frames << "\n";
        os << SEP2 << "\n";
        os << "  " << std::left
           << std::setw(18) << "Stage"
           << std::setw(7)  << "Count"
           << std::setw(11) << "Avg(ms)"
           << std::setw(11) << "p50(ms)"
           << std::setw(11) << "p95(ms)"
           << std::setw(11) << "Max(ms)" << "\n";
        for (const auto& s : stages) {
            os << "  " << std::left
               << std::setw(18) << s.name
               << std::setw(7)  << s.count
               << std::setw(11) << std::fixed << std::setprecision(3) << s.avg_ms
               << std::setw(11) << s.p50_ms
               << std::setw(11) << s.p95_ms
               << std::setw(11) << s.max_ms << "\n";
        }
        os << SEP1 << "\n";
        return os.str();
    }

    /// 覆盖写入快照文件
    static bool writeToFile(const std::string& path, const std::string& content) {
        std::ofstream ofs(path, std::ios::trunc);
        if (!ofs.is_open()) return false;
        ofs << content;
        ofs.flush();
        return true;
    }
};

}  // namespace perf

#endif  // COMMON_PERF_FILE_WRITER_HPP
