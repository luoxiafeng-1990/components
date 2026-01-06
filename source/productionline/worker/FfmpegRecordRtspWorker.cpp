#include "productionline/worker/FfmpegRecordRtspWorker.hpp"
#include "productionline/worker/RtspPacketSource.hpp"
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
    : WorkerBase(BufferAllocatorFactory::AllocatorType::AVFRAME)  // ⭐ 使用 AVFrameAllocator（支持 AVPacket）
    , packet_source_(nullptr)
    , is_open_(false)
    , packet_count_(0)
{
    LOG_DEBUG("[Worker] FfmpegRecordRtspWorker created (using AVFrameAllocator, v2.12 数据源抽象)");
}

FfmpegRecordRtspWorker::FfmpegRecordRtspWorker(const WorkerConfig& config)
    : WorkerBase(BufferAllocatorFactory::AllocatorType::AVFRAME, config)  // ⭐ 使用 AVFrameAllocator（支持 AVPacket）
    , packet_source_(nullptr)
    , is_open_(false)
    , packet_count_(0)
{
    LOG_DEBUG("[Worker] FfmpegRecordRtspWorker created (with config, using AVFrameAllocator, v2.12 数据源抽象)");
}

FfmpegRecordRtspWorker::~FfmpegRecordRtspWorker() {
    LOG_DEBUG("🧹 Destroying FfmpegRecordRtspWorker...");
    close();
}

// ============ IVideoReader 接口实现 ============
bool FfmpegRecordRtspWorker::open() {
    // ✅ 从 worker_config_ 读取 RTSP URL
    const char* rtsp_url = worker_config_.file.file_path.c_str();
    
    if (!rtsp_url || strlen(rtsp_url) == 0) {
        setError("RTSP URL not configured in worker_config_.file.file_path");
        LOG_ERROR("[Worker] ❌ Please configure RTSP URL in WorkerConfig");
        return false;
    }
    
    // 调用带参数的 open(path)
    return open(rtsp_url);
}


bool FfmpegRecordRtspWorker::open(const char* path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (packet_source_ && packet_source_->isOpen()) {
        LOG_WARN("[Worker] ⚠️  Stream already open, closing previous stream");
        close();
    }
    
    LOG_INFO("");
    LOG_INFO_FMT("📡 Opening RTSP stream for recording: %s", path);
    
    // ============ v2.12：创建数据源 ============
    packet_source_ = std::make_unique<RtspPacketSource>(std::string(path));
    
    if (!packet_source_->open()) {
        setError("Failed to open RTSP stream via RtspPacketSource");
        packet_source_.reset();
        return false;
    }
    
    // 获取编解码器参数，计算最大包大小
    const AVCodecParameters* codecpar = packet_source_->getCodecParameters();
    if (!codecpar) {
        setError("Failed to get codec parameters from packet source");
        packet_source_->close();
        packet_source_.reset();
        return false;
    }
    
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
        packet_source_->close();
        packet_source_.reset();
        return false;
    }
    
    // v2.3 新设计：注册为 Packet 缓冲池
    if (!registerBufferPool(BufferPoolType::PACKET_VIDEO, pool_id)) {
        setError("Failed to register BufferPool");
        packet_source_->close();
        packet_source_.reset();
        return false;
    }
    
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    std::string pool_name = pool ? pool->getName() : "Unknown";
    
    is_open_ = true;
    packet_count_ = 0;
    
    LOG_DEBUG("[Worker] ✅ RTSP stream opened for recording (v2.12 数据源抽象)");
    LOG_DEBUG_FMT("[Worker]    Codec: %s", avcodec_get_name(codecpar->codec_id));
    LOG_DEBUG_FMT("[Worker]    Resolution: %dx%d", codecpar->width, codecpar->height);
    LOG_DEBUG_FMT("[Worker]    BufferPool: '%s' (ID: %lu, %d buffers, %zu bytes each)", 
           pool_name.c_str(), pool_id, buffer_count, max_packet_size);
    
    return true;
}


void FfmpegRecordRtspWorker::close() {
    if (!is_open_) {
        return;
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    LOG_INFO("");
    LOG_INFO("🛑 Closing RTSP recording stream...");
    
    // 清理 BufferPool
    clearAllBufferPools();
    
    // ============ v2.12：关闭数据源 ============
    if (packet_source_) {
        packet_source_->close();
        packet_source_.reset();
    }
    
    is_open_ = false;
    
    LOG_DEBUG("[Worker] ✅ RTSP recording stream closed (v2.12 数据源抽象)");
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
    if (!packet_source_) return 0;
    const AVCodecParameters* codecpar = packet_source_->getCodecParameters();
    return codecpar ? codecpar->width : 0;
}

int FfmpegRecordRtspWorker::getHeight() const {
    if (!packet_source_) return 0;
    const AVCodecParameters* codecpar = packet_source_->getCodecParameters();
    return codecpar ? codecpar->height : 0;
}

double FfmpegRecordRtspWorker::getBytesPerPixel() const {
    return 0.0;  // 原始码流没有像素概念
}

const char* FfmpegRecordRtspWorker::getPath() const {
    if (!packet_source_) return "";
    return packet_source_->getFilePath().c_str();
}

bool FfmpegRecordRtspWorker::hasMoreFrames() const {
    return packet_source_ && packet_source_->isOpen() && !packet_source_->isEof();
}

bool FfmpegRecordRtspWorker::isAtEnd() const {
    return !packet_source_ || packet_source_->isEof();
}

// ============ WorkerBase 接口实现 ============

bool FfmpegRecordRtspWorker::fillBuffer(int frame_index, Buffer* buffer) {
    (void)frame_index;
    
    if (!buffer) {
        LOG_ERROR("[Worker] ERROR: buffer is nullptr");
        return false;
    }
    
    if (!is_open_ || !packet_source_) {
        LOG_ERROR("[Worker] ERROR: Worker is not open or packet source is null");
        return false;
    }
    
    // 1. ⭐ 从 Buffer 获取预分配的 AVPacket（AVFrameAllocator 已经分配）
    AVPacket* packet = buffer->getAVPacket();
    if (!packet) {
        LOG_ERROR("[Worker] ERROR: Buffer has no AVPacket (AVFrameAllocator not used?)");
        return false;
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 2. ============ v2.12：通过数据源读取 AVPacket ============
    int ret = packet_source_->readPacket(packet);
    
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            LOG_DEBUG("🔄 RTSP EOF reached (via packet source)");
            return false;
        } else {
            char err_buf[128];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_ERROR_FMT("[Worker] ERROR: packet_source_->readPacket() failed: %d (%s)", ret, err_buf);
            return false;
        }
    }
    
    // 3. 检查是否是视频流（数据源已过滤，但保险起见再检查一次）
    int video_stream_index = packet_source_->getVideoStreamIndex();
    if (packet->stream_index != video_stream_index) {
        av_packet_unref(packet);  // 清空非视频流的数据
        return false;  // 不是视频流，让调用者继续读取
    }
    
    // 4. ⭐ 更新 Buffer 的地址指向 AVPacket 的数据（零拷贝）
    //    注意：packet->data 是 FFmpeg 内部管理的内存，Buffer 只是引用
    buffer->setVirtualAddress(packet->data);
    buffer->setUsedSize(packet->size);
    
    // 5. ⭐ 所有元数据（pts, dts, flags）已在 AVPacket 中，无需额外处理
    //    BufferWriter 可以直接通过 buffer->getAVPacket() 获取完整信息
    
    // 记录包信息（用于调试）
    packet_count_++;
    
    return true;
}

// ============ v2.12：内部实现（已简化，数据源抽象处理） ============

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

// ============ v2.12：编解码器信息获取（通过数据源） ============

const AVCodecParameters* FfmpegRecordRtspWorker::getCodecParameters() const {
    if (!packet_source_) {
        return nullptr;
    }
    return packet_source_->getCodecParameters();
}

AVRational FfmpegRecordRtspWorker::getTimeBase() const {
    if (!packet_source_) {
        return {1, 25};  // 默认25fps
    }
    
    // 从数据源获取编解码器参数，进而获取时间基
    // 注意：RtspPacketSource 没有直接提供 getTimeBase() 方法
    // 我们需要通过 AVCodecParameters 间接获取
    // 实际上，时间基通常在 AVStream 中，这里我们保持原有的默认值
    return {1, 25};  // 默认25fps（TODO: 如需精确时间基，需要扩展 IPacketSource 接口）
}

