#ifndef MMAP_VIDEO_READER_HPP
#define MMAP_VIDEO_READER_HPP

#include "IVideoReader.hpp"
#include "../buffer/Buffer.hpp"
#include <stddef.h>  // For size_t
#include <sys/types.h>  // For ssize_t

#define MAX_PATH_LENGTH 512  // Maximum path length

/**
 * MmapVideoReader - 基于 mmap 的视频读取器
 * 
 * 使用内存映射（mmap）技术读取视频文件：
 * - 将整个文件映射到进程地址空间
 * - 零拷贝访问（通过 memcpy）
 * - 适合随机访问和小到中等大小文件
 * 
 * 优势：
 * - 实现简单，兼容性好
 * - 随机访问性能优秀
 * - 内核自动管理页缓存
 * 
 * 适用场景：
 * - 文件大小 < 1GB
 * - 随机访问模式
 * - 单线程或少量线程
 */
class MmapVideoReader : public IVideoReader {
private:
    // ============ 文件资源 ============
    int fd_;                          // 文件描述符
    char path_[MAX_PATH_LENGTH];     // 文件路径
    void* mapped_file_;               // mmap映射的文件地址
    size_t mapped_size_;              // 映射的文件大小
    
    // ============ 视频属性 ============
    int width_;                       // 视频宽度（像素）
    int height_;                      // 视频高度（像素）
    int bits_per_pixel_;              // 每像素位数
    size_t frame_size_;               // 单帧大小（字节）
    
    // ============ 文件信息 ============
    long file_size_;                  // 文件大小（字节）
    int total_frames_;                // 总帧数
    int current_frame_index_;         // 当前帧索引
    
    // ============ 状态标志 ============
    bool is_open_;
    
    // ============ 格式检测 ============
    enum class FileFormat {
        UNKNOWN,
        RAW,          // 原始格式
        MP4,          // MP4容器
        H264,         // H.264裸流
        H265,         // H.265裸流
        AVI           // AVI容器
    };
    
    FileFormat detected_format_;
    
    // ============ 内部辅助方法 ============
    
    /**
     * 验证文件有效性
     */
    bool validateFile();
    
    /**
     * 检测文件格式（通过魔数）
     */
    FileFormat detectFileFormat();
    
    /**
     * 读取文件头（用于格式检测）
     */
    ssize_t readFileHeader(unsigned char* header, size_t size);
    
    /**
     * 从MP4头部解析格式信息
     */
    bool parseMP4Header();
    
    /**
     * 从H264流解析格式信息
     */
    bool parseH264Header();
    
    /**
     * 映射文件到内存
     */
    bool mapFile();
    
    /**
     * 解除文件映射
     */
    void unmapFile();

public:
    // ============ 构造/析构 ============
    
    MmapVideoReader();
    virtual ~MmapVideoReader();
    
    // 禁止拷贝（RAII资源管理）
    MmapVideoReader(const MmapVideoReader&) = delete;
    MmapVideoReader& operator=(const MmapVideoReader&) = delete;
    
    // ============ IVideoReader 接口实现 ============
    
    bool open(const char* path) override;
    bool openRaw(const char* path, int width, int height, int bits_per_pixel) override;
    void close() override;
    bool isOpen() const override;
    
    bool requiresExternalBuffer() const override {
        return true;  // 需要外部 buffer（从 mmap 区域拷贝到外部 buffer）
    }
    
    bool readFrameTo(Buffer& dest_buffer) override;
    bool readFrameTo(void* dest_buffer, size_t buffer_size) override;
    bool readFrameAt(int frame_index, Buffer& dest_buffer) override;
    bool readFrameAt(int frame_index, void* dest_buffer, size_t buffer_size) override;
    bool readFrameAtThreadSafe(int frame_index, void* dest_buffer, size_t buffer_size) const override;
    
    bool seek(int frame_index) override;
    bool seekToBegin() override;
    bool seekToEnd() override;
    bool skip(int frame_count) override;
    
    int getTotalFrames() const override;
    int getCurrentFrameIndex() const override;
    size_t getFrameSize() const override;
    long getFileSize() const override;
    int getWidth() const override;
    int getHeight() const override;
    int getBytesPerPixel() const override;
    const char* getPath() const override;
    bool hasMoreFrames() const override;
    bool isAtEnd() const override;
    
    const char* getReaderType() const override;
};

#endif // MMAP_VIDEO_READER_HPP







