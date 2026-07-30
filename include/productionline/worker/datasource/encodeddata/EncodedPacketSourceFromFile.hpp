#ifndef ENCODED_PACKET_SOURCE_FROM_FILE_HPP
#define ENCODED_PACKET_SOURCE_FROM_FILE_HPP

#include "productionline/worker/datasource/encodeddata/IEncodedPacketSource.hpp"  // 包含 PacketAcquireResult
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <string>
#include <memory>
#include <atomic>

// FFmpeg 前向声明
struct AVFormatContext;
struct AVCodecParameters;

/**
 * @brief EncodedPacketSourceFromFile - 从文件读取编码数据的数据源实现
 * 
 * 功能：从本地文件读取编码后的 AVPacket（H.264/H.265等）
 * 
 * 使用场景：
 * - 传统文件解码模式
 * - 从 MP4、AVI、MKV 等容器文件读取编码数据
 * - 从裸流文件（.h264/.h265）读取编码数据
 * 
 * v2.32 重构：
 * - 统一接口名：acquireEncodedPacket（替代 readEncodedPacket）
 * - 零拷贝设计：直接往调用者提供的 out_packet 填充数据
 */
class EncodedPacketSourceFromFile : public IEncodedPacketSource {
public:
    /**
     * @brief 构造函数
     * @param file_path 文件路径
     * @param max_frames 最大读取帧数（-1=无限制；跨循环累计）
     * @param loop_count 文件循环遍数（默认 1；<1 按 1 处理）
     */
    explicit EncodedPacketSourceFromFile(const std::string& file_path,
                                         int max_frames = -1,
                                         int loop_count = 1);
    
    /**
     * @brief 析构函数
     */
    ~EncodedPacketSourceFromFile() override;
    
    // 禁止拷贝
    EncodedPacketSourceFromFile(const EncodedPacketSourceFromFile&) = delete;
    EncodedPacketSourceFromFile& operator=(const EncodedPacketSourceFromFile&) = delete;
    
    // ============ IDataSourceNavigator 接口实现 ============
    
    // 数据源生命周期
    bool open() override;
    bool open(const char* path) override;
    void close() override;
    bool isOpen() const override;
    
    // 数据源导航
    bool seek(int frame_index) override;
    bool seekToBegin() override;
    bool seekToEnd() override;
    bool skip(int frame_count) override;
    
    // 数据源状态查询
    int getTotalFrames() const override;
    int getCurrentFrameIndex() const override;
    size_t getFrameSize() const override;
    long getFileSize() const override;
    std::string getPath() const override;
    bool hasMoreFrames() const override;
    bool isAtEnd() const override;
    
    // 数据源属性
    int getSourceWidth() const override;
    int getSourceHeight() const override;
    AVPixelFormat getSourcePixelFormat() const override;
    const AVCodecParameters* getCodecParameters() const override;
    SourceType getDataSourceType() const override;
    
    // ============ IEncodedPacketSource 特有方法（v2.32 统一接口）============
    
    /**
     * @brief 获取编码后的 packet（v2.32 统一接口）
     * @param out_packet 输出的 packet（必须提供，数据填充到此）
     * @param worker_id Worker 标识（File 模式不使用，忽略）
     * @return PacketAcquireResult 结果对象，result.packet() 返回 out_packet
     */
    PacketAcquireResult acquireEncodedPacket(AVPacket* out_packet, void* worker_id = nullptr) override;
    
    int getVideoStreamIndex() const override;
    
    // commit/cancel 使用接口默认实现（File 模式不需要）

private:
    std::string file_path_;              // 文件路径
    AVFormatContext* format_ctx_ptr_;   // FFmpeg 格式上下文
    int video_stream_index_;            // 视频流索引
    int total_frames_;                  // 总帧数（估算）
    int current_frame_index_;           // 当前帧索引
    std::atomic<bool> is_open_;         // 原子变量，保证线程安全的状态检查
    bool eof_reached_;                  // 是否到达文件末尾
    
    // ========================================
    // 帧数限制（v2.23 新增）
    // ========================================
    int max_frames_;                     // 最大读取帧数（-1=无限制）
    int frames_read_;                    // 已读取帧数计数（跨循环累计）
    int loop_count_;                     // 文件循环遍数（>=1）
    int loops_completed_;                // 已完成的遍数（从 0 起；EOF 重启前 +1）

    /// EOF 时若还有剩余遍数则 seek 到开头并继续
    bool tryRestartForLoop();

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
    
    // 日志器
    log4cplus::Logger logger_;
};

#endif // ENCODED_PACKET_SOURCE_FROM_FILE_HPP
