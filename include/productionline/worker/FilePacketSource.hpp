#ifndef FILE_PACKET_SOURCE_HPP
#define FILE_PACKET_SOURCE_HPP

#include "productionline/worker/IPacketSource.hpp"
#include <string>
#include <memory>
#include <atomic>

// FFmpeg 前向声明
struct AVFormatContext;
struct AVCodecParameters;

/**
 * @brief FilePacketSource - 文件数据源实现
 * 
 * 功能：从本地文件读取 AVPacket
 * 
 * 使用场景：
 * - 传统文件解码模式
 * - 从 MP4、AVI、MKV 等文件读取
 */
class FilePacketSource : public IPacketSource {
public:
    /**
     * @brief 构造函数
     * @param file_path 文件路径
     */
    explicit FilePacketSource(const std::string& file_path);
    
    /**
     * @brief 析构函数
     */
    ~FilePacketSource() override;
    
    // 禁止拷贝
    FilePacketSource(const FilePacketSource&) = delete;
    FilePacketSource& operator=(const FilePacketSource&) = delete;
    
    // IPacketSource 接口实现
    bool open() override;
    void close() override;
    bool isOpen() const override;
    int readPacket(AVPacket* packet) override;
    const AVCodecParameters* getCodecParameters() const override;
    int getVideoStreamIndex() const override;
    int getTotalFrames() const override;
    long getFileSize() const override;
    std::string getFilePath() const override;
    bool seek(int frame_index) override;
    bool isEof() const override;
    int getSourceWidth() const override;
    int getSourceHeight() const override;
    AVPixelFormat getSourcePixelFormat() const override;
    
private:
    std::string file_path_;              // 文件路径
    AVFormatContext* format_ctx_ptr_;   // FFmpeg 格式上下文
    int video_stream_index_;            // 视频流索引
    int total_frames_;                  // 总帧数（估算）
    std::atomic<bool> is_open_;         // 🎯 原子变量，保证线程安全的状态检查
    bool eof_reached_;                  // 是否到达文件末尾
    
    /**
     * @brief 查找视频流
     * @return true 如果成功
     */
    bool findVideoStream();
    
    /**
     * @brief 估算总帧数
     * @return 总帧数
     */
    int estimateTotalFrames();
};

#endif // FILE_PACKET_SOURCE_HPP

