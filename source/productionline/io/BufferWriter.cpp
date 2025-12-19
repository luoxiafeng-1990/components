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
    // 旧版简单写入（向后兼容）
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
    // 1. 获取图像元数据
    AVPixelFormat buf_format = buffer->getImageFormat();
    int buf_width = buffer->getImageWidth();
    int buf_height = buffer->getImageHeight();
    
    // 2. 验证格式（如果open时指定了格式）
    if (format_ != AV_PIX_FMT_NONE && buf_format != format_) {
        LOG_ERROR_FMT("[BufferWriter] Format mismatch: expected %s, got %s",
                av_get_pix_fmt_name(format_), av_get_pix_fmt_name(buf_format));
        return false;
    }
    
    // 3. 按格式系列分组处理（清晰且减少代码重复）
    bool success = false;
    
    switch (buf_format) {
        // ========== YUV 4:2:0 Semi-Planar (2 planes: Y + UV交错) ==========
        case AV_PIX_FMT_NV12:       // YUV420 8-bit NV12
        case AV_PIX_FMT_NV21:       // YUV420 8-bit NV21
            success = writeSemiPlanarYUV(buffer, 1);  // 8bit = 1 byte/component
            break;
            
        case AV_PIX_FMT_P010LE:     // YUV420 NV12 P010 (10bit in 16bit)
            success = writeSemiPlanarYUV(buffer, 2);  // 16bit = 2 bytes/component
            break;
        
        // ========== YUV 4:2:0 Planar (3 planes: Y + U + V) ==========
        case AV_PIX_FMT_YUV420P10LE:  // YUV420 P010 planar (10bit in 16bit)
            success = writePlanarYUV420(buffer, 2);   // 16bit = 2 bytes/component
            break;
        
        // ========== YUV 4:0:0 (Grayscale, 1 plane) ==========
        case AV_PIX_FMT_GRAY8:        // YUV400 8-bit
            success = writeGrayscale(buffer, 1);      // 1 byte/pixel
            break;
            
        case AV_PIX_FMT_GRAY10LE:     // YUV400 P010 (10bit in 16bit)
            success = writeGrayscale(buffer, 2);      // 2 bytes/pixel
            break;
        
        // ========== RGB Packed 24bpp (3 bytes/pixel) ==========
        case AV_PIX_FMT_RGB24:        // RGB888
        case AV_PIX_FMT_BGR24:        // BGR888
            success = writePackedRGB(buffer, 3);
            break;
        
        // ========== RGB Packed 32bpp (4 bytes/pixel) ==========
        case AV_PIX_FMT_ARGB:         // ARGB8888
        case AV_PIX_FMT_ABGR:         // ABGR8888
        case AV_PIX_FMT_RGBA:         // RGBA8888
        case AV_PIX_FMT_BGRA:         // BGRA8888
        case AV_PIX_FMT_RGB0:         // RGBX8888
        case AV_PIX_FMT_BGR0:         // BGRX8888
        case AV_PIX_FMT_0RGB:         // XRGB8888
        case AV_PIX_FMT_0BGR:         // XBGR8888
            success = writePackedRGB(buffer, 4);
            break;
        
        // ========== RGB Packed 48bpp (6 bytes/pixel, 16bit per channel) ==========
        case AV_PIX_FMT_RGB48LE:      // RGB161616
        case AV_PIX_FMT_BGR48LE:      // BGR161616
            success = writePackedRGB(buffer, 6);
            break;
        
        default:
            LOG_ERROR_FMT("[BufferWriter] Unsupported format: %s",
                    av_get_pix_fmt_name(buf_format));
            return false;
    }
    
    // 4. 成功则累加计数器
    if (success) {
        write_count_.fetch_add(1);
    }
    
    return success;
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

bool BufferWriter::writeSemiPlanarYUV(const Buffer* buffer, int bytes_per_component) {
    /*
     * Semi-Planar YUV 4:2:0 内存布局：
     * 
     * Plane 0 (Y):  [Y0 Y1 Y2 Y3 ...]  
     *               width * height * bytes_per_component
     * 
     * Plane 1 (UV): [U0 V0 U1 V1 ...]  (交错存储)
     *               width * (height/2) * bytes_per_component
     * 
     * bytes_per_component 说明：
     *   - 1: NV12/NV21 (8-bit)
     *   - 2: P010LE (10-bit in 16-bit storage, little-endian)
     * 
     * 应用场景：
     *   - NV12: 最常用的硬件加速格式 (GPU/VPU/ISP)
     *   - NV21: Android Camera常用
     *   - P010LE: HDR视频（10-bit）
     */
    
    int buf_width = buffer->getImageWidth();
    int buf_height = buffer->getImageHeight();
    const int* linesize = buffer->getImageLinesize();
    
    // 写入 Y 平面
    const uint8_t* y_data = buffer->getImagePlaneData(0);
    if (!writePlane(y_data, linesize[0], 
                   buf_width * bytes_per_component, buf_height)) {
        fprintf(stderr, "[BufferWriter] Failed to write Y plane (semi-planar)\n");
        return false;
    }
    
    // 写入 UV 平面（交错存储，高度减半）
    const uint8_t* uv_data = buffer->getImagePlaneData(1);
    if (!writePlane(uv_data, linesize[1], 
                   buf_width * bytes_per_component, buf_height / 2)) {
        fprintf(stderr, "[BufferWriter] Failed to write UV plane (semi-planar)\n");
        return false;
    }
    
    return true;
}

bool BufferWriter::writePlanarYUV420(const Buffer* buffer, int bytes_per_component) {
    /*
     * Planar YUV 4:2:0 内存布局：
     * 
     * Plane 0 (Y): [Y0 Y1 Y2 Y3 ...]     
     *              width * height * bytes_per_component
     * 
     * Plane 1 (U): [U0 U1 U2 U3 ...]     
     *              (width/2) * (height/2) * bytes_per_component
     * 
     * Plane 2 (V): [V0 V1 V2 V3 ...]     
     *              (width/2) * (height/2) * bytes_per_component
     * 
     * bytes_per_component 说明：
     *   - 1: YUV420P (8-bit, 标准I420)
     *   - 2: YUV420P10LE (10-bit in 16-bit storage)
     * 
     * 应用场景：
     *   - YUV420P: FFmpeg默认格式，软件编解码常用
     *   - YUV420P10LE: HDR视频处理
     */
    
    int buf_width = buffer->getImageWidth();
    int buf_height = buffer->getImageHeight();
    const int* linesize = buffer->getImageLinesize();
    
    // 写入 Y 平面
    if (!writePlane(buffer->getImagePlaneData(0), linesize[0], 
                   buf_width * bytes_per_component, buf_height)) {
        fprintf(stderr, "[BufferWriter] Failed to write Y plane (planar)\n");
        return false;
    }
    
    // 写入 U 平面
    if (!writePlane(buffer->getImagePlaneData(1), linesize[1], 
                   buf_width / 2 * bytes_per_component, buf_height / 2)) {
        fprintf(stderr, "[BufferWriter] Failed to write U plane (planar)\n");
        return false;
    }
    
    // 写入 V 平面
    if (!writePlane(buffer->getImagePlaneData(2), linesize[2], 
                   buf_width / 2 * bytes_per_component, buf_height / 2)) {
        fprintf(stderr, "[BufferWriter] Failed to write V plane (planar)\n");
        return false;
    }
    
    return true;
}

bool BufferWriter::writePackedRGB(const Buffer* buffer, int bytes_per_pixel) {
    /*
     * Packed RGB 内存布局（单 plane）：
     * 
     * 3 bytes/pixel: RGB24, BGR24
     *   [R0 G0 B0 | R1 G1 B1 | R2 G2 B2 | ...]
     *   总大小: width * height * 3
     * 
     * 4 bytes/pixel: ARGB8888, RGBA8888, BGRA8888, etc.
     *   [A0 R0 G0 B0 | A1 R1 G1 B1 | ...] (ARGB顺序示例)
     *   总大小: width * height * 4
     *   注意：不同格式的ARGB顺序不同
     * 
     * 6 bytes/pixel: RGB48LE, BGR48LE (16-bit per channel)
     *   [R0_L R0_H G0_L G0_H B0_L B0_H | R1_L R1_H ...]
     *   总大小: width * height * 6
     *   注意：LE表示little-endian字节序
     * 
     * 应用场景：
     *   - RGB24/BGR24: 标准RGB，无Alpha通道
     *   - RGBA/BGRA: 图形渲染、合成
     *   - RGB48LE/BGR48LE: HDR图像、专业摄影
     */
    
    int buf_width = buffer->getImageWidth();
    int buf_height = buffer->getImageHeight();
    const int* linesize = buffer->getImageLinesize();
    
    const uint8_t* rgb_data = buffer->getImagePlaneData(0);
    if (!writePlane(rgb_data, linesize[0], 
                   buf_width * bytes_per_pixel, buf_height)) {
        fprintf(stderr, "[BufferWriter] Failed to write RGB plane (%d bpp)\n", 
                bytes_per_pixel * 8);
        return false;
    }
    
    return true;
}

bool BufferWriter::writeGrayscale(const Buffer* buffer, int bytes_per_pixel) {
    /*
     * Grayscale 内存布局（单 plane，只有亮度信息）：
     * 
     * 1 byte/pixel: GRAY8 (YUV400 8-bit)
     *   [Y0 Y1 Y2 Y3 Y4 ...]
     *   总大小: width * height
     * 
     * 2 bytes/pixel: GRAY10LE (YUV400 P010, 10-bit in 16-bit)
     *   [Y0_L Y0_H | Y1_L Y1_H | Y2_L Y2_H ...]
     *   总大小: width * height * 2
     *   注意：10-bit数据存储在16-bit中，高6位补0
     * 
     * 应用场景：
     *   - GRAY8: 黑白图像、文档扫描、监控
     *   - GRAY10LE: 科学成像、红外热成像
     */
    
    int buf_width = buffer->getImageWidth();
    int buf_height = buffer->getImageHeight();
    const int* linesize = buffer->getImageLinesize();
    
    const uint8_t* gray_data = buffer->getImagePlaneData(0);
    if (!writePlane(gray_data, linesize[0], 
                   buf_width * bytes_per_pixel, buf_height)) {
        fprintf(stderr, "[BufferWriter] Failed to write grayscale plane (%d-bit)\n",
                bytes_per_pixel * 8);
        return false;
    }
    
    return true;
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
