#include "productionline/worker/RawFrameSourceFromFile.hpp"
#include "common/Logger.hpp"
#include <sys/stat.h>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
}

RawFrameSourceFromFile::RawFrameSourceFromFile(const std::string& file_path,
                                               int width,
                                               int height,
                                               AVPixelFormat pix_fmt)
    : file_path_(file_path)
    , width_(width)
    , height_(height)
    , pix_fmt_(pix_fmt)
    , file_ptr_(nullptr)
    , current_frame_index_(0)
    , total_frames_(-1)
    , frame_size_(0)
    , is_open_(false)
    , eof_reached_(false)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.RawFrameSource.File")))
{
    frame_size_ = calculateFrameSize();
    LOG4CPLUS_DEBUG_FMT(logger_, "构造函数: file='%s', %dx%d, pix_fmt=%d, frame_size=%zu",
                        file_path_.c_str(), width_, height_, pix_fmt_, frame_size_);
}

RawFrameSourceFromFile::~RawFrameSourceFromFile() {
    LOG4CPLUS_DEBUG(logger_, "析构函数开始");
    close();
    LOG4CPLUS_DEBUG(logger_, "析构函数结束");
}

size_t RawFrameSourceFromFile::calculateFrameSize() const {
    // 使用 FFmpeg 的 av_image_get_buffer_size 计算精确帧大小
    int size = av_image_get_buffer_size(pix_fmt_, width_, height_, 1);
    return size > 0 ? static_cast<size_t>(size) : 0;
}

bool RawFrameSourceFromFile::open() {
    if (is_open_.load(std::memory_order_acquire)) {
        return true;  // 已经打开
    }
    
    if (file_path_.empty()) {
        LOG4CPLUS_ERROR(logger_, "文件路径为空");
        return false;
    }
    
    if (width_ <= 0 || height_ <= 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "无效的分辨率: %dx%d", width_, height_);
        return false;
    }
    
    if (frame_size_ == 0) {
        LOG4CPLUS_ERROR(logger_, "无法计算帧大小，请检查像素格式");
        return false;
    }
    
    // 打开文件
    file_ptr_ = fopen(file_path_.c_str(), "rb");
    if (!file_ptr_) {
        LOG4CPLUS_ERROR_FMT(logger_, "无法打开文件: %s", file_path_.c_str());
        return false;
    }
    
    // 计算总帧数
    long file_size = getFileSize();
    if (file_size > 0 && frame_size_ > 0) {
        total_frames_ = static_cast<int>(file_size / frame_size_);
    }
    
    is_open_.store(true, std::memory_order_release);
    eof_reached_ = false;
    current_frame_index_ = 0;
    
    LOG4CPLUS_INFO_FMT(logger_, "打开成功: '%s'", file_path_.c_str());
    LOG4CPLUS_INFO_FMT(logger_, "   分辨率: %dx%d, 像素格式: %d", width_, height_, pix_fmt_);
    LOG4CPLUS_INFO_FMT(logger_, "   总帧数: %d, 帧大小: %zu bytes", total_frames_, frame_size_);
    
    return true;
}

bool RawFrameSourceFromFile::open(const char* path) {
    if (path && path[0] != '\0') {
        file_path_ = path;
    }
    return open();
}

void RawFrameSourceFromFile::close() {
    bool expected = true;
    if (!is_open_.compare_exchange_strong(expected, false,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        return;  // 已经关闭
    }
    
    if (file_ptr_) {
        fclose(file_ptr_);
        file_ptr_ = nullptr;
    }
    
    current_frame_index_ = 0;
    eof_reached_ = false;
    
    LOG4CPLUS_DEBUG(logger_, "关闭完成");
}

bool RawFrameSourceFromFile::isOpen() const {
    return is_open_.load(std::memory_order_acquire);
}

int RawFrameSourceFromFile::readRawFrame(AVFrame* frame) {
    if (!is_open_.load(std::memory_order_acquire) || !file_ptr_ || !frame) {
        return AVERROR(EINVAL);
    }
    
    if (eof_reached_) {
        return AVERROR_EOF;
    }
    
    // 设置 AVFrame 基本属性
    frame->format = pix_fmt_;
    frame->width = width_;
    frame->height = height_;
    
    // 分配 AVFrame 内部缓冲区（若尚未分配）。64 字节对齐以适配硬件编码器常见 stride（与 backup 一致）。
    if (!frame->data[0]) {
        int ret = av_frame_get_buffer(frame, 64);
        if (ret < 0) {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG4CPLUS_ERROR_FMT(logger_, "av_frame_get_buffer 失败: %s", err_buf);
            return ret;
        }
    }
    
    // 确保帧可写（若被引用可能触发内部拷贝，内存不足时会失败）
    int ret = av_frame_make_writable(frame);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "av_frame_make_writable 失败: %s (可能是内存不足或帧被引用)", err_buf);
        return ret;
    }
    
    // 根据像素格式读取数据
    size_t bytes_read = 0;
    
    switch (pix_fmt_) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21: {
            // Semi-planar: Y 平面 + UV 交错平面
            // Y 平面大小: width * height
            // UV 平面大小: width * height / 2
            size_t y_size = static_cast<size_t>(width_) * static_cast<size_t>(height_);
            size_t uv_size = static_cast<size_t>(width_) * static_cast<size_t>(height_) / 2;
            
            // 读取 Y 平面
            bytes_read = fread(frame->data[0], 1, y_size, file_ptr_);
            if (bytes_read < y_size) {
                eof_reached_ = true;
                LOG4CPLUS_DEBUG(logger_, "EOF: Y 平面读取不完整");
                return AVERROR_EOF;
            }
            
            // 读取 UV 平面
            bytes_read = fread(frame->data[1], 1, uv_size, file_ptr_);
            if (bytes_read < uv_size) {
                eof_reached_ = true;
                LOG4CPLUS_DEBUG(logger_, "EOF: UV 平面读取不完整");
                return AVERROR_EOF;
            }
            break;
        }
        
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P: {
            // Planar: Y + U + V 分开的三个平面
            size_t y_size = static_cast<size_t>(width_) * static_cast<size_t>(height_);
            size_t u_size = static_cast<size_t>(width_) * static_cast<size_t>(height_) / 4;
            size_t v_size = static_cast<size_t>(width_) * static_cast<size_t>(height_) / 4;
            
            // 读取 Y 平面
            bytes_read = fread(frame->data[0], 1, y_size, file_ptr_);
            if (bytes_read < y_size) {
                eof_reached_ = true;
                LOG4CPLUS_DEBUG(logger_, "EOF: Y 平面读取不完整");
                return AVERROR_EOF;
            }
            
            // 读取 U 平面
            bytes_read = fread(frame->data[1], 1, u_size, file_ptr_);
            if (bytes_read < u_size) {
                eof_reached_ = true;
                LOG4CPLUS_DEBUG(logger_, "EOF: U 平面读取不完整");
                return AVERROR_EOF;
            }
            
            // 读取 V 平面
            bytes_read = fread(frame->data[2], 1, v_size, file_ptr_);
            if (bytes_read < v_size) {
                eof_reached_ = true;
                LOG4CPLUS_DEBUG(logger_, "EOF: V 平面读取不完整");
                return AVERROR_EOF;
            }
            break;
        }
        
        default: {
            // 通用处理：一次性读取整帧数据
            // 创建临时缓冲区
            uint8_t* buffer = static_cast<uint8_t*>(av_malloc(frame_size_));
            if (!buffer) {
                LOG4CPLUS_ERROR(logger_, "内存分配失败");
                return AVERROR(ENOMEM);
            }
            
            bytes_read = fread(buffer, 1, frame_size_, file_ptr_);
            if (bytes_read < frame_size_) {
                av_free(buffer);
                eof_reached_ = true;
                LOG4CPLUS_DEBUG(logger_, "EOF: 帧数据读取不完整");
                return AVERROR_EOF;
            }
            
            // 使用 av_image_fill_arrays 将数据填充到 AVFrame
            ret = av_image_fill_arrays(frame->data, frame->linesize,
                                       buffer, pix_fmt_, width_, height_, 1);
            if (ret < 0) {
                av_free(buffer);
                char err_buf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, err_buf, sizeof(err_buf));
                LOG4CPLUS_ERROR_FMT(logger_, "av_image_fill_arrays 失败: %s", err_buf);
                return ret;
            }
            
            // 注意：buffer 的内存所有权已转移给 frame
            // 如果 frame 被正确释放（av_frame_free），buffer 也会被释放
            break;
        }
    }
    
    // 设置帧属性
    frame->pts = current_frame_index_;
    frame->pkt_dts = current_frame_index_;
    
    current_frame_index_++;
    
    return 0;
}

bool RawFrameSourceFromFile::seek(int frame_index) {
    if (!is_open_.load(std::memory_order_acquire) || !file_ptr_) {
        LOG4CPLUS_ERROR(logger_, "seek 失败：文件未打开");
        return false;
    }
    
    if (frame_index < 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "无效的帧索引: %d (不能为负)", frame_index);
        return false;
    }
    
    if (total_frames_ > 0 && frame_index >= total_frames_) {
        LOG4CPLUS_ERROR_FMT(logger_, "帧索引超出范围: %d (总帧数: %d)", 
                           frame_index, total_frames_);
        return false;
    }
    
    // 计算文件偏移
    long offset = static_cast<long>(frame_index) * static_cast<long>(frame_size_);
    
    if (fseek(file_ptr_, offset, SEEK_SET) != 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "fseek 失败: frame_index=%d, offset=%ld",
                           frame_index, offset);
        return false;
    }
    
    current_frame_index_ = frame_index;
    eof_reached_ = false;
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Seek 成功: frame_index=%d", frame_index);
    return true;
}

bool RawFrameSourceFromFile::seekToBegin() {
    return seek(0);
}

bool RawFrameSourceFromFile::seekToEnd() {
    if (total_frames_ > 0) {
        return seek(total_frames_ - 1);
    }
    LOG4CPLUS_WARN(logger_, "seekToEnd 失败：总帧数未知");
    return false;
}

bool RawFrameSourceFromFile::skip(int frame_count) {
    int target = current_frame_index_ + frame_count;
    if (target < 0) {
        target = 0;
    }
    return seek(target);
}

int RawFrameSourceFromFile::getTotalFrames() const {
    return total_frames_;
}

int RawFrameSourceFromFile::getCurrentFrameIndex() const {
    return current_frame_index_;
}

size_t RawFrameSourceFromFile::getFrameSize() const {
    return frame_size_;
}

long RawFrameSourceFromFile::getFileSize() const {
    if (file_path_.empty()) {
        return -1;
    }
    
    struct stat st;
    if (stat(file_path_.c_str(), &st) == 0) {
        return static_cast<long>(st.st_size);
    }
    
    LOG4CPLUS_WARN_FMT(logger_, "无法获取文件大小: %s", file_path_.c_str());
    return -1;
}

std::string RawFrameSourceFromFile::getPath() const {
    return file_path_;
}

bool RawFrameSourceFromFile::hasMoreFrames() const {
    return !isAtEnd();
}

bool RawFrameSourceFromFile::isAtEnd() const {
    return eof_reached_;
}
