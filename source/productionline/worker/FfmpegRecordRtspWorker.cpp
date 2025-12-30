#include "productionline/worker/FfmpegRecordRtspWorker.hpp"
#include "common/Logger.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include <climits>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

// ============ 构造/析构 ============

FfmpegRecordRtspWorker::FfmpegRecordRtspWorker()
    : WorkerBase(BufferAllocatorFactory::AllocatorType::NORMAL)
    , format_ctx_ptr_(nullptr)
    , video_stream_index_(-1)
    , is_open_(false)
    , connected_(false)
    , eof_reached_(false)
    , packet_count_(0)
{
    LOG_DEBUG("[Worker] FfmpegRecordRtspWorker created");
}

FfmpegRecordRtspWorker::FfmpegRecordRtspWorker(const WorkerConfig& config)
    : WorkerBase(BufferAllocatorFactory::AllocatorType::NORMAL, config)
    , format_ctx_ptr_(nullptr)
    , video_stream_index_(-1)
    , is_open_(false)
    , connected_(false)
    , eof_reached_(false)
    , packet_count_(0)
{
    LOG_DEBUG("[Worker] FfmpegRecordRtspWorker created (with config)");
}

FfmpegRecordRtspWorker::~FfmpegRecordRtspWorker() {
    LOG_DEBUG("🧹 Destroying FfmpegRecordRtspWorker...");
    close();
}

// ============ IVideoReader 接口实现 ============

bool FfmpegRecordRtspWorker::open(const char* path) {
    if (is_open_) {
        LOG_WARN("[Worker] Stream already open, closing previous stream");
        close();
    }
    
    rtsp_url_ = path;
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    LOG_INFO("");
    LOG_INFO_FMT("📡 Opening RTSP stream for recording: %s", rtsp_url_.c_str());
    
    // 连接RTSP流
    if (!openMediaSource()) {
        return false;
    }
    
    // 获取视频流信息，计算最大包大小
    AVCodecParameters* codecpar = format_ctx_ptr_->streams[video_stream_index_]->codecpar;
    
    // 估算最大包大小：对于H.264/H.265，通常单个包不超过 1MB
    // 保守估计：宽×高×1.5（考虑I帧可能很大）
    size_t max_packet_size = codecpar->width * codecpar->height * 3 / 2;
    if (max_packet_size < 256 * 1024) {
        max_packet_size = 256 * 1024;  // 最小 256KB
    }
    
    int buffer_count = 64;  // RTSP录制建议更多Buffer
    
    // 创建 BufferPool
    uint64_t pool_id = allocator_facade_.allocatePoolWithBuffers(
        buffer_count,
        max_packet_size,
        std::string("FfmpegRecordRtspWorker_") + std::string(path),
        "RTSP_RECORD"
    );
    
    if (pool_id == 0) {
        setError("Failed to create BufferPool via Allocator");
        closeMediaSource();
        return false;
    }
    
    // v2.0 新设计：注册为 Packet 缓冲池
    if (!registerBufferPool(BufferPoolType::PACKET_VIDEO, pool_id)) {
        setError("Failed to register BufferPool");
        closeMediaSource();
        return false;
    }
    
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    std::string pool_name = pool ? pool->getName() : "Unknown";
    
    is_open_ = true;
    eof_reached_ = false;
    packet_count_ = 0;
    
    LOG_DEBUG("[Worker] RTSP stream opened for recording");
    LOG_DEBUG_FMT("[Worker]    Codec: %s", avcodec_get_name(codecpar->codec_id));
    LOG_DEBUG_FMT("[Worker]    Resolution: %dx%d", codecpar->width, codecpar->height);
    LOG_DEBUG_FMT("[Worker]    BufferPool: '%s' (ID: %lu, %d buffers, %zu bytes each)", 
           pool_name.c_str(), pool_id, buffer_count, max_packet_size);
    
    return true;
}

bool FfmpegRecordRtspWorker::open(const char* path, int width, int height, int bits_per_pixel) {
    // 录制不需要这些参数，直接调用简单版本
    (void)width;
    (void)height;
    (void)bits_per_pixel;
    return open(path);
}

void FfmpegRecordRtspWorker::close() {
    if (!is_open_) {
        return;
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    LOG_INFO("");
    LOG_INFO("🛑 Closing RTSP recording stream...");
    
    clearAllBufferPools();
    closeMediaSource();
    
    is_open_ = false;
    connected_ = false;
    
    LOG_DEBUG("[Worker] RTSP recording stream closed");
    LOG_INFO_FMT("   Recorded packets: %d", packet_count_.load());
}

bool FfmpegRecordRtspWorker::isOpen() const {
    return is_open_;
}

bool FfmpegRecordRtspWorker::seek(int frame_index) {
    (void)frame_index;
    LOG_WARN("[Worker] RTSP stream does not support seeking");
    return false;
}

bool FfmpegRecordRtspWorker::seekToBegin() {
    LOG_WARN("[Worker] RTSP stream does not support seeking");
    return false;
}

bool FfmpegRecordRtspWorker::seekToEnd() {
    LOG_WARN("[Worker] RTSP stream does not support seeking");
    return false;
}

bool FfmpegRecordRtspWorker::skip(int frame_count) {
    (void)frame_count;
    LOG_WARN("[Worker] RTSP stream does not support skipping");
    return false;
}

int FfmpegRecordRtspWorker::getTotalFrames() const {
    return INT_MAX;
}

int FfmpegRecordRtspWorker::getCurrentFrameIndex() const {
    return packet_count_.load();
}

size_t FfmpegRecordRtspWorker::getFrameSize() const {
    // 返回Buffer大小（最大包大小）
    uint64_t pool_id = getOutputBufferPoolId(BufferPoolType::PACKET_VIDEO);
    if (pool_id == 0) return 0;
    
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    if (!pool) return 0;
    
    Buffer* buffer = pool->acquireFree(false, 0);
    if (!buffer) return 0;
    
    size_t size = buffer->size();
    pool->releaseFree(buffer);
    return size;
}

long FfmpegRecordRtspWorker::getFileSize() const {
    return -1;
}

int FfmpegRecordRtspWorker::getWidth() const {
    if (!format_ctx_ptr_ || video_stream_index_ < 0) return 0;
    return format_ctx_ptr_->streams[video_stream_index_]->codecpar->width;
}

int FfmpegRecordRtspWorker::getHeight() const {
    if (!format_ctx_ptr_ || video_stream_index_ < 0) return 0;
    return format_ctx_ptr_->streams[video_stream_index_]->codecpar->height;
}

int FfmpegRecordRtspWorker::getBytesPerPixel() const {
    return 0;  // 原始码流没有像素概念
}

const char* FfmpegRecordRtspWorker::getPath() const {
    return rtsp_url_.c_str();
}

bool FfmpegRecordRtspWorker::hasMoreFrames() const {
    return connected_.load() && !eof_reached_.load();
}

bool FfmpegRecordRtspWorker::isAtEnd() const {
    return eof_reached_.load();
}

// ============ WorkerBase 接口实现 ============

bool FfmpegRecordRtspWorker::fillBuffer(int frame_index, Buffer* buffer) {
    (void)frame_index;
    
    if (!buffer) {
        LOG_ERROR("[Worker] ERROR: buffer is nullptr");
        return false;
    }
    
    if (!is_open_) {
        LOG_ERROR("[Worker] ERROR: Worker is not open");
        return false;
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // ⭐ 录制Worker自己分配AVPacket（不依赖Buffer中的AVPacket*）
    AVPacket* packet_ptr = av_packet_alloc();
    if (!packet_ptr) {
        LOG_ERROR("[Worker] ERROR: Failed to allocate AVPacket");
        return false;
    }
    
    // 读取一个 packet
    int ret = av_read_frame(format_ctx_ptr_, packet_ptr);
    
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            LOG_DEBUG("🔄 RTSP EOF reached");
            av_packet_free(&packet_ptr);
            eof_reached_ = true;
            return false;
        } else {
            char err_buf[128];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_ERROR_FMT("[Worker] ERROR: av_read_frame failed: %d (%s)", ret, err_buf);
            av_packet_free(&packet_ptr);
            return false;
        }
    }
    
    // 检查是否是视频流
    if (packet_ptr->stream_index != video_stream_index_) {
        av_packet_free(&packet_ptr);
        return false;  // 不是视频流，让调用者继续读取
    }
    
    // 检查包大小是否超过Buffer容量
    if ((size_t)packet_ptr->size > buffer->size()) {
        LOG_ERROR_FMT("[Worker] ERROR: Packet size (%d) exceeds buffer size (%zu)", 
                      packet_ptr->size, buffer->size());
        av_packet_free(&packet_ptr);
        return false;
    }
    
    // 将 AVPacket 数据拷贝到 Buffer
    memcpy(buffer->getVirtualAddress(), packet_ptr->data, packet_ptr->size);
    
    // 更新 Buffer 的实际使用大小
    buffer->setUsedSize(packet_ptr->size);
    
    // 释放 AVPacket（数据已拷贝）
    av_packet_free(&packet_ptr);
    
    // 记录包信息（用于调试）
    packet_count_++;
    
    return true;
}

// ============ 内部实现 ============

bool FfmpegRecordRtspWorker::openMediaSource() {
    format_ctx_ptr_ = avformat_alloc_context();
    if (!format_ctx_ptr_) {
        setError("Failed to allocate AVFormatContext");
        return false;
    }
    
    // 设置RTSP选项
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "stimeout", "5000000", 0);
    av_dict_set(&options, "max_delay", "500000", 0);
    
    int ret = avformat_open_input(&format_ctx_ptr_, rtsp_url_.c_str(), nullptr, &options);
    av_dict_free(&options);
    
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        setError(std::string("Failed to open RTSP stream: ") + errbuf);
        avformat_free_context(format_ctx_ptr_);
        format_ctx_ptr_ = nullptr;
        return false;
    }
    
    ret = avformat_find_stream_info(format_ctx_ptr_, nullptr);
    if (ret < 0) {
        setError("Failed to find stream information");
        avformat_close_input(&format_ctx_ptr_);
        return false;
    }
    
    if (!findVideoStream()) {
        avformat_close_input(&format_ctx_ptr_);
        return false;
    }
    
    connected_ = true;
    
    LOG_DEBUG("[Worker] Opened RTSP media source");
    LOG_INFO_FMT("   Codec: %s", 
                 avcodec_get_name(format_ctx_ptr_->streams[video_stream_index_]->codecpar->codec_id));
    LOG_INFO_FMT("   Resolution: %dx%d", 
                 format_ctx_ptr_->streams[video_stream_index_]->codecpar->width,
                 format_ctx_ptr_->streams[video_stream_index_]->codecpar->height);
    
    return true;
}

void FfmpegRecordRtspWorker::closeMediaSource() {
    if (format_ctx_ptr_) {
        avformat_close_input(&format_ctx_ptr_);
        format_ctx_ptr_ = nullptr;
    }
    
    video_stream_index_ = -1;
    connected_ = false;
}

bool FfmpegRecordRtspWorker::findVideoStream() {
    video_stream_index_ = -1;
    
    for (unsigned int i = 0; i < format_ctx_ptr_->nb_streams; i++) {
        if (format_ctx_ptr_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = (int)i;
            break;
        }
    }
    
    if (video_stream_index_ == -1) {
        setError("No video stream found in RTSP source");
        return false;
    }
    
    return true;
}

void FfmpegRecordRtspWorker::setError(const std::string& error, int ffmpeg_error) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = error;
    
    if (ffmpeg_error != 0) {
        char err_buf[128];
        av_strerror(ffmpeg_error, err_buf, sizeof(err_buf));
        LOG_ERROR_FMT("[Worker] FfmpegRecordRtspWorker Error: %s (FFmpeg: %s)", error.c_str(), err_buf);
    } else {
        LOG_ERROR_FMT("[Worker] FfmpegRecordRtspWorker Error: %s", error.c_str());
    }
}

std::string FfmpegRecordRtspWorker::getLastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

