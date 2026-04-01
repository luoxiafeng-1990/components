#pragma once

#include "buffer/bufferpool/Buffer.hpp"
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <atomic>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <cstdio>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <string>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

// FFmpeg标准格式定义
extern "C" {
#include <libavutil/pixfmt.h>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <libavcodec/avcodec.h>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <libavformat/avformat.h>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
}

namespace consumptionline {
namespace io {

/**
 * @brief BufferWriter - Buffer输出工具（简化版）
 * 
 * 设计原则：
 * - 使用FFmpeg标准格式定义（AVPixelFormat）
 * - 只保存裸YUV/RGB数据（无容器格式）
 * - 接口极简化（open/close/write）
 * - 原子计数器（线程安全）
 * 
 * 支持的格式（共21种）：
 * 
 * YUV格式（8种）：
 *   - AV_PIX_FMT_GRAY8       (YUV400 8-bit)
 *   - AV_PIX_FMT_GRAY10LE    (YUV400 P010)
 *   - AV_PIX_FMT_NV12        (YUV420 8-bit NV12) ⭐ 最常用
 *   - AV_PIX_FMT_P010LE      (YUV420 NV12 P010)
 *   - AV_PIX_FMT_NV21        (YUV420 8-bit NV21)
 *   - AV_PIX_FMT_YUV420P10LE (YUV420 P010 planar)
 *   - AV_PIX_FMT_YUV422P     (YUV422 Planar) ⭐ 新增
 *   - AV_PIX_FMT_YUV444P     (YUV444 Planar) ⭐ 新增
 * 
 * RGB格式（13种）：
 *   - AV_PIX_FMT_RGB24       (RGB888)
 *   - AV_PIX_FMT_BGR24       (BGR888)
 *   - AV_PIX_FMT_ARGB        (ARGB8888)
 *   - AV_PIX_FMT_ABGR        (ABGR8888)
 *   - AV_PIX_FMT_RGBA        (RGBA8888)
 *   - AV_PIX_FMT_BGRA        (BGRA8888) ⭐ Windows常用
 *   - AV_PIX_FMT_RGB0        (RGBX8888)
 *   - AV_PIX_FMT_BGR0        (BGRX8888)
 *   - AV_PIX_FMT_0RGB        (XRGB8888)
 *   - AV_PIX_FMT_0BGR        (XBGR8888)
 *   - AV_PIX_FMT_RGB48LE     (RGB161616)
 *   - AV_PIX_FMT_BGR48LE     (BGR161616)
 *   - AV_PIX_FMT_GBRP        (GBR Planar) ⭐ 新增
 * 
 * 使用示例：
 * ```cpp
 * // 保存原始 YUV 数据
 * BufferWriter writer;
 * writer.openRaw("output.yuv", AV_PIX_FMT_NV12, 1920, 1080);
 * 
 * while (running) {
 *     Buffer* buffer = pool->acquireFilled(true, 100);
 *     if (buffer) {
 *         writer.write(buffer);  // 自动累加计数
 *         pool->releaseFilled(buffer);
 *     }
 * }
 * 
 * writer.close();
 * printf("Written: %d frames\n", writer.getWriteCount());
 * ```
 */
class BufferWriter {
public:
    /**
     * @brief 构造函数
     */
    BufferWriter();
    
    /**
     * @brief 析构函数（自动关闭文件）
     */
    ~BufferWriter();
    
    /**
     * @brief 禁止拷贝构造
     */
    BufferWriter(const BufferWriter&) = delete;
    
    /**
     * @brief 禁止拷贝赋值
     */
    BufferWriter& operator=(const BufferWriter&) = delete;
    
    // ============ 核心接口 ============
    
    /**
     * @brief 打开原始图像数据文件（裸数据模式）
     * 
     * @param path 输出文件路径（建议扩展名：.raw）
     * @param format 像素格式（AVPixelFormat，支持21种格式：YUV 8种 + RGB 13种）
     * @param width 图像宽度（像素，必须与实际输出一致）
     * @param height 图像高度（像素，必须与实际输出一致）
     * @return true 成功，false 失败
     * 
     * @note 调用顺序：openRaw() → write() × N → close()
     * @note 播放方式：ffplay -f rawvideo -pixel_format nv12 -video_size 1920x1080 output.raw
     */
    bool openRaw(const char* path, 
                 AVPixelFormat format,
                 int width, 
                 int height);
    
    /**
     * @brief 打开编码流文件（容器格式模式）
     * 
     * @param path 输出文件路径（扩展名决定容器格式）
     * @param codec_params 编解码器参数（从 Worker 的 getCodecParameters() 获取）
     * @param time_base 时间基（从 Worker 的 getTimeBase() 获取）
     * @return true 成功，false 失败
     * 
     * @note 支持的容器格式（H.264/H.265，共7种）：
     *       MP4, MKV, MOV, TS, FLV, AVI, 3GP
     * @note 不支持：WebM（需VP8/VP9）, OGG（需Theora）
     * @note 调用顺序：openEncoded() → write() × N → close()
     */
    bool openEncoded(const char* path, 
                     const AVCodecParameters* codec_params,
                     const AVRational& time_base);
    
    /**
     * @brief 写入Buffer
     * 
     * @param buffer Buffer指针（不能为nullptr）
     * @return true 成功，false 失败
     * 
     * @note v2.6: 会自动从Buffer的图像元数据中获取格式、stride等信息
     * @note 如果Buffer有元数据，会根据格式正确处理stride和plane
     * @note 如果Buffer没有元数据，会回退到旧的简单模式
     * @note 成功写入后，写入计数器自动+1（原子操作，线程安全）
     */
    bool write(const Buffer* buffer);
    
    /**
     * @brief 关闭文件
     * 
     * @note 析构函数会自动调用close()
     * @note 重复调用close()是安全的
     */
    void close();
    
    // ============ 状态查询 ============
    
    /**
     * @brief 获取写入次数
     * 
     * @return 成功写入的次数
     * 
     * @note 原子操作，线程安全
     * @note 每次write()成功后自动+1
     */
    int getWriteCount() const { return write_count_.load(); }
    
    /**
     * @brief 获取格式不匹配的帧数
     * 
     * @return 格式不匹配的统计数量
     * 
     * @note ⭐ v2.17：用于统计write()时因格式/尺寸不匹配而拒绝写入的帧数
     * @note 原子操作，线程安全
     */
    int64_t getMismatchCount() const { return mismatch_count_.load(); }
    
    /**
     * @brief 检查文件是否已打开
     * @return true 如果文件已打开，否则返回 false
     */
    bool isOpen() const { return file_ != nullptr; }

private:
    // ============ 核心成员（图像模式）============
    FILE* file_;                     // 文件句柄
    AVPixelFormat format_;           // 像素格式（FFmpeg标准）
    int width_;                      // 图像宽度
    int height_;                     // 图像高度
    std::atomic<int> write_count_;   // 写入计数器（原子，线程安全）
    std::atomic<int64_t> mismatch_count_;  // ⭐ v2.17：格式不匹配统计（原子，线程安全）
    
    // ============ 核心成员（编码流模式）============
    AVFormatContext* output_format_ctx_;  // MP4输出上下文（非空表示编码流模式）
    int video_stream_index_;              // 输出视频流索引
    int64_t packet_count_;                // packet计数器（用于生成时间戳）
    AVRational time_base_;                // 时间基
    int64_t last_dts_;                    // 上一个包的DTS（用于确保单调递增）
    
    // ⭐ v2.15：时间戳重置支持（解决 RTSP 流时间戳不从 0 开始的问题）
    int64_t first_pts_;                   // 第一个包的原始 PTS（用于时间戳重置）
    int64_t first_dts_;                   // 第一个包的原始 DTS（用于时间戳重置）
    
    // 对象ID（用于日志区分）
    uint64_t writer_id_;
    static std::atomic<uint64_t> next_id_;
    
    // 日志前缀（用于清晰标识对象）
    std::string log_prefix_;
    
    // ============ 内部辅助方法 ============
    
    /**
     * @brief 检查格式是否支持
     * @param format FFmpeg像素格式
     * @return true 支持，false 不支持
     */
    static bool isSupportedFormat(AVPixelFormat format);
    
    /**
     * @brief 计算帧大小
     * @param format 像素格式
     * @param width 宽度
     * @param height 高度
     * @return 帧大小（字节）
     */
    static size_t calculateFrameSize(AVPixelFormat format, int width, int height);
    
    /**
     * @brief 获取格式名称（调试用）
     * @param format 像素格式
     * @return 格式名称字符串
     */
    static const char* getFormatName(AVPixelFormat format);
    
    /**
     * @brief 使用元数据写入（v2.6新增）
     * @param buffer Buffer指针（必须有图像元数据）
     * @return true 成功，false 失败
     */
    bool writeWithMetadata(const Buffer* buffer);
    
    /**
     * @brief 简单写入模式（向后兼容）
     * @param buffer Buffer指针
     * @return true 成功，false 失败
     */
    bool writeSimple(const Buffer* buffer);
    
    /**
     * @brief 写入单个plane（去除stride）
     * @param data plane数据指针
     * @param stride plane的stride（字节）
     * @param width 有效数据宽度（字节）
     * @param height plane高度（行数）
     * @return true 成功，false 失败
     */
    bool writePlane(const uint8_t* data, int stride, int width, int height);
    
    /**
     * @brief 写入Semi-Planar YUV (NV12/NV21/P010LE通用)
     * 
     * @param buffer Buffer指针
     * @param bytes_per_component 每个分量的字节数（1=8bit, 2=10/16bit）
     * @return true 成功，false 失败
     * 
     * @note Semi-Planar布局：Plane0(Y) + Plane1(UV交错)
     */
    bool writeSemiPlanarYUV(const Buffer* buffer, int bytes_per_component);
    
    /**
     * @brief 写入Planar YUV420 (YUV420P10LE通用)
     * 
     * @param buffer Buffer指针
     * @param bytes_per_component 每个分量的字节数（1=8bit, 2=10/16bit）
     * @return true 成功，false 失败
     * 
     * @note Planar布局：Plane0(Y) + Plane1(U) + Plane2(V)
     */
    bool writePlanarYUV420(const Buffer* buffer, int bytes_per_component);
    
    /**
     * @brief 写入Packed RGB (所有RGB格式通用)
     * 
     * @param buffer Buffer指针
     * @param bytes_per_pixel 每像素字节数（3/4/6）
     * @return true 成功，false 失败
     * 
     * @note 单plane，packed存储
     */
    bool writePackedRGB(const Buffer* buffer, int bytes_per_pixel);
    
    /**
     * @brief 写入灰度图 (GRAY8/GRAY10LE通用)
     * 
     * @param buffer Buffer指针
     * @param bytes_per_pixel 每像素字节数（1=8bit, 2=10/16bit）
     * @return true 成功，false 失败
     * 
     * @note 单plane，只有Y分量
     */
    bool writeGrayscale(const Buffer* buffer, int bytes_per_pixel);
    
    /**
     * @brief 写入编码流数据（封装成MP4等容器格式）
     * 
     * @param buffer Buffer指针（包含编码后的packet数据）
     * @return true 成功，false 失败
     * 
     * @note 自动生成时间戳（基于packet_count_）
     * @note 自动处理remux（无需转码）
     */
    bool writeEncoded(const Buffer* buffer);
    
    // 日志器
    log4cplus::Logger logger_;
};

} // namespace io
} // namespace consumptionline
