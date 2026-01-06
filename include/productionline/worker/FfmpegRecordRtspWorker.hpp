#pragma once

#include "productionline/worker/WorkerBase.hpp"
#include <string>
#include <atomic>
#include <mutex>

// Forward declarations
struct AVFormatContext;
struct AVPacket;

/**
 * @brief RTSP原始码流录制Worker
 * 
 * 功能：从RTSP流读取原始编码数据（AVPacket），不解码，填充到Buffer中
 * 
 * 使用场景：
 * - 配合 VideoProductionLine 作为生产者
 * - 配合消费者保存原始码流到文件
 * - 用于对比测试（相同码流，不同解码器）
 * 
 * @example
 * // 创建 Worker
 * auto config = WorkerConfigBuilder()
 *     .setFileConfig(FileConfigBuilder().setFilePath("rtsp://...").build())
 *     .setWorkerType(WorkerType::FFMPEG_RTSP_RECORD)
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
class FfmpegRecordRtspWorker : public WorkerBase {
public:
    FfmpegRecordRtspWorker();
    explicit FfmpegRecordRtspWorker(const WorkerConfig& config);
    virtual ~FfmpegRecordRtspWorker() override;
    
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
    
    virtual int getWidth() const override;
    virtual int getHeight() const override;
    virtual double getBytesPerPixel() const override;
    virtual const char* getPath() const override;
    
    virtual bool hasMoreFrames() const override;
    virtual bool isAtEnd() const override;
    
    // ============ WorkerBase 接口实现 ============
    virtual bool fillBuffer(int frame_index, Buffer* buffer) override;
    
    /**
     * @brief 获取 Worker 类型名称
     */
    const char* getWorkerType() const override {
        return "FfmpegRecordRtspWorker";
    }
    
    /**
     * @brief 获取主要 BufferPool 类型
     * 
     * RTSP 录制 Worker 的主要输出是编码后的 packet 数据
     */
    BufferPoolType getPrimaryBufferPoolType() const override {
        return BufferPoolType::PACKET_VIDEO;
    }
    
    /**
     * @brief 获取编解码器参数（供BufferWriter使用）
     * @return 编解码器参数指针，如果未打开则返回nullptr
     */
    const struct AVCodecParameters* getCodecParameters() const;
    
    /**
     * @brief 获取时间基（供BufferWriter使用）
     * @return 时间基
     */
    struct AVRational getTimeBase() const;

private:
    bool openMediaSource();
    void closeMediaSource();
    bool findVideoStream();
    
    std::string getLastError() const;
    void setError(const std::string& error, int ffmpeg_error = 0);

private:
    // FFmpeg 上下文
    AVFormatContext* format_ctx_ptr_;
    int video_stream_index_;
    
    // RTSP 配置
    std::string rtsp_url_;
    
    // 状态
    std::atomic<bool> is_open_;
    std::atomic<bool> connected_;
    std::atomic<bool> eof_reached_;
    std::atomic<int> packet_count_;
    
    // 错误信息
    mutable std::mutex error_mutex_;
    std::string last_error_;
    
    // 互斥锁
    mutable std::recursive_mutex mutex_;
};

