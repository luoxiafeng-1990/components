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
 * 使用方式（统一智能接口）：
 * 
 * **编码视频（MP4, AVI, RTSP等）：**
 * ```cpp
 * VideoFile video;
 * video.setReaderType(VideoReaderFactory::ReaderType::FFMPEG);
 * video.open("video.mp4");  // 自动检测格式，无需指定宽高
 * video.readFrameTo(buffer);
 * ```
 * 
 * **Raw视频：**
 * ```cpp
 * VideoFile video;
 * video.setReaderType(VideoReaderFactory::ReaderType::MMAP);
 * video.open("video.raw", 1920, 1080, 32);  // 指定格式参数
 * video.readFrameTo(buffer);
 * ```
 * 
 * **通用方式（推荐）：**
 * ```cpp
 * // 调用者无需关心视频类型，只需传入所有可能需要的参数
 * // 门面类会根据Reader类型自动判断是否使用这些参数
 * video.open(path, width, height, bpp);
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
     * @param type 读取器类型（默认AUTO，自动选择最优实现）
     * 
     * @note 推荐做法：不依赖默认值，显式调用 setReaderType() 来明确读取器类型
     * 
     * 使用示例：
     * @code
     * VideoFile video;
     * video.setReaderType(VideoReaderFactory::ReaderType::MMAP);  // 明确指定
     * video.openRaw(path, width, height, bpp);
     * @endcode
     */
    VideoFile(VideoReaderFactory::ReaderType type = VideoReaderFactory::ReaderType::AUTO);
    
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
    
    /**
     * 打开视频文件（统一智能接口）
     * @param path 文件路径
     * @param width 视频宽度（可选，用于raw视频）
     * @param height 视频高度（可选，用于raw视频）
     * @param bits_per_pixel 每像素位数（可选，用于raw视频）
     * @return 成功返回true
     * 
     * @note 门面类会根据Reader类型自动判断：
     *       - 编码视频Reader（FFMPEG, RTSP）：忽略 width/height/bpp，自动检测格式
     *       - Raw视频Reader（MMAP, IOURING）：使用传入的 width/height/bpp 参数
     * 
     * @note 调用者无需关心内部实现细节，只需传入所有可能需要的参数即可
     * 
     * 使用示例：
     * @code
     * // 编码视频（自动检测格式）
     * video.setReaderType(VideoReaderFactory::ReaderType::FFMPEG);
     * video.open("video.mp4");  // width/height/bpp 被忽略
     * 
     * // Raw视频（需要格式参数）
     * video.setReaderType(VideoReaderFactory::ReaderType::MMAP);
     * video.open("video.raw", 1920, 1080, 32);  // 使用格式参数
     * @endcode
     */
    bool open(const char* path, int width = 0, int height = 0, int bits_per_pixel = 0);
    
    /**
     * 打开raw视频文件（向后兼容接口）
     * @deprecated 推荐直接使用 open(path, width, height, bpp)
     */
    bool openRaw(const char* path, int width, int height, int bits_per_pixel);
    
    void close();
    bool isOpen() const;
    
    // ============ 读取操作（转发） ============
    
    bool readFrameTo(Buffer& dest_buffer);
    bool readFrameTo(void* dest_buffer, size_t buffer_size);
    bool readFrameAt(int frame_index, Buffer& dest_buffer);
    bool readFrameAt(int frame_index, void* dest_buffer, size_t buffer_size);
    bool readFrameAtThreadSafe(int frame_index, void* dest_buffer, size_t buffer_size) const;
    
    /**
     * 🆕 读取并填充一帧（统一接口）
     */
    bool readFrame(int frame_index, Buffer* buffer);
    
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
    
    /**
     * 查询 Reader 是否需要外部提供 buffer
     * @return true - 需要外部 buffer（预分配模式）
     *         false - 不需要外部 buffer（动态注入模式）
     */
    bool requiresExternalBuffer() const;
    
    // ============ 可选依赖注入（透传到 Reader）============
    
    /**
     * 设置BufferPool（透传到底层Reader）
     * 
     * 用于支持零拷贝优化（如RTSP流解码器）
     * 
     * @param pool BufferPool指针
     */
    void setBufferPool(void* pool);
    
    /**
     * 🆕 获取输出 BufferPool（如果有）
     */
    void* getOutputBufferPool() const;
};

#endif // VIDEOFILE_HPP

