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
     * - 文件模式：config.decoder.use_buffer_mode = false
     * - Buffer 模式：config.decoder.use_buffer_mode = true
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
    bool open(const char* path) override;
    bool open(const char* path, int width, int height, int bits_per_pixel) override;
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
    int getBytesPerPixel() const override;
    const char* getPath() const override;
    bool hasMoreFrames() const override;
    bool isAtEnd() const override;
    
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
     * @brief 打印统计信息
     */
    void printStats() const;

private:
    // ============ 数据源抽象（v2.9新增）============
    std::unique_ptr<IPacketSource> packet_source_;  // 数据源抽象（文件或Buffer）
    
    // ============ FFmpeg 资源 ============
    // ⚠️ 注意：format_ctx_ptr_ 已移除，由 FilePacketSource 管理
    AVCodecContext* codec_ctx_ptr_;
    // ⚠️ 注意：frame_packet_map_ 已移除（未使用）
    // ⚠️ 注意：sws_ctx_ptr_ 已移除（当前未使用格式转换功能，如需要可在未来添加）
    // ⚠️ 注意：video_stream_index_ 已移除，视频流索引从数据源（IPacketSource::getVideoStreamIndex()）获取
    
    // ============ 文件信息 ============
    // ⚠️ 注意：file_path_ 已移除，文件路径由数据源类（FilePacketSource）管理
    // ⚠️ 注意：width_ 和 height_ 已移除，原始宽高从数据源（IPacketSource）获取
    int output_width_;                 // 输出宽度（可能缩放）
    int output_height_;                 // 输出高度（可能缩放）
    int output_bpp_;                   // 输出位深（如 32 for ARGB888）
    // ⚠️ 注意：output_pixel_format_ 已移除（未使用）
    
    // ============ 解码状态 ============
    // ⚠️ 注意：total_frames_ 已移除，总帧数从数据源（IPacketSource::getTotalFrames()）获取
    int current_frame_index_;          // 当前帧索引
    // ⚠️ 注意：is_open_ 已移除，打开状态从数据源（IPacketSource::isOpen()）获取
    // ⚠️ 注意：eof_reached_ 已移除，EOF 状态从数据源（IPacketSource::isEof()）获取
    
    // ============ 零拷贝模式 ============
    // ⚠️ 注意：zero_copy_buffer_pool_ptr_ 已移除（未使用）
    
    // ============ 解码器配置（用于特殊解码器）============
    bool use_hardware_decoder_;        // 是否使用硬件解码
    std::string decoder_name_;         // 指定解码器名称（如 "h264_taco"），空字符串表示自动选择
    AVDictionary* codec_options_ptr_;      // 解码器选项（用于 h264_taco 配置）
    
    // ============ 线程安全 ============
    // 使用递归锁避免同一线程重入时死锁（例如 fillBuffer -> seek）
    mutable std::recursive_mutex mutex_;
    
    // ============ 统计信息 ============
    std::atomic<int> decoded_frames_;
    std::atomic<int> decode_errors_;
    
    // ============ 错误处理 ============
    std::string last_error_;
    int last_ffmpeg_error_;
    
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

