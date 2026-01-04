#include "productionline/worker/FilePacketSource.hpp"
#include "common/Logger.hpp"
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

FilePacketSource::FilePacketSource(const std::string& file_path)
    : file_path_(file_path)
    , format_ctx_ptr_(nullptr)
    , video_stream_index_(-1)
    , total_frames_(-1)
    , is_open_(false)  // 🎯 原子变量初始化
    , eof_reached_(false)
{
    LOG_DEBUG_FMT("[FilePacketSource] 构造函数: file_path='%s'", file_path_.c_str());
}

FilePacketSource::~FilePacketSource() {
    LOG_DEBUG("[FilePacketSource] 析构函数开始");
    close();
    LOG_DEBUG("[FilePacketSource] 析构函数体结束");
}

bool FilePacketSource::open() {
    if (is_open_.load(std::memory_order_acquire)) {
        return true;  // 已经打开
    }
    
    // 1. 打开输入文件
    format_ctx_ptr_ = avformat_alloc_context();
    if (!format_ctx_ptr_) {
        LOG_ERROR("[FilePacketSource] Failed to allocate AVFormatContext");
        return false;
    }
    
    int ret = avformat_open_input(&format_ctx_ptr_, file_path_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR_FMT("[FilePacketSource] Failed to open file '%s': %s", 
                     file_path_.c_str(), err_buf);
        format_ctx_ptr_ = nullptr;
        return false;
    }
    
    // 2. 读取流信息
    ret = avformat_find_stream_info(format_ctx_ptr_, nullptr);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR_FMT("[FilePacketSource] Failed to find stream info: %s", err_buf);
        close();
        return false;
    }
    
    // 3. 查找视频流
    if (!findVideoStream()) {
        close();
        return false;
    }
    
    // 4. 估算总帧数
    total_frames_ = estimateTotalFrames();
    
    is_open_.store(true, std::memory_order_release);  // 🎯 原子操作设置状态
    eof_reached_ = false;  // 重置 EOF 状态
    LOG_DEBUG_FMT("[FilePacketSource] Opened file '%s', video stream index: %d, total frames: %d",
                 file_path_.c_str(), video_stream_index_, total_frames_);
    
    return true;
}

void FilePacketSource::close() {
    // 🎯 原子检查并设置：如果 is_open_ 是 true，则设置为 false
    bool expected = true;
    if (!is_open_.compare_exchange_strong(expected, false,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        // is_open_ 已经是 false，说明已经关闭过了，直接返回
        return;
    }
    
    if (format_ctx_ptr_) {
        avformat_close_input(&format_ctx_ptr_);
        format_ctx_ptr_ = nullptr;
    }
    
    video_stream_index_ = -1;
    total_frames_ = -1;
    // is_open_ 已经在上面设置为 false，不需要再次设置
    eof_reached_ = false;  // 重置 EOF 状态
}

bool FilePacketSource::isOpen() const {
    return is_open_.load(std::memory_order_acquire);  // 🎯 原子操作读取状态
}

int FilePacketSource::readPacket(AVPacket* packet) {
    if (!is_open_.load(std::memory_order_acquire) || !format_ctx_ptr_ || !packet) {
        return AVERROR(EINVAL);
    }
    
    // 从文件读取 packet
    int ret = av_read_frame(format_ctx_ptr_, packet);
    
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            LOG_DEBUG("[FilePacketSource] EOF reached");
            eof_reached_ = true;  // 设置 EOF 状态
        } else {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_WARN_FMT("[FilePacketSource] av_read_frame failed: %s", err_buf);
        }
        return ret;
    }
    
    // 检查是否是视频流
    if (packet->stream_index != video_stream_index_) {
        // 不是视频流，释放并继续读取下一个（循环直到找到视频流或EOF）
        av_packet_unref(packet);
        // 使用循环而不是递归，避免栈溢出
        const int MAX_NON_VIDEO_PACKETS = 100;  // 最大跳过非视频包数量
        int skipped = 0;
        while (skipped < MAX_NON_VIDEO_PACKETS) {
            int ret = av_read_frame(format_ctx_ptr_, packet);
            if (ret < 0) {
                return ret;  // EOF 或错误
            }
            if (packet->stream_index == video_stream_index_) {
                return 0;  // 找到视频流
            }
            av_packet_unref(packet);
            skipped++;
        }
        // 跳过了太多非视频包，可能有问题
        LOG_WARN("[FilePacketSource] Skipped too many non-video packets");
        return AVERROR(EINVAL);
    }
    
    return 0;  // 成功
}

const AVCodecParameters* FilePacketSource::getCodecParameters() const {
    if (!is_open_.load(std::memory_order_acquire) || !format_ctx_ptr_ || video_stream_index_ < 0) {
        return nullptr;
    }
    
    return format_ctx_ptr_->streams[video_stream_index_]->codecpar;
}

int FilePacketSource::getVideoStreamIndex() const {
    return video_stream_index_;
}

int FilePacketSource::getTotalFrames() const {
    return total_frames_;
}

bool FilePacketSource::findVideoStream() {
    video_stream_index_ = -1;
    
    for (unsigned int i = 0; i < format_ctx_ptr_->nb_streams; i++) {
        if (format_ctx_ptr_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = (int)i;
            break;
        }
    }
    
    if (video_stream_index_ == -1) {
        LOG_ERROR("[FilePacketSource] No video stream found in file");
        return false;
    }
    
    return true;
}

int FilePacketSource::estimateTotalFrames() {
    if (!format_ctx_ptr_ || video_stream_index_ < 0) {
        return -1;
    }
    
    AVStream* stream = format_ctx_ptr_->streams[video_stream_index_];
    if (!stream) {
        return -1;
    }
    
    // 估算总帧数（参考 FfmpegDecodeVideoFileWorker 的逻辑）
    int64_t duration = stream->duration;
    AVRational time_base = stream->time_base;
    AVRational frame_rate = stream->avg_frame_rate;
    
    if (duration != AV_NOPTS_VALUE && time_base.num > 0 && frame_rate.num > 0) {
        double duration_seconds = (double)duration * time_base.num / time_base.den;
        double fps = (double)frame_rate.num / frame_rate.den;
        int frames = (int)(duration_seconds * fps);
        return frames > 0 ? frames : -1;
    }
    
    return -1;
}

long FilePacketSource::getFileSize() const {
    if (!is_open_.load(std::memory_order_acquire) || !format_ctx_ptr_) {
        return -1;
    }
    
    // 尝试从格式上下文获取
    AVIOContext* io_ctx = format_ctx_ptr_->pb;
    if (io_ctx) {
        return avio_size(io_ctx);
    }
    
    return -1;
}

std::string FilePacketSource::getFilePath() const {
    return file_path_;
}

bool FilePacketSource::seek(int frame_index) {
    if (!is_open_.load(std::memory_order_acquire) || !format_ctx_ptr_ || video_stream_index_ < 0) {
        LOG_ERROR("[FilePacketSource] Cannot seek: not open or invalid state");
        return false;
    }
    
    if (frame_index < 0) {
        LOG_ERROR_FMT("[FilePacketSource] Invalid frame index: %d", frame_index);
        return false;
    }
    
    AVStream* stream = format_ctx_ptr_->streams[video_stream_index_];
    if (!stream) {
        LOG_ERROR("[FilePacketSource] Invalid video stream");
        return false;
    }
    
    // 计算目标时间戳
    // 方法1：如果有 nb_frames，使用帧索引计算
    int64_t timestamp;
    if (stream->nb_frames > 0 && stream->avg_frame_rate.num > 0) {
        // timestamp = frame_index * time_base.den / fps
        AVRational time_base = stream->time_base;
        AVRational frame_rate = stream->avg_frame_rate;
        timestamp = (int64_t)frame_index * time_base.den * frame_rate.den / (time_base.num * frame_rate.num);
    } else if (stream->duration != AV_NOPTS_VALUE && stream->avg_frame_rate.num > 0) {
        // 方法2：根据总时长和帧率估算
        AVRational time_base = stream->time_base;
        AVRational frame_rate = stream->avg_frame_rate;
        double duration_seconds = (double)stream->duration * time_base.num / time_base.den;
        double fps = (double)frame_rate.num / frame_rate.den;
        int total_frames = (int)(duration_seconds * fps);
        if (total_frames > 0 && frame_index < total_frames) {
            timestamp = (int64_t)frame_index * time_base.den * frame_rate.den / (time_base.num * frame_rate.num);
        } else {
            LOG_ERROR_FMT("[FilePacketSource] Frame index %d out of range (estimated total: %d)", 
                         frame_index, total_frames);
            return false;
        }
    } else {
        LOG_ERROR("[FilePacketSource] Cannot calculate timestamp: missing stream information");
        return false;
    }
    
    // 使用 FFmpeg 的 av_seek_frame() 实现真正的定位
    // AVSEEK_FLAG_BACKWARD: 如果找不到精确位置，定位到最近的关键帧
    int ret = av_seek_frame(format_ctx_ptr_, video_stream_index_, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR_FMT("[FilePacketSource] av_seek_frame failed: %s", err_buf);
        return false;
    }
    
    eof_reached_ = false;  // seek 后重置 EOF 状态
    LOG_DEBUG_FMT("[FilePacketSource] Successfully seeked to frame %d (timestamp: %ld)", frame_index, timestamp);
    return true;
}

bool FilePacketSource::isEof() const {
    return eof_reached_;
}

