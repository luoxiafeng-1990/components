#ifndef FFMPEG_ENCODE_WORKER_HPP
#define FFMPEG_ENCODE_WORKER_HPP

#include "productionline/worker/base/WorkerBase.hpp"
#include "productionline/worker/datasource/rawdata/IRawFrameSource.hpp"
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>

// FFmpeg 前向声明
struct AVCodecContext;
struct AVCodecParameters;
struct AVFrame;
struct AVPacket;
struct AVDictionary;
struct SwsContext;

/**
 * @brief FFmpegEncodeWorker - FFmpeg 编码 Worker
 * 
 * 功能：将原始帧（AVFrame）编码为压缩数据（AVPacket）
 * 
 * 支持的编码器：
 * - h264_taco: TACO H.264 硬件编码器
 * - hevc_taco: TACO H.265/HEVC 硬件编码器
 * - jpeg_taco: TACO JPEG 硬件编码器
 * - libx264: 软件 H.264 编码器
 * - libx265: 软件 H.265 编码器
 * - mjpeg: 软件 MJPEG 编码器
 * 
 * 使用场景：
 * - 视频转码（解码 → 编码）
 * - 实时编码（摄像头 → 编码）
 * - 视频压缩
 * 
 * 工作流程：
 * 1. 从 IRawFrameSource 获取原始帧（YUV/RGB）
 * 2. 发送帧到编码器（avcodec_send_frame）
 * 3. 接收编码后的 packet（avcodec_receive_packet）
 * 4. 将 packet 填充到输出 Buffer
 * 
 * 设计模式：
 * - 遵循 FFmpegDecodeWorker 的架构设计
 * - 使用 WorkerBase 提供的 BufferPool 管理
 * - 支持 Buffer 模式（从解码 Worker 获取输入）
 * - 支持文件模式（从 YUV 文件获取输入）
 */
class FFmpegEncodeWorker : public WorkerBase {
public:
    /**
     * @brief 构造函数
     * @param config Worker 配置
     */
    explicit FFmpegEncodeWorker(const WorkerConfig& config);
    
    /**
     * @brief 析构函数
     */
    ~FFmpegEncodeWorker() override;
    
    // 禁止拷贝
    FFmpegEncodeWorker(const FFmpegEncodeWorker&) = delete;
    FFmpegEncodeWorker& operator=(const FFmpegEncodeWorker&) = delete;
    
    // ==================== WorkerBase 接口实现 ====================
    
    /**
     * @brief 填充 Buffer（编码一帧）
     * 
     * 流程：
     * 1. 从 frame_source_ 获取原始帧
     * 2. 发送帧到编码器
     * 3. 接收编码后的 packet
     * 4. 将 packet 填充到输出 Buffer
     * 
     * @param frame_index 帧索引（未使用，兼容接口）
     * @param buffer 输出 Buffer（存储编码后的 AVPacket）
     * @return FillResult 结果对象
     * 
     * v2.33 变更：返回类型从 bool 改为 FillResult
     */
    FillResult fillBuffer(int frame_index, Buffer* buffer) override;
    
    /**
     * @brief 获取 Worker 类型名称
     */
    const char* getWorkerType() const override { return "FFmpegEncodeWorker"; }
    
    /**
     * @brief 获取主要 BufferPool 类型
     */
    BufferPoolType getPrimaryBufferPoolType() const override {
        return BufferPoolType::ENCODE_VIDEO_OUTPUT;
    }
    
    // ==================== IDataSourceNavigator 接口实现 ====================
    
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
    bool isAtEnd() const override;
    SourceType getDataSourceType() const override;
    
    // ==================== Worker 输出属性 ====================
    
    int getSourceWidth() const override;
    int getSourceHeight() const override;
    AVPixelFormat getSourcePixelFormat() const override;
    int getOutputWidth() const override;
    int getOutputHeight() const override;
    double getOutputBytesPerPixel(int channel = 0) const override;
    
    // ==================== 编码器特有接口 ====================
    
    /**
     * @brief 设置源 BufferPool（Buffer 模式）
     * @param pool_weak 解码 Worker 的 BufferPool
     * @return true 成功，false 失败
     */
    bool setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) override;
    
    /**
     * @brief 获取编码器名称
     */
    const char* getEncoderName() const;
    
    /**
     * @brief 获取编解码器参数（用于 BufferWriter 等场景）
     */
    const AVCodecParameters* getCodecParameters() const override;
    
    /**
     * @brief 获取时间基
     */
    AVRational getTimeBase() const override;

private:
    // ==================== 内部方法 ====================
    
    /**
     * @brief 初始化编码器
     * @return true 成功，false 失败
     */
    bool initializeEncoder();

    /**
     * @brief 从 codec_ctx 生成输出码流参数（供 MultiWorker / EncodedPacketSourceFromBuffer）
     */
    bool syncOutputCodecParameters();

    void freeOutputCodecParameters();
    
    /**
     * @brief 配置 TACO 编码器参数
     * @return true 成功，false 失败
     */
    bool configureTacoEncoder();
    
    /**
     * @brief 从 AVPacket 填充 Buffer 元数据
     */
    bool fillBufferMetadataFromPacket(AVPacket* packet, Buffer* buffer);
    
    /**
     * @brief 读取并发送帧到编码器
     * @param temp_frame 临时帧（用于从 frame_source_ 读取）
     * @return FillResult 结果对象
     * 
     * v2.33 变更：返回类型从 bool 改为 FillResult
     */
    FillResult readAndSendFrame(AVFrame* temp_frame);
    
    // ==================== 成员变量 ====================
    
    // 日志器
    log4cplus::Logger logger_;
    
    // 帧数据源
    std::shared_ptr<IRawFrameSource> frame_source_;
    
    // 编码器上下文
    AVCodecContext* codec_ctx_ptr_;
    AVDictionary* codec_options_ptr_;

    /// 编码器输出码流参数（avcodec_parameters_from_context，供下游解码订阅）
    AVCodecParameters* out_codec_params_;

    /**
     * @brief 标记：out_codec_params_ 中 extradata 是否已就绪
     *
     * 一些硬件编码器（如 h264_taco）可能需要产生首个 packet 后才填充 SPS/PPS（extradata）。
     * decoder 若在此之前初始化，可能拿不到关键参数集导致 0 frames。
     */
    bool codec_params_extradata_ready_;
    
    // 输出分辨率
    int output_width_;
    int output_height_;
    
    // 编码器配置
    bool use_hardware_encoder_;
    std::string encoder_name_;
    
    // 统计信息
    std::atomic<int> encoded_frames_;
    std::atomic<int> dropped_frames_;
    
    // 线程安全
    mutable std::recursive_mutex mutex_;
    
    // 缓存的编码后 packet（一帧可能产生多个 packet）
    std::vector<AVPacket*> cached_packets_;
    
    // Buffer 模式状态（共享模式下使用）
    AVFrame* current_frame_ptr_;
    bool frame_acquired_;

    /**
     * 文件模式：复用单帧 AVFrame（与 backup 分支一致）。
     * 避免每帧 av_frame_alloc + readRawFrame 内 av_frame_get_buffer 造成瞬时内存峰值与嵌入式 OOM。
     */
    AVFrame* input_frame_;

    /// 源分辨率与编码分辨率不一致时，readRawFrame → swscale → 编码
    bool input_scale_needed_ = false;
    SwsContext* sws_ctx_ = nullptr;
    AVFrame* scaled_frame_ = nullptr;
};

#endif // FFMPEG_ENCODE_WORKER_HPP
