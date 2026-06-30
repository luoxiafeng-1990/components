#ifndef COMMON_STAGE_TIMER_HPP
#define COMMON_STAGE_TIMER_HPP

#include <chrono>
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace perf {

/// 单个阶段的统计结果
struct StageTiming {
    std::string name;
    int64_t     count    = 0;
    double      avg_ms   = 0;
    double      min_ms   = 1e18;
    double      max_ms   = 0;
    double      p50_ms   = 0;
    double      p95_ms   = 0;
    double      p99_ms   = 0;
    double      total_ms = 0;
};

/// 线程安全的阶段计时收集器
class StageTimer {
public:
    explicit StageTimer(const std::string& stage_name, size_t reserve = 1024)
        : name_(stage_name) {
        samples_ms_.reserve(reserve);
    }

    /// 记录一次采样（单位：毫秒）
    void record(double elapsed_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_ms_.push_back(elapsed_ms);
    }

    /// RAII 作用域计时守卫
    class ScopedRecord {
    public:
        explicit ScopedRecord(StageTimer& timer)
            : timer_(timer), start_(std::chrono::steady_clock::now()) {}
        ~ScopedRecord() {
            auto end = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start_).count();
            timer_.record(ms);
        }
        ScopedRecord(const ScopedRecord&) = delete;
        ScopedRecord& operator=(const ScopedRecord&) = delete;
    private:
        StageTimer& timer_;
        std::chrono::steady_clock::time_point start_;
    };

    /// 生成统计结果
    StageTiming summarize() const {
        std::lock_guard<std::mutex> lock(mutex_);
        StageTiming result;
        result.name = name_;
        result.count = static_cast<int64_t>(samples_ms_.size());
        if (samples_ms_.empty()) return result;

        std::vector<double> sorted = samples_ms_;
        std::sort(sorted.begin(), sorted.end());

        result.total_ms = std::accumulate(sorted.begin(), sorted.end(), 0.0);
        result.avg_ms   = result.total_ms / result.count;
        result.min_ms   = sorted.front();
        result.max_ms   = sorted.back();
        result.p50_ms   = percentile(sorted, 0.50);
        result.p95_ms   = percentile(sorted, 0.95);
        result.p99_ms   = percentile(sorted, 0.99);
        return result;
    }

    const std::string& name() const { return name_; }
    size_t sampleCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return samples_ms_.size();
    }

private:
    static double percentile(const std::vector<double>& sorted, double p) {
        if (sorted.empty()) return 0;
        double idx = p * (sorted.size() - 1);
        size_t lo = static_cast<size_t>(idx);
        size_t hi = std::min(lo + 1, sorted.size() - 1);
        double frac = idx - lo;
        return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
    }

    std::string name_;
    mutable std::mutex mutex_;
    std::vector<double> samples_ms_;
};

}  // namespace perf

#endif  // COMMON_STAGE_TIMER_HPP
