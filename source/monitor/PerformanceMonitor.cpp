#include "monitor/PerformanceMonitor.hpp"
#include "common/Logger.hpp"
#include <string.h>
#include <utility>  // for std::piecewise_construct, std::forward_as_tuple

// ============ 构造函数和析构函数 ============

PerformanceMonitor::PerformanceMonitor()
    : is_started_(false)
    , is_paused_(false)
    , report_timer_id_(0)
    , report_interval_ms_(1000)  // 默认1秒报告一次
{
}

PerformanceMonitor::~PerformanceMonitor() {
    // 确保停止定时器
    stopReportTimer();
}

// ============ 生命周期管理 ============

void PerformanceMonitor::start() {
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    start_time_ = std::chrono::steady_clock::now();
    last_report_time_ = start_time_;
    is_started_ = true;
    is_paused_ = false;
    
    // 启动定时器服务
    report_timer_.start();
    
    
    // 启动报告定时器
    startReportTimer();
    
    LOG_INFO("📊 PerformanceMonitor started (auto-report enabled)");
}

void PerformanceMonitor::reset() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // 重置所有指标
    for (auto& pair : metrics_) {
        pair.second.count.store(0);
        pair.second.total_time_us.store(0);
        pair.second.is_timing.store(false);
    }
    
    start_time_ = std::chrono::steady_clock::now();
    last_report_time_ = start_time_;
}

void PerformanceMonitor::pause() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    is_paused_ = true;
}

void PerformanceMonitor::resume() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    is_paused_ = false;
}

void PerformanceMonitor::stop() {
    Timer::TimerId timer_id = 0;
    
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!is_started_) {
            return;  // 未启动，无需停止
        }
        // 🔧 修复：先获取定时器ID并重置，然后设置 is_started_ = false
        // 这样可以确保在取消定时器之前，不会有新的回调被调度
        if (report_timer_id_ != 0) {
            timer_id = report_timer_id_;
            report_timer_id_ = 0;
        }
        is_started_ = false;
        is_paused_ = false;
    }
    
    // 在锁外取消定时器（Timer是线程安全的）
    if (timer_id != 0) {
        report_timer_.cancel(timer_id);
    }
    
    // 🔧 修复：停止定时器服务本身，确保定时器完全停止
    // 注意：在锁外调用，因为 Timer 内部可能有自己的锁，避免死锁
    report_timer_.stop();
    
    LOG_INFO("📊 PerformanceMonitor stopped");
}

// ============ 通用接口（动态监控）===========

void PerformanceMonitor::recordMetric(const std::string& metric_name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_started_ || is_paused_) {
        return;
    }
    getOrCreateMetric(metric_name).count.fetch_add(1);
}

void PerformanceMonitor::beginTiming(const std::string& metric_name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_started_ || is_paused_) {
        return;
    }
    MetricData& metric = getOrCreateMetric(metric_name);
    metric.start_time = std::chrono::steady_clock::now();
    metric.is_timing.store(true);
}

void PerformanceMonitor::endTiming(const std::string& metric_name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_started_ || is_paused_) {
        return;
    }
    
    MetricData& metric = getOrCreateMetric(metric_name);
    if (!metric.is_timing.load()) {
        return;  // 没有开始计时，忽略
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        end - metric.start_time);
    
    metric.total_time_us.fetch_add(duration.count());
    metric.count.fetch_add(1);
    metric.is_timing.store(false);
}

int PerformanceMonitor::getMetricCount(const std::string& metric_name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const MetricData* metric = getMetric(metric_name);
    if (!metric) {
        return 0;
    }
    return metric->count.load();
}

double PerformanceMonitor::getMetricFPS(const std::string& metric_name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const MetricData* metric = getMetric(metric_name);
    if (!metric) {
        return 0.0;
    }
    return calculateAverageFPS(metric->count.load());
}

double PerformanceMonitor::getMetricAverageTime(const std::string& metric_name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const MetricData* metric = getMetric(metric_name);
    if (!metric) {
        return 0.0;
    }
    
    int count = metric->count.load();
    if (count == 0) {
        return 0.0;
    }
    
    long long total_us = metric->total_time_us.load();
    return (double)total_us / count / 1000.0;  // 转换为毫秒
}

// ============ 统计信息获取 ============
// 注意：便捷接口（getLoadedFrames等）已在头文件中内联实现

double PerformanceMonitor::getTotalTime() const {
    return getTotalDuration();
}

double PerformanceMonitor::getElapsedTime() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_started_) {
        return 0.0;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - start_time_);
    
    return duration.count() / 1000.0;
}

// ============ 报告输出 ============

void PerformanceMonitor::printStatistics() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    LOG_INFO("");
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO("          Performance Statistics");
    LOG_INFO("═══════════════════════════════════════════════════════");
    
    double total_time = getTotalDuration();
    
    // 打印所有指标
    if (metrics_.empty()) {
        LOG_INFO("No metrics recorded yet.");
    } else {
        for (const auto& pair : metrics_) {
            const std::string& name = pair.first;
            const MetricData& metric = pair.second;
            int count = metric.count.load();
            
            if (count > 0) {
                LOG_INFO("");
                LOG_INFO_FMT("📊 Metric: %s", name.c_str());
                LOG_INFO_FMT("   Count: %d", count);
                LOG_INFO_FMT("   Average FPS: %.2f fps", calculateAverageFPS(count));
                
                long long total_us = metric.total_time_us.load();
                if (total_us > 0) {
                    double avg_time = (double)total_us / count / 1000.0;
                    LOG_INFO_FMT("   Average Time: %.2f ms/event", avg_time);
                }
            }
        }
    }
    
    LOG_INFO("");
    LOG_INFO_FMT("⏱️  Total Time:       %.2f seconds", total_time);
    LOG_INFO("═══════════════════════════════════════════════════════");
    LOG_INFO("");
}

void PerformanceMonitor::printMetric(const std::string& metric_name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const MetricData* metric = getMetric(metric_name);
    if (!metric) {
        LOG_WARN_FMT("Metric '%s' not found.", metric_name.c_str());
        return;
    }
    
    int count = metric->count.load();
    if (count == 0) {
        LOG_INFO_FMT("Metric '%s': No data recorded yet.", metric_name.c_str());
        return;
    }
    
    LOG_INFO_FMT("📊 Metric: %s", metric_name.c_str());
    LOG_INFO_FMT("   Count: %d", count);
    LOG_INFO_FMT("   Average FPS: %.2f fps", calculateAverageFPS(count));
    
    long long total_us = metric->total_time_us.load();
    if (total_us > 0) {
        double avg_time = (double)total_us / count / 1000.0;
        LOG_INFO_FMT("   Average Time: %.2f ms/event", avg_time);
    }
}

void PerformanceMonitor::printRealTimeStats() {
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // 🔧 修复：双重检查，确保定时器未被停止
    // 检查 is_started_ 和 report_timer_id_，如果定时器已被取消（report_timer_id_ == 0），直接返回
    if (!is_started_ || report_timer_id_ == 0) {
        return;
    }
    
    // 注意：现在不再需要手动节流检查，因为Timer会自动控制调用频率
    // 但保留 last_report_time_ 用于兼容性
    
    auto now = std::chrono::steady_clock::now();
    last_report_time_ = now;
    
    // 打印实时统计
    std::string stats_line = "📊 Real-time Stats: ";
    
    bool first = true;
    for (const auto& pair : metrics_) {
        const std::string& name = pair.first;
        const MetricData& metric = pair.second;
        int count = metric.count.load();
        
        if (count > 0) {
            if (!first) {
                stats_line += " ";
            }
            // 计算这个周期内的FPS（基于报告间隔）
            double period_seconds = report_interval_ms_ / 1000.0;
            double period_fps = (period_seconds > 0) ? count / period_seconds : 0.0;
            
            // 计算平均时间（毫秒）
            long long total_us = metric.total_time_us.load();
            double avg_time_ms = (count > 0 && total_us > 0) ? 
                (double)total_us / count / 1000.0 : 0.0;
            
            char buf[256];
            snprintf(buf, sizeof(buf), "%s=%d (%.1f fps, avg=%.2f ms)", 
                    name.c_str(), count, period_fps, avg_time_ms);
            stats_line += buf;
            first = false;
        }
    }
    
    char time_buf[64];
    snprintf(time_buf, sizeof(time_buf), " Time=%.1fs", getElapsedTime());
    stats_line += time_buf;
    
    LOG_INFO(stats_line.c_str());
    
    // 打印后重置所有计数器（从0开始统计下一个周期）
    for (auto& pair : metrics_) {
        pair.second.count.store(0);
        pair.second.total_time_us.store(0);
        pair.second.is_timing.store(false);
    }
    // 重置开始时间（用于下一个周期的FPS计算）
    start_time_ = now;
}

void PerformanceMonitor::generateReport(char* buffer, size_t buffer_size) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!buffer || buffer_size == 0) {
        return;
    }
    
    int offset = 0;
    double total_time = getTotalDuration();
    
    offset += snprintf(buffer + offset, buffer_size - offset,
                      "Performance Report:\n");
    
    // 打印所有指标
    for (const auto& pair : metrics_) {
        const std::string& name = pair.first;
        const MetricData& metric = pair.second;
        int count = metric.count.load();
        
        if (count > 0) {
            offset += snprintf(buffer + offset, buffer_size - offset,
                              "  %s: %d events, %.2f fps\n",
                              name.c_str(), count, calculateAverageFPS(count));
        }
    }
    
    snprintf(buffer + offset, buffer_size - offset,
             "  Total time: %.2f seconds\n", total_time);
}

// ============ 配置 ============

void PerformanceMonitor::setReportInterval(int interval_ms) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    
    if (interval_ms <= 0) {
        LOG_WARN_FMT("⚠️  Invalid report interval: %d ms, must be > 0", interval_ms);
        return;
    }
    
    report_interval_ms_ = interval_ms;
    
    // 如果定时器正在运行，需要重启以应用新间隔
    if (is_started_ && report_timer_id_ != 0) {
        // 保存旧定时器ID，然后释放锁
        Timer::TimerId old_timer_id = report_timer_id_;
        report_timer_id_ = 0;
        lock.unlock();  // 释放锁，避免死锁（stopReportTimer和startReportTimer内部会加锁）
        
        // 在锁外调用Timer操作（Timer内部是线程安全的）
        report_timer_.cancel(old_timer_id);
        
        // 重新加锁并创建新定时器
        lock.lock();
        report_timer_id_ = report_timer_.scheduleRepeated(
            report_interval_ms_,
            [this]() {
                this->printRealTimeStats();
            }
        );
    }
}

// ============ 内部辅助方法 ============

PerformanceMonitor::MetricData& PerformanceMonitor::getOrCreateMetric(const std::string& metric_name) {
    // 注意：调用者必须已经持有 mutex_
    auto it = metrics_.find(metric_name);
    if (it == metrics_.end()) {
        // 创建新指标：使用 piecewise_construct 就地构造，避免复制 std::atomic 成员
        it = metrics_.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(metric_name),
            std::forward_as_tuple()
        ).first;
    }
    return it->second;
}

const PerformanceMonitor::MetricData* PerformanceMonitor::getMetric(const std::string& metric_name) const {
    // 注意：调用者必须已经持有 mutex_
    auto it = metrics_.find(metric_name);
    if (it == metrics_.end()) {
        return nullptr;
    }
    return &it->second;
}

double PerformanceMonitor::calculateAverageFPS(int count) const {
    // 注意：这个方法已经在调用者处加锁，不需要再次加锁
    if (!is_started_ || count == 0) {
        return 0.0;
    }
    
    double duration = getTotalDuration();
    if (duration <= 0.0) {
        return 0.0;
    }
    
    return count / duration;
}

double PerformanceMonitor::getTotalDuration() const {
    // 注意：这个方法已经在调用者处加锁，不需要再次加锁
    if (!is_started_) {
        return 0.0;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - start_time_);
    
    return duration.count() / 1000.0;
}

void PerformanceMonitor::startReportTimer() {
    
    Timer::TimerId old_timer_id = 0;
    
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        old_timer_id = report_timer_id_;
        report_timer_id_ = 0;  // 先重置，避免在锁外操作时被其他线程看到
    }
    
    // 在锁外取消旧定时器（Timer是线程安全的）
    if (old_timer_id != 0) {
        report_timer_.cancel(old_timer_id);
    }
    
    // 在锁外创建新定时器（Timer是线程安全的）
    Timer::TimerId new_timer_id = report_timer_.scheduleRepeated(
        report_interval_ms_,
        [this]() {
            // 在定时器线程中调用，但 printRealTimeStats 内部会加锁，所以是安全的
            this->printRealTimeStats();
        }
    );
    
    
    // 更新timer_id（需要加锁保护）
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        report_timer_id_ = new_timer_id;
    }
}

void PerformanceMonitor::stopReportTimer() {
    Timer::TimerId timer_id = 0;
    
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (report_timer_id_ != 0) {
            timer_id = report_timer_id_;
            report_timer_id_ = 0;
        }
    }
    
    // 在锁外取消定时器（Timer是线程安全的）
    if (timer_id != 0) {
        report_timer_.cancel(timer_id);
    }
    
    // 停止定时器服务（如果没有其他定时器在使用）
    // 注意：这里我们保留定时器服务运行，以便将来可能添加其他定时器
    // 如果需要完全停止，可以调用 report_timer_.stop()
}
