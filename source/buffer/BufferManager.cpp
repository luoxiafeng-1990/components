#include "../../include/buffer/BufferManager.hpp"
#include "../../include/videoFile/VideoFile.hpp"
#include "../../include/videoFile/IoUringVideoReader.hpp"
#include "../../include/monitor/PerformanceMonitor.hpp"
#include "../../include/monitor/Timer.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <chrono>
#include <stdexcept>
#include <algorithm>

// CMA/DMA-BUF 相关头文件（如果系统支持）
#ifdef __linux__
// 尝试包含 DMA-BUF 头文件
#if __has_include(<linux/dma-buf.h>)
#include <linux/dma-buf.h>
#endif
#if __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#define HAS_DMA_HEAP 1
#else
#define HAS_DMA_HEAP 0
// 如果系统不支持，定义必要的结构体
struct dma_heap_allocation_data {
    unsigned long len;
    unsigned int fd;
    unsigned int fd_flags;
    unsigned long heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC 0
#endif
#endif

// ============ 构造函数 ============

BufferManager::BufferManager(int buffer_count, size_t buffer_size, bool use_cma)
    : buffer_size_(buffer_size)
    , use_cma_(use_cma)
    , producer_running_(false)
    , producer_state_(ProducerState::STOPPED)
    , producer_thread_count_(0)
    , next_frame_index_(0)
{
    printf("\n📦 Initializing BufferManager...\n");
    printf("   Buffer count: %d\n", buffer_count);
    printf("   Buffer size: %zu bytes\n", buffer_size);
    printf("   Memory type: %s\n", use_cma ? "CMA (Physical Contiguous)" : "Normal");
    
    // 预分配容器空间
    buffers_.reserve(buffer_count);
    memory_blocks_.reserve(buffer_count);
    if (use_cma) {
        dma_fds_.reserve(buffer_count);
    }
    
    // 分配每个 Buffer
    for (int i = 0; i < buffer_count; i++) {
        void* addr = nullptr;
        int dma_fd = -1;
        
        if (use_cma_) {
            addr = allocateCMAMemory(buffer_size, dma_fd);
            if (addr == nullptr) {
                printf("⚠️  Warning: CMA allocation failed for buffer %d, falling back to normal memory\n", i);
                use_cma_ = false;  // 回退到普通内存
                addr = allocateNormalMemory(buffer_size);
            } else {
                dma_fds_.push_back(dma_fd);
            }
        } else {
            addr = allocateNormalMemory(buffer_size);
        }
        
        if (addr == nullptr) {
            printf("❌ ERROR: Failed to allocate memory for buffer %d\n", i);
            // 清理已分配的资源
            for (size_t j = 0; j < memory_blocks_.size(); j++) {
                if (use_cma_ && j < dma_fds_.size()) {
                    freeCMAMemory(memory_blocks_[j], buffer_size_, dma_fds_[j]);
                } else {
                    freeNormalMemory(memory_blocks_[j]);
                }
            }
            throw std::runtime_error("Buffer allocation failed");
        }
        
        // 创建 Buffer 对象
        buffers_.emplace_back(addr, buffer_size);
        memory_blocks_.push_back(addr);
        
        // 放入空闲队列
        free_queue_.push(&buffers_[i]);
    }
    
    printf("✅ BufferManager initialized successfully\n");
    printf("   Free buffers: %d\n", (int)free_queue_.size());
    printf("   Filled buffers: %d\n", (int)filled_queue_.size());
}

// ============ 析构函数 ============

BufferManager::~BufferManager() {
    printf("\n🧹 Cleaning up BufferManager...\n");
    
    // 停止生产者线程
    if (producer_running_) {
        printf("   Stopping producer thread...\n");
        stopVideoProducer();
    }
    
    // 释放所有内存
    for (size_t i = 0; i < memory_blocks_.size(); i++) {
        if (use_cma_ && i < dma_fds_.size()) {
            freeCMAMemory(memory_blocks_[i], buffer_size_, dma_fds_[i]);
        } else {
            freeNormalMemory(memory_blocks_[i]);
        }
    }
    
    printf("✅ BufferManager cleaned up\n");
}

// ============ 生产者接口 ============

Buffer* BufferManager::acquireFreeBuffer(bool blocking, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (blocking) {
        if (timeout_ms > 0) {
            // 带超时的等待
            auto timeout = std::chrono::milliseconds(timeout_ms);
            if (!free_cv_.wait_for(lock, timeout, [this] { return !free_queue_.empty(); })) {
                // 超时
                return nullptr;
            }
        } else {
            // 无限等待
            free_cv_.wait(lock, [this] { return !free_queue_.empty(); });
        }
    } else {
        // 非阻塞模式
        if (free_queue_.empty()) {
            return nullptr;
        }
    }
    
    // 从空闲队列取出一个 buffer
    Buffer* buffer = free_queue_.front();
    free_queue_.pop();
    
    return buffer;
}

void BufferManager::submitFilledBuffer(Buffer* buffer) {
    if (buffer == nullptr) {
        printf("⚠️  Warning: Trying to submit null buffer\n");
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        filled_queue_.push(buffer);
        
        // ✅ 在锁内通知，避免丢失唤醒
        filled_cv_.notify_all();
    }
}

// ============ 消费者接口 ============

Buffer* BufferManager::acquireFilledBuffer(bool blocking, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (blocking) {
        if (timeout_ms > 0) {
            // 带超时的等待
            auto timeout = std::chrono::milliseconds(timeout_ms);
            if (!filled_cv_.wait_for(lock, timeout, [this] { return !filled_queue_.empty(); })) {
                // 超时
                return nullptr;
            }
        } else {
            // 无限等待
            filled_cv_.wait(lock, [this] { return !filled_queue_.empty(); });
        }
    } else {
        // 非阻塞模式
        if (filled_queue_.empty()) {
            return nullptr;
        }
    }
    
    // 从就绪队列取出一个 buffer
    Buffer* buffer = filled_queue_.front();
    filled_queue_.pop();
    
    return buffer;
}

void BufferManager::recycleBuffer(Buffer* buffer) {
    if (buffer == nullptr) {
        printf("⚠️  Warning: Trying to recycle null buffer\n");
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        free_queue_.push(buffer);
        
        // ✅ 关键修复：在锁内通知，避免丢失唤醒
        // 通知所有等待的生产者（在多线程生产者场景下更高效）
        free_cv_.notify_all();
    }  // 锁在这里释放，此时通知已经发出
}

// ============ 查询接口 ============

int BufferManager::getFreeBufferCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(free_queue_.size());
}

int BufferManager::getFilledBufferCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(filled_queue_.size());
}

int BufferManager::getTotalBufferCount() const {
    return static_cast<int>(buffers_.size());
}

size_t BufferManager::getBufferSize() const {
    return buffer_size_;
}

// ============ 内部方法：内存分配 ============

void* BufferManager::allocateCMAMemory(size_t size, int& out_fd) {
#ifdef __linux__
    // 尝试打开 DMA heap 设备
    const char* heap_paths[] = {
        "/dev/dma_heap/linux,cma",
        "/dev/dma_heap/system",
        "/dev/ion",  // 旧版本 Android
    };
    
    int heap_fd = -1;
    for (const char* path : heap_paths) {
        heap_fd = open(path, O_RDWR);
        if (heap_fd >= 0) {
            printf("   Opened DMA heap: %s\n", path);
            break;
        }
    }
    
    if (heap_fd < 0) {
        printf("   CMA device not available\n");
        return nullptr;
    }
    
    // 分配 DMA buffer
    struct dma_heap_allocation_data heap_data;
    memset(&heap_data, 0, sizeof(heap_data));
    heap_data.len = size;
    heap_data.fd_flags = O_RDWR | O_CLOEXEC;
    
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &heap_data) < 0) {
        printf("   DMA_HEAP_IOCTL_ALLOC failed: %s\n", strerror(errno));
        close(heap_fd);
        return nullptr;
    }
    
    out_fd = heap_data.fd;
    close(heap_fd);
    
    // mmap DMA buffer
    void* addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, out_fd, 0);
    if (addr == MAP_FAILED) {
        printf("   mmap DMA buffer failed: %s\n", strerror(errno));
        close(out_fd);
        return nullptr;
    }
    
    return addr;
#else
    // 非 Linux 系统不支持 CMA
    return nullptr;
#endif
}

void* BufferManager::allocateNormalMemory(size_t size) {
    // 使用 posix_memalign 分配对齐的内存（4KB 对齐）
    void* addr = nullptr;
    int ret = posix_memalign(&addr, 4096, size);
    if (ret != 0) {
        printf("   posix_memalign failed: %s\n", strerror(ret));
        return nullptr;
    }
    
    // 清零
    memset(addr, 0, size);
    
    return addr;
}

void BufferManager::freeCMAMemory(void* addr, size_t size, int fd) {
    if (addr != nullptr) {
        munmap(addr, size);
    }
    if (fd >= 0) {
        close(fd);
    }
}

void BufferManager::freeNormalMemory(void* addr) {
    if (addr != nullptr) {
        free(addr);
    }
}

// ============ 生产者线程接口实现 ============

// 统一的内部实现：支持单线程和多线程模式
bool BufferManager::startVideoProducerInternal(int thread_count,
                                              const char* video_file_path, 
                                              int width, int height, int bits_per_pixel,
                                              bool loop,
                                              ErrorCallback error_callback) {
    // 检查是否已经在运行
    if (producer_running_) {
        printf("⚠️  Warning: Producer thread(s) already running\n");
        return false;
    }
    
    if (thread_count < 1) {
        printf("❌ ERROR: Thread count must be >= 1\n");
        return false;
    }
    
    printf("\n🎬 Starting %d video producer thread(s)...\n", thread_count);
    printf("   Video file: %s\n", video_file_path);
    printf("   Resolution: %dx%d\n", width, height);
    printf("   Bits per pixel: %d\n", bits_per_pixel);
    printf("   Loop mode: %s\n", loop ? "enabled" : "disabled");
    
    // 保存错误回调
    error_callback_ = error_callback;
    
    // 🆕 创建共享的 VideoFile 对象（所有线程共享，只打开一次文件）
    shared_video_file_ = std::make_shared<VideoFile>();
    if (!shared_video_file_->openRaw(video_file_path, width, height, bits_per_pixel)) {
        printf("❌ ERROR: Failed to open shared video file\n");
        shared_video_file_.reset();
        return false;
    }
    
    int total_frames = shared_video_file_->getTotalFrames();
    size_t frame_size = shared_video_file_->getFrameSize();
    
    printf("   Total frames: %d\n", total_frames);
    printf("   Frame size: %zu bytes\n", frame_size);
    
    // 检查帧大小是否匹配
    if (frame_size != buffer_size_) {
        printf("❌ ERROR: Frame size mismatch: video=%zu, buffer=%zu\n",
               frame_size, buffer_size_);
        shared_video_file_.reset();
        return false;
    }
    
    // 重置状态
    producer_running_ = true;
    producer_state_ = ProducerState::RUNNING;
    producer_thread_count_ = thread_count;
    last_error_.clear();
    next_frame_index_ = 0;  // 重置帧索引
    
    // 启动线程
    producer_threads_.reserve(thread_count);
    for (int i = 0; i < thread_count; i++) {
        try {
            // 🆕 传递共享的 VideoFile 对象（所有线程使用同一个对象）
            producer_threads_.emplace_back(&BufferManager::multiVideoProducerThread, this,
                                              i, shared_video_file_, loop, total_frames);
            
            if (thread_count == 1) {
                printf("✅ Video producer thread started\n");
            } else {
                printf("   ✅ Producer thread #%d started\n", i);
            }
        } catch (const std::exception& e) {
            printf("❌ ERROR: Failed to start producer thread #%d: %s\n", i, e.what());
            // 停止已启动的线程
            producer_running_ = false;
            for (auto& thread : producer_threads_) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            producer_threads_.clear();
            shared_video_file_.reset();  // 清理共享VideoFile
            producer_state_ = ProducerState::ERROR;
            std::string error_msg = std::string("Failed to start producer thread: ") + e.what();
            setError(error_msg);
            return false;
        }
    }
    
    if (thread_count > 1) {
        printf("✅ All %d video producer threads started successfully\n", thread_count);
    }
    
    return true;
}

// 单线程模式便利接口（内部调用统一实现）
bool BufferManager::startVideoProducer(const char* video_file_path, 
                                      int width, int height, int bits_per_pixel,
                                      bool loop,
                                      ErrorCallback error_callback) {
    // 调用统一实现，thread_count = 1
    return startVideoProducerInternal(1, video_file_path, width, height, 
                                     bits_per_pixel, loop, error_callback);
}

void BufferManager::stopVideoProducer() {
    if (!producer_running_) {
        return;
    }
    
    printf("\n🛑 Stopping video producer thread(s)...\n");
    
    // 设置停止标志
    producer_running_ = false;
    
    // 唤醒可能在等待的线程
    free_cv_.notify_all();
    filled_cv_.notify_all();
    
    // 等待所有线程退出（统一使用 producer_threads_）
    for (auto& thread : producer_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    producer_threads_.clear();
    
    // 清理共享的 VideoFile（如果有）
    if (shared_video_file_) {
        printf("🧹 Closing shared video file...\n");
        shared_video_file_.reset();
    }
    
    // 清理 io_uring readers（如果有）
    if (!iouring_readers_.empty()) {
        printf("🧹 Cleaning up %zu IoUringVideoReader(s)...\n", iouring_readers_.size());
        for (void* r : iouring_readers_) {
            delete static_cast<IoUringVideoReader*>(r);
        }
        iouring_readers_.clear();
    }
    
    producer_state_ = ProducerState::STOPPED;
    printf("✅ Video producer thread(s) stopped (count: %d)\n", producer_thread_count_);
    producer_thread_count_ = 0;
}

BufferManager::ProducerState BufferManager::getProducerState() const {
    return producer_state_.load();
}

std::string BufferManager::getLastProducerError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

bool BufferManager::isProducerRunning() const {
    return producer_running_.load();
}


// ============ 错误处理辅助函数 ============

void BufferManager::setError(const std::string& error_msg) {
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
    printf("❌ Producer Error: %s\n", error_msg.c_str());
}

// ============ 多生产者线程接口实现 ============

// 多线程模式便利接口（内部调用统一实现）
bool BufferManager::startMultipleVideoProducers(int thread_count,
                                               const char* video_file_path, 
                                               int width, int height, int bits_per_pixel,
                                               bool loop,
                                               ErrorCallback error_callback) {
    // 调用统一实现
    return startVideoProducerInternal(thread_count, video_file_path, width, height, 
                                     bits_per_pixel, loop, error_callback);
}

// ============ 多生产者线程函数 ============

namespace {
    // 定时器回调数据结构
    struct ThreadTimerData {
        int thread_id;
        PerformanceMonitor* monitor_periodic;  // 周期统计（每1秒重置）
    };

    // 定时器回调函数（每1秒打印线程统计并重置周期monitor）
    void threadTimerCallback(void* data) {
        ThreadTimerData* stats = static_cast<ThreadTimerData*>(data);
        
        // 从周期monitor读取统计数据
        int loaded_frames = stats->monitor_periodic->getLoadedFrames();
        double avg_fps = stats->monitor_periodic->getAverageLoadFPS();
        double total_time = stats->monitor_periodic->getTotalTime();
        
        // 计算平均每帧加载时间
        double avg_time_per_frame = 0.0;
        if (loaded_frames > 0 && total_time > 0) {
            avg_time_per_frame = (total_time * 1000.0) / loaded_frames;
        }
        
        // 打印本周期（过去1秒）的统计
        printf("🔄 [Thread #%d] Last 1s: Loaded %d frames (%.1f fps) | Avg: %.2f ms/frame\n",
               stats->thread_id,
               loaded_frames,
               avg_fps,
               avg_time_per_frame);
        
        // 🔑 关键：打印后重置周期monitor，为下一个1秒周期做准备
        stats->monitor_periodic->reset();
    }
}

void BufferManager::multiVideoProducerThread(int thread_id,
                                            std::shared_ptr<VideoFile> shared_video,
                                            bool loop, int total_frames) {
    // 🎉 使用共享的 VideoFile 对象，无需每个线程打开一次文件
    // shared_video 是通过 shared_ptr 传递的，引用计数管理生命周期
    
    if (!shared_video || !shared_video->isOpen()) {
        std::string error_msg = std::string("Thread #") + std::to_string(thread_id) + 
                                ": Shared video file is not available";
        setError(error_msg);
        printf("❌ %s\n", error_msg.c_str());
        producer_state_ = ProducerState::ERROR;
        return;
    }
    
    int frames_produced = 0;
    
    printf("🚀 Thread #%d: Using shared VideoFile with thread-safe reading\n", thread_id);
    
    // 主循环
    int loop_iterations = 0;
    int skipped_frames = 0;  // 读取失败的帧数（仅统计视频文件读取错误）
    int consecutive_failures = 0;  // 连续失败计数
    
    // 🎯 创建两个性能监控器
    // (1) 周期统计monitor：每1秒重置，用于打印每秒的瞬时性能
    PerformanceMonitor monitor_periodic;
    monitor_periodic.start();
    
    // (2) 总体统计monitor：从线程启动到结束，累计所有帧数和时间
    PerformanceMonitor monitor_total;
    monitor_total.start();
    
    // 创建定时器数据（只传递周期monitor，因为回调只需要打印和重置周期monitor）
    ThreadTimerData timer_data = { thread_id, &monitor_periodic };
    
    // 创建定时器（负责定时触发打印）
    // 参数：间隔1秒，回调函数，数据，延迟0秒，总时长0秒（永久运行）
    Timer timer(1.0, threadTimerCallback, &timer_data, 0.0, 40);
    timer.start();
    while (producer_running_) {
        loop_iterations++;
        
        // 原子地获取下一个帧号
        int frame_index = next_frame_index_.fetch_add(1);
        
        // 处理循环模式和文件边界
        if (frame_index >= total_frames) {
            if (loop) {
                // 循环模式：归一化到 0-total_frames 范围
                frame_index = frame_index % total_frames;
                
                // 尝试重置计数器，避免整数溢出
                int current = next_frame_index_.load();
                if (current > total_frames * 2) {
                    int expected = current;
                    int new_value = frame_index + 1;
                    next_frame_index_.compare_exchange_strong(expected, new_value);
                }
            } else {
                // 非循环模式：没有更多帧可读
                break;
            }
        }
        
        // 获取空闲 buffer - 循环等待直到成功（不跳帧，保证视频连续性）
        Buffer* buffer = nullptr;
        while (producer_running_ && buffer == nullptr) {
            buffer = acquireFreeBuffer(true, 100);  // 100ms 超时，持续重试
            // 如果获取失败但仍在运行，说明队列满了，继续等待消费者释放buffer
            if (buffer == nullptr && producer_running_) {
                printf("   [Thread #%d] Failed to acquire free buffer, waiting for 100ms...\n", thread_id);
            }
        }
        
        // 检查是否因为停止信号退出循环
        if (!producer_running_) {
            printf("   [Producer] Stopped, exiting...\n");
            break;
        }
        
        // 🎯 开始计时（同时记录到两个monitor）
        monitor_periodic.beginLoadFrameTiming();
        monitor_total.beginLoadFrameTiming();
        
        // 🔑 使用线程安全的读取方法（不修改VideoFile内部状态）
        bool read_success = shared_video->readFrameAtThreadSafe(frame_index, buffer->data(), buffer->size());
        
        // 🎯 结束计时（同时记录到两个monitor）
        monitor_periodic.endLoadFrameTiming();
        monitor_total.endLoadFrameTiming();
        
        if (!read_success) {
            skipped_frames++;
            printf("⚠️  Thread #%d: Failed to read frame %d/%d\n", 
                   thread_id, frame_index, total_frames);
            
            recycleBuffer(buffer);  // 归还 buffer
            
            // 连续失败检测
            consecutive_failures++;
            if (consecutive_failures > 5) {
                char error_msg[256];
                snprintf(error_msg, sizeof(error_msg),
                        "Thread #%d: Too many consecutive read failures (%d)",
                        thread_id, consecutive_failures);
                setError(error_msg);
                producer_state_ = ProducerState::ERROR;
                break;
            }
            continue;  // 继续下一帧
        }
        
        // 重置失败计数
        consecutive_failures = 0;
        // 将填充好的 buffer 提交到就绪队列
        submitFilledBuffer(buffer);
        frames_produced++;
        
        // 如果出错，退出主循环
        if (producer_state_ == ProducerState::ERROR) {
            break;
        }
    }  // end of while loop
    
    // 停止定时器
    timer.stop();
    
    // 注意：不需要 close()，因为使用的是共享的 VideoFile
    // shared_video 的生命周期由 shared_ptr 管理，会在所有线程退出后自动清理
    
    // 🎯 打印最终的总体统计（使用 monitor_total，它累计了从开始到结束的所有数据）
    printf("\n🏁 Thread #%d finished:\n", thread_id);
    printf("   📊 Produced %d frames, skipped %d frames\n", frames_produced, skipped_frames);
    printf("   📊 Total loaded frames: %d\n", monitor_total.getLoadedFrames());
    printf("   📊 Average load FPS: %.2f\n", monitor_total.getAverageLoadFPS());
    printf("   📊 Total time: %.2f seconds\n", monitor_total.getTotalTime());
    if (monitor_total.getLoadedFrames() > 0) {
        printf("   📊 Average time per frame: %.2f ms\n", 
               (monitor_total.getTotalTime() * 1000.0) / monitor_total.getLoadedFrames());
    }
}

// ============ io_uring 生产者接口实现 ============

bool BufferManager::startMultipleVideoProducersIoUring(int thread_count,
                                                       const char* video_file_path, 
                                                       int width, int height, int bits_per_pixel,
                                                       bool loop,
                                                       ErrorCallback error_callback) {
    // 检查是否已经在运行
    if (producer_running_) {
        printf("⚠️  Warning: Producer thread(s) already running\n");
        return false;
    }
    
    if (thread_count < 1) {
        printf("❌ ERROR: Thread count must be >= 1\n");
        return false;
    }
    
    printf("\n🚀 Starting %d io_uring video producer thread(s)...\n", thread_count);
    printf("   Video file: %s\n", video_file_path);
    printf("   Resolution: %dx%d\n", width, height);
    printf("   Bits per pixel: %d\n", bits_per_pixel);
    printf("   Loop mode: %s\n", loop ? "enabled" : "disabled");
    printf("   I/O Mode: io_uring (async, zero-copy)\n");
    
    // 保存错误回调
    error_callback_ = error_callback;
    
    // 首先创建一个临时reader来获取总帧数和验证文件
    IoUringVideoReader* temp_reader = new IoUringVideoReader(video_file_path, width, height, 
                                                              bits_per_pixel, 32);
    
    if (!temp_reader->isInitialized()) {
        printf("❌ ERROR: Failed to initialize IoUringVideoReader\n");
        delete temp_reader;
        return false;
    }
    
    int total_frames = temp_reader->getTotalFrames();
    printf("   Total frames: %d\n", total_frames);
    
    // 为每个线程分配**连续的帧块**（关键优化：顺序读取避免随机I/O）
    // 例如：Thread #0: 0-197, Thread #1: 198-394, Thread #2: 395-591
    std::vector<std::vector<int>> thread_frames(thread_count);
    int frames_per_thread = (total_frames + thread_count - 1) / thread_count;
    
    for (int t = 0; t < thread_count; t++) {
        int start = t * frames_per_thread;
        int end = std::min(start + frames_per_thread, total_frames);
        
        for (int i = start; i < end; i++) {
            thread_frames[t].push_back(i);
        }
        
        printf("   Thread #%d will read frames %d-%d (%d frames)\n", 
               t, start, end - 1, end - start);
    }
    
    // 不再需要临时reader
    delete temp_reader;
    
    // 重置状态
    producer_running_ = true;
    producer_state_ = ProducerState::RUNNING;
    producer_thread_count_ = thread_count;
    last_error_.clear();
    
    // 为每个线程创建独立的IoUringVideoReader并启动线程
    // 注意：每个线程需要自己的reader，因为io_uring ring不是线程安全的
    producer_threads_.reserve(thread_count);
    iouring_readers_.clear();  // 清空之前的readers
    iouring_readers_.reserve(thread_count);
    
    for (int i = 0; i < thread_count; i++) {
        // 为每个线程创建独立的reader
        IoUringVideoReader* reader = new IoUringVideoReader(video_file_path, width, height, 
                                                             bits_per_pixel, 32);
        if (!reader->isInitialized()) {
            printf("❌ ERROR: Failed to initialize IoUringVideoReader for thread #%d\n", i);
            // 清理已创建的readers
            for (void* r : iouring_readers_) {
                delete static_cast<IoUringVideoReader*>(r);
            }
            iouring_readers_.clear();
            producer_running_ = false;
            producer_state_ = ProducerState::ERROR;
            return false;
        }
        
        // 保存reader指针（转换为void*以避免头文件依赖）
        iouring_readers_.push_back(static_cast<void*>(reader));
        
        try {
            producer_threads_.emplace_back(
                &IoUringVideoReader::asyncProducerThread, 
                reader,                      // 每个线程有自己的reader
                i,                          // thread_id
                this,                       // BufferManager*
                thread_frames[i],           // frame_indices - 按值传递（拷贝）
                std::ref(producer_running_),// running flag
                loop                        // loop
            );
            printf("   ✅ io_uring producer thread #%d started (%zu frames)\n", 
                   i, thread_frames[i].size());
        } catch (const std::exception& e) {
            printf("❌ ERROR: Failed to start producer thread #%d: %s\n", i, e.what());
            producer_running_ = false;
            for (auto& thread : producer_threads_) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            producer_threads_.clear();
            producer_state_ = ProducerState::ERROR;
            // 清理所有readers
            for (void* r : iouring_readers_) {
                delete static_cast<IoUringVideoReader*>(r);
            }
            iouring_readers_.clear();
            return false;
        }
    }
    
    printf("✅ All %d io_uring video producer threads started successfully\n", thread_count);
    
    // readers会在stopVideoProducer()中清理
    
    return true;
}

