#pragma once

#include "buffer/bufferpool/Buffer.hpp"
#include <atomic>
#include <cstdio>

// FFmpeg标准格式定义
extern "C" {
#include <libavutil/pixfmt.h>
}

namespace productionline {
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
 * 支持的格式（共18种，基于ARCHITECTURE.md表格）：
 * 
 * YUV格式（6种）：
 *   - AV_PIX_FMT_GRAY8       (YUV400 8-bit)
 *   - AV_PIX_FMT_GRAY10LE    (YUV400 P010)
 *   - AV_PIX_FMT_NV12        (YUV420 8-bit NV12) ⭐ 最常用
 *   - AV_PIX_FMT_P010LE      (YUV420 NV12 P010)
 *   - AV_PIX_FMT_NV21        (YUV420 8-bit NV21)
 *   - AV_PIX_FMT_YUV420P10LE (YUV420 P010 planar)
 * 
 * RGB格式（12种）：
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
 * 
 * 使用示例：
 * ```cpp
 * BufferWriter writer;
 * writer.open("output.yuv", AV_PIX_FMT_NV12, 1920, 1080);
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
     * @brief 打开输出文件
     * 
     * @param path 文件路径
     * @param format 像素格式（使用FFmpeg标准AVPixelFormat）
     * @param width 图像宽度
     * @param height 图像高度
     * @return true 成功，false 失败
     * 
     * @note 支持18种格式，详见类注释
     * @note 如果文件已打开，会先关闭再重新打开
     */
    bool open(const char* path, 
              AVPixelFormat format,
              int width, 
              int height);
    
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
     * @brief 检查文件是否已打开
     * @return true 如果文件已打开，否则返回 false
     */
    bool isOpen() const { return file_ != nullptr; }

private:
    // ============ 核心成员 ============
    FILE* file_;                     // 文件句柄
    AVPixelFormat format_;           // 像素格式（FFmpeg标准）
    int width_;                      // 图像宽度
    int height_;                     // 图像高度
    std::atomic<int> write_count_;   // 写入计数器（原子，线程安全）
    
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
};

} // namespace io
} // namespace productionline
