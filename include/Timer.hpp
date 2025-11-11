#ifndef TIMER_HPP
#define TIMER_HPP

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

/**
 * Timer - 简单的周期性定时器
 * 
 * 职责：
 * - 按固定间隔触发回调函数
 * - 支持延迟启动
 * - 支持自动停止
 * - 线程安全
 * 
 * 使用场景：
 * - 定期打印统计信息
 * - 定时执行任务
 * - 自动停止测试
 */
class Timer {
public:
    /**
     * 构造函数
     * 
     * @param interval_seconds   触发间隔（秒），如 1.0 表示每秒触发一次
     * @param callback           回调函数指针
     * @param user_data          传递给回调函数的用户数据
     * @param delay_seconds      延迟启动时间（秒），0表示立即开始，默认0
     * @param duration_seconds   总运行时长（秒），0表示永久运行，默认0
     * 
     * 示例：
     *   Timer timer(1.0, callback, data);              // 立即开始，永久运行
     *   Timer timer(1.0, callback, data, 5.0);         // 5秒后开始，永久运行
     *   Timer timer(1.0, callback, data, 0.0, 60.0);   // 立即开始，60秒后停止
     *   Timer timer(1.0, callback, data, 5.0, 60.0);   // 5秒后开始，60秒后停止
     */
    Timer(double interval_seconds, 
          void (*callback)(void*), 
          void* user_data,
          double delay_seconds = 0.0,
          double duration_seconds = 0.0);
    
    /**
     * 析构函数
     * 自动停止定时器并等待线程结束
     */
    ~Timer();
    
    /**
     * 启动定时器
     * 
     * 启动后台线程开始周期性触发回调。
     * 如果已经在运行，调用此方法无效。
     */
    void start();
    
    /**
     * 停止定时器
     * 
     * 停止后台线程并等待其完全退出。
     * 可以安全地多次调用。
     */
    void stop();
    
    /**
     * 检查定时器是否正在运行
     * 
     * @return true 如果定时器正在运行，false 否则
     */
    bool isRunning() const;

private:
    // 后台线程
    std::thread thread_;
    
    // 运行状态标志
    std::atomic<bool> running_;
    
    // 同步原语
    std::mutex mutex_;
    std::condition_variable cv_;
    
    // 定时器参数
    double interval_seconds_;   // 触发间隔
    double delay_seconds_;      // 延迟启动时间
    double duration_seconds_;   // 总运行时长
    
    // 回调函数
    void (*callback_)(void*);
    void* user_data_;
    
    /**
     * 定时器线程函数
     * 在后台线程中运行，处理延迟、周期触发和自动停止
     */
    void timerLoop();
};

#endif // TIMER_HPP

