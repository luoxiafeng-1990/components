#include "../../include/monitor/Timer.hpp"
#include <stdio.h>

Timer::Timer(double interval_seconds, 
             void (*callback)(void*), 
             void* user_data,
             double delay_seconds,
             double duration_seconds,
             void (*delay_end_callback)(void*))
    : running_(false)
    , interval_seconds_(interval_seconds)
    , delay_seconds_(delay_seconds)
    , duration_seconds_(duration_seconds)
    , callback_(callback)
    , delay_end_callback_(delay_end_callback)
    , user_data_(user_data)
{
    if (interval_seconds_ <= 0) {
        printf("⚠️  Warning: Timer interval must be > 0, using 1.0 second\n");
        interval_seconds_ = 1.0;
    }
    
    if (delay_seconds_ < 0) {
        printf("⚠️  Warning: Delay must be >= 0, using 0\n");
        delay_seconds_ = 0.0;
    }
    
    if (duration_seconds_ < 0) {
        printf("⚠️  Warning: Duration must be >= 0, using 0 (infinite)\n");
        duration_seconds_ = 0.0;
    }
}

Timer::~Timer() {
    stop();
}

void Timer::start() {
    if (running_) {
        printf("⚠️  Timer is already running\n");
        return;
    }
    
    running_ = true;
    
    try {
        thread_ = std::thread(&Timer::timerLoop, this);
        printf("✅ Timer started (interval: %.1fs, delay: %.1fs, duration: %.1fs)\n",
               interval_seconds_, delay_seconds_, 
               duration_seconds_ > 0 ? duration_seconds_ : -1.0);
    } catch (const std::exception& e) {
        running_ = false;
        printf("❌ Failed to start timer thread: %s\n", e.what());
    }
}

void Timer::stop() {
    if (!running_) {
        return;
    }
    
    // 设置停止标志
    running_ = false;
    
    // 唤醒等待中的线程
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_all();
    }
    
    // 等待线程退出
    if (thread_.joinable()) {
        thread_.join();
    }
    
    printf("🛑 Timer stopped\n");
}

bool Timer::isRunning() const {
    return running_.load();
}

void Timer::timerLoop() {
    auto start_time = std::chrono::steady_clock::now();
    
    // ========== 处理延迟启动 ==========
    if (delay_seconds_ > 0) {
        printf("⏳ Timer waiting for %.1f seconds before starting...\n", delay_seconds_);
        
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, std::chrono::duration<double>(delay_seconds_),
                         [this] { return !running_; })) {
            // 被 stop() 唤醒，提前退出
            return;
        }
        
        if (!running_) {
            return;
        }
        
        printf("▶️  Timer delay period ended\n");
        
        // 🎯 触发延迟结束回调（如果提供了）
        if (delay_end_callback_) {
            printf("🔔 Triggering delay end callback...\n");
            try {
                delay_end_callback_(user_data_);
            } catch (const std::exception& e) {
                printf("⚠️  Exception in delay end callback: %s\n", e.what());
            } catch (...) {
                printf("⚠️  Unknown exception in delay end callback\n");
            }
        }
        
        printf("▶️  Starting periodic callbacks...\n");
    }
    
    // 记录实际开始时间（跳过延迟后）
    auto actual_start_time = std::chrono::steady_clock::now();
    int trigger_count = 0;
    
    // ========== 周期性触发循环 ==========
    while (running_) {
        // 检查是否超过总运行时长
        if (duration_seconds_ > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration<double>(now - actual_start_time).count();
            
            if (elapsed >= duration_seconds_) {
                printf("⏰ Timer reached duration limit (%.1fs), stopping...\n", 
                       duration_seconds_);
                running_ = false;
                break;
            }
        }
        
        // 等待下一个触发时刻
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (cv_.wait_for(lock, std::chrono::duration<double>(interval_seconds_),
                             [this] { return !running_; })) {
                // 被 stop() 唤醒，退出循环
                break;
            }
        }
        
        // 触发回调
        if (running_ && callback_) {
            trigger_count++;
            try {
                callback_(user_data_);
            } catch (const std::exception& e) {
                printf("⚠️  Exception in timer callback: %s\n", e.what());
            } catch (...) {
                printf("⚠️  Unknown exception in timer callback\n");
            }
        }
    }
    
    // 计算总运行时间
    auto end_time = std::chrono::steady_clock::now();
    auto total_elapsed = std::chrono::duration<double>(end_time - start_time).count();
    
    printf("🏁 Timer finished: %d triggers in %.1f seconds\n", trigger_count, total_elapsed);
}

