#ifndef RTSP_PACKET_SOURCE_HPP
#define RTSP_PACKET_SOURCE_HPP

#include "productionline/worker/IPacketSource.hpp"
#include <string>
#include <memory>
#include <atomic>

// FFmpeg 前向声明
struct AVFormatContext;
struct AVCodecParameters;

/**
 * @brief RtspPacketSource - RTSP 流数据源实现
 * 
 * 功能：从 RTSP 流读取 AVPacket
 * 
 * 使用场景：
 * - RTSP 实时流解码
 * - IP 摄像头视频流
 * - 网络视频监控
 * 
 * 特性：
 * - 实时流（无总帧数概念）
 * - 不支持 seek 操作
 * - 需要保持连接
 * - 自动处理超时和重连
 */
class RtspPacketSource : public IPacketSource {
public:
    /**
     * @brief 构造函数
     * @param rtsp_url RTSP 流地址
     */
    explicit RtspPacketSource(const std::string& rtsp_url);
    
    /**
     * @brief 析构函数
     */
    ~RtspPacketSource() override;
    
    // 禁止拷贝
    RtspPacketSource(const RtspPacketSource&) = delete;
    RtspPacketSource& operator=(const RtspPacketSource&) = delete;
    
    // IPacketSource 接口实现
    bool open() override;
    void close() override;
    bool isOpen() const override;
    int readPacket(AVPacket* packet) override;
    const AVCodecParameters* getCodecParameters() const override;
    int getVideoStreamIndex() const override;
    int getTotalFrames() const override;      // 返回 INT_MAX（实时流无限）
    long getFileSize() const override;        // 返回 -1（实时流无文件大小）
    std::string getFilePath() const override; // 返回 RTSP URL
    bool seek(int frame_index) override;      // 返回 false（实时流不支持 seek）
    bool isEof() const override;
    
private:
    std::string rtsp_url_;              // RTSP 流地址
    AVFormatContext* format_ctx_ptr_;   // FFmpeg 格式上下文
    int video_stream_index_;            // 视频流索引
    std::atomic<bool> is_open_;         // 🎯 原子变量，保证线程安全的状态检查
    std::atomic<bool> connected_;       // 连接状态
    std::atomic<bool> eof_reached_;     // 是否到达流末尾
    
    /**
     * @brief 查找视频流
     * @return true 如果成功
     */
    bool findVideoStream();
    
    /**
     * @brief 初始化 RTSP 连接
     * @return true 如果成功
     */
    bool initializeConnection();
};

#endif // RTSP_PACKET_SOURCE_HPP
