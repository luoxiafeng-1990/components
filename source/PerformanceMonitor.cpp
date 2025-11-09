#include "../include/PerformanceMonitor.hpp"
#include "../include/BufferManager.hpp"
#include <stdio.h>
#include <string.h>

// ============ 构造函数和析构函数 ============

PerformanceMonitor::PerformanceMonitor()
    : frames_loaded_(0)
    , frames_decoded_(0)
    , frames_displayed_(0)
    , total_load_time_us_(0)
    , total_decode_time_us_(0)
    , total_display_time_us_(0)
    , is_started_(false)
    , is_paused_(false)
    , report_interval_ms_(1000)  // 默认1秒报告一次
    , timer_interval_seconds_(1.0)  // 默认1秒触发一次
    , timer_delay_seconds_(0.0)  // 默认无延迟
    , timer_running_(false)
    , timer_in_delay_period_(false)  // 默认：不在延迟期间
    , timer_task_type_(TASK_PRINT_FULL_STATS)  // 默认任务：完整统计
    , is_oneshot_timer_(false)  // 默认：周期性定时器
    , user_callback_(NULL)  // 默认：无用户回调
    , user_callback_data_(NULL)
    , last_frames_loaded_(0)
    , last_frames_decoded_(0)
    , last_frames_displayed_(0)
    , timer_start_frames_loaded_(0)
    , timer_start_frames_decoded_(0)
    , timer_start_frames_displayed_(0)
    , timer_real_start_time_()  // 默认构造为无效值
    , auto_stop_timer_(nullptr)  // 初始化为空指针
    , baseline_time_()  // 默认构造为无效值
    , baseline_display_frames_(0)
    , baseline_load_frames_(0)
    , baseline_decode_frames_(0)
    , buffer_manager_(nullptr)  // 初始化为空指针
{
}

PerformanceMonitor::~PerformanceMonitor() {
    // 清理自动停止定时器（必须在stopTimer之前，避免使用已删除的对象）
    if (auto_stop_timer_) {
        delete auto_stop_timer_;
        auto_stop_timer_ = nullptr;
    }
    
    // 确保定时器停止
    stopTimer();
}

// ============ 生命周期管理 ============

void PerformanceMonitor::start() {
    start_time_ = std::chrono::steady_clock::now();
    last_report_time_ = start_time_;
    is_started_ = true;
    is_paused_ = false;
    
    printf("📊 PerformanceMonitor started\n");
}

void PerformanceMonitor::reset() {
    frames_loaded_ = 0;
    frames_decoded_ = 0;
    frames_displayed_ = 0;
    total_load_time_us_ = 0;
    total_decode_time_us_ = 0;
    total_display_time_us_ = 0;
    
    start_time_ = std::chrono::steady_clock::now();
    last_report_time_ = start_time_;
    
    printf("📊 PerformanceMonitor reset\n");
}

void PerformanceMonitor::pause() {
    is_paused_ = true;
}

void PerformanceMonitor::resume() {
    is_paused_ = false;
}

// ============ 简单事件记录 ============

void PerformanceMonitor::recordFrameLoaded() {
    if (!is_started_ || is_paused_) {
        return;
    }
    frames_loaded_++;
}

void PerformanceMonitor::recordFrameDecoded() {
    if (!is_started_ || is_paused_) {
        return;
    }
    frames_decoded_++;
}

void PerformanceMonitor::recordFrameDisplayed() {
    if (!is_started_ || is_paused_) {
        return;
    }
    frames_displayed_++;
}

// ============ 带计时的事件记录 ============

void PerformanceMonitor::beginLoadFrameTiming() {
    if (!is_started_ || is_paused_) {
        return;
    }
    load_start_ = std::chrono::steady_clock::now();
}

void PerformanceMonitor::endLoadFrameTiming() {
    if (!is_started_ || is_paused_) {
        return;
    }
    
    // 如果在延迟期间，不记录数据
    if (timer_in_delay_period_.load()) {
        return;  // 延迟期间，不统计
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        end - load_start_);
    
    total_load_time_us_ += duration.count();
    frames_loaded_++;
}

void PerformanceMonitor::beginDecodeFrameTiming() {
    if (!is_started_ || is_paused_) {
        return;
    }
    decode_start_ = std::chrono::steady_clock::now();
}

void PerformanceMonitor::endDecodeFrameTiming() {
    if (!is_started_ || is_paused_) {
        return;
    }
    
    // 如果在延迟期间，不记录数据
    if (timer_in_delay_period_.load()) {
        return;  // 延迟期间，不统计
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        end - decode_start_);
    
    total_decode_time_us_ += duration.count();
    frames_decoded_++;
}

void PerformanceMonitor::beginDisplayFrameTiming() {
    if (!is_started_ || is_paused_) {
        return;
    }
    display_start_ = std::chrono::steady_clock::now();
}

void PerformanceMonitor::endDisplayFrameTiming() {
    if (!is_started_ || is_paused_) {
        return;
    }
    
    // 如果在延迟期间，不记录数据
    if (timer_in_delay_period_.load()) {
        return;  // 延迟期间，不统计
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        end - display_start_);
    
    total_display_time_us_ += duration.count();
    frames_displayed_++;
}

// ============ 统计信息获取 ============

int PerformanceMonitor::getLoadedFrames() const {
    return frames_loaded_;
}

int PerformanceMonitor::getDecodedFrames() const {
    return frames_decoded_;
}

int PerformanceMonitor::getDisplayedFrames() const {
    return frames_displayed_;
}

double PerformanceMonitor::getAverageLoadFPS() const {
    return calculateAverageFPS(frames_loaded_);
}

double PerformanceMonitor::getAverageDecodeFPS() const {
    return calculateAverageFPS(frames_decoded_);
}

double PerformanceMonitor::getAverageDisplayFPS() const {
    return calculateAverageFPS(frames_displayed_);
}

double PerformanceMonitor::getTotalTime() const {
    return getTotalDuration();
}

double PerformanceMonitor::getElapsedTime() const {
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
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("          Performance Statistics\n");
    printf("═══════════════════════════════════════════════════════\n");
    
    double total_time = getTotalDuration();
    
    // 帧数统计
    if (frames_loaded_ > 0) {
        printf("📥 Loaded Frames:    %d frames\n", frames_loaded_);
        printf("   Average Load FPS: %.2f fps\n", getAverageLoadFPS());
        if (total_load_time_us_ > 0) {
            double avg_load_time = (double)total_load_time_us_ / frames_loaded_ / 1000.0;
            printf("   Average Load Time: %.2f ms/frame\n", avg_load_time);
        }
    }
    
    if (frames_decoded_ > 0) {
        printf("\n🎬 Decoded Frames:   %d frames\n", frames_decoded_);
        printf("   Average Decode FPS: %.2f fps\n", getAverageDecodeFPS());
        if (total_decode_time_us_ > 0) {
            double avg_decode_time = (double)total_decode_time_us_ / frames_decoded_ / 1000.0;
            printf("   Average Decode Time: %.2f ms/frame\n", avg_decode_time);
        }
    }
    
    if (frames_displayed_ > 0) {
        printf("\n📺 Displayed Frames: %d frames\n", frames_displayed_);
        printf("   Average Display FPS: %.2f fps\n", getAverageDisplayFPS());
        if (total_display_time_us_ > 0) {
            double avg_display_time = (double)total_display_time_us_ / frames_displayed_ / 1000.0;
            printf("   Average Display Time: %.2f ms/frame\n", avg_display_time);
        }
    }
    
    printf("\n⏱️  Total Time:       %.2f seconds\n", total_time);
    printf("═══════════════════════════════════════════════════════\n\n");
}

void PerformanceMonitor::printRealTimeStats() {
    if (!is_started_) {
        return;
    }
    
    // 节流：检查距离上次报告的时间
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_report_time_);
    
    if (duration.count() < report_interval_ms_) {
        return;  // 未到报告时间
    }
    
    // 更新上次报告时间
    last_report_time_ = now;
    
    // 打印实时统计
    printf("📊 Real-time Stats: ");
    
    if (frames_loaded_ > 0) {
        printf("Loaded=%d (%.1f fps) ", frames_loaded_, getAverageLoadFPS());
    }
    
    if (frames_decoded_ > 0) {
        printf("Decoded=%d (%.1f fps) ", frames_decoded_, getAverageDecodeFPS());
    }
    
    if (frames_displayed_ > 0) {
        printf("Displayed=%d (%.1f fps) ", frames_displayed_, getAverageDisplayFPS());
    }
    
    printf("Time=%.1fs\n", getElapsedTime());
}

void PerformanceMonitor::generateReport(char* buffer, size_t buffer_size) const {
    if (!buffer || buffer_size == 0) {
        return;
    }
    
    int offset = 0;
    double total_time = getTotalDuration();
    
    offset += snprintf(buffer + offset, buffer_size - offset,
                      "Performance Report:\n");
    
    if (frames_loaded_ > 0) {
        offset += snprintf(buffer + offset, buffer_size - offset,
                          "  Loaded: %d frames, %.2f fps\n",
                          frames_loaded_, getAverageLoadFPS());
    }
    
    if (frames_decoded_ > 0) {
        offset += snprintf(buffer + offset, buffer_size - offset,
                          "  Decoded: %d frames, %.2f fps\n",
                          frames_decoded_, getAverageDecodeFPS());
    }
    
    if (frames_displayed_ > 0) {
        offset += snprintf(buffer + offset, buffer_size - offset,
                          "  Displayed: %d frames, %.2f fps\n",
                          frames_displayed_, getAverageDisplayFPS());
    }
    
    snprintf(buffer + offset, buffer_size - offset,
             "  Total time: %.2f seconds\n", total_time);
}

// ============ 配置 ============

void PerformanceMonitor::setReportInterval(int interval_ms) {
    report_interval_ms_ = interval_ms;
}

// ============ 内部辅助方法 ============

double PerformanceMonitor::calculateAverageFPS(int frame_count) const {
    if (!is_started_ || frame_count == 0) {
        return 0.0;
    }
    
    double duration = getTotalDuration();
    if (duration <= 0.0) {
        return 0.0;
    }
    
    return frame_count / duration;
}

double PerformanceMonitor::getTotalDuration() const {
    if (!is_started_) {
        return 0.0;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - start_time_);
    
    return duration.count() / 1000.0;
}

// ============ 定时器控制实现 ============

void PerformanceMonitor::setTimerTask(TimerTaskType task) {
    timer_task_type_ = task;
    
    const char* task_name = "";
    switch (task) {
        case TASK_PRINT_FULL_STATS:
            task_name = "完整统计";
            break;
        case TASK_PRINT_FPS_ONLY:
            task_name = "只显示FPS";
            break;
        case TASK_PRINT_SIMPLE:
            task_name = "简化统计";
            break;
        case TASK_PRINT_FRAME_COUNT:
            task_name = "只显示帧数";
            break;
        case TASK_PRINT_ELAPSED_TIME:
            task_name = "只显示运行时间";
            break;
        case TASK_PRINT_WITH_BUFFERMANAGER:
            task_name = "完整统计 + BufferManager 状态";
            break;
    }
    
    printf("📋 Timer task set to: %s\n", task_name);
}

void PerformanceMonitor::setBufferManager(BufferManager* manager) {
    buffer_manager_ = manager;
    printf("📦 BufferManager pointer set for monitoring\n");
}

void PerformanceMonitor::setTimerInterval(double interval_seconds, double delay_seconds) {
    timer_interval_seconds_ = interval_seconds;
    timer_delay_seconds_ = delay_seconds;
    is_oneshot_timer_ = false;  // 设置为周期性定时器
    
    if (delay_seconds > 0.0) {
        printf("⏱️  Timer interval set to %.2f seconds (periodic, delayed %.2f seconds)\n", 
               interval_seconds, delay_seconds);
    } else {
        printf("⏱️  Timer interval set to %.2f seconds (periodic)\n", interval_seconds);
    }
}

void PerformanceMonitor::setOneShotTimer(double seconds) {
    timer_interval_seconds_ = seconds;
    is_oneshot_timer_ = true;  // 设置为一次性定时器
    printf("⏱️  One-shot timer set to %.2f seconds\n", seconds);
}

void PerformanceMonitor::setTimerCallback(void (*callback)(void*), void* user_data) {
    user_callback_ = callback;
    user_callback_data_ = user_data;
    
    if (callback) {
        printf("📞 Timer callback registered\n");
    } else {
        printf("📞 Timer callback cleared\n");
    }
}

void PerformanceMonitor::startTimer() {
    // 如果定时器已经在运行，忽略
    if (timer_running_) {
        printf("⚠️  Timer is already running\n");
        return;
    }
    
    // 记录基准值（用于最终统计）
    baseline_time_ = std::chrono::steady_clock::now();
    baseline_display_frames_ = frames_displayed_;
    baseline_load_frames_ = frames_loaded_;
    baseline_decode_frames_ = frames_decoded_;
    
    // 初始化增量统计的基准点
    last_frames_loaded_ = frames_loaded_;
    last_frames_decoded_ = frames_decoded_;
    last_frames_displayed_ = frames_displayed_;
    last_timer_trigger_time_ = std::chrono::steady_clock::now();
    
    // 保存定时器启动时的基准值（用于计算累计帧数）
    timer_start_frames_loaded_ = frames_loaded_;
    timer_start_frames_decoded_ = frames_decoded_;
    timer_start_frames_displayed_ = frames_displayed_;
    
    // 初始化定时器实际开始统计的时间点
    // 如果没有延迟，就立即设置为当前时间；如果有延迟，等延迟结束后再设置
    if (timer_delay_seconds_ <= 0.0) {
        timer_real_start_time_ = std::chrono::steady_clock::now();
        timer_in_delay_period_.store(false);  // 没有延迟，不在延迟期间
    } else {
        timer_in_delay_period_.store(true);   // 有延迟，标记为在延迟期间
    }
    // 如果有延迟，timer_real_start_time_ 会在延迟结束时设置
    
    // 设置运行标志
    timer_running_ = true;
    
    // 启动后台线程
    timer_thread_ = std::thread(&PerformanceMonitor::timerThreadFunction, this);
    
    printf("✅ Timer started (interval: %.2f seconds)\n", timer_interval_seconds_);
}

void PerformanceMonitor::stopTimer() {
    // 设置停止标志
    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        if (!timer_running_) {
            // 定时器已经停止，但可能线程还没被 join
            // （例如一次性定时器自然退出的情况）
            if (timer_thread_.joinable()) {
                timer_thread_.join();
            }
            return;
        }
        timer_running_ = false;
    }
    
    // 唤醒定时器线程（如果它在等待）
    timer_cv_.notify_one();
    
    // 等待线程退出
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
    
    printf("⏹️  Timer stopped\n");
}

void PerformanceMonitor::timerThreadFunction() {
    printf("🧵 Timer thread started");
    if (is_oneshot_timer_) {
        printf(" (one-shot, %.1fs)\n\n", timer_interval_seconds_);
    } else {
        if (timer_delay_seconds_ > 0.0) {
            printf(" (periodic, %.1fs interval, delayed %.1fs)\n\n", 
                   timer_interval_seconds_, timer_delay_seconds_);
        } else {
            printf(" (periodic, %.1fs interval)\n\n", timer_interval_seconds_);
        }
    }
    
    bool first_iteration = true;  // 标记第一次迭代
    
    while (true) {
        // 等待指定的时间间隔
        {
            std::unique_lock<std::mutex> lock(timer_mutex_);
            
            // 第一次迭代：如果设置了延迟，则等待延迟时间；否则等待正常间隔
            // 之后的迭代：始终等待正常间隔
            double wait_time = (first_iteration && timer_delay_seconds_ > 0.0) 
                             ? timer_delay_seconds_ 
                             : timer_interval_seconds_;
            
            // 使用 wait_for 实现定时等待，同时可以被 notify_one 中断
            auto wait_duration = std::chrono::duration<double>(wait_time);
            timer_cv_.wait_for(lock, wait_duration);
            
            // 检查是否需要退出
            if (!timer_running_) {
                break;
            }
        }
        
        // 检查再次确认（避免在 wait_for 超时后才设置 timer_running_ = false 的情况）
        if (!timer_running_) {
            break;
        }
        
        // 如果是第一次迭代且设置了延迟，跳过任务执行（只是延迟）
        if (first_iteration && timer_delay_seconds_ > 0.0) {
            printf("⏰ Delay period (%.1fs) finished, starting periodic tasks...\n\n", 
                   timer_delay_seconds_);
            
            auto now = std::chrono::steady_clock::now();
            
            // 重置基准点（从延迟结束后开始统计）
            // 1. 重置增量统计基准（用于计算每秒的帧数变化）
            last_frames_loaded_ = frames_loaded_;
            last_frames_decoded_ = frames_decoded_;
            last_frames_displayed_ = frames_displayed_;
            last_timer_trigger_time_ = now;
            
            // 2. 重置累计统计基准（用于计算从延迟结束后的总累计帧数）
            timer_start_frames_loaded_ = frames_loaded_;
            timer_start_frames_decoded_ = frames_decoded_;
            timer_start_frames_displayed_ = frames_displayed_;
            
            // 3. 设置定时器实际开始统计的时间点（用于计算总运行时间）
            timer_real_start_time_ = now;
            
            // 4. 清除延迟期间标志（延迟结束）
            timer_in_delay_period_.store(false);
            
            first_iteration = false;
            continue;  // 跳过任务执行，进入下一次循环
        }
        
        first_iteration = false;  // 标记不再是第一次迭代
        
        // 定时器触发：执行任务
        if (user_callback_) {
            // 如果用户注册了回调，优先执行用户回调
            user_callback_(user_callback_data_);
        } else {
            // 否则执行预定义的统计任务
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_timer_trigger_time_);
            double actual_interval = duration.count() / 1000.0;
            
            // 计算这个时间间隔内的帧数增量
            int loaded_delta = frames_loaded_ - last_frames_loaded_;
            int decoded_delta = frames_decoded_ - last_frames_decoded_;
            int displayed_delta = frames_displayed_ - last_frames_displayed_;
            
            // 根据任务类型执行不同的任务
            switch (timer_task_type_) {
                case TASK_PRINT_FULL_STATS:
                    executeTaskFullStats(actual_interval, loaded_delta, decoded_delta, displayed_delta);
                    break;
                    
                case TASK_PRINT_FPS_ONLY:
                    executeTaskFpsOnly(actual_interval, displayed_delta);
                    break;
                    
                case TASK_PRINT_SIMPLE:
                    executeTaskSimple(actual_interval, displayed_delta);
                    break;
                    
                case TASK_PRINT_FRAME_COUNT:
                    executeTaskFrameCount(displayed_delta);
                    break;
                    
                case TASK_PRINT_ELAPSED_TIME:
                    executeTaskElapsedTime();
                    break;
                    
                case TASK_PRINT_WITH_BUFFERMANAGER:
                    executeTaskWithBufferManager(actual_interval, loaded_delta, decoded_delta, displayed_delta);
                    break;
            }
            
            // 更新基准点，为下次统计做准备
            last_frames_loaded_ = frames_loaded_;
            last_frames_decoded_ = frames_decoded_;
            last_frames_displayed_ = frames_displayed_;
            last_timer_trigger_time_ = now;
        }
        
        // 如果是一次性定时器，触发后立即停止
        if (is_oneshot_timer_) {
            printf("⏰ One-shot timer triggered, stopping...\n");
            timer_running_ = false;
            break;
        }
    }
    
    printf("🧵 Timer thread exited\n");
}

// ============ 定时器任务执行函数实现 ============

void PerformanceMonitor::executeTaskFullStats(double interval, int load_delta, int decode_delta, int display_delta) {
    // 计算这个时间间隔内的FPS
    double load_fps = (interval > 0) ? (load_delta / interval) : 0.0;
    double decode_fps = (interval > 0) ? (decode_delta / interval) : 0.0;
    double display_fps = (interval > 0) ? (display_delta / interval) : 0.0;
    
    // 计算从定时器启动开始的累计帧数
    int cumulative_displayed = frames_displayed_ - timer_start_frames_displayed_;
    int cumulative_decoded = frames_decoded_ - timer_start_frames_decoded_;
    int cumulative_loaded = frames_loaded_ - timer_start_frames_loaded_;
    
    // 计算总运行时间（从定时器实际开始统计的时间点算起，跳过延迟）
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - timer_real_start_time_;
    double total_time = elapsed.count();
    
    // 打印完整统计信息
    printf("┌─────────────────────────────────────────────────────┐\n");
    printf("│      ⏱️  过去 %.1f 秒内的性能统计               │\n", interval);
    printf("└─────────────────────────────────────────────────────┘\n");
    
    if (!is_started_) {
        printf("⚠️  Monitor not started yet\n");
    } else {
        // 显示增量统计
        if (display_delta > 0 || cumulative_displayed > 0) {
            printf("📺 显示操作: %d 次 (%.1f ops/s) | 累计: %d 次\n", 
                   display_delta, display_fps, cumulative_displayed);
        }
        
        if (decode_delta > 0 || cumulative_decoded > 0) {
            printf("🎬 解码操作: %d 次 (%.1f ops/s) | 累计: %d 次\n", 
                   decode_delta, decode_fps, cumulative_decoded);
        }
        
        if (load_delta > 0 || cumulative_loaded > 0) {
            printf("📥 加载帧: %d 帧 (%.1f fps) | 累计: %d 帧\n", 
                   load_delta, load_fps, cumulative_loaded);
        }
        
        printf("⏱️  总运行时间: %.2f 秒\n", total_time);
    }
    
    printf("\n");
}

void PerformanceMonitor::executeTaskFpsOnly(double interval, int display_delta) {
    if (!is_started_) {
        return;
    }
    
    // 计算总运行时间（从定时器实际开始统计的时间点算起，跳过延迟）
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - timer_real_start_time_;
    double total_time = elapsed.count();
    
    double display_fps = (interval > 0) ? (display_delta / interval) : 0.0;
    printf("⏱️  [%.1fs] Display: %.1f ops/s\n", total_time, display_fps);
}

void PerformanceMonitor::executeTaskSimple(double interval, int display_delta) {
    if (!is_started_) {
        return;
    }
    
    // 计算总运行时间（从定时器实际开始统计的时间点算起，跳过延迟）
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - timer_real_start_time_;
    double total_time = elapsed.count();
    
    double display_fps = (interval > 0) ? (display_delta / interval) : 0.0;
    printf("📊 [%.1fs] %d ops | %.1f ops/s\n", 
           total_time, display_delta, display_fps);
}

void PerformanceMonitor::executeTaskFrameCount(int display_delta) {
    if (!is_started_) {
        return;
    }
    
    int cumulative_displayed = frames_displayed_ - timer_start_frames_displayed_;
    printf("📺 过去 %.1f 秒: %d 次操作 | 累计: %d 次\n", 
           timer_interval_seconds_, display_delta, cumulative_displayed);
}

void PerformanceMonitor::executeTaskElapsedTime() {
    if (!is_started_) {
        return;
    }
    
    // 计算总运行时间（从定时器实际开始统计的时间点算起，跳过延迟）
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - timer_real_start_time_;
    double total_time = elapsed.count();
    
    int cumulative_displayed = frames_displayed_ - timer_start_frames_displayed_;
    printf("⏱️  运行时间: %.2f 秒 | 显示操作: %d 次\n", 
           total_time, cumulative_displayed);
}

void PerformanceMonitor::executeTaskWithBufferManager(double interval, int load_delta, int decode_delta, int display_delta) {
    // 首先打印完整的性能统计（复用 executeTaskFullStats 的逻辑）
    // 计算这个时间间隔内的FPS
    double load_fps = (interval > 0) ? (load_delta / interval) : 0.0;
    double decode_fps = (interval > 0) ? (decode_delta / interval) : 0.0;
    double display_fps = (interval > 0) ? (display_delta / interval) : 0.0;
    
    // 计算从定时器启动开始的累计帧数
    int cumulative_displayed = frames_displayed_ - timer_start_frames_displayed_;
    int cumulative_decoded = frames_decoded_ - timer_start_frames_decoded_;
    int cumulative_loaded = frames_loaded_ - timer_start_frames_loaded_;
    
    // 计算总运行时间（从定时器实际开始统计的时间点算起，跳过延迟）
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - timer_real_start_time_;
    double total_time = elapsed.count();
    
    // 打印完整统计信息
    printf("┌─────────────────────────────────────────────────────┐\n");
    printf("│      ⏱️  过去 %.1f 秒内的性能统计               │\n", interval);
    printf("└─────────────────────────────────────────────────────┘\n");
    
    if (!is_started_) {
        printf("⚠️  Monitor not started yet\n");
    } else {
        // 显示增量统计
        if (display_delta > 0 || cumulative_displayed > 0) {
            printf("📺 显示操作: %d 次 (%.1f ops/s) | 累计: %d 次\n", 
                   display_delta, display_fps, cumulative_displayed);
        }
        
        if (decode_delta > 0 || cumulative_decoded > 0) {
            printf("🎬 解码操作: %d 次 (%.1f ops/s) | 累计: %d 次\n", 
                   decode_delta, decode_fps, cumulative_decoded);
        }
        
        if (load_delta > 0 || cumulative_loaded > 0) {
            printf("📥 加载帧: %d 帧 (%.1f fps) | 累计: %d 帧\n", 
                   load_delta, load_fps, cumulative_loaded);
        }
        
        printf("⏱️  总运行时间: %.2f 秒\n", total_time);
    }
    
    // 打印 BufferManager 状态
    if (buffer_manager_ != nullptr) {
        printf("┌─────────────────────────────────────────────────────┐\n");
        printf("│      📦 BufferManager 状态                      │\n");
        printf("└─────────────────────────────────────────────────────┘\n");
        
        // 获取生产者状态
        auto state = buffer_manager_->getProducerState();
        const char* state_str = "";
        switch (state) {
            case BufferManager::ProducerState::STOPPED:
                state_str = "🛑 STOPPED";
                break;
            case BufferManager::ProducerState::RUNNING:
                state_str = "✅ RUNNING";
                break;
            case BufferManager::ProducerState::ERROR:
                state_str = "❌ ERROR";
                break;
        }
        
        printf("🎬 生产者状态: %s\n", state_str);
        printf("📊 已填充buffer: %d 个\n", buffer_manager_->getFilledBufferCount());
        printf("📦 空闲buffer: %d 个\n", buffer_manager_->getFreeBufferCount());
        printf("📈 总buffer数: %d 个\n", buffer_manager_->getTotalBufferCount());
    } else {
        printf("⚠️  BufferManager pointer not set\n");
    }
    
    printf("\n");
}

// ============ 最终统计报告 ============

void PerformanceMonitor::printFinalStats() const {
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Final Statistics (after warm-up period)\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    // 计算总运行时间（从延迟结束后开始算）
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - timer_real_start_time_);
    double stats_time = duration.count() / 1000.0;
    
    // 总运行时间（包括延迟）
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - baseline_time_);
    double total_time = total_duration.count() / 1000.0;
    
    // 计算延迟后的操作数（使用定时器的基准值，这是在延迟结束后记录的）
    int effective_display_ops = frames_displayed_ - timer_start_frames_displayed_;
    int effective_load_frames = frames_loaded_ - timer_start_frames_loaded_;
    int effective_decode_frames = frames_decoded_ - timer_start_frames_decoded_;
    
    // 加载统计
    if (effective_load_frames > 0 || baseline_load_frames_ > 0) {
        if (baseline_load_frames_ > 0) {
            // 有预加载的帧（如 test_4frame_loop）
            printf("📥 Loaded Unique Frames: %d frames", baseline_load_frames_);
            if (timer_delay_seconds_ > 0) {
                printf(" (loaded before stats)");
            }
            printf("\n");
        } else if (effective_load_frames > 0) {
            // 没有预加载，是实时加载（如 test_sequential_playback）
            printf("📥 Loaded Frames: %d frames (loaded during stats)\n", effective_load_frames);
        }
    }
    
    // 显示统计
    if (effective_display_ops > 0) {
        printf("\n📺 Display Statistics:\n");
        printf("   Display Operations: %d times (after warm-up)\n", effective_display_ops);
        
        if (stats_time > 0) {
            double display_fps = effective_display_ops / stats_time;
            printf("   Display FPS: %.2f ops/sec (buffer switches per second)\n", display_fps);
            printf("   Avg Switch Time: %.2f ms per operation\n", 
                   (stats_time * 1000.0) / effective_display_ops);
        }
    }
    
    // 解码统计（如果有）
    if (effective_decode_frames > 0) {
        printf("\n🎬 Decode Statistics:\n");
        printf("   Decoded Frames: %d frames\n", effective_decode_frames);
        if (stats_time > 0) {
            printf("   Decode FPS: %.2f fps\n", effective_decode_frames / stats_time);
        }
    }
    
    // 时间统计
    printf("\n⏱️  Time Statistics:\n");
    printf("   Total Runtime: %.2f seconds\n", total_time);
    if (timer_delay_seconds_ > 0) {
        printf("   Warm-up Period: %.2f seconds (excluded from stats)\n", timer_delay_seconds_);
        printf("   Stats Period: %.2f seconds\n", stats_time);
    }
    
    printf("═══════════════════════════════════════════════════════\n");
}

// ============ 自动停止功能 ============

void PerformanceMonitor::setAutoStopAfterStats(double stats_duration, void (*callback)(void*), void* user_data) {
    // 清理旧的自动停止定时器
    if (auto_stop_timer_) {
        delete auto_stop_timer_;
        auto_stop_timer_ = nullptr;
    }
    
    // 创建新的自动停止定时器
    auto_stop_timer_ = new PerformanceMonitor();
    
    // 计算实际停止时间 = 延迟 + 统计时长
    double actual_stop_time = timer_delay_seconds_ + stats_duration;
    
    // 配置一次性定时器
    auto_stop_timer_->setOneShotTimer(actual_stop_time);
    auto_stop_timer_->setTimerCallback(callback, user_data);
    auto_stop_timer_->startTimer();
    
    printf("   ⏰ Auto-stop: %.0fs stats + %.0fs warm-up = %.0fs total\n",
           stats_duration, timer_delay_seconds_, actual_stop_time);
}

