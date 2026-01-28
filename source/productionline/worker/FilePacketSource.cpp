#include "productionline/worker/FilePacketSource.hpp"
#include "common/Logger.hpp"
#include <cstring>
#include <sys/stat.h>

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
    , current_frame_index_(0)  // 当前帧索引初始化
    , is_open_(false)  // 🎯 原子变量初始化
    , eof_reached_(false)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.DataSource.File"))){
    LOG4CPLUS_DEBUG_FMT(logger_, "构造函数: file_path='%s'", file_path_.c_str());
}

FilePacketSource::~FilePacketSource() {
    LOG4CPLUS_DEBUG(logger_, "析构函数开始");
    close();
    LOG4CPLUS_DEBUG(logger_, "析构函数体结束");
}

bool FilePacketSource::open() {
    if (is_open_.load(std::memory_order_acquire)) {
        return true;  // 已经打开
    }
    
    // 1. 打开输入文件
    format_ctx_ptr_ = avformat_alloc_context();
    if (!format_ctx_ptr_) {
        LOG4CPLUS_ERROR(logger_, "Failed to allocate AVFormatContext");
        return false;
    }
    
    int ret = avformat_open_input(&format_ctx_ptr_, file_path_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to open file '%s': %s", 
                     file_path_.c_str(), err_buf);
        format_ctx_ptr_ = nullptr;
        return false;
    }
    
    // 2. 读取流信息
    ret = avformat_find_stream_info(format_ctx_ptr_, nullptr);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to find stream info: %s", err_buf);
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
    current_frame_index_ = 0;  // 重置当前帧索引
    LOG4CPLUS_DEBUG_FMT(logger_, "Opened file '%s', video stream index: %d, total frames: %d",
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
    current_frame_index_ = 0;  // 重置当前帧索引
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
            LOG4CPLUS_DEBUG(logger_, "EOF reached");
            eof_reached_ = true;  // 设置 EOF 状态
        } else {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG4CPLUS_WARN_FMT(logger_, "av_read_frame failed: %s", err_buf);
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
                current_frame_index_++;  // 更新当前帧索引
                return 0;  // 找到视频流
            }
            av_packet_unref(packet);
            skipped++;
        }
        // 跳过了太多非视频包，可能有问题
        LOG4CPLUS_WARN(logger_, "Skipped too many non-video packets");
        return AVERROR(EINVAL);
    }
    
    current_frame_index_++;  // 更新当前帧索引
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
        LOG4CPLUS_ERROR(logger_, "No video stream found in file");
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
    
    // 方法1：基于容器元数据估算（适用于MP4、AVI、MKV等容器格式）
    int64_t duration = stream->duration;
    AVRational time_base = stream->time_base;
    AVRational frame_rate = stream->avg_frame_rate;
    
    if (duration != AV_NOPTS_VALUE && time_base.num > 0 && frame_rate.num > 0) {
        double duration_seconds = (double)duration * time_base.num / time_base.den;
        double fps = (double)frame_rate.num / frame_rate.den;
        int frames = (int)(duration_seconds * fps);
        if (frames > 0) {
            LOG4CPLUS_DEBUG_FMT(logger_, "Estimated frames from metadata: %d", frames);
            return frames;
        }
    }
    
    // 方法2：基于文件大小估算（适用于裸数据流：.h264/.h265/.yuv等）
    LOG4CPLUS_DEBUG(logger_, "Cannot estimate from metadata, trying file size method...");
    
    // 获取文件大小
    long file_size = getFileSize();
    if (file_size <= 0) {
        LOG4CPLUS_WARN(logger_, "Cannot get file size, total frames unknown");
        return -1;
    }
    
    // 获取编解码器参数
    const AVCodecParameters* codecpar = stream->codecpar;
    if (!codecpar) {
        LOG4CPLUS_WARN(logger_, "Cannot get codec parameters");
        return -1;
    }
    
    int width = codecpar->width;
    int height = codecpar->height;
    
    if (width <= 0 || height <= 0) {
        LOG4CPLUS_WARN_FMT(logger_, "Invalid resolution: %dx%d", width, height);
        return -1;
    }
    
    // 根据编解码器类型估算
    int estimated_frames = -1;
    
    // 情况A：裸YUV数据（未编码）
    if (codecpar->codec_id == AV_CODEC_ID_RAWVIDEO) {
        // 每帧大小固定 = width * height * bytes_per_pixel
        int bytes_per_pixel = 1;  // 默认YUV420
        
        // 根据像素格式确定 bytes_per_pixel
        switch (codecpar->format) {
            case AV_PIX_FMT_YUV420P:
            case AV_PIX_FMT_NV12:
            case AV_PIX_FMT_NV21:
                bytes_per_pixel = 3;  // YUV420: 1.5 bytes per pixel (实际是 width*height*3/2)
                estimated_frames = (int)(file_size / (width * height * bytes_per_pixel / 2));
                break;
            case AV_PIX_FMT_YUV422P:
                bytes_per_pixel = 2;  // YUV422: 2 bytes per pixel
                estimated_frames = (int)(file_size / (width * height * bytes_per_pixel));
                break;
            case AV_PIX_FMT_RGB24:
            case AV_PIX_FMT_BGR24:
                bytes_per_pixel = 3;  // RGB24: 3 bytes per pixel
                estimated_frames = (int)(file_size / (width * height * bytes_per_pixel));
                break;
            case AV_PIX_FMT_RGBA:
            case AV_PIX_FMT_BGRA:
            case AV_PIX_FMT_ARGB:
            case AV_PIX_FMT_ABGR:
                bytes_per_pixel = 4;  // RGBA: 4 bytes per pixel
                estimated_frames = (int)(file_size / (width * height * bytes_per_pixel));
                break;
            default:
                // 默认假设YUV420
                estimated_frames = (int)(file_size / (width * height * 3 / 2));
                break;
        }
        
        LOG4CPLUS_DEBUG_FMT(logger_, "Raw video detected, estimated frames from file size: %d (resolution: %dx%d)",
                     estimated_frames, width, height);
    }
    // 情况B：编码数据（H.264/H.265/VP9等）
    else {
        // 编码数据的帧大小不固定，但可以基于平均码率估算
        // 假设平均每帧大小（这是个粗略估算）
        
        // 如果有码率信息，使用码率估算
        if (codecpar->bit_rate > 0 && frame_rate.num > 0 && frame_rate.den > 0) {
            double fps = (double)frame_rate.num / frame_rate.den;
            double avg_frame_size = codecpar->bit_rate / 8.0 / fps;  // bytes per frame
            estimated_frames = (int)(file_size / avg_frame_size);
            LOG4CPLUS_DEBUG_FMT(logger_, "Encoded video, estimated frames from bitrate: %d (bitrate: %ld, fps: %.2f)",
                         estimated_frames, codecpar->bit_rate, fps);
        } else {
            // 没有码率信息，使用经验值
            // 假设压缩比：H.264通常是原始数据的1/100 ~ 1/200
            // 原始YUV420大小 = width * height * 1.5
            // 压缩后平均每帧 = width * height * 1.5 / 压缩比
            int yuv420_frame_size = width * height * 3 / 2;
            int compression_ratio = 100;  // 假设100:1的压缩比
            int avg_encoded_frame_size = yuv420_frame_size / compression_ratio;
            
            // 保证最小帧大小（避免过小）
            if (avg_encoded_frame_size < 1024) {
                avg_encoded_frame_size = width * height / 10;  // 更保守的估算
            }
            
            estimated_frames = (int)(file_size / avg_encoded_frame_size);
            LOG4CPLUS_DEBUG_FMT(logger_, "Encoded video (no bitrate), estimated frames from file size: %d (resolution: %dx%d, avg_frame_size: %d)",
                         estimated_frames, width, height, avg_encoded_frame_size);
        }
    }
    
    return estimated_frames > 0 ? estimated_frames : -1;
}

long FilePacketSource::getFileSize() const {
    if (!file_path_.empty()) {
        struct stat st;
        if (stat(file_path_.c_str(), &st) == 0) {
            LOG4CPLUS_DEBUG_FMT(logger_, "Got file size from stat(): %lld bytes", (long long)st.st_size);
            return (long)st.st_size;
        }
        LOG4CPLUS_WARN_FMT(logger_, "stat() failed for file: %s", file_path_.c_str());
    }
    
    return -1;
}

std::string FilePacketSource::getPath() const {
    return file_path_;
}

bool FilePacketSource::seek(int frame_index) {
    if (!is_open_.load(std::memory_order_acquire) || !format_ctx_ptr_ || video_stream_index_ < 0) {
        LOG4CPLUS_ERROR(logger_, "Cannot seek: not open or invalid state");
        return false;
    }
    
    if (frame_index < 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "Invalid frame index: %d", frame_index);
        return false;
    }
    
    AVStream* stream = format_ctx_ptr_->streams[video_stream_index_];
    if (!stream) {
        LOG4CPLUS_ERROR(logger_, "Invalid video stream");
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
            LOG4CPLUS_ERROR_FMT(logger_, "Frame index %d out of range (estimated total: %d)", 
                         frame_index, total_frames);
            return false;
        }
    } else {
        LOG4CPLUS_ERROR(logger_, "Cannot calculate timestamp: missing stream information");
        return false;
    }
    
    // 使用 FFmpeg 的 av_seek_frame() 实现真正的定位
    // AVSEEK_FLAG_BACKWARD: 如果找不到精确位置，定位到最近的关键帧
    int ret = av_seek_frame(format_ctx_ptr_, video_stream_index_, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "av_seek_frame failed: %s", err_buf);
        return false;
    }
    
    eof_reached_ = false;  // seek 后重置 EOF 状态
    current_frame_index_ = frame_index;  // 更新当前帧索引
    LOG4CPLUS_DEBUG_FMT(logger_, "Successfully seeked to frame %d (timestamp: %ld)", frame_index, timestamp);
    return true;
}

bool FilePacketSource::isAtEnd() const {
    return eof_reached_;
}

// ⭐ v2.18: 新增获取输入源信息的接口
int FilePacketSource::getSourceWidth() const {
    const AVCodecParameters* params = getCodecParameters();
    return params ? params->width : 0;
}

int FilePacketSource::getSourceHeight() const {
    const AVCodecParameters* params = getCodecParameters();
    return params ? params->height : 0;
}

AVPixelFormat FilePacketSource::getSourcePixelFormat() const {
    const AVCodecParameters* params = getCodecParameters();
    return params ? static_cast<AVPixelFormat>(params->format) : AV_PIX_FMT_NONE;
}

IDataSourceNavigator::SourceType FilePacketSource::getDataSourceType() const {
    return SourceType::FILE_SOURCE;
}

bool FilePacketSource::open(const char* path) {
    if (path && path[0] != '\0') {
        file_path_ = path;
    }
    return open();
}

bool FilePacketSource::seekToBegin() {
    return seek(0);
}

bool FilePacketSource::seekToEnd() {
    if (total_frames_ > 0) {
        return seek(total_frames_ - 1);
    }
    return false;
}

bool FilePacketSource::skip(int frame_count) {
    int target = current_frame_index_ + frame_count;
    if (target < 0) {
        target = 0;
    }
    return seek(target);
}

int FilePacketSource::getCurrentFrameIndex() const {
    return current_frame_index_;
}

size_t FilePacketSource::getFrameSize() const {
    long file_size = getFileSize();
    if (file_size > 0 && total_frames_ > 0) {
        return static_cast<size_t>(file_size / total_frames_);
    }
    return 0;
}

bool FilePacketSource::hasMoreFrames() const {
    return !isAtEnd();
}