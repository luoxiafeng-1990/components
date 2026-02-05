#ifndef OPENCV_WORKER_HPP
#define OPENCV_WORKER_HPP

#include "productionline/worker/WorkerBase.hpp"
#include "productionline/worker/IEncodedPacketSource.hpp"
#include "buffer/bufferpool/Buffer.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>   // 包含 AVCodecContext, AVPacket, AVFrame
#include <libavformat/avformat.h> // 包含 AVCodecParameters
}

#include "opencv2/opencv.hpp"
#include "opencv2/core/tacv.hpp"

#define MAX_VIDEO_PATH_LENGTH 512

class OpencvWorker : public WorkerBase {
public:
    explicit OpencvWorker(const WorkerConfig& config);
    virtual ~OpencvWorker();
    
    // 禁止拷贝
    OpencvWorker(const OpencvWorker&) = delete;
    OpencvWorker& operator=(const OpencvWorker&) = delete;
    
    bool fillBuffer(int frame_index, Buffer* buffer) override;
    const char* getWorkerType() const override {
        return "OpencvWorker";
    }
    
    bool open() override;
    bool open(const char* path) override;
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
    std::string getPath() const override;
    bool hasMoreFrames() const override;
    SourceType getDataSourceType() const override;
    bool isAtEnd() const override;

    bool setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) override;
    
    /**
     * 获取已解码帧数
     */
    int getDecodedFrames() const { return decoded_frames_.load(); }
    
    /**
     * 获取丢帧数
     */
    int getDroppedFrames() const { return dropped_frames_.load(); }
    
    /**
     * 获取连接状态
     */
    bool isConnected() const;
    
    /**
     * @brief 获取编解码器参数（用于 BufferWriter 等场景）
     * @return 编解码器参数指针，如果不可用则返回 nullptr
     */
    const AVCodecParameters* getCodecParameters() const override;
    
    /**
     * @brief 获取时间基（用于 BufferWriter 等场景）
     * @return 时间基
     */
    struct AVRational getTimeBase() const override;
    int getSourceWidth() const override;
    int getSourceHeight() const override;
    AVPixelFormat getSourcePixelFormat() const override;
    
    /**
     * @brief 获取 Worker 输出的视频宽度
     */
    int getOutputWidth() const override;
    
    /**
     * @brief 获取 Worker 输出的视频高度
     */
    int getOutputHeight() const override;
    
    /**
     * @brief 获取 Worker 输出的每像素字节数
     * @param channel 通道编号（默认 0）
     */
    double getOutputBytesPerPixel(int channel = 0) const override;
    
    /**
     * @brief 获取编解码器名称
     */
    const char* getCodecName() const;
    
    /**
     * 打印统计信息
     */
    void printStats() const;

private:
    // ============ Logger ============
    log4cplus::Logger logger_;
    
    // ============ 数据源抽象（v2.12新增）============
    // ⭐ v2.18 修改：从 unique_ptr 改为 shared_ptr（支持共享模式）
    std::shared_ptr<IEncodedPacketSource> packet_source_;  // 数据源抽象（文件/RTSP流/共享Buffer）
    
    // ============ FFmpeg 资源 ============
    AVCodecContext* codec_ctx_ptr_;
    
    // ============ 输出参数（运行时状态）============
    int output_width_;                 // 输出宽度（运行时状态，可能与config不同）
    int output_height_;                // 输出高度（运行时状态，可能与config不同）
    
    // ============ 解码器配置（v2.2新增）============
    bool use_hardware_decoder_;        // 是否使用硬件解码器（从WorkerConfig获取）
    std::string decoder_name_;         // 指定解码器名称（如 "h264_taco"），空字符串表示自动选择
    struct AVDictionary* codec_options_ptr_;  // 解码器选项（用于 h264_taco 配置）
    
    // ============ 统计信息 ============
    std::atomic<int> decoded_frames_;
    std::atomic<int> dropped_frames_;
    
    // ============ 线程安全 ============
    mutable std::recursive_mutex mutex_;  // 使用递归锁避免死锁
    
    // ============ 帧缓存（用于多通道解码）============
    std::vector<AVFrame*> cached_frames_;  // 缓存解码后的帧（用于处理双通道等场景）
    
    // ============ v3.0 新增：Packet 状态管理（用于 Buffer 模式共享）============
    AVPacket* current_packet_ptr_;         // 当前持有的 packet 指针（Buffer 模式下使用）
    bool packet_acquired_;                 // 是否已获取 packet（Buffer 模式下使用）
    
    bool initializeDecoder(const AVCodecParameters* codec_params);
    
    bool configureSpecialDecoder();
    
    bool readAndSendPacket(AVPacket* packet_ptr);
    
    bool fillBufferMetadataFromFrame(AVFrame* frame_ptr, Buffer* buffer);

    virtual bool extractHardwareAddressFromMetadata(struct AVFrame* frame, Buffer* buffer) override;
    
    double getTacoChannelBytesPerPixel(int channel) const;
    
    static OutputFormat mapRgbDriverValueToEnum(int driver_value);
    
    static double getBytesPerPixelFromFormat(OutputFormat format);
};

#endif

