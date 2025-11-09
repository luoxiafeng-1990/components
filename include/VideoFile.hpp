#ifndef VIDEOFILE_HPP
#define VIDEOFILE_HPP

#include "Buffer.hpp"
#include <stddef.h>  // For size_t
#include <sys/types.h>  // For ssize_t

#define MAX_PATH_LENGTH 512  // Maximum path length

/**
 * VideoFile - 视频文件管理类
 * 
 * 管理原始视频文件的完整生命周期，提供便利的文件操作接口。
 * 
 * 支持的操作：
 * - 打开/关闭文件（RAII）
 * - 读取帧（顺序读、随机读）
 * - 导航操作（seek、rewind）
 * - 元数据查询（帧数、尺寸等）
 * 
 * 文件格式：
 * - 原始像素数据（raw format）
 * - 无头部，直接存储像素
 * - 帧大小 = (width × height × bits_per_pixel + 7) / 8（向上取整到字节）
 * 
 * RAII机制：
 * - 构造时可以打开文件
 * - 析构时自动关闭文件
 */
class VideoFile {
private:
    // ============ 文件资源 ============
    int fd_;                          // 文件描述符
    char path_[MAX_PATH_LENGTH];     // 文件路径
    void* mapped_file_;               // mmap映射的文件地址
    size_t mapped_size_;              // 映射的文件大小
    
    // ============ 视频属性 ============
    int width_;                       // 视频宽度（像素）
    int height_;                      // 视频高度（像素）
    int bits_per_pixel_;              // 每像素位数（可以是非整数字节，如12bit、16bit、24bit等）
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
        RAW,          // 原始格式（需要配置文件）
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
     * @return 检测到的格式
     */
    FileFormat detectFileFormat();
    
    /**
     * 读取文件头（用于格式检测）
     * @param header 存储文件头的缓冲区（至少16字节）
     * @param size 要读取的字节数
     * @return 实际读取的字节数
     */
    ssize_t readFileHeader(unsigned char* header, size_t size);
    
    /**
     * 从MP4头部解析格式信息
     * @return 成功返回true
     */
    bool parseMP4Header();
    
    /**
     * 从H264流解析格式信息
     * @return 成功返回true
     */
    bool parseH264Header();
    
    /**
     * 映射文件到内存
     * 使用 mmap 将整个文件映射到进程地址空间
     * @return 成功返回true
     */
    bool mapFile();
    
    /**
     * 解除文件映射
     * 使用 munmap 解除内存映射
     */
    void unmapFile();

public:
    // ============ 构造函数 ============
    
    VideoFile();
    ~VideoFile();
    
    // 禁止拷贝（RAII资源管理）
    VideoFile(const VideoFile&) = delete;
    VideoFile& operator=(const VideoFile&) = delete;
    
    // ============ 文件操作 ============
    
    /**
     * 打开编码视频文件（自动检测格式）
     * 
     * 格式检测策略：
     * 1. 读取文件头，检测魔数（MP4/H264/H265/AVI等）
     * 2. 解析文件头获取宽高等信息
     * 
     * 适用于：MP4、H264、H265、AVI等编码格式
     * 
     * @param path 文件路径
     * @return 成功返回true，失败返回false
     */
    bool open(const char* path);
    
    /**
     * 打开原始视频文件（需手动指定格式）
     * 
     * 用于原始格式视频（raw format），无头部信息。
     * 必须手动提供格式参数。
     * 
     * @param path 文件路径
     * @param width 视频宽度（像素）
     * @param height 视频高度（像素）
     * @param bits_per_pixel 每像素位数（如RGB24=24, RGBA32=32, 12bit=12）
     * @return 成功返回true，失败返回false
     */
    bool openRaw(const char* path, int width, int height, int bits_per_pixel);
    
    /**
     * 关闭视频文件
     */
    void close();
    
    /**
     * 检查文件是否已打开
     */
    bool isOpen() const;
    
    // ============ 读取操作 ============
    
    /**
     * 读取一帧到指定Buffer
     * 读取当前帧，自动前进到下一帧
     * @param dest_buffer 目标Buffer
     * @return 成功返回true
     */
    bool readFrameTo(Buffer& dest_buffer);
    
    /**
     * 读取一帧到指定地址
     * @param dest_buffer 目标地址
     * @param buffer_size 目标缓冲区大小
     * @return 成功返回true
     */
    bool readFrameTo(void* dest_buffer, size_t buffer_size);
    
    /**
     * 读取指定帧到Buffer
     * @param frame_index 帧索引
     * @param dest_buffer 目标Buffer
     * @return 成功返回true
     */
    bool readFrameAt(int frame_index, Buffer& dest_buffer);
    
    /**
     * 读取指定帧到地址
     * @param frame_index 帧索引
     * @param dest_buffer 目标地址
     * @param buffer_size 目标缓冲区大小
     * @return 成功返回true
     */
    bool readFrameAt(int frame_index, void* dest_buffer, size_t buffer_size);
    
    // ============ 导航操作 ============
    
    /**
     * 跳转到指定帧
     * @param frame_index 帧索引
     * @return 成功返回true
     */
    bool seek(int frame_index);
    
    /**
     * 回到文件开头
     */
    bool seekToBegin();
    
    /**
     * 跳转到文件末尾
     */
    bool seekToEnd();
    
    /**
     * 跳过N帧（可正可负）
     * @param frame_count 跳过的帧数
     * @return 成功返回true
     */
    bool skip(int frame_count);
    
    // ============ 信息查询 ============
    
    /**
     * 获取总帧数
     */
    int getTotalFrames() const;
    
    /**
     * 获取当前帧索引
     */
    int getCurrentFrameIndex() const;
    
    /**
     * 获取单帧大小（字节）
     */
    size_t getFrameSize() const;
    
    /**
     * 获取文件大小（字节）
     */
    long getFileSize() const;
    
    // ============ 元数据 ============
    
    /**
     * 获取视频宽度
     */
    int getWidth() const;
    
    /**
     * 获取视频高度
     */
    int getHeight() const;
    
    /**
     * 获取每像素字节数
     */
    int getBytesPerPixel() const;
    
    /**
     * 获取文件路径
     */
    const char* getPath() const;
    
    // ============ 状态查询 ============
    
    /**
     * 检查是否还有更多帧
     */
    bool hasMoreFrames() const;
    
    /**
     * 检查是否到达文件末尾
     */
    bool isAtEnd() const;
};

#endif // VIDEOFILE_HPP

