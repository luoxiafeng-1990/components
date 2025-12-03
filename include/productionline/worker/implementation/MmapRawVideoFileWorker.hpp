#ifndef MMAP_RAW_VIDEO_FILE_WORKER_HPP
#define MMAP_RAW_VIDEO_FILE_WORKER_HPP

#include "../base/WorkerBase.hpp"
#include "../../../buffer/Buffer.hpp"
#include <stddef.h>  // For size_t
#include <sys/types.h>  // For ssize_t

#define MAX_PATH_LENGTH 512  // Maximum path length

/**
 * @brief MmapRawVideoFileWorker - Mmap方式打开raw视频文件Worker
 * 
 * 架构角色：Worker（工人）- Mmap方式打开raw视频文件类型
 * 
 * 功能：使用mmap内存映射方式打开raw视频文件
 * 目的：填充Buffer，得到填充后的buffer
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
class MmapRawVideoFileWorker : public WorkerBase {
public:
    // ============ 文件资源 ============
    int fd_;                          // 文件描述符
    char path_[MAX_PATH_LENGTH];     // 文件路径
    void* mapped_file_ptr_;               // mmap映射的文件地址
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
    
    MmapRawVideoFileWorker();
    virtual ~MmapRawVideoFileWorker();
    
    // 禁止拷贝（RAII资源管理）
    MmapRawVideoFileWorker(const MmapRawVideoFileWorker&) = delete;
    MmapRawVideoFileWorker& operator=(const MmapRawVideoFileWorker&) = delete;
    
    // ============ WorkerBase 接口实现 ============
    
    // Buffer填充功能（原IBufferFillingWorker的方法）
    bool fillBuffer(int frame_index, Buffer* buffer) override;
    const char* getWorkerType() const override {
        return "MmapRawVideoFileWorker";
    }
    
    // 文件导航功能（继承自IVideoFileNavigator）
    bool open(const char* path) override;
    bool open(const char* path, int width, int height, int bits_per_pixel) override;
    void close() override;
    bool isOpen() const override;
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

private:
    // ============ 文件资源 ============
    int fd_;                          // 文件描述符
    char path_[MAX_PATH_LENGTH];     // 文件路径
    void* mapped_file_ptr_;               // mmap映射的文件地址
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
};

#endif // MMAP_RAW_VIDEO_FILE_WORKER_HPP

