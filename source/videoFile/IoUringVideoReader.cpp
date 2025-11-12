#include "../../include/videoFile/IoUringVideoReader.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <chrono>
#include <thread>

// ============ 构造函数 ============

IoUringVideoReader::IoUringVideoReader(const char* video_path, 
                                     int width, int height, int bits_per_pixel,
                                     int queue_depth)
    : queue_depth_(queue_depth)
    , initialized_(false)
    , video_fd_(-1)
    , video_path_(video_path)
    , width_(width)
    , height_(height)
    , bits_per_pixel_(bits_per_pixel)
{
    printf("\n📖 Initializing IoUringVideoReader...\n");
    printf("   Video file: %s\n", video_path);
    printf("   Resolution: %dx%d @ %d bpp\n", width, height, bits_per_pixel);
    printf("   Queue depth: %d\n", queue_depth);
    
    // 计算帧大小
    frame_size_ = (size_t)width * height * (bits_per_pixel / 8);
    printf("   Frame size: %zu bytes (%.2f MB)\n", frame_size_, frame_size_ / (1024.0 * 1024.0));
    
    // 1. 打开视频文件
    video_fd_ = open(video_path, O_RDONLY);
    if (video_fd_ < 0) {
        printf("❌ ERROR: Failed to open video file: %s\n", strerror(errno));
        return;
    }
    
    // 2. 获取文件大小并计算总帧数
    struct stat st;
    if (fstat(video_fd_, &st) < 0) {
        printf("❌ ERROR: Failed to stat video file: %s\n", strerror(errno));
        close(video_fd_);
        video_fd_ = -1;
        return;
    }
    
    total_frames_ = st.st_size / frame_size_;
    printf("   File size: %ld bytes (%.2f MB)\n", st.st_size, st.st_size / (1024.0 * 1024.0));
    printf("   Total frames: %d\n", total_frames_);
    
    if (total_frames_ == 0) {
        printf("❌ ERROR: Invalid video file (no frames)\n");
        close(video_fd_);
        video_fd_ = -1;
        return;
    }
    
    // 3. 初始化io_uring
    int ret = io_uring_queue_init(queue_depth, &ring_, 0);
    if (ret < 0) {
        printf("❌ ERROR: io_uring_queue_init failed: %s\n", strerror(-ret));
        close(video_fd_);
        video_fd_ = -1;
        return;
    }
    
    initialized_ = true;
    printf("✅ IoUringVideoReader initialized successfully\n");
}

// ============ 析构函数 ============

IoUringVideoReader::~IoUringVideoReader() {
    if (initialized_) {
        // 打印统计信息
        Stats stats = getStats();
        printf("\n📊 IoUringVideoReader Statistics:\n");
        printf("   Total reads: %ld\n", stats.total_reads);
        printf("   Successful: %ld\n", stats.successful_reads);
        printf("   Failed: %ld\n", stats.failed_reads);
        printf("   Total bytes: %ld (%.2f MB)\n", 
               stats.total_bytes, stats.total_bytes / (1024.0 * 1024.0));
        printf("   Avg latency: %.2f μs\n", stats.avg_latency_us);
        
        io_uring_queue_exit(&ring_);
    }
    
    if (video_fd_ >= 0) {
        close(video_fd_);
    }
    
    printf("✅ IoUringVideoReader cleaned up\n");
}

// ============ 提交批量读取请求 ============

int IoUringVideoReader::submitReadBatch(BufferManager* manager, 
                                       const std::vector<int>& frame_indices) {
    int submitted = 0;
    
    for (int frame_idx : frame_indices) {
        // 1. 获取空闲buffer
        Buffer* buffer = manager->acquireFreeBuffer(false, 0);  // 非阻塞
        if (!buffer) {
            // 没有空闲buffer，先收割已完成的请求
            harvestCompletions(manager, false);
            buffer = manager->acquireFreeBuffer(true, 100);  // 阻塞等待100ms
            if (!buffer) {
                continue;  // 仍然没有，跳过这个请求
            }
        }
        
        // 2. 获取SQ entry
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            // SQ已满，先提交现有请求
            io_uring_submit(&ring_);
            
            // 收割一些完成的请求，释放SQ空间
            harvestCompletions(manager, false);
            
            // 重新获取SQE
            sqe = io_uring_get_sqe(&ring_);
            if (!sqe) {
                // 仍然没有，回收buffer并跳过
                manager->recycleBuffer(buffer);
                continue;
            }
        }
        
        // 3. 准备读取操作
        off_t offset = (off_t)frame_idx * frame_size_;
        io_uring_prep_read(sqe, video_fd_, buffer->data(), frame_size_, offset);
        
        // 4. 设置用户数据
        ReadRequest* req = new ReadRequest{
            buffer, 
            frame_idx, 
            manager,
            std::chrono::high_resolution_clock::now()
        };
        io_uring_sqe_set_data(sqe, req);
        
        submitted++;
    }
    
    // 5. 提交所有请求
    if (submitted > 0) {
        int ret = io_uring_submit(&ring_);
        if (ret < 0) {
            printf("⚠️  io_uring_submit failed: %s\n", strerror(-ret));
            return 0;
        }
    }
    
    return submitted;
}

// ============ 收割完成的I/O请求 ============

int IoUringVideoReader::harvestCompletions(BufferManager* manager, bool blocking) {
    struct io_uring_cqe *cqe;
    int completed = 0;
    
    // 循环处理所有已完成的请求
    while (true) {
        int ret;
        
        if (blocking && completed == 0) {
            // 阻塞等待至少一个完成
            ret = io_uring_wait_cqe(&ring_, &cqe);
        } else {
            // 非阻塞检查
            ret = io_uring_peek_cqe(&ring_, &cqe);
        }
        
        if (ret < 0) {
            if (ret == -EAGAIN) {
                // 没有更多完成的请求
                break;
            }
            printf("⚠️  io_uring_wait_cqe failed: %s\n", strerror(-ret));
            break;
        }
        
        // 获取请求信息
        ReadRequest* req = (ReadRequest*)io_uring_cqe_get_data(cqe);
        if (!req) {
            io_uring_cqe_seen(&ring_, cqe);
            continue;
        }
        
        // 计算延迟
        auto end_time = std::chrono::high_resolution_clock::now();
        auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
            end_time - req->start_time).count();
        
        // 更新统计
        total_reads_++;
        total_latency_us_ += latency_us;
        
        // 检查I/O结果
        if (cqe->res < 0) {
            // 读取失败
            printf("⚠️  Read failed for frame %d: %s\n", 
                   req->frame_index, strerror(-cqe->res));
            failed_reads_++;
            manager->recycleBuffer(req->buffer);
        } else if (cqe->res != (int)frame_size_) {
            // 部分读取
            printf("⚠️  Partial read for frame %d: %d/%zu bytes\n",
                   req->frame_index, cqe->res, frame_size_);
            failed_reads_++;
            manager->recycleBuffer(req->buffer);
        } else {
            // 读取成功
            successful_reads_++;
            total_bytes_ += cqe->res;
            
            // 诊断：打印严重延迟（超过1秒）
            if (latency_us > 1000000) {  // 超过1秒
                printf("⚠️  [Thread] Frame %d I/O took %.2f ms (SLOW!)\n", 
                       req->frame_index, latency_us / 1000.0);
            }
            
            manager->submitFilledBuffer(req->buffer);
        }
        
        // 标记CQE已处理
        io_uring_cqe_seen(&ring_, cqe);
        delete req;
        completed++;
    }
    
    return completed;
}

// ============ 异步生产者线程 ============

void IoUringVideoReader::asyncProducerThread(int thread_id,
                                            BufferManager* manager,
                                            const std::vector<int>& frame_indices,
                                            std::atomic<bool>& running,
                                            bool loop) {
    printf("🚀 Thread #%d: Starting async producer (frames=%zu, loop=%s)\n",
           thread_id, frame_indices.size(), loop ? "yes" : "no");
    
    // 单线程顺序I/O模式：根据存储速度调整并发度
    // 慢速存储（网络/慢盘）需要更保守的参数，避免队列堆积
    const int BATCH_SIZE = 4;  
    const int MAX_IN_FLIGHT = 8;  // 慢速存储：降低并发，避免延迟累积
    
    std::vector<int> batch;
    batch.reserve(BATCH_SIZE);
    
    size_t frame_idx = 0;
    int frames_submitted = 0;
    int frames_completed = 0;
    
    while (running) {
        
        // 1. 先收割已完成的I/O（关键：先收割再提交！）
        int completed = harvestCompletions(manager, false);
        frames_completed += completed;
        
        // 2. 计算当前飞行中的请求数量
        int in_flight = frames_submitted - frames_completed;
        
        // 3. 如果飞行中的请求太多，等待一些完成
        if (in_flight >= MAX_IN_FLIGHT) {
            // 积极收割，直到飞行中的数量降下来
            while (in_flight >= MAX_IN_FLIGHT && running) {
                completed = harvestCompletions(manager, false);
                frames_completed += completed;
                in_flight = frames_submitted - frames_completed;
                
                if (completed == 0) {
                    // 没有完成的，短暂休眠
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            continue;  // 重新开始循环
        }
        
        // 4. 准备一小批帧索引
        batch.clear();
        for (int i = 0; i < BATCH_SIZE && running; i++) {
            if (frame_idx >= frame_indices.size()) {
                if (loop) {
                    frame_idx = 0;  // 循环
                } else {
                    break;  // 完成
                }
            }
            batch.push_back(frame_indices[frame_idx]);
            frame_idx++;
        }
        
        // 5. 如果没有帧了且不循环，退出
        if (batch.empty()) {
            break;
        }
        
        // 6. 提交批量读取
        int submitted = submitReadBatch(manager, batch);
        frames_submitted += submitted;
        
        // 7. 立即再次收割（提交后可能有些已经完成了）
        completed = harvestCompletions(manager, false);
        frames_completed += completed;
        
        // 8. 如果提交失败（没有空闲buffer），积极收割
        if (submitted == 0) {
            for (int retry = 0; retry < 5 && running; retry++) {
                completed = harvestCompletions(manager, false);
                frames_completed += completed;
                if (completed > 0) {
                    break;  // 收割到了一些，下次循环可以继续提交
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }
    
    // 处理所有剩余的请求
    printf("🔄 Thread #%d: Processing remaining requests...\n", thread_id);
    while (frames_completed < frames_submitted && running) {
        int completed = harvestCompletions(manager, false);  // 改为非阻塞
        frames_completed += completed;
        if (completed == 0) {
            // 没有完成的请求，短暂休眠后继续检查
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    int in_flight = frames_submitted - frames_completed;
    printf("✅ Thread #%d: Completed (submitted=%d, completed=%d, in_flight=%d)\n",
           thread_id, frames_submitted, frames_completed, in_flight);
}

// ============ 统计信息 ============

IoUringVideoReader::Stats IoUringVideoReader::getStats() const {
    Stats stats;
    stats.total_reads = total_reads_.load();
    stats.successful_reads = successful_reads_.load();
    stats.failed_reads = failed_reads_.load();
    stats.total_bytes = total_bytes_.load();
    
    long total_latency = total_latency_us_.load();
    long total = stats.total_reads;
    stats.avg_latency_us = (total > 0) ? ((double)total_latency / total) : 0.0;
    
    return stats;
}

void IoUringVideoReader::resetStats() {
    total_reads_ = 0;
    successful_reads_ = 0;
    failed_reads_ = 0;
    total_bytes_ = 0;
    total_latency_us_ = 0;
}


