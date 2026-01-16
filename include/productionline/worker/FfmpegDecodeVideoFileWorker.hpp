#ifndef FFMPEG_DECODE_VIDEO_FILE_WORKER_HPP
#define FFMPEG_DECODE_VIDEO_FILE_WORKER_HPP

#include "productionline/worker/WorkerBase.hpp"
#include "productionline/worker/IPacketSource.hpp"
#include "buffer/bufferpool/Buffer.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include <string>
#include <memory>
#include <atomic>
#include <mutex>

// FFmpeg 前向声明
struct AVFormatContext;
struct AVCodecContext;
struct AVCodecParameters;
struct AVPacket;
struct AVFrame;
struct SwsContext;
struct AVDictionary;


/**
 * @brief FfmpegDecodeVideoFileWorker - FFmpeg解码视频文件Worker
 * 
 * 架构角色：Worker（工人）- FFmpeg解码视频文件类型
 * 
 * 功能：使用FFmpeg解码视频文件（MP4, AVI, MKV, MOV, FLV等）
 * 目的：填充Buffer，得到填充后的buffer
 * 
 * 职责：
 * - 打开本地编码视频文件（MP4, AVI, MKV, MOV, FLV等）
 * - 使用 FFmpeg 进行解码（支持软/硬件加速）
 * - 支持两种工作模式：
 *   1. 普通模式：解码后 memcpy 到外部 Buffer
 *   2. 零拷贝模式：利用特殊解码器（如 h264_taco）的物理地址，动态注入 BufferPool
 * - 提供BufferPool（原材料）给ProductionLine
 * 
 * 特性：
 * - 支持多种编码格式（H.264, H.265, VP9, AV1等）
 * - 自动检测硬件加速能力
 * - 支持格式转换（YUV → ARGB888）
 * - 零拷贝优化（当硬件支持时）
 * - 线程安全的帧访问
 * 
 * 使用方式：
 * ```cpp
 * FfmpegDecodeVideoFileWorker worker;
 * worker.open("video.mp4");
 * // v2.0: Worker在open()时自动创建BufferPool并注册到Registry
 * uint64_t pool_id = worker.getOutputBufferPoolId();
 * // 从Registry获取Pool
 * auto pool = BufferPoolRegistry::getInstance().getPool(pool_id);
 * Buffer buffer(frame_size);
 * worker.fillBuffer(0, &buffer);  // 填充buffer
 * ```
 */
class FfmpegDecodeVideoFileWorker : public WorkerBase {
public:
    // ============ 构造/析构 ============
    
    /**
     * @brief 构造函数（必须提供配置）
     * @param config Worker配置（包含解码器配置、数据源配置等）
     * 
     * 注意：不再提供默认构造函数，所有 Worker 必须通过配置创建
     * - 文件数据源模式：config.decoder.datasource_buffer_mode = false
     * - Buffer 数据源模式：config.decoder.datasource_buffer_mode = true
     */
    explicit FfmpegDecodeVideoFileWorker(const WorkerConfig& config);
    
    virtual ~FfmpegDecodeVideoFileWorker();
    
    // 禁止拷贝
    FfmpegDecodeVideoFileWorker(const FfmpegDecodeVideoFileWorker&) = delete;
    FfmpegDecodeVideoFileWorker& operator=(const FfmpegDecodeVideoFileWorker&) = delete;
    
    // ============ WorkerBase 接口实现 ============
    
    // Buffer填充功能（原IBufferFillingWorker的方法）
    bool fillBuffer(int frame_index, Buffer* buffer) override;
    const char* getWorkerType() const override {
        return "FfmpegDecodeVideoFileWorker";
    }
    
    // 文件导航功能（继承自IVideoFileNavigator）
    /**
     * @brief 打开视频文件（单参数版本，支持覆盖 config 中的路径）
     * 
     * v2.13 架构：Worker 从 worker_config_ 读取配置参数
     * - worker_config_.display.width/height/bits_per_pixel
     * - worker_config_.decoder.name（解码器名称）
     * - worker_config_.decoder.taco（TACO 配置）
     * 
     * @param path 视频文件路径（可以覆盖 config 中的路径）
     * @return 成功返回 true
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
    
    // ============ v2.13 BufferPacketSource 配置 ============
    
    /**
     * @brief 设置 BufferPacketSource 的源 BufferPool（用于 Buffer 模式）
     * @param pool_weak Record Worker 的 BufferPool（weak_ptr）
     * @return 成功返回 true，如果不是 Buffer 模式或数据源类型不对，返回 false
     * 
     * 使用场景：
     * - MultiWorkerProductionLine 创建消费者 Worker 后，需要关联 Record Worker 的 BufferPool
     * - 必须在 open() 之前调用
     * 
     * 示例：
     * ```cpp
     * // 1. 创建 Record Worker 并获取 BufferPool
     * uint64_t record_pool_id = record_worker.getOutputBufferPoolId(BufferPoolType::PACKET_VIDEO);
     * auto record_pool_weak = BufferPoolRegistry::getInstance().getPool(record_pool_id);
     * 
     * // 2. 创建消费者 Worker（Buffer 模式）
     * FfmpegDecodeVideoFileWorker consumer_worker(config);
     * 
     * // 3. 关联 Record BufferPool
     * consumer_worker.setSourceBufferPool(record_pool_weak);
     * 
     * // 4. 打开并使用
     * consumer_worker.open();
     * ```
     */
    bool setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) override;
    
    // ============ 信息查询 ============
    
    /**
     * @brief 获取最后错误信息
     */
    std::string getLastError() const;
    
    /**
     * @brief 获取编解码器名称
     */
    const char* getCodecName() const;
    
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
    int getSourceWidth() const override;
    int getSourceHeight() const override;
    AVPixelFormat getSourcePixelFormat() const override;
    
    /**
     * @brief 打印统计信息
     */
    void printStats() const;

private:
    // ============ Logger ============
    log4cplus::Logger logger_;
    
    // ============ 数据源抽象（v2.9新增）============
    // ⭐ v2.20 修改：从 unique_ptr 改为 shared_ptr（支持 ONE_TO_MANY 共享模式）
    std::shared_ptr<IPacketSource> packet_source_;  // 数据源抽象（文件或Buffer）
    
    
    AVCodecContext* codec_ctx_ptr_;
  
    int output_width_;                 // 输出宽度（运行时状态，可能缩放）
    int output_height_;                // 输出高度（运行时状态，可能缩放）
    int current_frame_index_;          // 当前帧索引
   
    
    // ============ 解码器配置（用于特殊解码器）============
    bool use_hardware_decoder_;        // 是否使用硬件解码
    std::string decoder_name_;         // 指定解码器名称（如 "h264_taco"），空字符串表示自动选择
    AVDictionary* codec_options_ptr_;      // 解码器选项（用于 h264_taco 配置）
    
    // ============ 线程安全 ============
    // 使用递归锁避免同一线程重入时死锁（例如 fillBuffer -> seek）
    mutable std::recursive_mutex mutex_;
    
    // ============ 统计信息 ============
    std::atomic<int> decoded_frames_;
    std::atomic<int> dropped_frames_;  // 丢帧计数
    
    // ============ 错误处理 ============
    std::string last_error_;
    
    // ============ 帧缓存（用于多通道解码）============
    std::vector<AVFrame*> cached_frames_;  // 缓存解码后的帧（用于处理双通道等场景）
    
    // ============ 内部辅助方法 ============
    
    /**
     * @brief 初始化解码器
     * @param codec_params 编解码器参数（必须提供，从 packet_source_ 获取）
     */
    bool initializeDecoder(const AVCodecParameters* codec_params);
    
    /**
     * @brief 配置特殊解码器（如 h264_taco）
     */
    bool configureSpecialDecoder();
    
    /**
     * @brief 从数据源读取 packet 并发送到解码器
     * @param packet_ptr AVPacket 指针（必须已分配）
     * @return true 成功发送 packet 到解码器，false 失败或 EOF
     * 
     * 功能：
     * - 从 packet_source_ 读取 packet
     * - 处理损坏的 packet（自动重试）
     * - 过滤非视频流的 packet（仅文件模式）
     * - 调用 avcodec_send_packet 发送到解码器
     */
    bool readAndSendPacket(AVPacket* packet_ptr);
    
    /**
     * @brief 从 AVFrame 填充 Buffer 的元数据
     * @param frame_ptr AVFrame 指针（必须已填充数据）
     * @param buffer Buffer 指针（用于存储元数据）
     * @return true 成功设置元数据，false 失败
     * 
     * 功能：
     * - 提取硬件解码器的物理地址（如果使用硬件解码）
     * - 设置虚拟地址（frame->data[0]）
     * - 计算并设置帧大小
     * - 设置图像元数据（格式、宽高、linesize 等）
     * - 更新统计计数器
     */
    bool fillBufferMetadataFromFrame(AVFrame* frame_ptr, Buffer* buffer);
    
    /**
     * @brief 从AVFrame元数据中提取硬件解码器的物理内存地址（重写基类）
     * 
     * 实现 h264_taco 硬件解码器的物理地址提取逻辑：
     * - 从 AVFrame->metadata 中提取 "pool_blk_id"
     * - 调用 taco_sys_handle2_phys_addr() 转换为物理地址
     * - 将物理地址存储到 Buffer
     * 
     * @param frame AVFrame 指针
     * @param buffer Buffer 指针
     * @return true 成功提取物理地址，false 提取失败
     */
    virtual bool extractHardwareAddressFromMetadata(struct AVFrame* frame, Buffer* buffer) override;
    
    /**
     * @brief 设置错误信息
     */
    void setError(const std::string& error, int ffmpeg_error = 0);
    
    /**
     * @brief 获取原始宽度（从数据源获取）
     * @return 原始宽度，如果不可用则返回 0
     */
    int getOriginalWidth() const;
    
    /**
     * @brief 获取原始高度（从数据源获取）
     * @return 原始高度，如果不可用则返回 0
     */
    int getOriginalHeight() const;
};

#endif // FFMPEG_DECODE_VIDEO_FILE_WORKER_HPP

