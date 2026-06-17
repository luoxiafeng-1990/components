#include "consumptionline/types/writer/BufferWriter.hpp"
#include "common/ImageMeta.hpp"
#include "common/Logger.hpp"
#include <cstring>
#include <cerrno>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace consumptionline {
namespace io {

// 静态ID生成器
std::atomic<uint64_t> BufferWriter::next_id_{0};

// ========== 构造函数和析构函数 ==========

BufferWriter::BufferWriter()
    : file_(nullptr)
    , format_(AV_PIX_FMT_NONE)
    , width_(0)
    , height_(0)
    , write_count_(0)
    , mismatch_count_(0)
    , output_format_ctx_(nullptr)
    , video_stream_index_(-1)
    , packet_count_(0)
    , time_base_({1, 25})
    , last_dts_(AV_NOPTS_VALUE)
    , first_pts_(AV_NOPTS_VALUE)
    , first_dts_(AV_NOPTS_VALUE)
    , writer_id_(++next_id_)
    , log_prefix_("[" + std::to_string(writer_id_) + "]")
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.BufferWriter")))
{
    // 打印生命周期开始
    LOG4CPLUS_INFO(logger_, "");
    LOG4CPLUS_INFO(logger_, log_prefix_ << " " << std::string(67, '='));
    LOG4CPLUS_INFO(logger_, log_prefix_ << " 构造");
    LOG4CPLUS_INFO(logger_, log_prefix_ << " " << std::string(67, '='));
}

BufferWriter::~BufferWriter() {
    // 打印生命周期结束
    LOG4CPLUS_INFO(logger_, "");
    LOG4CPLUS_INFO(logger_, log_prefix_ << " " << std::string(67, '='));
    LOG4CPLUS_INFO(logger_, log_prefix_ << " 析构: 共写入 " << write_count_.load() << " 帧");
    LOG4CPLUS_INFO(logger_, log_prefix_ << " " << std::string(67, '='));
    
    close();
}

// ========== 核心接口实现 ==========

/**
 * @brief 打开原始图像数据文件（裸数据模式）
 * 详细说明参见头文件注释
 */
bool BufferWriter::openRaw(const char* path, 
                           AVPixelFormat save_format,
                           int width, 
                           int height) {
    // 1. 参数校验
    if (!path) {
        LOG4CPLUS_ERROR(logger_, "Error: Invalid path (nullptr)");
        return false;
    }
    
    if (width <= 0 || height <= 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "Error: Invalid dimensions (%dx%d)", 
                width, height);
        return false;
    }
    
    // 2. 检查格式支持
    if (!isSupportedFormat(save_format)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Error: Unsupported save_format: %s (%d)",
                av_get_pix_fmt_name(save_format), save_format);
        LOG4CPLUS_ERROR(logger_, "Supported formats (22): "
                "GRAY8, GRAY10LE, NV12, P010LE, NV21, YUV420P, YUV420P10LE, YUV422P, YUV444P, "
                "RGB24, BGR24, ARGB, ABGR, RGBA, BGRA, GBRP, "
                "RGB0, BGR0, 0RGB, 0BGR, RGB48LE, BGR48LE");
        return false;
    }
    
    // 3. 如果已打开，先关闭
    if (file_) {
        close();
    }
    
    // 4. 打开文件（二进制写入模式）
    file_ = fopen(path, "wb");
    if (!file_) {
        LOG4CPLUS_ERROR_FMT(logger_, "Error: Failed to open file: %s "
                "(errno=%d: %s)", path, errno, strerror(errno));
        return false;
    }
    
    // 5. 保存配置
    format_ = save_format;
    width_ = width;
    height_ = height;
    write_count_.store(0);  // 重置计数器
    mismatch_count_.store(0);  // ⭐ v2.17：重置格式不匹配计数器
    
    // 6. 打印成功信息
    LOG4CPLUS_INFO_FMT(logger_, "Opened: %s", path);
    LOG4CPLUS_INFO_FMT(logger_, "  Format: %s", getFormatName(format_));
    LOG4CPLUS_INFO_FMT(logger_, "  Resolution: %dx%d", width_, height_);
    LOG4CPLUS_INFO_FMT(logger_, "  Frame size: %zu bytes", calculateFrameSize(format_, width_, height_));
    
    return true;
}

bool BufferWriter::write(const Buffer* buffer) {
    // 1. 参数校验
    if (!buffer) {
        LOG4CPLUS_ERROR(logger_, "Error: Invalid buffer");
        return false;
    }
    
    // 2. ⭐ 编码流模式（MP4等容器格式）
    if (output_format_ctx_) {
        return writeEncoded(buffer);
    }
    
    // 3. 图像模式（原有逻辑）
    if (!file_) {
        LOG4CPLUS_ERROR(logger_, "Error: file not opened");
        return false;
    }
    
    // 4. ⭐⭐⭐ v2.17 需求4：格式和尺寸验证（在 BufferWriter 内部完成）
    auto img = ImageMeta::fromBuffer(buffer);
    if (img.isValid()) {
        AVPixelFormat actual_format = img.format();
        int actual_width = img.width();
        int actual_height = img.height();
        
        bool format_match = (actual_format == format_);
        bool size_match = (actual_width == width_ && actual_height == height_);
        
        if (!format_match || !size_match) {
            mismatch_count_.fetch_add(1);
            int64_t current_count = mismatch_count_.load();
            
            // 只打印前5次错误，避免刷屏
            if (current_count <= 5) {
                LOG4CPLUS_ERROR_FMT(logger_, "❌ Format/size mismatch (count: %lld)", 
                             (long long)current_count);
                if (!format_match) {
                    LOG4CPLUS_ERROR_FMT(logger_, "  Expected format: %s, got: %s",
                                 av_get_pix_fmt_name(format_),
                                 av_get_pix_fmt_name(actual_format));
                }
                if (!size_match) {
                    LOG4CPLUS_ERROR_FMT(logger_, "  Expected size: %dx%d, got: %dx%d",
                                 width_, height_, actual_width, actual_height);
                }
            }
            
            return false;  // ⭐ 不匹配则拒绝写入
        }
    }
    
    // 5. ⭐ 检查Buffer是否有图像元数据
    if (img.isValid()) {
        // 使用元数据模式（v2.6新功能）
        return writeWithMetadata(buffer);
    } else {
        // 回退到简单模式（兼容旧代码）
        return writeSimple(buffer);
    }
}

bool BufferWriter::writeSimple(const Buffer* buffer) {
    // 1. 参数校验：空指针检查（防御性编程）
    if (!buffer) {
        LOG4CPLUS_ERROR(logger_, "[BufferWriter::writeSimple] Error: buffer is nullptr");
        return false;
    }
    
    // 2. ⭐ 尝试从AVFrame获取格式信息（如果Buffer有关联的AVFrame）
    AVFrame* frame = buffer->getAVFrame();
    if (frame && format_ != AV_PIX_FMT_NONE) {
        AVPixelFormat frame_format = static_cast<AVPixelFormat>(frame->format);
        if (frame_format != format_) {
            // 格式不匹配，静默跳过
            return true;
        }
    }
    
    // 3. 旧版简单写入（向后兼容）
    if (!buffer->isValid()) {
        LOG4CPLUS_ERROR(logger_, "Error: Buffer validation failed");
        return false;
    }
    
    void* data = buffer->getVirtualAddress();
    size_t buffer_size = buffer->size();
    
    if (!data || buffer_size == 0) {
        LOG4CPLUS_ERROR(logger_, "Error: Buffer has no data or zero size");
        return false;
    }
    
    size_t expected_size = calculateFrameSize(format_, width_, height_);
    
    if (buffer_size < expected_size) {
        LOG4CPLUS_ERROR(logger_, "Error: Buffer size mismatch");
        LOG4CPLUS_ERROR_FMT(logger_, "  Expected: %zu bytes (format=%s, %dx%d)",
                expected_size, getFormatName(format_), width_, height_);
        LOG4CPLUS_ERROR_FMT(logger_, "  Got: %zu bytes", buffer_size);
        return false;
    }
    
    size_t written = fwrite(data, 1, expected_size, file_);
    if (written != expected_size) {
        LOG4CPLUS_ERROR(logger_, "Error: Write failed");
        LOG4CPLUS_ERROR_FMT(logger_, "  Expected to write: %zu bytes", expected_size);
        LOG4CPLUS_ERROR_FMT(logger_, "  Actually wrote: %zu bytes", written);
        LOG4CPLUS_ERROR_FMT(logger_, "  errno=%d: %s", errno, strerror(errno));
        return false;
    }
    
    write_count_.fetch_add(1);
    return true;
}

bool BufferWriter::writeWithMetadata(const Buffer* buffer) {
    // 1. 参数校验：空指针检查（防御性编程）
    if (!buffer) {
        LOG4CPLUS_ERROR(logger_, "[BufferWriter::writeWithMetadata] Error: buffer is nullptr");
        return false;
    }
    
    // 2. 获取图像元数据（通过 ImageMeta 从载荷中提取）
    auto img = ImageMeta::fromBuffer(buffer);
    AVPixelFormat buf_format = img.format();
    int buf_width = img.width();
    int buf_height = img.height();
    const int linesize[4] = { img.linesize(0), img.linesize(1), img.linesize(2), img.linesize(3) };
    
    // 3. ⭐ 格式过滤：只保存匹配期望格式的帧（静默跳过不匹配的帧）
    if (format_ != AV_PIX_FMT_NONE && buf_format != format_) {
        // 格式不匹配，静默跳过（这是正常行为，不是错误）
        // 场景：TACO解码器同时输出多种格式，每个BufferWriter只保存自己关心的格式
        LOG4CPLUS_INFO_FMT(logger_, "Format not matched: %s (expected: %s)",
                av_get_pix_fmt_name(buf_format), av_get_pix_fmt_name(format_));
        return true;  // 返回true表示"已处理"（虽然未写入，但这是预期行为）
    }
    
    // 3.5 ⭐ 数据有效性检查：如果第一个plane的数据为空，静默跳过整个帧
    // 场景：Buffer 报告了格式但实际数据未就绪，或者帧不完整
    const uint8_t* first_plane = img.planeData(0);
    if (!first_plane) {
        // 数据未就绪，静默跳过
        LOG4CPLUS_ERROR(logger_, "Warning: First image plane data is null, skipping frame");
        return true;
    }
    
    // 4. 根据格式写入数据
    switch (buf_format) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21: {
            // Semi-planar: Plane 0 (Y) + Plane 1 (UV/VU)
            const uint8_t* y_data = img.planeData(0);
            const uint8_t* uv_data = img.planeData(1);
            
            // 写入Y平面（去除stride）
            if (!writePlane(y_data, linesize[0], buf_width, buf_height)) {
                LOG4CPLUS_ERROR(logger_, "Error: Write Y plane failed");
                return false;
            }
            
            // 写入UV平面（去除stride，高度为height/2）
            if (!writePlane(uv_data, linesize[1], buf_width, buf_height / 2)) {
                LOG4CPLUS_ERROR(logger_, "Error: Write UV plane failed");
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV420P10LE: {
            // Planar: Plane 0 (Y) + Plane 1 (U) + Plane 2 (V)
            int bytes_per_pixel = (buf_format == AV_PIX_FMT_YUV420P10LE) ? 2 : 1;
            
            // Y平面
            if (!writePlane(img.planeData(0), linesize[0], 
                          buf_width * bytes_per_pixel, buf_height)) {
                return false;
            }
            
            // U平面
            if (!writePlane(img.planeData(1), linesize[1], 
                          buf_width / 2 * bytes_per_pixel, buf_height / 2)) {
                return false;
            }
            
            // V平面
            if (!writePlane(img.planeData(2), linesize[2], 
                          buf_width / 2 * bytes_per_pixel, buf_height / 2)) {
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_ARGB:
        case AV_PIX_FMT_BGRA:
        case AV_PIX_FMT_RGBA:
        case AV_PIX_FMT_ABGR:
        case AV_PIX_FMT_RGB0:
        case AV_PIX_FMT_BGR0:
        case AV_PIX_FMT_0RGB:
        case AV_PIX_FMT_0BGR: {
            // Packed RGB: 单plane，4 bytes/pixel
            const uint8_t* rgb_data = img.planeData(0);
            if (!writePlane(rgb_data, linesize[0], buf_width * 4, buf_height)) {
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_RGB24:
        case AV_PIX_FMT_BGR24: {
            // Packed RGB: 单plane，3 bytes/pixel
            const uint8_t* rgb_data = img.planeData(0);
            if (!writePlane(rgb_data, linesize[0], buf_width * 3, buf_height)) {
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_RGB48LE:
        case AV_PIX_FMT_BGR48LE: {
            // Packed RGB: 单plane，6 bytes/pixel
            const uint8_t* rgb_data = img.planeData(0);
            if (!writePlane(rgb_data, linesize[0], buf_width * 6, buf_height)) {
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_GRAY8: {
            // 灰度：单plane，1 byte/pixel
            const uint8_t* gray_data = img.planeData(0);
            if (!writePlane(gray_data, linesize[0], buf_width, buf_height)) {
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_GRAY10LE: {
            // 灰度10bit：单plane，2 bytes/pixel
            const uint8_t* gray_data = img.planeData(0);
            if (!writePlane(gray_data, linesize[0], buf_width * 2, buf_height)) {
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_P010LE: {
            // YUV420 P010: Semi-planar 16bit
            const uint8_t* y_data = img.planeData(0);
            const uint8_t* uv_data = img.planeData(1);
            
            // Y平面（16bit/pixel）
            if (!writePlane(y_data, linesize[0], buf_width * 2, buf_height)) {
                return false;
            }
            
            // UV平面（16bit/pixel）
            if (!writePlane(uv_data, linesize[1], buf_width * 2, buf_height / 2)) {
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_YUV422P: {
            // YUV422 Planar: Plane 0 (Y) + Plane 1 (U) + Plane 2 (V)
            // Y: full resolution, U/V: half width
            const uint8_t* y_data = img.planeData(0);
            const uint8_t* u_data = img.planeData(1);
            const uint8_t* v_data = img.planeData(2);
            
            // Y平面（full resolution）
            if (!writePlane(y_data, linesize[0], buf_width, buf_height)) {
                return false;
            }
            
            // U平面（half width, full height）
            if (!writePlane(u_data, linesize[1], buf_width / 2, buf_height)) {
                return false;
            }
            
            // V平面（half width, full height）
            if (!writePlane(v_data, linesize[2], buf_width / 2, buf_height)) {
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_YUV444P: {
            // YUV444 Planar: Plane 0 (Y) + Plane 1 (U) + Plane 2 (V)
            // All planes are full resolution
            const uint8_t* y_data = img.planeData(0);
            const uint8_t* u_data = img.planeData(1);
            const uint8_t* v_data = img.planeData(2);
            
            // Y平面（full resolution）
            if (!writePlane(y_data, linesize[0], buf_width, buf_height)) {
                return false;
            }
            
            // U平面（full resolution）
            if (!writePlane(u_data, linesize[1], buf_width, buf_height)) {
                return false;
            }
            
            // V平面（full resolution）
            if (!writePlane(v_data, linesize[2], buf_width, buf_height)) {
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_GBRP: {
            // GBR Planar: Plane 0 (G) + Plane 1 (B) + Plane 2 (R)
            // All planes are full resolution
            const uint8_t* g_data = img.planeData(0);
            const uint8_t* b_data = img.planeData(1);
            const uint8_t* r_data = img.planeData(2);
            
            // G平面
            if (!writePlane(g_data, linesize[0], buf_width, buf_height)) {
                return false;
            }
            
            // B平面
            if (!writePlane(b_data, linesize[1], buf_width, buf_height)) {
                return false;
            }
            
            // R平面
            if (!writePlane(r_data, linesize[2], buf_width, buf_height)) {
                return false;
            }
            break;
        }
        
        default:
            LOG4CPLUS_ERROR_FMT(logger_, "Unsupported format: %s",
                    av_get_pix_fmt_name(buf_format));
            return false;
    }
    
    // 5. 累加计数器
    write_count_.fetch_add(1);
    return true;
}

bool BufferWriter::writePlane(const uint8_t* data, int stride, 
                               int width, int height) {
    if (!data) {
        return false;  // 返回true表示"已处理"（虽然未写入）
    }
    
    if (stride == width) {
        // 无padding，直接写入
        size_t written = fwrite(data, 1, width * height, file_);
        return (written == (size_t)(width * height));
    } else {
        // 有padding，逐行写入（去除padding）
        for (int y = 0; y < height; y++) {
            size_t written = fwrite(data + y * stride, 1, width, file_);
            if (written != (size_t)width) {
                return false;
            }
        }
        return true;
    }
    return false;
}

void BufferWriter::close() {
    // ⭐ 编码流模式
    if (output_format_ctx_) {
        // 写入MP4 trailer
        if (output_format_ctx_->pb) {
            av_write_trailer(output_format_ctx_);
        }
        
        // 关闭文件
        if (!(output_format_ctx_->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&output_format_ctx_->pb);
        }
        
        // 释放上下文
        avformat_free_context(output_format_ctx_);
        output_format_ctx_ = nullptr;
        video_stream_index_ = -1;
        packet_count_ = 0;
        
        LOG4CPLUS_INFO_FMT(logger_, "Closed (written %d packets)", 
               write_count_.load());
        return;
    }
    
    // ========== 图像模式（原有逻辑）==========
    if (file_) {
        fflush(file_);
        fclose(file_);
        file_ = nullptr;
        
        LOG4CPLUS_INFO_FMT(logger_, "Closed (written %d frames)", 
               write_count_.load());
    }
}

// ========== 内部辅助方法实现 ==========

bool BufferWriter::isSupportedFormat(AVPixelFormat format) {
    switch (format) {
        // ========== YUV格式（8种）==========
        
        // YUV400（灰度）
        case AV_PIX_FMT_GRAY8:        // YUV400 8-bit
        case AV_PIX_FMT_GRAY10LE:     // YUV400 P010
        
        // YUV420 NV12（UV交错）
        case AV_PIX_FMT_NV12:         // YUV420 8-bit NV12 ⭐
        case AV_PIX_FMT_P010LE:       // YUV420 NV12 P010
        
        // YUV420 NV21（VU交错）
        case AV_PIX_FMT_NV21:         // YUV420 8-bit NV21
        
        // YUV420 Planar
        case AV_PIX_FMT_YUV420P:      // YUV420 8-bit Planar ⭐
        case AV_PIX_FMT_YUV420P10LE:  // YUV420 P010 (planar)
        
        // YUV422 Planar（新增）
        case AV_PIX_FMT_YUV422P:      // YUV422 Planar 8-bit
        
        // YUV444 Planar（新增）
        case AV_PIX_FMT_YUV444P:      // YUV444 Planar 8-bit
        
        // ========== RGB格式（13种）==========
        
        // 8bit RGB（无Alpha）
        case AV_PIX_FMT_RGB24:        // RGB888
        case AV_PIX_FMT_BGR24:        // BGR888
        
        // 8bit RGB（带Alpha）
        case AV_PIX_FMT_ARGB:         // ARGB8888
        case AV_PIX_FMT_ABGR:         // ABGR8888
        case AV_PIX_FMT_RGBA:         // RGBA8888
        case AV_PIX_FMT_BGRA:         // BGRA8888 ⭐
        
        // 8bit RGB（带填充）
        case AV_PIX_FMT_RGB0:         // RGBX8888
        case AV_PIX_FMT_BGR0:         // BGRX8888
        case AV_PIX_FMT_0RGB:         // XRGB8888
        case AV_PIX_FMT_0BGR:         // XBGR8888
        
        // 16bit RGB
        case AV_PIX_FMT_RGB48LE:      // RGB161616
        case AV_PIX_FMT_BGR48LE:      // BGR161616
        
        // RGB Planar（新增）
        case AV_PIX_FMT_GBRP:         // GBR Planar 8-bit
            return true;
            
        default:
            return false;
    }
}

size_t BufferWriter::calculateFrameSize(AVPixelFormat format, int width, int height) {
    size_t size = 0;
    
    switch (format) {
        // ========== YUV格式 ==========
        
        // YUV400（灰度）
        case AV_PIX_FMT_GRAY8:
            // YYYY... (W×H)
            size = width * height;
            break;
            
        case AV_PIX_FMT_GRAY10LE:
            // YYYY... (16bit/pixel, W×H×2)
            size = width * height * 2;
            break;
        
        // YUV420 NV12/NV21（Semi-planar）
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21:
            // YYYY... (W×H) + UVUV... (W×H/2)
            // = W×H×1.5
            size = width * height * 3 / 2;
            break;
        
        // YUV420 NV12 P010
        case AV_PIX_FMT_P010LE:
            // YYYY... (16bit×W×H) + UVUV... (16bit×W×H/2)
            // = W×H×3 (每个分量16bit)
            size = width * height * 3;
            break;
        
        // YUV420 Planar 8bit
        case AV_PIX_FMT_YUV420P:
            // YYYY... (W×H) + UUUU... (W×H/4) + VVVV... (W×H/4)
            // = W×H×1.5
            size = width * height * 3 / 2;
            break;
        
        // YUV420 Planar 10bit
        case AV_PIX_FMT_YUV420P10LE:
            // YYYY... (16bit×W×H) + UUUU... (16bit×W×H/4) + VVVV... (16bit×W×H/4)
            // = W×H×2 + W×H/2 + W×H/2 = W×H×3
            size = width * height * 3;
            break;
        
        // YUV422 Planar
        case AV_PIX_FMT_YUV422P:
            // YYYY... (W×H) + UUUU... (W×H/2) + VVVV... (W×H/2)
            // = W×H + W×H/2 + W×H/2 = W×H×2
            size = width * height * 2;
            break;
        
        // YUV444 Planar
        case AV_PIX_FMT_YUV444P:
            // YYYY... (W×H) + UUUU... (W×H) + VVVV... (W×H)
            // = W×H×3
            size = width * height * 3;
            break;
        
        // ========== RGB格式 ==========
        
        // 8bit RGB（24bit/pixel）
        case AV_PIX_FMT_RGB24:
        case AV_PIX_FMT_BGR24:
            size = width * height * 3;
            break;
        
        // 8bit RGB（32bit/pixel）
        case AV_PIX_FMT_ARGB:
        case AV_PIX_FMT_ABGR:
        case AV_PIX_FMT_RGBA:
        case AV_PIX_FMT_BGRA:
        case AV_PIX_FMT_RGB0:
        case AV_PIX_FMT_BGR0:
        case AV_PIX_FMT_0RGB:
        case AV_PIX_FMT_0BGR:
            size = width * height * 4;
            break;
        
        // 16bit RGB（48bit/pixel）
        case AV_PIX_FMT_RGB48LE:
        case AV_PIX_FMT_BGR48LE:
            size = width * height * 6;
            break;
        
        // Planar RGB
        case AV_PIX_FMT_GBRP:
            // GGGG... (W×H) + BBBB... (W×H) + RRRR... (W×H)
            // = W×H×3
            size = width * height * 3;
            break;
        
        default:
            size = 0;
            break;
    }
    
    return size;
}

const char* BufferWriter::getFormatName(AVPixelFormat format) {
    // 使用FFmpeg的标准函数
    const char* name = av_get_pix_fmt_name(format);
    return name ? name : "UNKNOWN";
}

// ========== 编码流模式实现 ==========

/**
 * @brief 打开编码流文件（容器格式模式）
 * 详细说明参见头文件注释
 */
bool BufferWriter::openEncoded(const char* path, const AVCodecParameters* codec_params, const AVRational& time_base) {
    // 1. 参数校验
    if (!path || !codec_params) {
        LOG4CPLUS_ERROR(logger_, "Error: Invalid parameters for encoded mode");
        return false;
    }
    
    // 1.5 ⭐ 保存时间基（用于时间戳转换）
    time_base_ = time_base;
    
    // 2. 如果已打开，先关闭
    if (file_ || output_format_ctx_) {
        close();
    }
    
    // 3. 分配输出上下文（根据文件扩展名自动识别格式）
    int ret = avformat_alloc_output_context2(&output_format_ctx_, nullptr, nullptr, path);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG4CPLUS_ERROR_FMT(logger_, "Error: Failed to allocate output context: %s", errbuf);
        return false;
    }
    
    // 4. 创建输出视频流
    AVStream* out_stream = avformat_new_stream(output_format_ctx_, nullptr);
    if (!out_stream) {
        LOG4CPLUS_ERROR(logger_, "Error: Failed to create output stream");
        avformat_free_context(output_format_ctx_);
        output_format_ctx_ = nullptr;
        return false;
    }
    
    video_stream_index_ = out_stream->index;
    
    // 5. 复制编解码器参数
    ret = avcodec_parameters_copy(out_stream->codecpar, codec_params);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG4CPLUS_ERROR_FMT(logger_, "Error: Failed to copy codec parameters: %s", errbuf);
        avformat_free_context(output_format_ctx_);
        output_format_ctx_ = nullptr;
        return false;
    }
    
    // 6. 设置 codec_tag 为 0（让 muxer 自动选择）
    out_stream->codecpar->codec_tag = 0;
    
    // 7. 设置时间基（默认假设25fps）
    time_base_ = {1, 25};
    out_stream->time_base = time_base_;
    
    // 7.5 ⭐ 设置自动处理负时间戳（让 FFmpeg 自动归一化）
    //     AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE：自动将所有时间戳平移为非负数
    //     解决 RTSP 流时间戳不从 0 开始的问题
    output_format_ctx_->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE;
    
    // 8. 打开输出文件
    if (!(output_format_ctx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&output_format_ctx_->pb, path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG4CPLUS_ERROR_FMT(logger_, "Error: Failed to open output file: %s", errbuf);
            avformat_free_context(output_format_ctx_);
            output_format_ctx_ = nullptr;
            return false;
        }
    }
    
    // 9. 写入文件头
    ret = avformat_write_header(output_format_ctx_, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG4CPLUS_ERROR_FMT(logger_, "Error: Failed to write header: %s", errbuf);
        if (!(output_format_ctx_->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&output_format_ctx_->pb);
        }
        avformat_free_context(output_format_ctx_);
        output_format_ctx_ = nullptr;
        return false;
    }
    
    // 10. 重置计数器和时间戳跟踪
    packet_count_ = 0;
    write_count_.store(0);
    last_dts_ = AV_NOPTS_VALUE;  // 重置上一个DTS
    
    // ⭐ v2.15: 重置时间戳偏移量（用于处理 RTSP 流从非零时间戳开始的情况）
    first_pts_ = AV_NOPTS_VALUE;
    first_dts_ = AV_NOPTS_VALUE;
    
    // 11. 打印成功信息
    LOG4CPLUS_INFO(logger_, "");
    LOG4CPLUS_INFO_FMT(logger_, "Opened (encoded mode): %s", path);
    LOG4CPLUS_INFO_FMT(logger_, "  Format: %s", output_format_ctx_->oformat->name);
    LOG4CPLUS_INFO_FMT(logger_, "  Codec: %s", avcodec_get_name(out_stream->codecpar->codec_id));
    LOG4CPLUS_INFO_FMT(logger_, "  Resolution: %dx%d", out_stream->codecpar->width, out_stream->codecpar->height);
    
    return true;
}

bool BufferWriter::writeEncoded(const Buffer* buffer) {
    if (!output_format_ctx_) {
        LOG4CPLUS_ERROR(logger_, "Error: Not in encoded mode");
        return false;
    }
    
    if (!buffer || !buffer->isValid()) {
        LOG4CPLUS_ERROR(logger_, "Error: Invalid buffer");
        return false;
    }
    
    // 1. ⭐ 直接从 Buffer 获取 AVPacket（包含所有数据和元数据）
    AVPacket* src_packet = buffer->getAVPacket();
    if (!src_packet || !src_packet->data) {
        LOG4CPLUS_ERROR(logger_, "Error: Buffer has no AVPacket data");
        return false;
    }
    
    // 2. ⭐ 创建临时 AVPacket（栈上，零内存分配）
    AVPacket pkt;
    av_init_packet(&pkt);
    
    // 3. ⭐ 引用源 packet 的数据（零拷贝，只增加引用计数）
    int ret = av_packet_ref(&pkt, src_packet);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG4CPLUS_ERROR_FMT(logger_, "Error: Failed to reference packet: %s", errbuf);
        return false;
    }
    
    // 4. 修改流索引（指向输出文件的视频流）
    pkt.stream_index = video_stream_index_;
    
    // 5. ⭐ 时间戳校验和修正（确保单调递增）
    AVStream* out_stream = output_format_ctx_->streams[video_stream_index_];
    
    // 5.1 转换时间基
    av_packet_rescale_ts(&pkt, time_base_, out_stream->time_base);
    
    // 5.2 ⭐⭐ v2.16: 生成单调递增的时间戳（替代源时间戳）
    //     解决 RTSP 流时间戳不连续/混乱的问题
    //     策略：完全忽略源时间戳，基于帧率生成新的时间戳序列
    //     - 假设恒定帧率（从输出流的 avg_frame_rate 获取）
    //     - 时间戳从 0 开始，每帧递增固定 duration
    
    // 计算每帧的 duration（基于输出流的时间基）
    int64_t frame_duration = av_rescale_q(1, av_inv_q(out_stream->avg_frame_rate), out_stream->time_base);
    if (frame_duration <= 0) {
        frame_duration = av_rescale_q(1, (AVRational){1, 25}, out_stream->time_base); // 默认25fps
    }
    
    // 生成新的时间戳
    pkt.dts = packet_count_ * frame_duration;
    pkt.pts = pkt.dts;  // 假设无 B 帧，PTS = DTS
    
    // ⭐ 调试：打印前几个包的时间戳
    if (packet_count_ < 3) {
        LOG4CPLUS_DEBUG_FMT(logger_, "Packet #%lld: Generated DTS=%lld, PTS=%lld (frame_duration=%lld)",
                     (long long)packet_count_, (long long)pkt.dts, (long long)pkt.pts, (long long)frame_duration);
    }
    
    // ⭐ v2.16: 时间戳已由上面的代码生成，无需额外检查
    
    // 更新 last_dts_
    last_dts_ = pkt.dts;
    
    // 6. ⭐ 写入文件（会自动 unref，但不影响源 Buffer 的 AVPacket）
    ret = av_interleaved_write_frame(output_format_ctx_, &pkt);
    
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG4CPLUS_ERROR_FMT(logger_, "Error: Failed to write packet: %s", errbuf);
        return false;
    }
    
    // 7. 累加计数器
    packet_count_++;
    write_count_.fetch_add(1);
    
    return true;
}

} // namespace io
} // namespace consumptionline
