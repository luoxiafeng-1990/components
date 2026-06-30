#ifndef COMMON_SYSTEM_SNAPSHOT_HPP
#define COMMON_SYSTEM_SNAPSHOT_HPP

#include "common/PerfReport.hpp"

namespace perf {

class SystemSnapshotCollector {
public:
    void begin();
    PerfReport::SystemSnapshot end();

private:
    struct CpuStat {
        long long user = 0, nice = 0, system = 0, idle = 0;
        long long iowait = 0, irq = 0, softirq = 0;
        long long total() const { return user + nice + system + idle + iowait + irq + softirq; }
        long long active() const { return total() - idle - iowait; }
    };

    static CpuStat readCpuStat();
    static double  readMemoryUsedMB();
    static double  readMemoryTotalMB();
    static double  readNpuUsage();

    CpuStat begin_cpu_;
};

}  // namespace perf

#endif  // COMMON_SYSTEM_SNAPSHOT_HPP
