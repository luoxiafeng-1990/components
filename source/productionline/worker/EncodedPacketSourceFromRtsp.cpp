#include "productionline/worker/EncodedPacketSourceFromRtsp.hpp"
#include "common/Logger.hpp"
#include <cstring>
#include <climits>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/dict.h>
}

// ============ 静态成员定义 ============

std::atomic<bool> EncodedPacketSourceFromRtsp::interrupt_requested_(false);

// ============ 构造/析构 ============

EncodedPacketSourceFromRtsp::EncodedPacketSourceFromRtsp(const std::string& rtsp_url, int max_frames)
    : rtsp_url_(rtsp_url)
    , format_ctx_ptr_(nullptr)
    , video_stream_index_(-1)
    , current_frame_index_(0)  // 当前帧索引初始化
    , is_open_(false)
    , connected_(false)
    , eof_reached_(false)
    , max_frames_(max_frames)   // v2.32 新增：帧数限制
    , frames_read_(0)           // v2.32 新增：已读取帧数计数
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.DataSource.Rtsp"))){
    LOG4CPLUS_DEBUG_FMT(logger_, "构造函数: rtsp_url='%s', max_frames=%d", rtsp_url_.c_str(), max_frames_);
}

EncodedPacketSourceFromRtsp::~EncodedPacketSourceFromRtsp() {
    LOG4CPLUS_DEBUG(logger_, "析构函数开始");
    close();
    LOG4CPLUS_DEBUG(logger_, "析构函数体结束");
}

bool EncodedPacketSourceFromRtsp::open() {
    LOG4CPLUS_DEBUG_FMT(logger_, "尝试打开 RTSP 流: %s", rtsp_url_.c_str());
    
    // 检查是否已经打开
    bool expected = false;
    if (!is_open_.compare_exchange_strong(expected, true)) {
        LOG4CPLUS_WARN(logger_, "RTSP stream is already open");
        return true;  // 已经打开
    }
    
    // 1. 分配格式上下文
    format_ctx_ptr_ = avformat_alloc_context();
    if (!format_ctx_ptr_) {
        LOG4CPLUS_ERROR(logger_, "Failed to allocate AVFormatContext");
        is_open_.store(false, std::memory_order_release);
        return false;
    }
    
    // 2. 设置中断回调（用于响应 Ctrl+C）
    format_ctx_ptr_->interrupt_callback.callback = interrupt_callback;
    format_ctx_ptr_->interrupt_callback.opaque = this;
    LOG4CPLUS_DEBUG(logger_, "✅ 已设置 FFmpeg 中断回调");
    
    // 3. 设置 RTSP 选项（超时、传输协议等）
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);  // 使用 TCP 传输
    av_dict_set(&options, "stimeout", "5000000", 0);    // 5秒超时
    av_dict_set(&options, "max_delay", "500000", 0);    // 最大延迟0.5秒
    
    // 3. 打开 RTSP 流
    int ret = avformat_open_input(&format_ctx_ptr_, rtsp_url_.c_str(), nullptr, &options);
    av_dict_free(&options);
    
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to open RTSP stream: %s", errbuf);
        avformat_free_context(format_ctx_ptr_);
        format_ctx_ptr_ = nullptr;
        is_open_.store(false, std::memory_order_release);
        return false;
    }
    
    // 4. 获取流信息
    ret = avformat_find_stream_info(format_ctx_ptr_, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to find stream information: %s", errbuf);
        avformat_close_input(&format_ctx_ptr_);
        is_open_.store(false, std::memory_order_release);
        return false;
    }
    
    // 5. 查找视频流
    if (!findVideoStream()) {
        avformat_close_input(&format_ctx_ptr_);
        is_open_.store(false, std::memory_order_release);
        return false;
    }
    
    // 6. 设置状态
    connected_.store(true, std::memory_order_release);
    eof_reached_.store(false, std::memory_order_release);
    current_frame_index_.store(0, std::memory_order_release);  // 重置当前帧索引
    frames_read_ = 0;  // v2.32：重置已读取帧数
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Opened RTSP stream '%s', video stream index: %d, max_frames: %d", 
                  rtsp_url_.c_str(), video_stream_index_, max_frames_);
    
    return true;
}

void EncodedPacketSourceFromRtsp::close() {
    // 使用原子操作检查并重置 is_open_
    bool expected = true;
    if (!is_open_.compare_exchange_strong(expected, false)) {
        return;  // 已经关闭过了
    }
    
    LOG4CPLUS_DEBUG(logger_, "关闭 RTSP 流...");
    
    if (format_ctx_ptr_) {
        avformat_close_input(&format_ctx_ptr_);
        format_ctx_ptr_ = nullptr;
    }
    
    video_stream_index_ = -1;
    current_frame_index_.store(0, std::memory_order_release);  // 重置当前帧索引
    connected_.store(false, std::memory_order_release);
    eof_reached_.store(false, std::memory_order_release);
    
    LOG4CPLUS_DEBUG(logger_, "RTSP 流已关闭");
}

bool EncodedPacketSourceFromRtsp::isOpen() const {
    return is_open_.load(std::memory_order_acquire);
}

PacketAcquireResult EncodedPacketSourceFromRtsp::acquireEncodedPacket(AVPacket* out_packet, void* worker_id) {
    (void)worker_id;  // RTSP 模式不需要 worker_id
    
    using Result = PacketAcquireResult;
    
    if (!is_open_.load(std::memory_order_acquire) || !format_ctx_ptr_ || !out_packet) {
        LOG4CPLUS_ERROR(logger_, "Cannot acquire packet: not open or out_packet is nullptr");
        return Result::failure(AcquireStatus::InvalidMode);
    }
    
    // v2.32 新增：检查是否达到最大帧数限制
    if (max_frames_ > 0 && frames_read_ >= max_frames_) {
        LOG4CPLUS_DEBUG_FMT(logger_, "Reached max frames limit: %d", max_frames_);
        eof_reached_.store(true, std::memory_order_release);
        return Result::eof();
    }
    
    // 从 RTSP 流读取 packet 到调用者提供的 out_packet（零拷贝）
    // 循环处理损坏的 packet
    const int AVERROR_INVALIDDATA_VALUE = -1094995529;  // AVERROR(0x41444e49)
    const int MAX_CORRUPTED_RETRIES = 10;  // 最大重试次数
    int corrupted_retries = 0;
    
    int ret = av_read_frame(format_ctx_ptr_, out_packet);
    
    while (ret == AVERROR_INVALIDDATA_VALUE && corrupted_retries < MAX_CORRUPTED_RETRIES) {
        // 损坏的 packet，重试
        corrupted_retries++;
        LOG4CPLUS_WARN_FMT(logger_, "Corrupted packet detected (attempt %d/%d), skipping...", 
                     corrupted_retries, MAX_CORRUPTED_RETRIES);
        av_packet_unref(out_packet);
        ret = av_read_frame(format_ctx_ptr_, out_packet);
    }
    
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            LOG4CPLUS_DEBUG(logger_, "EOF reached");
            eof_reached_.store(true, std::memory_order_release);
        } else if (ret == AVERROR_INVALIDDATA_VALUE) {
            LOG4CPLUS_ERROR_FMT(logger_, "Too many corrupted packets (%d), giving up", corrupted_retries);
        } else {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG4CPLUS_ERROR_FMT(logger_, "av_read_frame failed: %d (%s)", ret, errbuf);
        }
        return Result::eof();
    }
    
    // 检查是否是视频流
    if (out_packet->stream_index != video_stream_index_) {
        // 不是视频流，释放并返回 Again 让调用者重试
        av_packet_unref(out_packet);
        return Result::again();
    }
    
    // 成功读取到视频流的 packet
    current_frame_index_.fetch_add(1, std::memory_order_relaxed);
    frames_read_++;
    return Result::success(out_packet);  // 返回填充后的 out_packet
}

const AVCodecParameters* EncodedPacketSourceFromRtsp::getCodecParameters() const {
    if (!is_open_.load(std::memory_order_acquire) || !format_ctx_ptr_) {
        return nullptr;
    }
    
    if (video_stream_index_ < 0 || video_stream_index_ >= (int)format_ctx_ptr_->nb_streams) {
        return nullptr;
    }
    
    return format_ctx_ptr_->streams[video_stream_index_]->codecpar;
}

int EncodedPacketSourceFromRtsp::getVideoStreamIndex() const {
    return video_stream_index_;
}

int EncodedPacketSourceFromRtsp::getTotalFrames() const {
    // RTSP 实时流是无限的，返回一个很大的值以适配接口
    return INT_MAX;
}

long EncodedPacketSourceFromRtsp::getFileSize() const {
    // RTSP 流没有文件大小概念
    return -1;
}

std::string EncodedPacketSourceFromRtsp::getPath() const {
    return rtsp_url_;
}

bool EncodedPacketSourceFromRtsp::seek(int frame_index) {
    (void)frame_index;
    LOG4CPLUS_WARN(logger_, "RTSP stream does not support seeking");
    return false;
}

bool EncodedPacketSourceFromRtsp::isAtEnd() const {
    return eof_reached_.load(std::memory_order_acquire);
}

int EncodedPacketSourceFromRtsp::getSourceWidth() const {
    const AVCodecParameters* params = getCodecParameters();
    return params ? params->width : 0;
}

int EncodedPacketSourceFromRtsp::getSourceHeight() const {
    const AVCodecParameters* params = getCodecParameters();
    return params ? params->height : 0;
}

AVPixelFormat EncodedPacketSourceFromRtsp::getSourcePixelFormat() const {
    const AVCodecParameters* params = getCodecParameters();
    return params ? static_cast<AVPixelFormat>(params->format) : AV_PIX_FMT_NONE;
}

bool EncodedPacketSourceFromRtsp::findVideoStream() {
    if (!format_ctx_ptr_) {
        LOG4CPLUS_ERROR(logger_, "format_ctx_ptr_ is nullptr");
        return false;
    }
    
    video_stream_index_ = -1;
    
    for (unsigned int i = 0; i < format_ctx_ptr_->nb_streams; i++) {
        if (format_ctx_ptr_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = (int)i;
            LOG4CPLUS_DEBUG_FMT(logger_, "Found video stream at index: %d", video_stream_index_);
            return true;
        }
    }
    
    LOG4CPLUS_ERROR(logger_, "No video stream found in RTSP source");
    return false;
}

// ============ 中断控制实现 ============

int EncodedPacketSourceFromRtsp::interrupt_callback(void* ctx) {
    (void)ctx;  // 暂时不使用上下文参数
    
    // FFmpeg 会定期调用此函数检查是否需要中断
    bool should_interrupt = interrupt_requested_.load(std::memory_order_acquire);
    
    if (should_interrupt) {
        // 仅在第一次中断时输出日志，避免刷屏
        static bool logged = false;
        if (!logged) {
            auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.DataSource.Rtsp"));
            LOG4CPLUS_INFO(logger, "🛑 FFmpeg 中断回调: 检测到中断请求，正在中断操作...");
            logged = true;
        }
    }
    
    return should_interrupt ? 1 : 0;
}

void EncodedPacketSourceFromRtsp::requestInterrupt() {
    bool was_interrupted = interrupt_requested_.exchange(true, std::memory_order_release);
    if (!was_interrupted) {
        auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.DataSource.Rtsp"));
        LOG4CPLUS_INFO(logger, "🛑 收到中断请求: 所有 RTSP 流操作将被中断");
    }
}

void EncodedPacketSourceFromRtsp::clearInterrupt() {
    interrupt_requested_.store(false, std::memory_order_release);
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.DataSource.Rtsp"));
    LOG4CPLUS_DEBUG(logger, "✅ 中断标志已清除");
}

IDataSourceNavigator::SourceType EncodedPacketSourceFromRtsp::getDataSourceType() const {
    return SourceType::NETWORK_SOURCE;
}

bool EncodedPacketSourceFromRtsp::open(const char* path) {
    (void)path;
    LOG4CPLUS_WARN(logger_, "RTSP source does not support open(path), use open() with pre-configured URL");
    return false;
}

bool EncodedPacketSourceFromRtsp::seekToBegin() {
    LOG4CPLUS_WARN(logger_, "RTSP stream does not support seekToBegin");
    return false;
}

bool EncodedPacketSourceFromRtsp::seekToEnd() {
    LOG4CPLUS_WARN(logger_, "RTSP stream does not support seekToEnd");
    return false;
}

bool EncodedPacketSourceFromRtsp::skip(int frame_count) {
    (void)frame_count;
    LOG4CPLUS_WARN(logger_, "RTSP stream does not support skip");
    return false;
}

int EncodedPacketSourceFromRtsp::getCurrentFrameIndex() const {
    return current_frame_index_.load(std::memory_order_acquire);
}

size_t EncodedPacketSourceFromRtsp::getFrameSize() const {
    // RTSP 实时流无法估算帧大小
    return 0;
}

bool EncodedPacketSourceFromRtsp::hasMoreFrames() const {
    return !isAtEnd();
}
