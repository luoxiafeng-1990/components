#include "../include/BufferManager.hpp"
#include "../include/VideoFile.hpp"
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
    }
    
    // 通知等待的消费者
    filled_cv_.notify_one();
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
    }
    
    // 通知等待的生产者
    free_cv_.notify_one();
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

bool BufferManager::startVideoProducer(const char* video_file_path, 
                                      int width, int height, int bits_per_pixel,
                                      bool loop,
                                      ErrorCallback error_callback) {
    // 检查是否已经在运行
    if (producer_running_) {
        printf("⚠️  Warning: Producer thread is already running\n");
        return false;
    }
    
    printf("\n🎬 Starting video producer thread...\n");
    printf("   Video file: %s\n", video_file_path);
    printf("   Resolution: %dx%d\n", width, height);
    printf("   Bits per pixel: %d\n", bits_per_pixel);
    printf("   Loop mode: %s\n", loop ? "enabled" : "disabled");
    
    // 保存错误回调
    error_callback_ = error_callback;
    
    // 重置状态
    producer_running_ = true;
    producer_state_ = ProducerState::RUNNING;
    last_error_.clear();
    
    // 启动线程
    try {
        producer_thread_ = std::thread(&BufferManager::videoProducerThread, this,
                                      video_file_path, width, height, bits_per_pixel, loop);
        printf("✅ Video producer thread started\n");
        return true;
    } catch (const std::exception& e) {
        producer_running_ = false;
        producer_state_ = ProducerState::ERROR;
        std::string error_msg = std::string("Failed to start producer thread: ") + e.what();
        setError(error_msg);
        printf("❌ %s\n", error_msg.c_str());
        return false;
    }
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
    
    // 等待单线程退出
    if (producer_thread_.joinable()) {
        producer_thread_.join();
    }
    
    // 等待所有多线程退出
    for (auto& thread : producer_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    producer_threads_.clear();
    
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

// ============ 生产者线程函数 ============

void BufferManager::videoProducerThread(const char* video_file_path, 
                                       int width, int height, int bits_per_pixel,
                                       bool loop) {
    printf("\n🎥 Video producer thread started (TID: %ld)\n", 
           (long)pthread_self());
    
    // 打开视频文件
    VideoFile video;
    if (!video.openRaw(video_file_path, width, height, bits_per_pixel)) {
        std::string error_msg = std::string("Failed to open video file: ") + video_file_path;
        setError(error_msg);
        printf("❌ %s\n", error_msg.c_str());
        producer_state_ = ProducerState::ERROR;
        producer_running_ = false;
        return;
    }
    
    printf("   Video opened successfully\n");
    printf("   Total frames: %d\n", video.getTotalFrames());
    printf("   Frame size: %zu bytes\n", video.getFrameSize());
    
    // 检查帧大小是否匹配
    if (video.getFrameSize() != buffer_size_) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg),
                "Frame size mismatch: video=%zu, buffer=%zu",
                video.getFrameSize(), buffer_size_);
        setError(error_msg);
        printf("❌ %s\n", error_msg);
        producer_state_ = ProducerState::ERROR;
        producer_running_ = false;
        return;
    }
    
    int frame_count = 0;
    
    // 主循环
    while (producer_running_) {
        // 获取空闲 buffer
        Buffer* buffer = acquireFreeBuffer(true, 100);  // 100ms 超时
        if (buffer == nullptr) {
            // 超时或被中断
            if (!producer_running_) {
                break;
            }
            continue;
        }
        
        // 读取一帧数据
        if (!video.readFrameTo(*buffer)) {
            // 读取失败，检查是否到达文件末尾
            if (video.isAtEnd()) {
                if (loop) {
                    // 循环模式：回到文件开头
                    printf("   Reached end of file, looping back...\n");
                    if (!video.seekToBegin()) {
                        setError("Failed to seek to beginning");
                        producer_state_ = ProducerState::ERROR;
                        recycleBuffer(buffer);  // 归还 buffer
                        break;
                    }
                    // 重新读取
                    if (!video.readFrameTo(*buffer)) {
                        setError("Failed to read frame after seeking");
                        producer_state_ = ProducerState::ERROR;
                        recycleBuffer(buffer);
                        break;
                    }
                } else {
                    // 非循环模式：正常结束
                    printf("   Reached end of file, stopping...\n");
                    recycleBuffer(buffer);  // 归还 buffer
                    producer_running_ = false;
                    break;
                }
            } else {
                // 读取错误
                setError("Failed to read frame from video file");
                producer_state_ = ProducerState::ERROR;
                recycleBuffer(buffer);
                break;
            }
        }
        
        // 将填充好的 buffer 提交到就绪队列
        submitFilledBuffer(buffer);
        
        frame_count++;
        if (frame_count % 100 == 0) {
            printf("   Produced %d frames (current: %d/%d)\n", 
                   frame_count, video.getCurrentFrameIndex(), video.getTotalFrames());
        }
    }
    
    printf("\n✅ Video producer thread exiting\n");
    printf("   Total frames produced: %d\n", frame_count);
    
    if (producer_state_ != ProducerState::ERROR) {
        producer_state_ = ProducerState::STOPPED;
    }
    producer_running_ = false;
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

bool BufferManager::startMultipleVideoProducers(int thread_count,
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
    
    printf("\n🎬 Starting %d video producer threads...\n", thread_count);
    printf("   Video file: %s\n", video_file_path);
    printf("   Resolution: %dx%d\n", width, height);
    printf("   Bits per pixel: %d\n", bits_per_pixel);
    printf("   Loop mode: %s\n", loop ? "enabled" : "disabled");
    
    // 先打开一个VideoFile来获取总帧数
    VideoFile test_video;
    if (!test_video.openRaw(video_file_path, width, height, bits_per_pixel)) {
        printf("❌ ERROR: Failed to open video file for validation\n");
        return false;
    }
    
    int total_frames = test_video.getTotalFrames();
    size_t frame_size = test_video.getFrameSize();
    
    printf("   Total frames: %d\n", total_frames);
    printf("   Frame size: %zu bytes\n", frame_size);
    
    // 检查帧大小是否匹配
    if (frame_size != buffer_size_) {
        printf("❌ ERROR: Frame size mismatch: video=%zu, buffer=%zu\n",
               frame_size, buffer_size_);
        return false;
    }
    
    test_video.close();
    
    // 保存错误回调
    error_callback_ = error_callback;
    
    // 重置状态
    producer_running_ = true;
    producer_state_ = ProducerState::RUNNING;
    producer_thread_count_ = thread_count;
    next_frame_index_ = 0;  // 重置帧索引
    last_error_.clear();
    
    // 启动多个生产者线程
    producer_threads_.reserve(thread_count);
    for (int i = 0; i < thread_count; i++) {
        try {
            producer_threads_.emplace_back(&BufferManager::multiVideoProducerThread, this,
                                          i, video_file_path, width, height, 
                                          bits_per_pixel, loop, total_frames);
            printf("   ✅ Producer thread #%d started\n", i);
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
            producer_state_ = ProducerState::ERROR;
            return false;
        }
    }
    
    printf("✅ All %d video producer threads started successfully\n", thread_count);
    return true;
}

// ============ 多生产者线程函数 ============

void BufferManager::multiVideoProducerThread(int thread_id,
                                            const char* video_file_path, 
                                            int width, int height, int bits_per_pixel,
                                            bool loop, int total_frames) {
    printf("\n🎥 Producer thread #%d started (TID: %ld)\n", 
           thread_id, (long)pthread_self());
    
    // 每个线程打开自己的VideoFile实例
    VideoFile video;
    if (!video.openRaw(video_file_path, width, height, bits_per_pixel)) {
        std::string error_msg = std::string("Thread #") + std::to_string(thread_id) + 
                                ": Failed to open video file";
        setError(error_msg);
        producer_state_ = ProducerState::ERROR;
        return;
    }
    
    int frames_produced = 0;
    
    // 主循环
    while (producer_running_) {
        // 原子地获取下一个要读取的帧索引
        int frame_index = next_frame_index_.fetch_add(1);
        
        // 检查是否超出范围
        if (frame_index >= total_frames) {
            if (loop) {
                // 循环模式：重置索引
                // 使用 compare_exchange 来安全地重置
                int expected = frame_index + 1;
                while (next_frame_index_.load() >= total_frames) {
                    next_frame_index_.compare_exchange_weak(expected, 0);
                    expected = next_frame_index_.load();
                }
                frame_index = next_frame_index_.fetch_add(1);
                if (frame_index >= total_frames) {
                    continue;  // 其他线程已经重置，重试
                }
                printf("   Thread #%d: Looping back to frame 0\n", thread_id);
            } else {
                // 非循环模式：退出
                printf("   Thread #%d: Reached end of file, exiting\n", thread_id);
                break;
            }
        }
        
        // 获取空闲 buffer
        Buffer* buffer = acquireFreeBuffer(true, 100);  // 100ms 超时
        if (buffer == nullptr) {
            // 超时或被中断
            if (!producer_running_) {
                break;
            }
            // 回退帧索引，让其他线程重试
            next_frame_index_.fetch_sub(1);
            continue;
        }
        
        // 跳转到指定帧并读取
        if (!video.readFrameAt(frame_index, *buffer)) {
            printf("⚠️  Thread #%d: Failed to read frame %d\n", thread_id, frame_index);
            recycleBuffer(buffer);  // 归还 buffer
            // 回退帧索引
            next_frame_index_.fetch_sub(1);
            continue;
        }
        
        // 将填充好的 buffer 提交到就绪队列
        submitFilledBuffer(buffer);
        
        frames_produced++;
        if (frames_produced % 50 == 0) {
            printf("   Thread #%d: Produced %d frames\n", thread_id, frames_produced);
        }
    }
    
    printf("\n✅ Producer thread #%d exiting (produced %d frames)\n", 
           thread_id, frames_produced);
}

