#include "common/SystemSnapshot.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <array>
#include <cstdio>

namespace perf {

void SystemSnapshotCollector::begin() {
    begin_cpu_ = readCpuStat();
}

PerfReport::SystemSnapshot SystemSnapshotCollector::end() {
    PerfReport::SystemSnapshot snap;
    CpuStat end_cpu = readCpuStat();

    long long total_diff  = end_cpu.total() - begin_cpu_.total();
    long long active_diff = end_cpu.active() - begin_cpu_.active();
    snap.cpu_usage_percent = (total_diff > 0)
        ? (static_cast<double>(active_diff) / total_diff) * 100.0
        : 0.0;

    snap.memory_used_mb  = readMemoryUsedMB();
    snap.memory_total_mb = readMemoryTotalMB();
    snap.npu_usage_percent = readNpuUsage();

    return snap;
}

SystemSnapshotCollector::CpuStat SystemSnapshotCollector::readCpuStat() {
    CpuStat stat{};
    std::ifstream f("/proc/stat");
    std::string line;
    if (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string cpu_label;
        iss >> cpu_label >> stat.user >> stat.nice >> stat.system
            >> stat.idle >> stat.iowait >> stat.irq >> stat.softirq;
    }
    return stat;
}

double SystemSnapshotCollector::readMemoryUsedMB() {
    std::ifstream f("/proc/meminfo");
    std::string line;
    long total_kb = 0, avail_kb = 0;
    while (std::getline(f, line)) {
        if (line.find("MemTotal:") == 0) {
            std::sscanf(line.c_str(), "MemTotal: %ld kB", &total_kb);
        } else if (line.find("MemAvailable:") == 0) {
            std::sscanf(line.c_str(), "MemAvailable: %ld kB", &avail_kb);
        }
    }
    return static_cast<double>(total_kb - avail_kb) / 1024.0;
}

double SystemSnapshotCollector::readMemoryTotalMB() {
    std::ifstream f("/proc/meminfo");
    std::string line;
    long total_kb = 0;
    while (std::getline(f, line)) {
        if (line.find("MemTotal:") == 0) {
            std::sscanf(line.c_str(), "MemTotal: %ld kB", &total_kb);
            break;
        }
    }
    return static_cast<double>(total_kb) / 1024.0;
}

double SystemSnapshotCollector::readNpuUsage() {
    std::array<char, 256> buffer;
    std::string result;
    FILE* pipe = popen("tps-smi --query-npu=utilization.npu --format=csv,noheader 2>/dev/null", "r");
    if (!pipe) return -1.0;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    int ret = pclose(pipe);
    if (ret != 0 || result.empty()) return -1.0;

    try {
        return std::stod(result);
    } catch (...) {
        return -1.0;
    }
}

}  // namespace perf
