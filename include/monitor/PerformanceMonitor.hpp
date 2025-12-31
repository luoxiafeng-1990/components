#ifndef PERFORMANCE_MONITOR_HPP
#define PERFORMANCE_MONITOR_HPP

#include <stddef.h>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <string>
#include <atomic>
#include "common/Timer.hpp"  // 新增：引入Timer

/**
 * PerformanceMonitor - 性能监控类（动态设计 + 定时器集成）
 * 
 * 职责：统计和计算性能数据
 * - 动态监控指标（支持运行时添加任意指标）
 * - 计数统计
 * - 时间统计
 * - FPS计算
 * - 实时报告（使用Timer自动触发）
 * 
 * 设计特点：
 * - 动态扩展：使用字符串标识符动态注册监控指标
 * - 通用接口：recordMetric(name), beginTiming(name), endTiming(name)
 * - 线程安全：所有操作都有互斥锁保护，可在多线程环境中使用
 * - 自动报告：使用Timer自动触发周期性报告，无需手动调用
 * 
 * 使用场景：
 * - 视频播放性能测试
 * - 解码性能评估
 * - 显示性能分析
 * - 任意自定义指标监控
 * 
 * 使用示例：
 * ```cpp
 * PerformanceMonitor monitor;
 * monitor.start();  // 自动开始定时报告
 * 
 * // 通用接口
 * monitor.recordMetric("buffer_filled");
 * monitor.beginTiming("decode_operation");
 * // ... 执行操作 ...
 * monitor.endTiming("decode_operation");
 * 
 * // 无需手动调用 printRealTimeStats()，Timer会自动触发
 * ```
 */
class PerformanceMonitor {
public:
    PerformanceMonitor();
    ~PerformanceMonitor();
    
    // ============ 生命周期管理 ============
    
    /**
     * 开始监控（自动启动定时报告）
     */
    void start();
    
    /**
     * 重置所有统计数据
     */
    void reset();
    
    /**
     * 暂停监控
     */
    void pause();
    
    /**
     * 恢复监控
     */
    void resume();
    
    /**
     * 停止监控（自动停止定时报告）
     */
    void stop();
    
    // ============ 通用接口（动态监控）===========
    
    /**
     * 记录一次指标计数（通用接口）
     * @param metric_name 指标名称（如 "load_frame", "buffer_filled"）
     */
    void recordMetric(const std::string& metric_name);
    
    /**
     * 开始计时（通用接口）
     * @param metric_name 指标名称
     */
    void beginTiming(const std::string& metric_name);
    
    /**
     * 结束计时并记录（通用接口）
     * @param metric_name 指标名称
     */
    void endTiming(const std::string& metric_name);
    
    /**
     * 获取指标计数
     * @param metric_name 指标名称
     * @return 计数
     */
    int getMetricCount(const std::string& metric_name) const;
    
    /**
     * 获取指标平均FPS
     * @param metric_name 指标名称
     * @return 平均FPS
     */
    double getMetricFPS(const std::string& metric_name) const;
    
    /**
     * 获取指标平均时间（毫秒）
     * @param metric_name 指标名称
     * @return 平均时间（毫秒），如果没有计时数据返回0
     */
    double getMetricAverageTime(const std::string& metric_name) const;
    
    /**
     * 获取总运行时间（秒）
     */
    double getTotalTime() const;
    
    /**
     * 获取从开始到现在的时间（秒）
     */
    double getElapsedTime() const;
    
    // ============ 报告输出 ============
    
    /**
     * 打印完整的统计报告（所有指标）
     */
    void printStatistics() const;
    
    /**
     * 打印单个指标的统计信息
     * @param metric_name 指标名称
     */
    void printMetric(const std::string& metric_name) const;
    
    /**
     * 实时打印统计（由Timer自动调用，无需手动调用）
     * 注意：此方法现在由Timer自动触发，但保留接口以支持手动调用
     */
    void printRealTimeStats();
    
    /**
     * 生成统计报告字符串（所有指标）
     */
    void generateReport(char* buffer, size_t buffer_size) const;
    
    // ============ 配置 ============
    
    /**
     * 设置实时报告的间隔（毫秒）
     * 注意：修改间隔会重启定时器
     * @param interval_ms 间隔时间（毫秒）
     */
    void setReportInterval(int interval_ms);

private:
    // ============ 监控指标数据结构 ============
    /**
     * @brief 单个监控指标的数据
     */
    struct MetricData {
        std::atomic<int> count{0};                    // 计数
        std::atomic<long long> total_time_us{0};     // 总时间（微秒）
        std::chrono::steady_clock::time_point start_time;  // 当前计时开始时间
        std::atomic<bool> is_timing{false};          // 是否正在计时
        
        MetricData() {
            count.store(0);
            total_time_us.store(0);
            is_timing.store(false);
        }
    };
    
    // ============ 线程安全保护 ============
    // 使用递归锁，允许同一线程多次加锁（避免 start() -> startReportTimer() 调用链中的死锁）
    mutable std::recursive_mutex mutex_;
    
    // ============ 时间记录 ============
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_report_time_;  // 保留用于兼容性
    
    // ============ 动态指标存储 ============
    std::unordered_map<std::string, MetricData> metrics_;  // 动态指标容器
    
    // ============ 状态标志 ============
    bool is_started_;
    bool is_paused_;
    
    // ============ 定时器集成 ============
    Timer report_timer_;                              // 定时器实例
    Timer::TimerId report_timer_id_;                 // 报告定时器ID（用于取消和重启）
    int report_interval_ms_;                          // 报告间隔（毫秒）
    
    // ============ 内部辅助方法 ============
    
    /**
     * 获取或创建指标数据（线程安全）
     * @param metric_name 指标名称
     * @return MetricData 的引用
     */
    MetricData& getOrCreateMetric(const std::string& metric_name);
    
    /**
     * 获取指标数据（只读，线程安全）
     * @param metric_name 指标名称
     * @return MetricData 的指针，如果不存在返回 nullptr
     */
    const MetricData* getMetric(const std::string& metric_name) const;
    
    /**
     * 计算平均FPS
     * @param count 计数
     * @return 平均FPS
     */
    double calculateAverageFPS(int count) const;
    
    /**
     * 获取从开始到现在的总时长（秒）
     */
    double getTotalDuration() const;
    
    /**
     * 启动或重启报告定时器（内部方法）
     */
    void startReportTimer();
    
    /**
     * 停止报告定时器（内部方法）
     */
    void stopReportTimer();
};

#endif // PERFORMANCE_MONITOR_HPP

