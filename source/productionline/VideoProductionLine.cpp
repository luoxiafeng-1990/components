#include "productionline/VideoProductionLine.hpp"
#include "buffer/BufferPoolRegistry.hpp"
#include "monitor/Timer.hpp"
#include <stdio.h>
#include <chrono>

// ============================================================
// 构造函数和析构函数
// ============================================================

VideoProductionLine::VideoProductionLine()
    : working_buffer_pool_id_(0)
    , working_buffer_pool_ptr_(nullptr)
    , running_(false)
    , produced_frames_(0)
    , skipped_frames_(0)
    , next_frame_index_(0)
    , total_frames_(0)
{
    printf("🎬 VideoProductionLine created (v2.0: Registry持有BufferPool)\n");
}

VideoProductionLine::~VideoProductionLine() {
    printf("🧹 Destroying VideoProductionLine...\n");
    if (running_) {
        stop();
    }
}

// ============================================================
// 核心接口实现
// ============================================================

bool VideoProductionLine::start(const Config& config) {
    // 检查是否已经在运行
    if (running_) {
        printf("⚠️  Warning: VideoProductionLine already running\n");
        return false;
    }
    
    // 验证配置
    if (config.file_path.empty()) {
        setError("Video file path is empty");
        return false;
    }
    
    if (config.thread_count < 1) {
        setError("Thread count must be >= 1");
        return false;
    }
    
    printf("\n🎬 Starting VideoProductionLine...\n");
    printf("   File: %s\n", config.file_path.c_str());
    printf("   Resolution: %dx%d\n", config.width, config.height);
    printf("   Bits per pixel: %d\n", config.bits_per_pixel);
    printf("   Loop mode: %s\n", config.loop ? "enabled" : "disabled");
    printf("   Thread count: %d\n", config.thread_count);
    
    // 保存配置
    config_ = config;
    
    // 创建共享的 BufferFillingWorkerFacade 对象
    worker_facade_sptr_ = std::make_shared<BufferFillingWorkerFacade>(config.worker_type);
    printf("   Worker type: %s\n", worker_facade_sptr_->getWorkerType());
    
    // 🎯 统一的open接口（传入所有参数，门面类内部智能判断）
    // - 对于编码视频（FFMPEG, RTSP）：自动检测格式，width/height/bpp 被忽略
    // - 对于raw视频（MMAP, IOURING）：使用 width/height/bpp 参数
    if (!worker_facade_sptr_->open(config.file_path.c_str(), 
                           config.width, 
                           config.height, 
                           config.bits_per_pixel)) {
        setError("Failed to open video file: " + config.file_path);
        worker_facade_sptr_.reset();
        return false;
    }
    
    // v2.0: Worker必须在open()时自动创建BufferPool（通过调用Allocator）
    // 获取 BufferPool ID
    uint64_t worker_pool_id = worker_facade_sptr_->getOutputBufferPoolId();
    if (worker_pool_id == 0) {
        setError("Worker failed to create BufferPool. Worker must create BufferPool in open() method by calling Allocator.");
        worker_facade_sptr_.reset();
        return false;
    }
    
    // v2.0: 记录 pool_id 并从 Registry 获取临时访问（返回 weak_ptr）
    working_buffer_pool_id_ = worker_pool_id;
    auto working_buffer_pool_weak = BufferPoolRegistry::getInstance().getPool(worker_pool_id);
    auto working_buffer_pool_sptr = working_buffer_pool_weak.lock();
    if (!working_buffer_pool_sptr) {
        setError("Failed to get BufferPool from Registry (pool may have been destroyed)");
        worker_facade_sptr_.reset();
        return false;
    }
    
    // 缓存原始指针用于快速访问（在ProductionLine运行期间有效）
    working_buffer_pool_ptr_ = working_buffer_pool_sptr.get();
    printf("   ✅ Using Worker's BufferPool: '%s' (ID: %lu, created by Worker via Allocator)\n", 
           working_buffer_pool_ptr_->getName().c_str(), worker_pool_id);
    
    total_frames_ = worker_facade_sptr_->getTotalFrames();
    size_t frame_size = worker_facade_sptr_->getFrameSize();
    
    printf("   Total frames: %d\n", total_frames_);
    printf("   Frame size: %zu bytes (%.2f MB)\n", frame_size, frame_size / (1024.0 * 1024.0));
    
    // Worker创建的BufferPool，不需要验证大小（Worker已经确保大小匹配）
    printf("   ⚡ Worker's BufferPool created via Allocator, size validation handled by Worker\n");
    
    // 重置状态
    running_ = true;
    produced_frames_ = 0;
    skipped_frames_ = 0;
    next_frame_index_ = 0;
    start_time_ = std::chrono::steady_clock::now();
    
    // 启动生产者线程
    threads_.reserve(config.thread_count);
    for (int i = 0; i < config.thread_count; i++) {
        try {
            threads_.emplace_back(&VideoProductionLine::producerThreadFunc, this, i);
            printf("   ✅ Producer thread #%d started\n", i);
        } catch (const std::exception& e) {
            printf("❌ ERROR: Failed to start thread #%d: %s\n", i, e.what());
            // 停止已启动的线程
            running_ = false;
            for (auto& thread : threads_) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            threads_.clear();
            worker_facade_sptr_.reset();
            setError(std::string("Failed to start producer thread: ") + e.what());
            return false;
        }
    }
    
    printf("✅ All %d producer thread(s) started successfully\n", config.thread_count);
    
    return true;
}

void VideoProductionLine::stop() {
    if (!running_) {
        return;
    }
    
    printf("\n🛑 Stopping VideoProductionLine...\n");
    
    // 设置停止标志
    running_ = false;
    
    // 等待所有线程退出
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
    
    // 关闭视频文件
    if (worker_facade_sptr_) {
        worker_facade_sptr_.reset();
    }
    
    printf("✅ VideoProductionLine stopped\n");
    printf("   Total produced: %d frames\n", produced_frames_.load());
    printf("   Total skipped: %d frames\n", skipped_frames_.load());
    printf("   Average FPS: %.2f\n", getAverageFPS());
}

// ============================================================
// 查询接口实现
// ============================================================

double VideoProductionLine::getAverageFPS() const {
    if (!running_ && threads_.empty()) {
        // 已停止，计算总体平均
        auto duration = std::chrono::steady_clock::now() - start_time_;
        double seconds = std::chrono::duration<double>(duration).count();
        if (seconds > 0) {
            return produced_frames_.load() / seconds;
        }
    } else if (running_) {
        // 正在运行，计算当前平均
        auto duration = std::chrono::steady_clock::now() - start_time_;
        double seconds = std::chrono::duration<double>(duration).count();
        if (seconds > 0) {
            return produced_frames_.load() / seconds;
        }
    }
    return 0.0;
}

int VideoProductionLine::getTotalFrames() const {
    return total_frames_;
}

BufferPool* VideoProductionLine::getWorkingBufferPool() const {
    // v2.0: 从缓存的指针返回（在start()时从Registry获取并缓存）
    return working_buffer_pool_ptr_;
}

std::string VideoProductionLine::getLastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void VideoProductionLine::printStats() const {
    printf("\n📊 VideoProductionLine Statistics:\n");
    printf("   Running: %s\n", running_.load() ? "Yes" : "No");
    printf("   Produced frames: %d\n", produced_frames_.load());
    printf("   Skipped frames: %d\n", skipped_frames_.load());
    printf("   Total frames: %d\n", total_frames_);
    printf("   Average FPS: %.2f\n", getAverageFPS());
    printf("   Thread count: %zu\n", threads_.size());
}

// ============================================================
// 内部方法实现
// ============================================================

void VideoProductionLine::producerThreadFunc(int thread_id) {
    printf("🚀 Thread #%d: Starting unified producer loop\n", thread_id);
    printf("   Working BufferPool: '%s'\n", working_buffer_pool_ptr_->getName().c_str());
    
    int thread_produced = 0;
    int thread_skipped = 0;
    int consecutive_failures = 0;
    
    // 🎯 创建 Timer 上下文（用于定时打印连续失败次数和进度）
    struct TimerContext {
        int thread_id;
        int* consecutive_failures_ptr;
        int* thread_produced_ptr;
        VideoProductionLine* self_ptr;
    } timer_context = { 
        thread_id, 
        &consecutive_failures,
        &thread_produced,
        this
    };
    
    // 🎯 定义 Timer 回调函数（同时打印失败次数和进度）
    auto timer_callback = [](void* user_data) {
        auto* ctx = static_cast<TimerContext*>(user_data);
        printf("🔔 [Timer] Thread #%d: consecutive_failures=%d, produced=%d, fps=%.1f\n", 
               ctx->thread_id, 
               *ctx->consecutive_failures_ptr,
               *ctx->thread_produced_ptr,
               ctx->self_ptr->getAverageFPS());
    };
    
    // 🎯 创建并启动定时器（每2秒打印一次）
    Timer failure_monitor_timer(
        2.0,              // interval_seconds: 每2秒触发一次
        timer_callback,   // callback: 回调函数
        &timer_context,   // user_data: 上下文数据
        0.0,              // delay_seconds: 立即开始
        0.0               // duration_seconds: 无限期运行
    );
    //failure_monitor_timer.start();
    
    while (running_) {
        // 1. 原子地获取下一个帧索引
        int frame_index = next_frame_index_.fetch_add(1);
        
        // 2. 处理循环模式和文件边界
        if (frame_index >= total_frames_) {
            if (config_.loop) {
                // 循环模式：归一化到 0-total_frames 范围
                frame_index = frame_index % total_frames_;
                
                // 尝试重置计数器，避免整数溢出
                int current = next_frame_index_.load();
                if (current > total_frames_ * 2) {
                    int expected = current;
                    int new_value = frame_index + 1;
                    next_frame_index_.compare_exchange_strong(expected, new_value);
                }
            } else {
                // 非循环模式：没有更多帧可读
                break;
            }
        }
        
        // 3. 🎯 统一的流程：从工作 BufferPool 获取 buffer
        Buffer* buffer = nullptr;
        while (running_ && buffer == nullptr) {
            buffer = working_buffer_pool_ptr_->acquireFree(true, 100);  // 100ms 超时
            if (buffer == nullptr && running_) {
                // 超时但仍在运行，继续等待
                printf("   [Thread #%d] Waiting for free buffer...\n", thread_id);
            }
        }
        
        // 检查是否因为停止信号退出循环
        if (!running_) {
            break;
        }
        
        // 4. 🎯 统一的接口：调用 Worker 填充 buffer（使用fillBuffer）
        bool fill_success = worker_facade_sptr_->fillBuffer(frame_index, buffer);
        
        // 5. 🎯 统一的处理：提交或归还
        if (fill_success) {
            // ✅ 填充成功：提交到 filled 队列（供消费者使用）
            working_buffer_pool_ptr_->submitFilled(buffer);
            produced_frames_.fetch_add(1);
            thread_produced++;
            consecutive_failures = 0;  // 重置失败计数
        } else {
            // ⚠️ 填充失败：归还到 free 队列（Buffer 未填充数据，状态为 LOCKED_BY_PRODUCER）
            working_buffer_pool_ptr_->releaseFree(buffer);
            skipped_frames_.fetch_add(1);
            thread_skipped++;
            // 🎯 累加连续失败次数（Timer 会每2秒自动打印）
            consecutive_failures++;
        }
    }
    
    // 🎯 Timer 会在析构时自动调用 stop()
    // 线程结束
    printf("🏁 Thread #%d finished: produced=%d, skipped=%d, final_consecutive_failures=%d\n",
           thread_id, thread_produced, thread_skipped, consecutive_failures);
}

void VideoProductionLine::setError(const std::string& error_msg) {
    // 保存错误消息
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = error_msg;
    }
    
    // 调用用户回调
    if (error_callback_) {
        try {
            error_callback_(error_msg);
        } catch (...) {
            printf("⚠️  Warning: Exception in error callback\n");
        }
    }
    
    // 打印到控制台
    printf("❌ VideoProductionLine Error: %s\n", error_msg.c_str());
}

