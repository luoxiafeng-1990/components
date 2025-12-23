#include "productionline/io/BufferWriter.hpp"
#include "common/Logger.hpp"
#include <cstring>
#include <cerrno>

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace productionline {
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
    , writer_id_(++next_id_)
    , log_prefix_("[BufferWriter::" + std::to_string(writer_id_) + "]")
{
    // 获取logger
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
    
    // 打印生命周期开始
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, log_prefix_ << " " << std::string(67, '='));
    LOG4CPLUS_INFO(logger, log_prefix_ << " 构造");
    LOG4CPLUS_INFO(logger, log_prefix_ << " " << std::string(67, '='));
}

BufferWriter::~BufferWriter() {
    // 获取logger
    auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components"));
    
    // 打印生命周期结束
    LOG4CPLUS_INFO(logger, "");
    LOG4CPLUS_INFO(logger, log_prefix_ << " " << std::string(67, '='));
    LOG4CPLUS_INFO(logger, log_prefix_ << " 析构: 共写入 " << write_count_.load() << " 帧");
    LOG4CPLUS_INFO(logger, log_prefix_ << " " << std::string(67, '='));
    
    close();
}

// ========== 核心接口实现 ==========

bool BufferWriter::open(const char* path, 
                        AVPixelFormat format,
                        int width, 
                        int height) {
    // 1. 参数校验
    if (!path) {
        LOG_ERROR("[BufferWriter] Error: Invalid path (nullptr)");
        return false;
    }
    
    if (width <= 0 || height <= 0) {
        LOG_ERROR_FMT("[BufferWriter] Error: Invalid dimensions (%dx%d)", 
                width, height);
        return false;
    }
    
    // 2. 检查格式支持
    if (!isSupportedFormat(format)) {
        LOG_ERROR_FMT("[BufferWriter] Error: Unsupported format: %s (%d)",
                av_get_pix_fmt_name(format), format);
        LOG_ERROR("[BufferWriter] Supported formats (18): "
                "GRAY8, GRAY10LE, NV12, P010LE, NV21, YUV420P10LE, "
                "RGB24, BGR24, ARGB, ABGR, RGBA, BGRA, "
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
        LOG_ERROR_FMT("[BufferWriter] Error: Failed to open file: %s "
                "(errno=%d: %s)", path, errno, strerror(errno));
        return false;
    }
    
    // 5. 保存配置
    format_ = format;
    width_ = width;
    height_ = height;
    write_count_.store(0);  // 重置计数器
    
    // 6. 打印成功信息
    LOG_INFO_FMT("[BufferWriter] Opened: %s", path);
    LOG_INFO_FMT("  Format: %s", getFormatName(format_));
    LOG_INFO_FMT("  Resolution: %dx%d", width_, height_);
    LOG_INFO_FMT("  Frame size: %zu bytes", calculateFrameSize(format_, width_, height_));
    
    return true;
}

bool BufferWriter::write(const Buffer* buffer) {
    // 1. 参数校验
    if (!buffer || !file_) {
        LOG_ERROR("[BufferWriter] Error: Invalid buffer or file not opened");
        return false;
    }
    
    // 2. ⭐ 检查Buffer是否有图像元数据
    if (buffer->hasImageMetadata()) {
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
        LOG_ERROR("[BufferWriter::writeSimple] Error: buffer is nullptr");
        return false;
    }
    
    // 2. 旧版简单写入（向后兼容）
    if (!buffer->isValid()) {
        LOG_ERROR("[BufferWriter] Error: Buffer validation failed");
        return false;
    }
    
    void* data = buffer->getVirtualAddress();
    size_t buffer_size = buffer->size();
    
    if (!data || buffer_size == 0) {
        LOG_ERROR("[BufferWriter] Error: Buffer has no data or zero size");
        return false;
    }
    
    size_t expected_size = calculateFrameSize(format_, width_, height_);
    
    if (buffer_size < expected_size) {
        LOG_ERROR("[BufferWriter] Error: Buffer size mismatch");
        LOG_ERROR_FMT("  Expected: %zu bytes (format=%s, %dx%d)",
                expected_size, getFormatName(format_), width_, height_);
        LOG_ERROR_FMT("  Got: %zu bytes", buffer_size);
        return false;
    }
    
    size_t written = fwrite(data, 1, expected_size, file_);
    if (written != expected_size) {
        LOG_ERROR("[BufferWriter] Error: Write failed");
        LOG_ERROR_FMT("  Expected to write: %zu bytes", expected_size);
        LOG_ERROR_FMT("  Actually wrote: %zu bytes", written);
        LOG_ERROR_FMT("  errno=%d: %s", errno, strerror(errno));
        return false;
    }
    
    write_count_.fetch_add(1);
    return true;
}

bool BufferWriter::writeWithMetadata(const Buffer* buffer) {
    // 1. 参数校验：空指针检查（防御性编程）
    if (!buffer) {
        LOG_ERROR("[BufferWriter::writeWithMetadata] Error: buffer is nullptr");
        return false;
    }
    
    // 2. 获取图像元数据
    AVPixelFormat buf_format = buffer->getImageFormat();
    int buf_width = buffer->getImageWidth();
    int buf_height = buffer->getImageHeight();
    const int* linesize = buffer->getImageLinesize();
    
    // 3. 验证格式（如果open时指定了格式）
    if (format_ != AV_PIX_FMT_NONE && buf_format != format_) {
        LOG_ERROR_FMT("[BufferWriter] Format mismatch: expected %s, got %s",
                av_get_pix_fmt_name(format_), av_get_pix_fmt_name(buf_format));
        return false;
    }
    
    // 4. 根据格式写入数据
    switch (buf_format) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21: {
            // Semi-planar: Plane 0 (Y) + Plane 1 (UV/VU)
            const uint8_t* y_data = buffer->getImagePlaneData(0);
            const uint8_t* uv_data = buffer->getImagePlaneData(1);
            
            // 写入Y平面（去除stride）
            if (!writePlane(y_data, linesize[0], buf_width, buf_height)) {
                return false;
            }
            
            // 写入UV平面（去除stride，高度为height/2）
            if (!writePlane(uv_data, linesize[1], buf_width, buf_height / 2)) {
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV420P10LE: {
            // Planar: Plane 0 (Y) + Plane 1 (U) + Plane 2 (V)
            int bytes_per_pixel = (buf_format == AV_PIX_FMT_YUV420P10LE) ? 2 : 1;
            
            // Y平面
            if (!writePlane(buffer->getImagePlaneData(0), linesize[0], 
                          buf_width * bytes_per_pixel, buf_height)) {
                return false;
            }
            
            // U平面
            if (!writePlane(buffer->getImagePlaneData(1), linesize[1], 
                          buf_width / 2 * bytes_per_pixel, buf_height / 2)) {
                return false;
            }
            
            // V平面
            if (!writePlane(buffer->getImagePlaneData(2), linesize[2], 
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
            const uint8_t* rgb_data = buffer->getImagePlaneData(0);
            if (!writePlane(rgb_data, linesize[0], buf_width * 4, buf_height)) {
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_RGB24:
        case AV_PIX_FMT_BGR24: {
            // Packed RGB: 单plane，3 bytes/pixel
            const uint8_t* rgb_data = buffer->getImagePlaneData(0);
            if (!writePlane(rgb_data, linesize[0], buf_width * 3, buf_height)) {
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_RGB48LE:
        case AV_PIX_FMT_BGR48LE: {
            // Packed RGB: 单plane，6 bytes/pixel
            const uint8_t* rgb_data = buffer->getImagePlaneData(0);
            if (!writePlane(rgb_data, linesize[0], buf_width * 6, buf_height)) {
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_GRAY8: {
            // 灰度：单plane，1 byte/pixel
            const uint8_t* gray_data = buffer->getImagePlaneData(0);
            if (!writePlane(gray_data, linesize[0], buf_width, buf_height)) {
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_GRAY10LE: {
            // 灰度10bit：单plane，2 bytes/pixel
            const uint8_t* gray_data = buffer->getImagePlaneData(0);
            if (!writePlane(gray_data, linesize[0], buf_width * 2, buf_height)) {
                return false;
            }
            break;
        }
        
        case AV_PIX_FMT_P010LE: {
            // YUV420 P010: Semi-planar 16bit
            const uint8_t* y_data = buffer->getImagePlaneData(0);
            const uint8_t* uv_data = buffer->getImagePlaneData(1);
            
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
        
        default:
            LOG_ERROR_FMT("[BufferWriter] Unsupported format: %s",
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
        LOG_ERROR("[BufferWriter] Error: plane data is nullptr");
        return false;
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
                LOG_ERROR_FMT("[BufferWriter] Error: Write plane failed at row %d", y);
                return false;
            }
        }
        return true;
    }
}

void BufferWriter::close() {
    if (file_) {
        fflush(file_);
        fclose(file_);
        file_ = nullptr;
        
        LOG_INFO_FMT("[BufferWriter] Closed (written %d frames)", 
               write_count_.load());
    }
}

// ========== 内部辅助方法实现 ==========

bool BufferWriter::isSupportedFormat(AVPixelFormat format) {
    switch (format) {
        // ========== YUV格式（6种）==========
        
        // YUV400（灰度）
        case AV_PIX_FMT_GRAY8:        // YUV400 8-bit
        case AV_PIX_FMT_GRAY10LE:     // YUV400 P010
        
        // YUV420 NV12（UV交错）
        case AV_PIX_FMT_NV12:         // YUV420 8-bit NV12 ⭐
        case AV_PIX_FMT_P010LE:       // YUV420 NV12 P010
        
        // YUV420 NV21（VU交错）
        case AV_PIX_FMT_NV21:         // YUV420 8-bit NV21
        
        // YUV420 Planar
        case AV_PIX_FMT_YUV420P10LE:  // YUV420 P010 (planar)
        
        // ========== RGB格式（12种）==========
        
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
        
        // YUV420 Planar 10bit
        case AV_PIX_FMT_YUV420P10LE:
            // YYYY... (16bit×W×H) + UUUU... (16bit×W×H/4) + VVVV... (16bit×W×H/4)
            // = W×H×2 + W×H/2 + W×H/2 = W×H×3
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

} // namespace io
} // namespace productionline
