#include "productionline/worker/FfmpegPacketRecorderWorker.hpp"
#include "productionline/worker/EncodedPacketSourceFromRtsp.hpp"
#include "productionline/worker/EncodedPacketSourceFromFile.hpp"
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

FfmpegPacketRecorderWorker::FfmpegPacketRecorderWorker()
    : WorkerBase(BufferAllocatorFactory::AllocatorType::AVFRAME)  // ⭐ 使用 AVFrameAllocator（支持 AVPacket）
    , packet_source_(nullptr)
    , is_open_(false)
    , packet_count_(0)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Recorder")))
{
    LOG4CPLUS_DEBUG(logger_, "FfmpegPacketRecorderWorker created (using AVFrameAllocator, v2.13 多数据源支持)");
}

FfmpegPacketRecorderWorker::FfmpegPacketRecorderWorker(const WorkerConfig& config)
    : WorkerBase(BufferAllocatorFactory::AllocatorType::AVFRAME, config)  // ⭐ 使用 AVFrameAllocator（支持 AVPacket）
    , packet_source_(nullptr)
    , is_open_(false)
    , packet_count_(0)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Recorder")))
{
    LOG4CPLUS_DEBUG(logger_, "FfmpegPacketRecorderWorker created (with config, using AVFrameAllocator, v2.13 多数据源支持)");
}

FfmpegPacketRecorderWorker::~FfmpegPacketRecorderWorker() {
    LOG4CPLUS_DEBUG(logger_, "🧹 Destroying FfmpegPacketRecorderWorker...");
    close();
}

// ============ IVideoReader 接口实现 ============
bool FfmpegPacketRecorderWorker::open() {
    // ✅ 从 worker_config_ 读取数据源路径
    const char* path = worker_config_.data_source.path.c_str();
    
    if (!path || strlen(path) == 0) {
        setError("Data source path not configured in worker_config_.data_source.path");
        LOG4CPLUS_ERROR(logger_, "❌ Please configure data source path in WorkerConfig");
        return false;
    }
    
    // 调用带参数的 open(path)
    return open(path);
}


bool FfmpegPacketRecorderWorker::open(const char* path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (packet_source_ && packet_source_->isOpen()) {
        LOG4CPLUS_WARN(logger_, "⚠️  Stream already open, closing previous stream");
        close();
    }
    
    LOG4CPLUS_INFO(logger_, "");
    LOG4CPLUS_INFO_FMT(logger_, "📡 Opening data source for recording: %s", path);
    
    // ============ v2.13：根据路径自动创建数据源 ============
    packet_source_ = createPacketSource(std::string(path));
    
    if (!packet_source_) {
        setError("Failed to create packet source for the given path");
        return false;
    }
    
    if (!packet_source_->open()) {
        setError("Failed to open data source via packet source");
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
    
    // 创建 BufferPool
    uint64_t pool_id = allocator_facade_.allocatePoolWithBuffers(
        worker_config_.data_source.buffer_count,
        max_packet_size,
        std::string("FfmpegPacketRecorderWorker_") + std::string(path),
        "PACKET_RECORD"
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
    
    LOG4CPLUS_DEBUG(logger_, "✅ Data source opened for recording (v2.13 多数据源支持)");
    LOG4CPLUS_DEBUG_FMT(logger_, "   Codec: %s", avcodec_get_name(codecpar->codec_id));
    LOG4CPLUS_DEBUG_FMT(logger_, "   Resolution: %dx%d", codecpar->width, codecpar->height);
    LOG4CPLUS_DEBUG_FMT(logger_, "   BufferPool: '%s' (ID: %lu, %d buffers, %zu bytes each)", 
           pool_name.c_str(), pool_id, worker_config_.data_source.buffer_count, max_packet_size);
    
    return true;
}


void FfmpegPacketRecorderWorker::close() {
    if (!is_open_) {
        return;
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    LOG4CPLUS_INFO(logger_, "");
    LOG4CPLUS_INFO(logger_, "🛑 Closing packet recording stream...");
    
    // 清理 BufferPool
    clearAllBufferPools();
    
    // ============ v2.13：关闭数据源 ============
    if (packet_source_) {
        packet_source_->close();
        packet_source_.reset();
    }
    
    is_open_ = false;
    
    LOG4CPLUS_DEBUG(logger_, "✅ Packet recording stream closed (v2.13 多数据源支持)");
    LOG4CPLUS_INFO_FMT(logger_, "   Recorded packets: %d", packet_count_.load());
}

bool FfmpegPacketRecorderWorker::isOpen() const {
    return is_open_;
}

bool FfmpegPacketRecorderWorker::seek(int frame_index) {
    if (!packet_source_) {
        LOG4CPLUS_WARN(logger_, "Cannot seek: packet source not initialized");
        return false;
    }
    return packet_source_->seek(frame_index);
}

bool FfmpegPacketRecorderWorker::seekToBegin() {
    if (!packet_source_) {
        LOG4CPLUS_WARN(logger_, "Cannot seek: packet source not initialized");
        return false;
    }
    return packet_source_->seek(0);
}

bool FfmpegPacketRecorderWorker::seekToEnd() {
    LOG4CPLUS_WARN(logger_, "Seek to end not supported");
    return false;
}

bool FfmpegPacketRecorderWorker::skip(int frame_count) {
    (void)frame_count;
    LOG4CPLUS_WARN(logger_, "Skip not supported");
    return false;
}

int FfmpegPacketRecorderWorker::getTotalFrames() const {
    if (!packet_source_) return -1;
    return packet_source_->getTotalFrames();
}

int FfmpegPacketRecorderWorker::getCurrentFrameIndex() const {
    return packet_count_.load();
}

size_t FfmpegPacketRecorderWorker::getFrameSize() const {
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

long FfmpegPacketRecorderWorker::getFileSize() const {
    if (!packet_source_) return -1;
    return packet_source_->getFileSize();
}

int FfmpegPacketRecorderWorker::getSourceWidth() const {
    if (!packet_source_) return 0;
    const AVCodecParameters* codecpar = packet_source_->getCodecParameters();
    return codecpar ? codecpar->width : 0;
}

int FfmpegPacketRecorderWorker::getSourceHeight() const {
    if (!packet_source_) return 0;
    const AVCodecParameters* codecpar = packet_source_->getCodecParameters();
    return codecpar ? codecpar->height : 0;
}

int FfmpegPacketRecorderWorker::getOutputWidth() const {
    return getSourceWidth();  // Recorder不处理，输出等于输入
}

int FfmpegPacketRecorderWorker::getOutputHeight() const {
    return getSourceHeight();  // Recorder不处理，输出等于输入
}

double FfmpegPacketRecorderWorker::getOutputBytesPerPixel(int channel) const {
    (void)channel;  // 未使用参数
    return 0.0;  // 原始码流没有像素概念
}

std::string FfmpegPacketRecorderWorker::getPath() const {
    if (!packet_source_) {
        return std::string();
    }
    return packet_source_->getPath();
}

IDataSourceNavigator::SourceType FfmpegPacketRecorderWorker::getDataSourceType() const {
    if (packet_source_) {
        return packet_source_->getDataSourceType();
    }
    return SourceType::NETWORK_SOURCE;  // 默认是网络流类型
}

bool FfmpegPacketRecorderWorker::hasMoreFrames() const {
    return packet_source_ && packet_source_->isOpen() && !packet_source_->isAtEnd();
}

bool FfmpegPacketRecorderWorker::isAtEnd() const {
    return !packet_source_ || packet_source_->isAtEnd();
}

// ============ WorkerBase 接口实现 ============

bool FfmpegPacketRecorderWorker::fillBuffer(int frame_index, Buffer* buffer) {
    (void)frame_index;
    
    if (!buffer) {
        LOG4CPLUS_ERROR(logger_, "ERROR: buffer is nullptr");
        return false;
    }
    
    if (!is_open_ || !packet_source_) {
        LOG4CPLUS_ERROR(logger_, "ERROR: Worker is not open or packet source is null");
        return false;
    }
    
    // 1. ⭐ 从 Buffer 获取预分配的 AVPacket（AVFrameAllocator 已经分配）
    AVPacket* packet = buffer->getAVPacket();
    if (!packet) {
        LOG4CPLUS_ERROR(logger_, "ERROR: Buffer has no AVPacket (AVFrameAllocator not used?)");
        return false;
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 2. ============ v2.13：通过数据源读取 AVPacket ============
    int ret = packet_source_->readEncodedPacket(packet);
    
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            LOG4CPLUS_DEBUG(logger_, "🔄 EOF reached (via packet source)");
            return false;
        } else {
            char err_buf[128];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG4CPLUS_ERROR_FMT(logger_, "ERROR: packet_source_->readEncodedPacket() failed: %d (%s)", ret, err_buf);
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

// ============ v2.13：数据源自动选择逻辑 ============

std::unique_ptr<IEncodedPacketSource> FfmpegPacketRecorderWorker::createPacketSource(const std::string& path) {
    // 根据 URL/路径前缀判断数据源类型
    if (path.find("rtsp://") == 0 || path.find("rtsps://") == 0) {
        LOG4CPLUS_INFO(logger_, "📡 Detected RTSP stream source");
        return std::make_unique<EncodedPacketSourceFromRtsp>(path);
    } 
    else if (path.find("rtmp://") == 0 || path.find("rtmps://") == 0) {
        LOG4CPLUS_INFO(logger_, "📡 Detected RTMP stream source");
        // 未来扩展：return std::make_unique<EncodedPacketSourceFromRtmp>(path);
        LOG4CPLUS_ERROR(logger_, "❌ RTMP protocol not supported yet");
        return nullptr;
    }
    else if (path.find("http://") == 0 || path.find("https://") == 0) {
        LOG4CPLUS_INFO(logger_, "🌐 Detected HTTP/HTTPS stream source (e.g., HLS)");
        // HTTP 流可以用 EncodedPacketSourceFromFile 处理（FFmpeg 原生支持）
        return std::make_unique<EncodedPacketSourceFromFile>(path);
    }
    else {
        LOG4CPLUS_INFO(logger_, "📁 Detected local file source");
        return std::make_unique<EncodedPacketSourceFromFile>(path);
    }
}

// ============ v2.13：内部实现 ============

void FfmpegPacketRecorderWorker::setError(const std::string& error, int ffmpeg_error) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = error;
    
    if (ffmpeg_error != 0) {
        char err_buf[128];
        av_strerror(ffmpeg_error, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "FfmpegPacketRecorderWorker Error: %s (FFmpeg: %s)", error.c_str(), err_buf);
    } else {
        LOG4CPLUS_ERROR_FMT(logger_, "FfmpegPacketRecorderWorker Error: %s", error.c_str());
    }
}

std::string FfmpegPacketRecorderWorker::getLastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

// ============ v2.13：编解码器信息获取（通过数据源） ============

const AVCodecParameters* FfmpegPacketRecorderWorker::getCodecParameters() const {
    if (!packet_source_) {
        return nullptr;
    }
    return packet_source_->getCodecParameters();
}

AVRational FfmpegPacketRecorderWorker::getTimeBase() const {
    if (!packet_source_) {
        return {1, 25};  // 默认25fps
    }
    
    // 从数据源获取编解码器参数，进而获取时间基
    // 注意：需要从 AVStream 获取，这里简化处理返回默认值
    return {1, 25};  // 默认25fps（TODO: 如需精确时间基，需要扩展 IEncodedPacketSource 接口）
}

AVPixelFormat FfmpegPacketRecorderWorker::getSourcePixelFormat() const {
    return packet_source_ ? packet_source_->getSourcePixelFormat() : AV_PIX_FMT_NONE;
}
