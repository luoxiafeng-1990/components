#ifndef ENCODED_PACKET_SOURCE_FROM_RTSP_HPP
#define ENCODED_PACKET_SOURCE_FROM_RTSP_HPP

#include "productionline/worker/datasource/IEncodedPacketSource.hpp"  // 包含 PacketAcquireResult
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <string>
#include <memory>
#include <atomic>

// FFmpeg 前向声明
struct AVFormatContext;
struct AVCodecParameters;
struct AVPacket;

/**
 * @brief EncodedPacketSourceFromRtsp - 从 RTSP 流读取编码数据的数据源实现
 * 
 * 功能：从 RTSP 流读取编码后的 AVPacket（H.264/H.265等）
 * 
 * 使用场景：
 * - RTSP 实时流解码
 * - IP 摄像头视频流
 * - 网络视频监控
 * 
 * 特性：
 * - 实时流（无总帧数概念，除非设置 max_frames）
 * - 不支持 seek 操作
 * - 需要保持连接
 * - 自动处理超时和重连
 * 
 * v2.32 重构：
 * - 新增 max_frames 参数，支持帧数限制
 * - 删除 readEncodedPacket，改用 acquireEncodedPacket 统一接口
 */
class EncodedPacketSourceFromRtsp : public IEncodedPacketSource {
public:
    /**
     * @brief 构造函数
     * @param rtsp_url RTSP 流地址
     * @param max_frames 最大读取帧数（-1=无限制）
     */
    explicit EncodedPacketSourceFromRtsp(const std::string& rtsp_url, int max_frames = -1);
    
    /**
     * @brief 析构函数
     */
    ~EncodedPacketSourceFromRtsp() override;
    
    // 禁止拷贝
    EncodedPacketSourceFromRtsp(const EncodedPacketSourceFromRtsp&) = delete;
    EncodedPacketSourceFromRtsp& operator=(const EncodedPacketSourceFromRtsp&) = delete;
    
    // ============ IDataSourceNavigator 接口实现 ============
    
    // 数据源生命周期
    bool open() override;
    bool open(const char* path) override;     // 返回 false（RTSP 需要完整配置）
    void close() override;
    bool isOpen() const override;
    
    // 数据源导航（实时流不支持导航）
    bool seek(int frame_index) override;      // 返回 false（实时流不支持 seek）
    bool seekToBegin() override;              // 返回 false（实时流不支持）
    bool seekToEnd() override;                // 返回 false（实时流不支持）
    bool skip(int frame_count) override;      // 返回 false（实时流不支持）
    
    // 数据源状态查询
    int getTotalFrames() const override;      // 返回 INT_MAX（实时流无限）
    int getCurrentFrameIndex() const override;// 返回已读取的帧数
    size_t getFrameSize() const override;     // 返回 0（实时流无法估算）
    long getFileSize() const override;        // 返回 -1（实时流无文件大小）
    std::string getPath() const override;     // 返回 RTSP URL
    bool hasMoreFrames() const override;      // 返回 !isAtEnd()
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
     * @param worker_id Worker 标识（RTSP 模式不使用，忽略）
     * @return PacketAcquireResult 结果对象，result.packet() 返回 out_packet
     */
    PacketAcquireResult acquireEncodedPacket(AVPacket* out_packet, void* worker_id = nullptr) override;
    
    int getVideoStreamIndex() const override;
    
    // commit/cancel 使用接口默认实现（RTSP 模式不需要）

    // ============ 中断控制接口 ============
    
    /**
     * @brief 请求中断所有 RTSP 流操作（用于响应 Ctrl+C）
     * 
     * 当用户按 Ctrl+C 时，信号处理器应调用此方法。
     * FFmpeg 会在下一次 I/O 检查时中断阻塞操作。
     */
    static void requestInterrupt();
    
    /**
     * @brief 清除中断标志（用于重新开始）
     */
    static void clearInterrupt();
    
private:
    // ============ 中断机制（静态，所有实例共享） ============
    
    /**
     * @brief 中断标志（静态成员，所有 EncodedPacketSourceFromRtsp 实例共享）
     * 
     * 当设置为 true 时，所有正在进行的 RTSP 流读取操作都会被中断。
     * 这是响应 Ctrl+C 的核心机制。
     */
    static std::atomic<bool> interrupt_requested_;
    
    /**
     * @brief FFmpeg 中断回调函数
     * @param ctx 用户自定义上下文（可选）
     * @return 1 表示需要中断，0 表示继续
     * 
     * FFmpeg 在执行阻塞 I/O 操作时会定期调用此函数。
     * 如果返回 1，FFmpeg 会立即中断操作并返回 AVERROR_EXIT。
     */
    static int interrupt_callback(void* ctx);
    
    // ============ 实例成员 ============
    
    std::string rtsp_url_;              // RTSP 流地址
    AVFormatContext* format_ctx_ptr_;   // FFmpeg 格式上下文
    int video_stream_index_;            // 视频流索引
    std::atomic<int> current_frame_index_;  // 当前帧索引（已读取的帧数）
    std::atomic<bool> is_open_;         // 原子变量，保证线程安全的状态检查
    std::atomic<bool> connected_;       // 连接状态
    std::atomic<bool> eof_reached_;     // 是否到达流末尾
    
    // ========================================
    // 帧数限制（v2.32 新增）
    // ========================================
    int max_frames_;                     // 最大读取帧数（-1=无限制）
    int frames_read_;                    // 已读取帧数计数
    
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
    
    // 日志器
    log4cplus::Logger logger_;
};

#endif // ENCODED_PACKET_SOURCE_FROM_RTSP_HPP
