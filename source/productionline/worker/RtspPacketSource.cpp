#include "productionline/worker/RtspPacketSource.hpp"
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

std::atomic<bool> RtspPacketSource::interrupt_requested_(false);

// ============ 构造/析构 ============

RtspPacketSource::RtspPacketSource(const std::string& rtsp_url)
    : rtsp_url_(rtsp_url)
    , format_ctx_ptr_(nullptr)
    , video_stream_index_(-1)
    , is_open_(false)
    , connected_(false)
    , eof_reached_(false)
{
    LOG_DEBUG_FMT("[RtspPacketSource] 构造函数: rtsp_url='%s'", rtsp_url_.c_str());
}

RtspPacketSource::~RtspPacketSource() {
    LOG_DEBUG("[RtspPacketSource] 析构函数开始");
    close();
    LOG_DEBUG("[RtspPacketSource] 析构函数体结束");
}

bool RtspPacketSource::open() {
    LOG_DEBUG_FMT("[RtspPacketSource] 尝试打开 RTSP 流: %s", rtsp_url_.c_str());
    
    // 检查是否已经打开
    bool expected = false;
    if (!is_open_.compare_exchange_strong(expected, true)) {
        LOG_WARN("[RtspPacketSource] RTSP stream is already open");
        return true;  // 已经打开
    }
    
    // 1. 分配格式上下文
    format_ctx_ptr_ = avformat_alloc_context();
    if (!format_ctx_ptr_) {
        LOG_ERROR("[RtspPacketSource] Failed to allocate AVFormatContext");
        is_open_.store(false, std::memory_order_release);
        return false;
    }
    
    // 2. 设置中断回调（用于响应 Ctrl+C）
    format_ctx_ptr_->interrupt_callback.callback = interrupt_callback;
    format_ctx_ptr_->interrupt_callback.opaque = this;
    LOG_DEBUG("[RtspPacketSource] ✅ 已设置 FFmpeg 中断回调");
    
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
        LOG_ERROR_FMT("[RtspPacketSource] Failed to open RTSP stream: %s", errbuf);
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
        LOG_ERROR_FMT("[RtspPacketSource] Failed to find stream information: %s", errbuf);
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
    
    LOG_DEBUG_FMT("[RtspPacketSource] Opened RTSP stream '%s', video stream index: %d", 
                  rtsp_url_.c_str(), video_stream_index_);
    
    return true;
}

void RtspPacketSource::close() {
    // 使用原子操作检查并重置 is_open_
    bool expected = true;
    if (!is_open_.compare_exchange_strong(expected, false)) {
        return;  // 已经关闭过了
    }
    
    LOG_DEBUG("[RtspPacketSource] 关闭 RTSP 流...");
    
    if (format_ctx_ptr_) {
        avformat_close_input(&format_ctx_ptr_);
        format_ctx_ptr_ = nullptr;
    }
    
    video_stream_index_ = -1;
    connected_.store(false, std::memory_order_release);
    eof_reached_.store(false, std::memory_order_release);
    
    LOG_DEBUG("[RtspPacketSource] RTSP 流已关闭");
}

bool RtspPacketSource::isOpen() const {
    return is_open_.load(std::memory_order_acquire);
}

int RtspPacketSource::readPacket(AVPacket* packet) {
    if (!is_open_.load(std::memory_order_acquire) || !format_ctx_ptr_) {
        LOG_ERROR("[RtspPacketSource] Cannot read packet: not open");
        return AVERROR(EINVAL);
    }
    
    if (!packet) {
        LOG_ERROR("[RtspPacketSource] Cannot read packet: packet is nullptr");
        return AVERROR(EINVAL);
    }
    
    // 循环读取，直到获取到视频流的 packet 或遇到错误/EOF
    const int AVERROR_INVALIDDATA_VALUE = -1094995529;  // AVERROR(0x41444e49)
    const int MAX_CORRUPTED_RETRIES = 10;  // 最大重试次数
    
    int corrupted_retries = 0;
    
    while (true) {
        int ret = av_read_frame(format_ctx_ptr_, packet);
        
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                LOG_DEBUG("[RtspPacketSource] EOF reached");
                eof_reached_.store(true, std::memory_order_release);
                return AVERROR_EOF;
            } else if (ret == AVERROR_INVALIDDATA_VALUE) {
                // 损坏的 packet，重试
                corrupted_retries++;
                if (corrupted_retries <= MAX_CORRUPTED_RETRIES) {
                    LOG_WARN_FMT("[RtspPacketSource] Corrupted packet detected (attempt %d/%d), skipping...", 
                                 corrupted_retries, MAX_CORRUPTED_RETRIES);
                    av_packet_unref(packet);
                    continue;  // 继续读取下一个 packet
                } else {
                    LOG_ERROR_FMT("[RtspPacketSource] Too many corrupted packets (%d), giving up", 
                                  corrupted_retries);
                    return ret;
                }
            } else {
                // 其他错误
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR_FMT("[RtspPacketSource] av_read_frame failed: %d (%s)", ret, errbuf);
                return ret;
            }
        }
        
        // 检查是否是视频流
        if (packet->stream_index == video_stream_index_) {
            // 成功读取到视频流的 packet
            return 0;
        } else {
            // 不是视频流，释放并继续读取
            av_packet_unref(packet);
            continue;
        }
    }
}

const AVCodecParameters* RtspPacketSource::getCodecParameters() const {
    if (!is_open_.load(std::memory_order_acquire) || !format_ctx_ptr_) {
        return nullptr;
    }
    
    if (video_stream_index_ < 0 || video_stream_index_ >= (int)format_ctx_ptr_->nb_streams) {
        return nullptr;
    }
    
    return format_ctx_ptr_->streams[video_stream_index_]->codecpar;
}

int RtspPacketSource::getVideoStreamIndex() const {
    return video_stream_index_;
}

int RtspPacketSource::getTotalFrames() const {
    // RTSP 实时流是无限的，返回一个很大的值以适配接口
    return INT_MAX;
}

long RtspPacketSource::getFileSize() const {
    // RTSP 流没有文件大小概念
    return -1;
}

std::string RtspPacketSource::getFilePath() const {
    return rtsp_url_;
}

bool RtspPacketSource::seek(int frame_index) {
    (void)frame_index;
    LOG_WARN("[RtspPacketSource] RTSP stream does not support seeking");
    return false;
}

bool RtspPacketSource::isEof() const {
    return eof_reached_.load(std::memory_order_acquire);
}

int RtspPacketSource::getSourceWidth() const {
    const AVCodecParameters* params = getCodecParameters();
    return params ? params->width : 0;
}

int RtspPacketSource::getSourceHeight() const {
    const AVCodecParameters* params = getCodecParameters();
    return params ? params->height : 0;
}

AVPixelFormat RtspPacketSource::getSourcePixelFormat() const {
    const AVCodecParameters* params = getCodecParameters();
    return params ? static_cast<AVPixelFormat>(params->format) : AV_PIX_FMT_NONE;
}

bool RtspPacketSource::findVideoStream() {
    if (!format_ctx_ptr_) {
        LOG_ERROR("[RtspPacketSource] format_ctx_ptr_ is nullptr");
        return false;
    }
    
    video_stream_index_ = -1;
    
    for (unsigned int i = 0; i < format_ctx_ptr_->nb_streams; i++) {
        if (format_ctx_ptr_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = (int)i;
            LOG_DEBUG_FMT("[RtspPacketSource] Found video stream at index: %d", video_stream_index_);
            return true;
        }
    }
    
    LOG_ERROR("[RtspPacketSource] No video stream found in RTSP source");
    return false;
}

// ============ 中断控制实现 ============

int RtspPacketSource::interrupt_callback(void* ctx) {
    (void)ctx;  // 暂时不使用上下文参数
    
    // FFmpeg 会定期调用此函数检查是否需要中断
    bool should_interrupt = interrupt_requested_.load(std::memory_order_acquire);
    
    if (should_interrupt) {
        // 仅在第一次中断时输出日志，避免刷屏
        static bool logged = false;
        if (!logged) {
            LOG_INFO("[RtspPacketSource] 🛑 FFmpeg 中断回调: 检测到中断请求，正在中断操作...");
            logged = true;
        }
    }
    
    return should_interrupt ? 1 : 0;
}

void RtspPacketSource::requestInterrupt() {
    bool was_interrupted = interrupt_requested_.exchange(true, std::memory_order_release);
    if (!was_interrupted) {
        LOG_INFO("[RtspPacketSource] 🛑 收到中断请求: 所有 RTSP 流操作将被中断");
    }
}

void RtspPacketSource::clearInterrupt() {
    interrupt_requested_.store(false, std::memory_order_release);
    LOG_DEBUG("[RtspPacketSource] ✅ 中断标志已清除");
}
