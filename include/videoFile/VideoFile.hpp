#ifndef VIDEOFILE_HPP
#define VIDEOFILE_HPP

#include "IVideoReader.hpp"
#include "VideoReaderFactory.hpp"
#include "../buffer/Buffer.hpp"
#include <memory>
#include <stddef.h>
#include <sys/types.h>

/**
 * VideoFile - 视频文件管理类（门面模式）
 * 
 * 设计模式：门面模式（Facade Pattern）
 * 
 * 职责：
 * - 为用户提供统一、简单的视频文件操作接口
 * - 隐藏底层多种实现（mmap、io_uring等）的复杂性
 * - 自动选择最优的读取实现
 * 
 * 特点：
 * - API 保持不变，向后兼容
 * - 底层实现可以透明切换
 * - 支持自动和手动选择读取器类型
 * 
 * 使用方式：
 * ```cpp
 * VideoFile video;  // 自动选择最优实现
 * video.openRaw(path, width, height, bpp);
 * video.readFrameTo(buffer);
 * ```
 * 
 * 高级用法：
 * ```cpp
 * VideoFile video;
 * video.setReaderType(VideoReaderFactory::ReaderType::IOURING);  // 手动指定
 * video.openRaw(...);
 * ```
 */
class VideoFile {
private:
    // ============ 门面模式：持有具体实现 ============
    std::unique_ptr<IVideoReader> reader_;  // 实际的读取器实现
    VideoReaderFactory::ReaderType preferred_type_;  // 用户偏好的类型

public:
    // ============ 构造/析构 ============
    
    /**
     * 构造函数
     * @param type 读取器类型（默认AUTO，自动选择）
     */
    VideoFile(VideoReaderFactory::ReaderType type = VideoReaderFactory::ReaderType::RTSP);
    
    /**
     * 析构函数
     */
    ~VideoFile();
    
    // 禁止拷贝
    VideoFile(const VideoFile&) = delete;
    VideoFile& operator=(const VideoFile&) = delete;
    
    // ============ 读取器类型控制 ============
    
    /**
     * 设置读取器类型（在 open 之前调用）
     * @param type 读取器类型
     */
    void setReaderType(VideoReaderFactory::ReaderType type);
    
    /**
     * 获取当前读取器类型名称
     * @return 类型名称（如 "MmapVideoReader"）
     */
    const char* getReaderType() const;
    
    // ============ 文件操作（转发到 reader_） ============
    
    bool open(const char* path);
    bool openRaw(const char* path, int width, int height, int bits_per_pixel);
    void close();
    bool isOpen() const;
    
    // ============ 读取操作（转发） ============
    
    bool readFrameTo(Buffer& dest_buffer);
    bool readFrameTo(void* dest_buffer, size_t buffer_size);
    bool readFrameAt(int frame_index, Buffer& dest_buffer);
    bool readFrameAt(int frame_index, void* dest_buffer, size_t buffer_size);
    bool readFrameAtThreadSafe(int frame_index, void* dest_buffer, size_t buffer_size) const;
    
    // ============ 导航操作（转发） ============
    
    bool seek(int frame_index);
    bool seekToBegin();
    bool seekToEnd();
    bool skip(int frame_count);
    
    // ============ 信息查询（转发） ============
    
    int getTotalFrames() const;
    int getCurrentFrameIndex() const;
    size_t getFrameSize() const;
    long getFileSize() const;
    int getWidth() const;
    int getHeight() const;
    int getBytesPerPixel() const;
    const char* getPath() const;
    bool hasMoreFrames() const;
    bool isAtEnd() const;
    
    // ============ 可选依赖注入（透传到 Reader）============
    
    /**
     * 设置BufferPool（透传到底层Reader）
     * 
     * 用于支持零拷贝优化（如RTSP流解码器）
     * 
     * @param pool BufferPool指针
     */
    void setBufferPool(void* pool);
};

#endif // VIDEOFILE_HPP

