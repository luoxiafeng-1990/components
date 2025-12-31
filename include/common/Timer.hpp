#ifndef TIMER_HPP
#define TIMER_HPP

#include <chrono>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <queue>
#include <vector>

/**
 * Timer - 企业级C++定时器实现
 * 
 * 设计参考：Google、Facebook、腾讯、阿里等大厂的定时器设计模式
 * 
 * 核心特性：
 * 1. 高精度：使用 std::chrono::steady_clock 确保单调递增，不受系统时间调整影响
 * 2. 线程安全：所有操作都支持多线程并发访问
 * 3. 支持单次和周期性定时器
 * 4. 支持取消定时器
 * 5. 高效：使用条件变量和优先队列，O(log n) 插入，O(1) 获取最小时间
 * 6. RAII：自动资源管理，析构时自动清理
 * 7. 异常安全：保证异常情况下资源正确释放
 * 
 * 使用场景：
 * - 周期性任务调度（如性能监控报告）
 * - 超时检测
 * - 延迟执行
 * - 心跳检测
 * 
 * 使用示例：
 * ```cpp
 * Timer timer;
 * 
 * // 单次定时器
 * timer.scheduleOnce(1000, []() {
 *     printf("1秒后执行\n");
 * });
 * 
 * // 周期性定时器
 * auto timer_id = timer.scheduleRepeated(500, []() {
 *     printf("每500ms执行一次\n");
 * });
 * 
 * // 取消定时器
 * timer.cancel(timer_id);
 * 
 * // 停止所有定时器
 * timer.stop();
 * ```
 */
class Timer {
public:
    using TimerId = uint64_t;
    using Callback = std::function<void()>;
    using TimePoint = std::chrono::steady_clock::time_point;
    using Duration = std::chrono::milliseconds;
    
    Timer();
    ~Timer();
    
    // 禁止拷贝和赋值（大厂最佳实践：定时器通常是单例或独占资源）
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    
    /**
     * 启动定时器服务（必须在调用其他方法前调用）
     */
    void start();
    
    /**
     * 停止定时器服务，取消所有待执行的定时器
     */
    void stop();
    
    /**
     * 调度一个单次定时器
     * @param delay_ms 延迟时间（毫秒）
     * @param callback 回调函数
     * @return 定时器ID，可用于取消
     */
    TimerId scheduleOnce(int delay_ms, Callback callback);
    
    /**
     * 调度一个周期性定时器
     * @param interval_ms 间隔时间（毫秒）
     * @param callback 回调函数
     * @return 定时器ID，可用于取消
     */
    TimerId scheduleRepeated(int interval_ms, Callback callback);
    
    /**
     * 取消指定的定时器
     * @param timer_id 定时器ID
     * @return 是否成功取消（如果定时器不存在或已执行，返回false）
     */
    bool cancel(TimerId timer_id);
    
    /**
     * 取消所有待执行的定时器
     */
    void cancelAll();
    
    /**
     * 获取当前待执行的定时器数量
     */
    size_t pendingCount() const;

private:
    /**
     * 定时器任务结构
     */
    struct TimerTask {
        TimerId id;
        TimePoint expire_time;      // 到期时间
        Duration interval;          // 间隔时间（0表示单次）
        Callback callback;          // 回调函数
        bool is_cancelled;          // 是否已取消
        
        // 用于优先队列的比较：到期时间越早优先级越高
        bool operator>(const TimerTask& other) const {
            return expire_time > other.expire_time;
        }
    };
    
    using TimerQueue = std::priority_queue<
        TimerTask,
        std::vector<TimerTask>,
        std::greater<TimerTask>
    >;
    
    // 定时器线程主循环
    void timerThreadLoop();
    
    // 执行到期的定时器
    void executeExpiredTimers();
    
    // 生成下一个定时器ID
    TimerId generateTimerId();
    
    // 线程安全保护
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    
    // 定时器任务队列（优先队列，按到期时间排序）
    TimerQueue timer_queue_;
    
    // 定时器线程
    std::thread timer_thread_;
    
    // 状态标志
    std::atomic<bool> is_running_;
    std::atomic<bool> should_stop_;
    
    // 定时器ID生成器
    std::atomic<TimerId> next_timer_id_;
};

#endif // TIMER_HPP
