#include "common/Timer.hpp"
#include <algorithm>
#include <cstdio>

// ============ 构造函数和析构函数 ============

Timer::Timer()
    : is_running_(false)
    , should_stop_(false)
    , next_timer_id_(1)
{
}

Timer::~Timer() {
    stop();
}

// ============ 生命周期管理 ============

void Timer::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (is_running_.load()) {
        return;  // 已经启动
    }
    
    is_running_.store(true);
    should_stop_.store(false);
    
    // 启动定时器线程
    timer_thread_ = std::thread(&Timer::timerThreadLoop, this);
    
    
    printf("⏰ Timer started\n");
}

void Timer::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (!is_running_.load()) {
            return;  // 未启动
        }
        
        should_stop_.store(true);
        is_running_.store(false);
        
        // 清空所有定时器
        while (!timer_queue_.empty()) {
            timer_queue_.pop();
        }
    }
    
    // 通知定时器线程退出
    cv_.notify_all();
    
    // 等待线程结束
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
    
    printf("⏰ Timer stopped\n");
}

// ============ 定时器调度 ============

Timer::TimerId Timer::scheduleOnce(int delay_ms, Callback callback) {
    if (delay_ms < 0) {
        delay_ms = 0;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_running_.load()) {
        printf("⚠️  Timer not started, call start() first\n");
        return 0;
    }
    
    TimerId id = generateTimerId();
    TimePoint now = std::chrono::steady_clock::now();
    TimePoint expire_time = now + std::chrono::milliseconds(delay_ms);
    
    TimerTask task;
    task.id = id;
    task.expire_time = expire_time;
    task.interval = Duration::zero();  // 单次定时器
    task.callback = std::move(callback);
    task.is_cancelled = false;
    
    timer_queue_.push(std::move(task));
    
    // 通知定时器线程检查新的定时器
    cv_.notify_one();
    
    return id;
}

Timer::TimerId Timer::scheduleRepeated(int interval_ms, Callback callback) {
    
    if (interval_ms <= 0) {
        printf("⚠️  Invalid interval: %d ms, must be > 0\n", interval_ms);
        return 0;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_running_.load()) {
        printf("⚠️  Timer not started, call start() first\n");
        return 0;
    }
    
    TimerId id = generateTimerId();
    TimePoint now = std::chrono::steady_clock::now();
    TimePoint expire_time = now + std::chrono::milliseconds(interval_ms);
    
    TimerTask task;
    task.id = id;
    task.expire_time = expire_time;
    task.interval = std::chrono::milliseconds(interval_ms);
    task.callback = std::move(callback);
    task.is_cancelled = false;
    
    timer_queue_.push(std::move(task));
    
    
    // 通知定时器线程检查新的定时器
    cv_.notify_one();
    
    return id;
}

// ============ 定时器取消 ============

bool Timer::cancel(TimerId timer_id) {
    if (timer_id == 0) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 由于优先队列不支持直接查找和修改，我们需要重建队列
    // 这是大厂常见做法：对于取消操作不频繁的场景，重建队列的开销可接受
    TimerQueue new_queue;
    bool found = false;
    
    while (!timer_queue_.empty()) {
        TimerTask task = timer_queue_.top();
        timer_queue_.pop();
        
        if (task.id == timer_id && !task.is_cancelled) {
            task.is_cancelled = true;
            found = true;
            // 不重新加入队列，相当于取消
        } else if (!task.is_cancelled) {
            new_queue.push(std::move(task));
        }
    }
    
    timer_queue_ = std::move(new_queue);
    
    if (found) {
        cv_.notify_one();
    }
    
    return found;
}

void Timer::cancelAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    while (!timer_queue_.empty()) {
        timer_queue_.pop();
    }
    
    cv_.notify_one();
}

// ============ 状态查询 ============

size_t Timer::pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 统计未取消的定时器数量
    size_t count = 0;
    TimerQueue temp_queue = timer_queue_;
    
    while (!temp_queue.empty()) {
        const TimerTask& task = temp_queue.top();
        if (!task.is_cancelled) {
            count++;
        }
        temp_queue.pop();
    }
    
    return count;
}

// ============ 内部实现 ============

void Timer::timerThreadLoop() {
    
    while (!should_stop_.load()) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // 等待直到有定时器到期或收到停止信号
        if (timer_queue_.empty()) {
            
            // 队列为空，等待新任务或停止信号
            // 注意：wait会释放锁，等待条件满足或收到通知
            cv_.wait(lock, [this] {
                return should_stop_.load() || !timer_queue_.empty();
            });
            
            // 如果收到停止信号，退出循环
            if (should_stop_.load()) {
                break;
            }
            
            // 如果队列仍然为空（虽然不太可能），继续下一次循环
            if (timer_queue_.empty()) {
                continue;
            }
        }
        
        // 队列不为空，处理定时器
        // 计算等待时间
        const TimerTask& next_task = timer_queue_.top();
        TimePoint now = std::chrono::steady_clock::now();
        
        
        if (next_task.expire_time <= now) {
            // 有定时器到期，执行它们
            lock.unlock();
            executeExpiredTimers();
            continue;
        }
        
        // 等待到下一个定时器到期时间
        auto wait_time = next_task.expire_time - now;
        cv_.wait_for(lock, wait_time, [this, &next_task] {
            if (should_stop_.load()) {
                return true;
            }
            // 检查是否有新的更早的定时器加入
            if (!timer_queue_.empty()) {
                const TimerTask& top = timer_queue_.top();
                return top.expire_time <= std::chrono::steady_clock::now() ||
                       top.expire_time < next_task.expire_time;
            }
            return false;
        });
        
        // 检查是否有到期的定时器
        if (!timer_queue_.empty()) {
            TimePoint now = std::chrono::steady_clock::now();
            const TimerTask& top = timer_queue_.top();
            
            if (top.expire_time <= now) {
                lock.unlock();
                executeExpiredTimers();
            }
        }
    }
}

void Timer::executeExpiredTimers() {
    
    std::vector<TimerTask> expired_tasks;
    std::vector<TimerTask> recurring_tasks;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        TimePoint now = std::chrono::steady_clock::now();
        
        // 收集所有到期的定时器
        while (!timer_queue_.empty()) {
            TimerTask task = timer_queue_.top();
            
            if (task.is_cancelled) {
                timer_queue_.pop();
                continue;
            }
            
            if (task.expire_time <= now) {
                timer_queue_.pop();
                
                if (task.interval.count() > 0) {
                    // 周期性定时器，需要重新调度
                    recurring_tasks.push_back(std::move(task));
                } else {
                    // 单次定时器
                    expired_tasks.push_back(std::move(task));
                }
            } else {
                break;  // 没有更多到期的定时器
            }
        }
        
        
        // 重新调度周期性定时器（更新到期时间，但保留回调函数）
        for (auto& task : recurring_tasks) {
            task.expire_time = std::chrono::steady_clock::now() + task.interval;
            // 注意：这里不能move，因为回调函数还需要在锁外执行
            timer_queue_.push(task);  // 使用拷贝而不是move
        }
    }
    
    // 在锁外执行回调，避免死锁和长时间阻塞
    // 🔧 修复：在执行回调之前，检查是否已经停止，避免在停止后继续执行回调
    if (should_stop_.load()) {
        return;  // 已经停止，不执行任何回调
    }
    
    // 先执行单次定时器的回调
    for (auto& task : expired_tasks) {
        // 🔧 修复：在执行每个回调之前，再次检查是否已经停止
        if (should_stop_.load()) {
            break;  // 已经停止，不再执行剩余回调
        }
        try {
            if (task.callback) {
                task.callback();
            }
        } catch (const std::exception& e) {
            printf("⚠️  Timer callback exception: %s\n", e.what());
        } catch (...) {
            printf("⚠️  Timer callback unknown exception\n");
        }
    }
    
    // 执行周期性定时器的回调（在重新调度之后，但回调函数仍然有效）
    for (const auto& task : recurring_tasks) {
        // 🔧 修复：在执行每个回调之前，再次检查是否已经停止
        if (should_stop_.load()) {
            break;  // 已经停止，不再执行剩余回调
        }
        try {
            if (task.callback) {
                task.callback();
            }
        } catch (const std::exception& e) {
            printf("⚠️  Timer callback exception: %s\n", e.what());
        } catch (...) {
            printf("⚠️  Timer callback unknown exception\n");
        }
    }
}

Timer::TimerId Timer::generateTimerId() {
    return next_timer_id_.fetch_add(1);
}
