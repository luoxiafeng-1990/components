#include "../../include/videoFile/IoUringVideoReader.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

// ============ 构造/析构 ============

IoUringVideoReader::IoUringVideoReader(int queue_depth)
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
    // io_uring 延迟初始化，在 open/openRaw 时初始化
}

IoUringVideoReader::~IoUringVideoReader() {
    close();
}

// ============ IVideoReader 接口实现 ============

bool IoUringVideoReader::open(const char* path) {
    printf("❌ ERROR: IoUringVideoReader does not support auto-detect format\n");
    printf("   Please use openRaw() for raw video files\n");
    return false;
}

bool IoUringVideoReader::openRaw(const char* path, int width, int height, int bits_per_pixel) {
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

void IoUringVideoReader::close() {
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

bool IoUringVideoReader::isOpen() const {
    return is_open_;
}

bool IoUringVideoReader::readFrameTo(Buffer& dest_buffer) {
    return readFrameTo(dest_buffer.data(), dest_buffer.size());
}

bool IoUringVideoReader::readFrameTo(void* dest_buffer, size_t buffer_size) {
    if (!is_open_) {
        printf("❌ ERROR: File not opened\n");
        return false;
    }
    
    if (current_frame_index_ >= total_frames_) {
        return false;
    }
    
    return readFrameAt(current_frame_index_++, dest_buffer, buffer_size);
}

bool IoUringVideoReader::readFrameAt(int frame_index, Buffer& dest_buffer) {
    return readFrameAt(frame_index, dest_buffer.data(), dest_buffer.size());
}

bool IoUringVideoReader::readFrameAt(int frame_index, void* dest_buffer, size_t buffer_size) {
    if (!is_open_ || !initialized_) {
        return false;
    }
    
    if (frame_index < 0 || frame_index >= total_frames_) {
        return false;
    }
    
    if (buffer_size < frame_size_) {
        return false;
    }
    
    // 使用 io_uring 异步读取
    off_t offset = (off_t)frame_index * frame_size_;
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        return false;
    }
    
    io_uring_prep_read(sqe, video_fd_, dest_buffer, frame_size_, offset);
    io_uring_sqe_set_data(sqe, dest_buffer);
    
    int ret = io_uring_submit(&ring_);
    if (ret < 0) {
        return false;
    }
    
    struct io_uring_cqe* cqe;
    ret = io_uring_wait_cqe(&ring_, &cqe);
    if (ret < 0) {
        return false;
    }
    
    bool success = (cqe->res == (int)frame_size_);
    io_uring_cqe_seen(&ring_, cqe);
    
    return success;
}

bool IoUringVideoReader::readFrameAtThreadSafe(int frame_index, void* dest_buffer, size_t buffer_size) const {
    // io_uring 的线程安全需要更复杂的处理
    // 简化实现：使用 pread 系统调用
    if (!is_open_) {
        return false;
    }
    
    if (frame_index < 0 || frame_index >= total_frames_) {
        return false;
    }
    
    if (buffer_size < frame_size_) {
        return false;
    }
    
    off_t offset = (off_t)frame_index * frame_size_;
    ssize_t bytes_read = pread(video_fd_, dest_buffer, frame_size_, offset);
    
    return (bytes_read == (ssize_t)frame_size_);
}

bool IoUringVideoReader::seek(int frame_index) {
    if (!is_open_) {
        return false;
    }
    
    if (frame_index < 0 || frame_index >= total_frames_) {
        return false;
    }
    
    current_frame_index_ = frame_index;
    return true;
}

bool IoUringVideoReader::seekToBegin() {
    return seek(0);
}

bool IoUringVideoReader::seekToEnd() {
    if (!is_open_) {
        return false;
    }
    
    current_frame_index_ = total_frames_;
    return true;
}

bool IoUringVideoReader::skip(int frame_count) {
    int target_frame = current_frame_index_ + frame_count;
    return seek(target_frame);
}

int IoUringVideoReader::getTotalFrames() const {
    return total_frames_;
}

int IoUringVideoReader::getCurrentFrameIndex() const {
    return current_frame_index_;
}

size_t IoUringVideoReader::getFrameSize() const {
    return frame_size_;
}

long IoUringVideoReader::getFileSize() const {
    return file_size_;
}

int IoUringVideoReader::getWidth() const {
    return width_;
}

int IoUringVideoReader::getHeight() const {
    return height_;
}

int IoUringVideoReader::getBytesPerPixel() const {
    return (bits_per_pixel_ + 7) / 8;
}

const char* IoUringVideoReader::getPath() const {
    return video_path_.c_str();
}

bool IoUringVideoReader::hasMoreFrames() const {
    return current_frame_index_ < total_frames_;
}

bool IoUringVideoReader::isAtEnd() const {
    return current_frame_index_ >= total_frames_;
}

const char* IoUringVideoReader::getReaderType() const {
    return "IoUringVideoReader";
}

// ============ IoUring 专有接口（保留原有功能）TODO: 需要重新实现 ============

void IoUringVideoReader::asyncProducerThread(int thread_id,
                                            BufferManager* manager,
                                            const std::vector<int>& frame_indices,
                                            std::atomic<bool>& running,
                                            bool loop) {
    printf("⚠️  Warning: asyncProducerThread not yet re-implemented\n");
}

int IoUringVideoReader::submitReadBatch(BufferManager* manager, 
                                       const std::vector<int>& frame_indices) {
    printf("⚠️  Warning: submitReadBatch not yet re-implemented\n");
    return 0;
}

int IoUringVideoReader::harvestCompletions(BufferManager* manager, bool blocking) {
    printf("⚠️  Warning: harvestCompletions not yet re-implemented\n");
    return 0;
}

IoUringVideoReader::Stats IoUringVideoReader::getStats() const {
    Stats stats;
    stats.total_reads = total_reads_.load();
    stats.successful_reads = successful_reads_.load();
    stats.failed_reads = failed_reads_.load();
    stats.total_bytes = total_bytes_.load();
    
    long total_latency = total_latency_us_.load();
    stats.avg_latency_us = (stats.total_reads > 0) 
        ? (double)total_latency / stats.total_reads 
        : 0.0;
    
    return stats;
}

void IoUringVideoReader::resetStats() {
    total_reads_.store(0);
    successful_reads_.store(0);
    failed_reads_.store(0);
    total_bytes_.store(0);
    total_latency_us_.store(0);
}
