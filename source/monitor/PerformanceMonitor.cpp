#include "monitor/PerformanceMonitor.hpp"
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
{
}

PerformanceMonitor::~PerformanceMonitor() {
    // 析构时无需特殊清理
}

// ============ 生命周期管理 ============

void PerformanceMonitor::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    start_time_ = std::chrono::steady_clock::now();
    last_report_time_ = start_time_;
    is_started_ = true;
    is_paused_ = false;
    
    printf("📊 PerformanceMonitor started\n");
}

void PerformanceMonitor::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_loaded_ = 0;
    frames_decoded_ = 0;
    frames_displayed_ = 0;
    total_load_time_us_ = 0;
    total_decode_time_us_ = 0;
    total_display_time_us_ = 0;
    
    start_time_ = std::chrono::steady_clock::now();
    last_report_time_ = start_time_;
}

void PerformanceMonitor::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    is_paused_ = true;
}

void PerformanceMonitor::resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    is_paused_ = false;
}

// ============ 简单事件记录 ============

void PerformanceMonitor::recordFrameLoaded() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_started_ || is_paused_) {
        return;
    }
    frames_loaded_++;
}

void PerformanceMonitor::recordFrameDecoded() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_started_ || is_paused_) {
        return;
    }
    frames_decoded_++;
}

void PerformanceMonitor::recordFrameDisplayed() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_started_ || is_paused_) {
        return;
    }
    frames_displayed_++;
}

// ============ 带计时的事件记录 ============

void PerformanceMonitor::beginLoadFrameTiming() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_started_ || is_paused_) {
        return;
    }
    load_start_ = std::chrono::steady_clock::now();
}

void PerformanceMonitor::endLoadFrameTiming() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_started_ || is_paused_) {
        return;
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        end - load_start_);
    
    total_load_time_us_ += duration.count();
    frames_loaded_++;
}

void PerformanceMonitor::beginDecodeFrameTiming() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_started_ || is_paused_) {
        return;
    }
    decode_start_ = std::chrono::steady_clock::now();
}

void PerformanceMonitor::endDecodeFrameTiming() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_started_ || is_paused_) {
        return;
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        end - decode_start_);
    
    total_decode_time_us_ += duration.count();
    frames_decoded_++;
}

void PerformanceMonitor::beginDisplayFrameTiming() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_started_ || is_paused_) {
        return;
    }
    display_start_ = std::chrono::steady_clock::now();
}

void PerformanceMonitor::endDisplayFrameTiming() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_started_ || is_paused_) {
        return;
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        end - display_start_);
    
    total_display_time_us_ += duration.count();
    frames_displayed_++;
}

// ============ 统计信息获取 ============

int PerformanceMonitor::getLoadedFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_loaded_;
}

int PerformanceMonitor::getDecodedFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_decoded_;
}

int PerformanceMonitor::getDisplayedFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
    report_interval_ms_ = interval_ms;
}

// ============ 内部辅助方法 ============

double PerformanceMonitor::calculateAverageFPS(int frame_count) const {
    // 注意：这个方法已经在调用者处加锁，不需要再次加锁
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
    // 注意：这个方法已经在调用者处加锁，不需要再次加锁
    if (!is_started_) {
        return 0.0;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - start_time_);
    
    return duration.count() / 1000.0;
}
