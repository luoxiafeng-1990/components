#ifndef PERFORMANCE_MONITOR_HPP
#define PERFORMANCE_MONITOR_HPP

#include <stddef.h>
#include <chrono>

/**
 * PerformanceMonitor - 性能监控类
 * 
 * 职责：统计和计算性能数据
 * - 帧数统计（加载、解码、显示）
 * - 时间统计
 * - FPS计算
 * - 实时报告
 * 
 * 注意：
 * - 不包含定时器功能（请使用独立的 Timer 类）
 * - 线程安全：适合在单个线程中使用
 * 
 * 使用场景：
 * - 视频播放性能测试
 * - 解码性能评估
 * - 显示性能分析
 */
class PerformanceMonitor {
private:
    // ============ 时间记录 ============
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_report_time_;
    
    // ============ 帧数统计 ============
    int frames_loaded_;               // 加载的帧数
    int frames_decoded_;              // 解码的帧数（如果有解码）
    int frames_displayed_;            // 显示的帧数
    
    // ============ 时间统计 ============
    long long total_load_time_us_;    // 总加载时间（微秒）
    long long total_decode_time_us_;  // 总解码时间（微秒）
    long long total_display_time_us_; // 总显示时间（微秒）
    
    // ============ 临时计时器 ============
    std::chrono::steady_clock::time_point load_start_;
    std::chrono::steady_clock::time_point decode_start_;
    std::chrono::steady_clock::time_point display_start_;
    
    // ============ 状态标志 ============
    bool is_started_;
    bool is_paused_;
    
    // ============ 节流控制（用于实时打印）============
    int report_interval_ms_;          // 报告间隔（毫秒）
    
    // ============ 内部辅助方法 ============
    
    /**
     * 计算平均FPS
     */
    double calculateAverageFPS(int frame_count) const;
    
    /**
     * 获取从开始到现在的总时长（秒）
     */
    double getTotalDuration() const;

public:
    PerformanceMonitor();
    ~PerformanceMonitor();
    
    // ============ 生命周期管理 ============
    
    /**
     * 开始监控
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
    
    // ============ 简单事件记录 ============
    
    /**
     * 记录一次帧加载
     */
    void recordFrameLoaded();
    
    /**
     * 记录一次帧解码
     */
    void recordFrameDecoded();
    
    /**
     * 记录一次帧显示
     */
    void recordFrameDisplayed();
    
    // ============ 带计时的事件记录 ============
    
    /**
     * 开始计时：帧加载
     */
    void beginLoadFrameTiming();
    
    /**
     * 结束计时：帧加载
     */
    void endLoadFrameTiming();
    
    /**
     * 开始计时：帧解码
     */
    void beginDecodeFrameTiming();
    
    /**
     * 结束计时：帧解码
     */
    void endDecodeFrameTiming();
    
    /**
     * 开始计时：帧显示
     */
    void beginDisplayFrameTiming();
    
    /**
     * 结束计时：帧显示
     */
    void endDisplayFrameTiming();
    
    // ============ 统计信息获取 ============
    
    /**
     * 获取已加载的帧数
     */
    int getLoadedFrames() const;
    
    /**
     * 获取已解码的帧数
     */
    int getDecodedFrames() const;
    
    /**
     * 获取已显示的帧数
     */
    int getDisplayedFrames() const;
    
    /**
     * 获取平均加载FPS
     */
    double getAverageLoadFPS() const;
    
    /**
     * 获取平均解码FPS
     */
    double getAverageDecodeFPS() const;
    
    /**
     * 获取平均显示FPS
     */
    double getAverageDisplayFPS() const;
    
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
     * 打印完整的统计报告
     */
    void printStatistics() const;
    
    /**
     * 实时打印统计（带节流，避免频繁打印）
     * 默认每1秒最多打印一次
     */
    void printRealTimeStats();
    
    /**
     * 生成统计报告字符串
     */
    void generateReport(char* buffer, size_t buffer_size) const;
    
    // ============ 配置 ============
    
    /**
     * 设置实时报告的间隔（毫秒）
     * @param interval_ms 间隔时间（毫秒）
     */
    void setReportInterval(int interval_ms);
};

#endif // PERFORMANCE_MONITOR_HPP

