#include "productionline/worker/implementation/IoUringRawVideoFileWorker.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

// ============ 构造/析构 ============

IoUringRawVideoFileWorker::IoUringRawVideoFileWorker(int queue_depth)
    : queue_depth_(queue_depth)
    , initialized_(false)
    , video_fd_(-1)
    , frame_size_(0)
    , file_size_(0)
    , total_frames_(0)
    , current_frame_index_(0)
    , width_(0)
    , height_(0)
    , bits_per_pixel_(0)
    , is_open_(false)
{
    // io_uring 延迟初始化，在 open() 时初始化
}

IoUringRawVideoFileWorker::~IoUringRawVideoFileWorker() {
    close();
}

// ============ IVideoReader 接口实现 ============

bool IoUringRawVideoFileWorker::open(const char* path) {
    printf("❌ ERROR: IoUringVideoReader does not support auto-detect format\n");
    printf("   Please use open(path, width, height, bits_per_pixel) for raw video files\n");
    return false;
}

bool IoUringRawVideoFileWorker::open(const char* path, int width, int height, int bits_per_pixel) {
    if (is_open_) {
        printf("⚠️  Warning: File already opened, closing previous file\n");
        close();
    }
    
    if (width <= 0 || height <= 0 || bits_per_pixel <= 0) {
        printf("❌ ERROR: Invalid parameters\n");
        return false;
    }
    
    video_path_ = path;
    width_ = width;
    height_ = height;
    bits_per_pixel_ = bits_per_pixel;
    
    frame_size_ = (size_t)width * height * (bits_per_pixel / 8);
    
    printf("📂 Opening raw video file: %s\n", path);
    printf("   Format: %dx%d, %d bits per pixel\n", width, height, bits_per_pixel);
    printf("   Frame size: %zu bytes\n", frame_size_);
    printf("   Reader: IoUringVideoReader (async I/O)\n");
    printf("   Queue depth: %d\n", queue_depth_);
    
    // 打开文件
    video_fd_ = ::open(path, O_RDONLY);
    if (video_fd_ < 0) {
        printf("❌ ERROR: Cannot open file: %s\n", strerror(errno));
        return false;
    }
    
    // 获取文件大小
    struct stat st;
    if (fstat(video_fd_, &st) < 0) {
        printf("❌ ERROR: Cannot get file size: %s\n", strerror(errno));
        ::close(video_fd_);
        video_fd_ = -1;
        return false;
    }
    
    file_size_ = st.st_size;
    total_frames_ = file_size_ / frame_size_;
    
    if (total_frames_ == 0) {
        printf("❌ ERROR: File too small\n");
        ::close(video_fd_);
        video_fd_ = -1;
        return false;
    }
    
    // 初始化 io_uring
    int ret = io_uring_queue_init(queue_depth_, &ring_, 0);
    if (ret < 0) {
        printf("❌ ERROR: io_uring_queue_init failed: %s\n", strerror(-ret));
        ::close(video_fd_);
        video_fd_ = -1;
        return false;
    }
    
    initialized_ = true;
    is_open_ = true;
    current_frame_index_ = 0;
    
    printf("✅ Raw video file opened successfully\n");
    printf("   File size: %ld bytes\n", file_size_);
    printf("   Total frames: %d\n", total_frames_);
    
    return true;
}

void IoUringRawVideoFileWorker::close() {
    if (!is_open_) {
        return;
    }
    
    if (initialized_) {
        io_uring_queue_exit(&ring_);
        initialized_ = false;
    }
    
    if (video_fd_ >= 0) {
        ::close(video_fd_);
        video_fd_ = -1;
    }
    
    is_open_ = false;
    current_frame_index_ = 0;
    
    printf("✅ Video file closed: %s\n", video_path_.c_str());
}

bool IoUringRawVideoFileWorker::isOpen() const {
    return is_open_;
}

bool IoUringRawVideoFileWorker::seek(int frame_index) {
    if (!is_open_) {
        return false;
    }
    
    if (frame_index < 0 || frame_index >= total_frames_) {
        return false;
    }
    
    current_frame_index_ = frame_index;
    return true;
}

bool IoUringRawVideoFileWorker::seekToBegin() {
    return seek(0);
}

bool IoUringRawVideoFileWorker::seekToEnd() {
    if (!is_open_) {
        return false;
    }
    
    current_frame_index_ = total_frames_;
    return true;
}

bool IoUringRawVideoFileWorker::skip(int frame_count) {
    int target_frame = current_frame_index_ + frame_count;
    return seek(target_frame);
}

int IoUringRawVideoFileWorker::getTotalFrames() const {
    return total_frames_;
}

int IoUringRawVideoFileWorker::getCurrentFrameIndex() const {
    return current_frame_index_;
}

size_t IoUringRawVideoFileWorker::getFrameSize() const {
    return frame_size_;
}

long IoUringRawVideoFileWorker::getFileSize() const {
    return file_size_;
}

int IoUringRawVideoFileWorker::getWidth() const {
    return width_;
}

int IoUringRawVideoFileWorker::getHeight() const {
    return height_;
}

int IoUringRawVideoFileWorker::getBytesPerPixel() const {
    return (bits_per_pixel_ + 7) / 8;
}

const char* IoUringRawVideoFileWorker::getPath() const {
    return video_path_.c_str();
}

bool IoUringRawVideoFileWorker::hasMoreFrames() const {
    return current_frame_index_ < total_frames_;
}

bool IoUringRawVideoFileWorker::isAtEnd() const {
    return current_frame_index_ >= total_frames_;
}

// ============================================================================
// 核心功能：填充Buffer
// ============================================================================

bool IoUringRawVideoFileWorker::fillBuffer(int frame_index, Buffer* buffer) {
    if (!buffer || !buffer->data()) {
        printf("❌ ERROR: Invalid buffer\n");
        return false;
    }
    
    if (!is_open_) {
        printf("❌ ERROR: Worker is not open\n");
        return false;
    }
    
    if (frame_index < 0 || frame_index >= total_frames_) {
        printf("❌ ERROR: Invalid frame index %d (valid: 0-%d)\n",
               frame_index, total_frames_ - 1);
        return false;
    }
    
    if (buffer->size() < frame_size_) {
        printf("❌ ERROR: Buffer too small (need %zu, got %zu)\n", 
               frame_size_, buffer->size());
        return false;
    }
    
    // 使用io_uring异步读取
    if (!submitReadRequest(frame_index, buffer->data(), buffer->size())) {
        return false;
    }
    
    // 等待完成
    return waitForCompletion();
}

// ============ IoUring 专有接口（保留原有功能）TODO: 需要重新实现 ============

void IoUringRawVideoFileWorker::asyncProducerThread(int thread_id,
                                            BufferPool* pool,
                                            const std::vector<int>& frame_indices,
                                            std::atomic<bool>& running,
                                            bool loop) {
    printf("⚠️  Warning: asyncProducerThread not yet re-implemented\n");
}

int IoUringRawVideoFileWorker::submitBatchReads(BufferPool* pool, 
                                       const std::vector<int>& frame_indices) {
    printf("⚠️  Warning: submitBatchReads not yet re-implemented\n");
    return 0;
}

// ============ 内部辅助方法实现 ============

bool IoUringRawVideoFileWorker::submitReadRequest(int frame_index, void* buffer, size_t buffer_size) {
    if (!initialized_ || video_fd_ < 0) {
        printf("❌ ERROR: IoUring not initialized or file not open\n");
        return false;
    }
    
    // 计算文件偏移量
    off_t offset = static_cast<off_t>(frame_index) * frame_size_;
    
    // 获取 SQE（Submission Queue Entry）
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        printf("❌ ERROR: Failed to get SQE from io_uring\n");
        return false;
    }
    
    // 准备读取请求
    io_uring_prep_read(sqe, video_fd_, buffer, frame_size_, offset);
    io_uring_sqe_set_data(sqe, buffer);  // 设置用户数据
    
    // 提交请求
    int ret = io_uring_submit(&ring_);
    if (ret < 0) {
        printf("❌ ERROR: io_uring_submit failed: %s\n", strerror(-ret));
        return false;
    }
    
    return true;
}

bool IoUringRawVideoFileWorker::waitForCompletion() {
    if (!initialized_) {
        printf("❌ ERROR: IoUring not initialized\n");
        return false;
    }
    
    // 等待完成事件
    struct io_uring_cqe* cqe;
    int ret = io_uring_wait_cqe(&ring_, &cqe);
    if (ret < 0) {
        printf("❌ ERROR: io_uring_wait_cqe failed: %s\n", strerror(-ret));
        return false;
    }
    
    // 检查读取结果
    if (cqe->res < 0) {
        printf("❌ ERROR: Read failed: %s\n", strerror(-cqe->res));
        io_uring_cqe_seen(&ring_, cqe);
        return false;
    }
    
    if ((size_t)cqe->res != frame_size_) {
        printf("❌ ERROR: Incomplete read: got %d bytes, expected %zu\n", 
               cqe->res, frame_size_);
        io_uring_cqe_seen(&ring_, cqe);
        return false;
    }
    
    // 标记CQE已处理
    io_uring_cqe_seen(&ring_, cqe);
    
    return true;
}
