#pragma once

#include "productionline/worker/WorkerBase.hpp"
#include "productionline/worker/IPacketSource.hpp"
#include <string>
#include <atomic>
#include <mutex>
#include <memory>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

// Forward declarations
struct AVPacket;

/**
 * @brief FFmpeg Packet 录制器 Worker（支持多种数据源）
 * 
 * 功能：从多种数据源读取原始编码数据（AVPacket），不解码，填充到Buffer中
 * 
 * 支持的数据源：
 * - RTSP/RTSPS 流：`rtsp://...` 或 `rtsps://...`
 * - 本地文件：`/path/to/video.mp4`、`.h264`、`.h265` 等
 * - HTTP/HTTPS 流：`http://...` 或 `https://...`（如 HLS）
 * - 未来可扩展：RTMP、其他网络协议
 * 
 * 使用场景：
 * - 配合 VideoProductionLine 作为生产者
 * - 配合消费者保存原始码流到文件
 * - 用于对比测试（相同码流，不同解码器）
 * 
 * @example
 * // RTSP 流
 * auto config1 = WorkerConfigBuilder()
 *     .setDataSourceConfig(DataSourceConfigBuilder().setPath("rtsp://192.168.1.100/stream").build())
 *     .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
 *     .build();
 * 
 * // 本地文件
 * auto config2 = WorkerConfigBuilder()
 *     .setDataSourceConfig(DataSourceConfigBuilder().setPath("/data/video.mp4").build())
 *     .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
 *     .build();
 * 
 * // 在 ProductionLine 中运行
 * VideoProductionLine producer(false, 1, false);
 * producer.start(config);
 * 
 * // 消费者保存文件
 * while (running) {
 *     Buffer* buffer = pool->acquireFilled(...);
 *     fwrite(buffer->getVirtualAddress(), 1, buffer->getUsedSize(), file);
 *     pool->releaseFilled(buffer);
 * }
 */
class FfmpegPacketRecorderWorker : public WorkerBase {
public:
    FfmpegPacketRecorderWorker();
    explicit FfmpegPacketRecorderWorker(const WorkerConfig& config);
    virtual ~FfmpegPacketRecorderWorker() override;
    
    // ============ IVideoReader 接口实现 ============
    virtual bool open() override;
    virtual bool open(const char* path) override;
    virtual void close() override;
    virtual bool isOpen() const override;
    
    virtual bool seek(int frame_index) override;
    virtual bool seekToBegin() override;
    virtual bool seekToEnd() override;
    virtual bool skip(int frame_count) override;
    
    virtual int getTotalFrames() const override;
    virtual int getCurrentFrameIndex() const override;
    virtual size_t getFrameSize() const override;
    virtual long getFileSize() const override;
    
    std::string getPath() const override;
    SourceType getDataSourceType() const override;
    
    virtual bool hasMoreFrames() const override;
    virtual bool isAtEnd() const override;
    
    // ============ WorkerBase 接口实现 ============
    virtual bool fillBuffer(int frame_index, Buffer* buffer) override;
    
    /**
     * @brief 获取 Worker 类型名称
     */
    const char* getWorkerType() const override {
        return "FfmpegPacketRecorderWorker";
    }
    
    /**
     * @brief 获取主要 BufferPool 类型
     * 
     * Packet 录制 Worker 的主要输出是编码后的 packet 数据
     */
    BufferPoolType getPrimaryBufferPoolType() const override {
        return BufferPoolType::PACKET_VIDEO;
    }
    
    /**
     * @brief 获取编解码器参数（供BufferWriter使用）
     * @return 编解码器参数指针，如果未打开则返回nullptr
     */
    const AVCodecParameters* getCodecParameters() const override;
    
    /**
     * @brief 获取时间基（供BufferWriter使用）
     * @return 时间基
     */
    struct AVRational getTimeBase() const override;
    int getSourceWidth() const override;
    int getSourceHeight() const override;
    AVPixelFormat getSourcePixelFormat() const override;
    
    /**
     * @brief 获取 Worker 输出宽度（Recorder不处理，等于数据源宽度）
     */
    int getOutputWidth() const override;
    
    /**
     * @brief 获取 Worker 输出高度（Recorder不处理，等于数据源高度）
     */
    int getOutputHeight() const override;
    
    /**
     * @brief 获取 Worker 输出每像素字节数（Recorder记录原始码流，返回0）
     */
    double getOutputBytesPerPixel(int channel = 0) const override;

private:
    std::string getLastError() const;
    void setError(const std::string& error, int ffmpeg_error = 0);
    
    /**
     * @brief 根据路径自动创建合适的数据源
     * @param path 数据源路径（RTSP URL、文件路径等）
     * @return 数据源指针，失败返回 nullptr
     */
    std::unique_ptr<IPacketSource> createPacketSource(const std::string& path);

private:
    // ============ v2.13 数据源抽象（支持自动选择） ============
    std::unique_ptr<IPacketSource> packet_source_;  // 数据源（RTSP/文件/Buffer）
    
    // 状态
    std::atomic<bool> is_open_;
    std::atomic<int> packet_count_;
    
    // 错误信息
    mutable std::mutex error_mutex_;
    std::string last_error_;
    
    // 互斥锁
    mutable std::recursive_mutex mutex_;
    
    // 日志器
    log4cplus::Logger logger_;
};
