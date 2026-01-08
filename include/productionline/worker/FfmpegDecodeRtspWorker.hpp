#ifndef FFMPEG_DECODE_RTSP_WORKER_HPP
#define FFMPEG_DECODE_RTSP_WORKER_HPP

#include "productionline/worker/WorkerBase.hpp"
#include "productionline/worker/IPacketSource.hpp"
#include "buffer/bufferpool/Buffer.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <memory>

// FFmpeg 前向声明
struct AVCodecContext;
struct AVCodecParameters;
struct AVPacket;
struct AVFrame;

// 前向声明 BufferPool（避免循环依赖）
class BufferPool;

#define MAX_RTSP_PATH_LENGTH 512

/**
 * @brief FfmpegDecodeRtspWorker - FFmpeg解码RTSP流Worker
 * 
 * 架构角色：Worker（工人）- FFmpeg解码RTSP流类型
 * 
 * 功能：使用FFmpeg解码RTSP视频流
 * 目的：填充Buffer，得到填充后的buffer
 * 
 * 功能：
 * - 连接 RTSP 视频流并解码
 * - 同步解码模式：fillBuffer() 直接解码到 AVFrame（与 VideoFileWorker 一致）
 * - 零拷贝模式：利用特殊解码器（如 h264_taco）的物理地址
 * - 支持硬件加速解码（可选，通过 WorkerConfig 配置）
 * 
 * 特点：
 * - 实时流处理（无总帧数概念）
 * - 线程安全的帧访问
 * - 支持解码器配置（v2.2）：硬件/软件、解码器名称、特殊配置
 * - BufferPool 自动创建（v2.0架构要求）
 * 
 * 使用方式：
 * ```cpp
 * // v2.2: 使用配置构造函数
 * auto config = WorkerConfigBuilder()
 *     .setDecoderConfig(
 *         DecoderConfigBuilder().useTaco("h264").build()
 *     )
 *     .build();
 * FfmpegDecodeRtspWorker worker(config);
 * worker.open("rtsp://192.168.1.100:8554/stream", 1920, 1080, 32);
 * 
 * // v2.0: 获取 BufferPool ID
 * uint64_t pool_id = worker.getOutputBufferPoolId();
 * auto pool = BufferPoolRegistry::getInstance().getPool(pool_id).lock();
 * 
 * // 同步解码
 * Buffer* buffer = pool->acquireFree(true, 100);
 * worker.fillBuffer(0, buffer);  // 直接解码到 Buffer 的 AVFrame
 * pool->submitFilled(buffer);
 * ```
 */
class FfmpegDecodeRtspWorker : public WorkerBase {
public:
    // ============ 构造/析构 ============
    
    /**
     * @brief 构造函数（必须提供配置）
     * @param config Worker配置（包含解码器配置、RTSP配置等）
     * 
     * 注意：不再提供默认构造函数，所有 Worker 必须通过配置创建
     */
    explicit FfmpegDecodeRtspWorker(const WorkerConfig& config);
    virtual ~FfmpegDecodeRtspWorker();
    
    // 禁止拷贝
    FfmpegDecodeRtspWorker(const FfmpegDecodeRtspWorker&) = delete;
    FfmpegDecodeRtspWorker& operator=(const FfmpegDecodeRtspWorker&) = delete;
    
    // ============ WorkerBase 接口实现 ============
    
    // Buffer填充功能（原IBufferFillingWorker的方法）
    bool fillBuffer(int frame_index, Buffer* buffer) override;
    const char* getWorkerType() const override {
        return "FfmpegDecodeRtspWorker";
    }
    
    // 文件导航功能（继承自IVideoFileNavigator）
    /**
     * @brief 打开 RTSP 流（无参版本，从 worker_config_ 读取所有参数）
     * 
     * v2.13 架构：Worker 从 worker_config_ 读取所有配置参数
     * 包括：
     * - worker_config_.data_source.path（RTSP URL）
     * - worker_config_.display.width/height/bits_per_pixel
     * - worker_config_.decoder.name（解码器名称）
     * - worker_config_.decoder.taco（TACO 配置）
     * 
     * @return 成功返回 true
     */
    bool open() override;
    
    /**
     * @brief 打开 RTSP 流（单参数版本）
     * 
     * ❌ RTSP 流不支持此方法，因为必须指定输出分辨率和格式
     * 请使用无参的 open()，并在 WorkerConfig 中配置参数
     * 
     * @return false（不支持）
     */
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
    int getWidth() const override;
    int getHeight() const override;
    double getBytesPerPixel() const override;
    const char* getPath() const override;
    bool hasMoreFrames() const override;
    bool isAtEnd() const override;
    
    // ============ RTSP 特有接口 ============
    
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
     * 获取最后错误信息
     */
    std::string getLastError() const;
    
    /**
     * @brief 获取编解码器参数（用于 BufferWriter 等场景）
     * @return 编解码器参数指针，如果不可用则返回 nullptr
     */
    const struct AVCodecParameters* getCodecParameters() const override;
    
    /**
     * @brief 获取时间基（用于 BufferWriter 等场景）
     * @return 时间基
     */
    struct AVRational getTimeBase() const override;
    
    /**
     * 打印统计信息
     */
    void printStats() const;

private:
    // ============ 数据源抽象（v2.12新增）============
    std::unique_ptr<IPacketSource> packet_source_;  // 数据源抽象（RTSP流）
    
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
    
    // ============ 错误处理 ============
    std::string last_error_;
    
    // ============ 内部辅助方法 ============
    
    /**
     * 初始化解码器（支持硬件解码和配置）
     * @param codec_params 编解码器参数（从 packet_source_ 获取）
     */
    bool initializeDecoder(const AVCodecParameters* codec_params);
    
    /**
     * 配置特殊解码器（如 h264_taco）
     * @return true 如果成功
     */
    bool configureSpecialDecoder();
    
    /**
     * 设置错误信息
     */
    void setError(const std::string& error, int ffmpeg_error = 0);
    
    /**
     * @brief 从 AVFrame 中提取硬件解码器的物理内存地址（重写基类虚函数）
     * 
     * 职责：从 AVFrame 中提取硬件解码器的物理内存地址
     * 
     * 设计原则：
     * - 此函数只在使用硬件解码器时调用（decoder_name_ 非空）
     * - 不同硬件解码器有不同的提取方式
     * - 提取失败返回 false，调用者会报错并终止解码
     * 
     * @param frame AVFrame 指针（需要包含 libavcodec/avcodec.h）
     * @param buffer Buffer 指针（用于存储提取的物理地址）
     * @return true 成功提取物理地址，false 提取失败或不支持
     * 
     * @note 与 FfmpegDecodeVideoFileWorker 保持一致的架构
     */
    virtual bool extractHardwareAddressFromMetadata(struct AVFrame* frame, Buffer* buffer) override;
};

#endif // FFMPEG_DECODE_RTSP_WORKER_HPP

