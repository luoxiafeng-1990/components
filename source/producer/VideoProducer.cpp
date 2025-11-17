#include "../../include/producer/VideoProducer.hpp"
#include <stdio.h>
#include <chrono>

// ============================================================
// 构造函数和析构函数
// ============================================================

VideoProducer::VideoProducer(BufferPool& pool)
    : buffer_pool_(pool)
    , buffer_pool_ptr_(nullptr)
    , running_(false)
    , produced_frames_(0)
    , skipped_frames_(0)
    , next_frame_index_(0)
    , total_frames_(0)
{
    printf("🎬 VideoProducer created (dependent on BufferPool)\n");
}

VideoProducer::~VideoProducer() {
    printf("🧹 Destroying VideoProducer...\n");
    if (running_) {
        stop();
    }
}

// ============================================================
// 核心接口实现
// ============================================================

bool VideoProducer::start(const Config& config) {
    // 检查是否已经在运行
    if (running_) {
        printf("⚠️  Warning: VideoProducer already running\n");
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
    
    printf("\n🎬 Starting VideoProducer...\n");
    printf("   File: %s\n", config.file_path.c_str());
    printf("   Resolution: %dx%d\n", config.width, config.height);
    printf("   Bits per pixel: %d\n", config.bits_per_pixel);
    printf("   Loop mode: %s\n", config.loop ? "enabled" : "disabled");
    printf("   Thread count: %d\n", config.thread_count);
    
    // 保存配置
    config_ = config;
    
    // 创建共享的 VideoFile 对象
    video_file_ = std::make_shared<VideoFile>(config.reader_type);
    printf("   Reader type: %s\n", video_file_->getReaderType());
    
    // 🎯 统一的open接口（传入所有参数，门面类内部智能判断）
    // - 对于编码视频（FFMPEG, RTSP）：自动检测格式，width/height/bpp 被忽略
    // - 对于raw视频（MMAP, IOURING）：使用 width/height/bpp 参数
    if (!video_file_->open(config.file_path.c_str(), 
                           config.width, 
                           config.height, 
                           config.bits_per_pixel)) {
        setError("Failed to open video file: " + config.file_path);
        video_file_.reset();
        return false;
    }
    
    // 🎯 确定工作 BufferPool
    void* reader_output_pool = video_file_->getOutputBufferPool();
    if (reader_output_pool) {
        // Reader 有自己的 BufferPool（如 TacoH264DecoderReader）
        buffer_pool_ptr_ = static_cast<BufferPool*>(reader_output_pool);
        printf("   ✅ Using Reader's output BufferPool: '%s'\n", 
               buffer_pool_ptr_->getName().c_str());
    } else {
        // Reader 使用外部 BufferPool（如 MmapVideoReader）
        buffer_pool_ptr_ = &buffer_pool_;
        printf("   ✅ Using external BufferPool: '%s'\n", 
               buffer_pool_ptr_->getName().c_str());
        
        // 注入 BufferPool（用于特殊 Reader，如 RTSP）
        video_file_->setBufferPool(&buffer_pool_);
    }
    
    total_frames_ = video_file_->getTotalFrames();
    size_t frame_size = video_file_->getFrameSize();
    
    printf("   Total frames: %d\n", total_frames_);
    printf("   Frame size: %zu bytes (%.2f MB)\n", frame_size, frame_size / (1024.0 * 1024.0));
    
    // 🎯 验证/设置帧大小（只对外部 BufferPool 需要）
    if (buffer_pool_ptr_ == &buffer_pool_) {
        size_t pool_buffer_size = buffer_pool_.getBufferSize();
        
        if (pool_buffer_size == 0) {
            // 动态注入模式：设置 buffer_size
            printf("   Dynamic injection mode detected, setting buffer size...\n");
            if (!buffer_pool_.setBufferSize(frame_size)) {
                setError("Failed to set buffer size for dynamic injection mode");
                video_file_.reset();
                return false;
            }
        } else if (frame_size != pool_buffer_size) {
            // 普通模式：验证大小匹配
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg),
                    "Frame size mismatch: video=%zu, buffer=%zu",
                    frame_size, pool_buffer_size);
            setError(error_msg);
            video_file_.reset();
            return false;
        } else {
            // 大小匹配
            printf("   Frame size matches BufferPool size: %zu bytes\n", frame_size);
        }
    } else {
        // Reader's BufferPool（零拷贝模式），不需要验证大小
        printf("   Using Reader's BufferPool (zero-copy mode), no size validation needed\n");
    }
    
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
            threads_.emplace_back(&VideoProducer::producerThreadFunc, this, i);
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
            video_file_.reset();
            setError(std::string("Failed to start producer thread: ") + e.what());
            return false;
        }
    }
    
    printf("✅ All %d producer thread(s) started successfully\n", config.thread_count);
    
    return true;
}

void VideoProducer::stop() {
    if (!running_) {
        return;
    }
    
    printf("\n🛑 Stopping VideoProducer...\n");
    
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
    if (video_file_) {
        video_file_.reset();
    }
    
    printf("✅ VideoProducer stopped\n");
    printf("   Total produced: %d frames\n", produced_frames_.load());
    printf("   Total skipped: %d frames\n", skipped_frames_.load());
    printf("   Average FPS: %.2f\n", getAverageFPS());
}

// ============================================================
// 查询接口实现
// ============================================================

double VideoProducer::getAverageFPS() const {
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

int VideoProducer::getTotalFrames() const {
    return total_frames_;
}

std::string VideoProducer::getLastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void VideoProducer::printStats() const {
    printf("\n📊 VideoProducer Statistics:\n");
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

void VideoProducer::producerThreadFunc(int thread_id) {
    printf("🚀 Thread #%d: Starting unified producer loop\n", thread_id);
    printf("   Working BufferPool: '%s'\n", buffer_pool_ptr_->getName().c_str());
    
    int thread_produced = 0;
    int thread_skipped = 0;
    int consecutive_failures = 0;
    
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
            buffer = buffer_pool_ptr_->acquireFree(true, 100);  // 100ms 超时
            if (buffer == nullptr && running_) {
                // 超时但仍在运行，继续等待
                // printf("   [Thread #%d] Waiting for free buffer...\n", thread_id);
            }
        }
        
        // 检查是否因为停止信号退出循环
        if (!running_) {
            break;
        }
        
        // 4. 🎯 统一的接口：调用 Reader 填充 buffer
        bool read_success = video_file_->readFrame(frame_index, buffer);
        
        // 5. 🎯 统一的处理：提交或归还
        if (read_success) {
            buffer_pool_ptr_->submitFilled(buffer);
            produced_frames_.fetch_add(1);
            thread_produced++;
            consecutive_failures = 0;  // 重置失败计数
        } else {
            buffer_pool_ptr_->releaseFilled(buffer);
            skipped_frames_.fetch_add(1);
            thread_skipped++;
            
            printf("⚠️  Thread #%d: Failed to read frame %d/%d\n",
                   thread_id, frame_index, total_frames_);
            
            // 连续失败检测
            consecutive_failures++;
            if (consecutive_failures > 10) {
                char error_msg[256];
                snprintf(error_msg, sizeof(error_msg),
                        "Thread #%d: Too many consecutive read failures (%d)",
                        thread_id, consecutive_failures);
                setError(error_msg);
                break;
            }
        }
        
        // 定期打印进度（每100帧）
        if (thread_produced % 100 == 0 && thread_produced > 0) {
            printf("   [Thread #%d] Produced %d frames (%.1f fps)\n",
                   thread_id, thread_produced, getAverageFPS());
        }
    }
    
    // 线程结束
    printf("🏁 Thread #%d finished: produced=%d, skipped=%d\n",
           thread_id, thread_produced, thread_skipped);
}

void VideoProducer::setError(const std::string& error_msg) {
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
    printf("❌ VideoProducer Error: %s\n", error_msg.c_str());
}

