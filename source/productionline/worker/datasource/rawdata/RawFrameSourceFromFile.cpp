#include "productionline/worker/datasource/rawdata/RawFrameSourceFromFile.hpp"
#include "common/Logger.hpp"
#include <sys/stat.h>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
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
    
    // 分配 AVFrame 内部缓冲区（若尚未分配）。
    // align=1 确保 linesize == 实际行字节数：TACO 硬件编码器按连续内存读取像素，
    // 不处理 linesize padding；若 linesize > width（如 align=64 时 144→192），编码数据错位。
    if (!frame->data[0]) {
        LOG4CPLUS_DEBUG_FMT(logger_, "av_frame_get_buffer 前: format=%d, width=%d, height=%d",
                           frame->format, frame->width, frame->height);
        int ret = av_frame_get_buffer(frame, 1);
        if (ret < 0) {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG4CPLUS_ERROR_FMT(logger_, "av_frame_get_buffer 失败: %s (format=%d, %dx%d)",
                               err_buf, frame->format, frame->width, frame->height);
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
    // 关键：裸文件中数据是紧密排列（无行尾 padding），而 av_frame_get_buffer 分配的
    // linesize 可能大于 width（64 字节对齐）。必须逐行读取，尊重 frame->linesize，
    // 否则当 width 不是对齐值的整数倍时（如 144x96），从第 2 行起数据全部错位。
    size_t bytes_read = 0;
    
    switch (pix_fmt_) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21: {
            // Y 平面：逐行读取 width 字节，写入 stride=linesize[0] 的目标
            for (int y = 0; y < height_; ++y) {
                uint8_t* dst = frame->data[0] + y * frame->linesize[0];
                bytes_read = fread(dst, 1, static_cast<size_t>(width_), file_ptr_);
                if (bytes_read < static_cast<size_t>(width_)) {
                    eof_reached_ = true;
                    LOG4CPLUS_DEBUG(logger_, "EOF: Y 平面读取不完整");
                    return AVERROR_EOF;
                }
            }
            // UV 交错平面：height/2 行，每行 width 字节
            const int uv_height = height_ / 2;
            for (int y = 0; y < uv_height; ++y) {
                uint8_t* dst = frame->data[1] + y * frame->linesize[1];
                bytes_read = fread(dst, 1, static_cast<size_t>(width_), file_ptr_);
                if (bytes_read < static_cast<size_t>(width_)) {
                    eof_reached_ = true;
                    LOG4CPLUS_DEBUG(logger_, "EOF: UV 平面读取不完整");
                    return AVERROR_EOF;
                }
            }
            break;
        }
        
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P: {
            // Y 平面：逐行读取
            for (int y = 0; y < height_; ++y) {
                uint8_t* dst = frame->data[0] + y * frame->linesize[0];
                bytes_read = fread(dst, 1, static_cast<size_t>(width_), file_ptr_);
                if (bytes_read < static_cast<size_t>(width_)) {
                    eof_reached_ = true;
                    LOG4CPLUS_DEBUG(logger_, "EOF: Y 平面读取不完整");
                    return AVERROR_EOF;
                }
            }
            // U 平面：width/2 字节/行，height/2 行
            const int chroma_w = width_ / 2;
            const int chroma_h = height_ / 2;
            for (int y = 0; y < chroma_h; ++y) {
                uint8_t* dst = frame->data[1] + y * frame->linesize[1];
                bytes_read = fread(dst, 1, static_cast<size_t>(chroma_w), file_ptr_);
                if (bytes_read < static_cast<size_t>(chroma_w)) {
                    eof_reached_ = true;
                    LOG4CPLUS_DEBUG(logger_, "EOF: U 平面读取不完整");
                    return AVERROR_EOF;
                }
            }
            // V 平面
            for (int y = 0; y < chroma_h; ++y) {
                uint8_t* dst = frame->data[2] + y * frame->linesize[2];
                bytes_read = fread(dst, 1, static_cast<size_t>(chroma_w), file_ptr_);
                if (bytes_read < static_cast<size_t>(chroma_w)) {
                    eof_reached_ = true;
                    LOG4CPLUS_DEBUG(logger_, "EOF: V 平面读取不完整");
                    return AVERROR_EOF;
                }
            }
            break;
        }

        case AV_PIX_FMT_YUYV422:
        case AV_PIX_FMT_UYVY422:
        case AV_PIX_FMT_YVYU422: {
            const int row_bytes = width_ * 2;
            for (int y = 0; y < height_; ++y) {
                uint8_t* dst = frame->data[0] + y * frame->linesize[0];
                bytes_read = fread(dst, 1, static_cast<size_t>(row_bytes), file_ptr_);
                if (bytes_read < static_cast<size_t>(row_bytes)) {
                    eof_reached_ = true;
                    LOG4CPLUS_DEBUG(logger_, "EOF: YUYV/UYVY 行读取不完整");
                    return AVERROR_EOF;
                }
            }
            break;
        }
        
        default: {
            // 通用处理：先读入紧密临时缓冲区，再用 av_image_copy 按 linesize 拷贝到 AVFrame
            uint8_t* temp_buf = static_cast<uint8_t*>(av_malloc(frame_size_));
            if (!temp_buf) {
                LOG4CPLUS_ERROR(logger_, "内存分配失败");
                return AVERROR(ENOMEM);
            }
            
            bytes_read = fread(temp_buf, 1, frame_size_, file_ptr_);
            if (bytes_read < frame_size_) {
                av_free(temp_buf);
                eof_reached_ = true;
                LOG4CPLUS_DEBUG(logger_, "EOF: 帧数据读取不完整");
                return AVERROR_EOF;
            }
            
            // 构建紧密（align=1）的源 data/linesize 描述
            uint8_t* src_data[4] = {nullptr, nullptr, nullptr, nullptr};
            int src_linesize[4] = {0, 0, 0, 0};
            ret = av_image_fill_arrays(src_data, src_linesize,
                                       temp_buf, pix_fmt_, width_, height_, 1);
            if (ret < 0) {
                av_free(temp_buf);
                char err_buf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, err_buf, sizeof(err_buf));
                LOG4CPLUS_ERROR_FMT(logger_, "av_image_fill_arrays 失败: %s", err_buf);
                return ret;
            }
            
            // 从紧密源拷贝到 AVFrame（自动处理 linesize 对齐差异）
            av_image_copy(frame->data, frame->linesize,
                          const_cast<const uint8_t**>(src_data), src_linesize,
                          pix_fmt_, width_, height_);
            
            av_free(temp_buf);
            break;
        }
    }
    
    // 设置帧属性
    frame->pts = current_frame_index_;
    frame->pkt_dts = current_frame_index_;
    
    current_frame_index_++;
    
    return 0;
}

int RawFrameSourceFromFile::readRawFrameByPts(int64_t pts, AVFrame* frame) {
    if (!seek(static_cast<int>(pts))) {
        LOG4CPLUS_ERROR_FMT(logger_, "readRawFrameByPts 失败: 无法定位到 PTS %ld", pts);
        return AVERROR(EINVAL);
    }
    return readRawFrame(frame);
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
