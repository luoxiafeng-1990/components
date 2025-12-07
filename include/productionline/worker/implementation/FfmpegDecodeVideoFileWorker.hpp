#ifndef FFMPEG_DECODE_VIDEO_FILE_WORKER_HPP
#define FFMPEG_DECODE_VIDEO_FILE_WORKER_HPP

#include "../base/WorkerBase.hpp"
#include "../../../buffer/Buffer.hpp"
#include "../../../buffer/BufferPool.hpp"
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <map>

// FFmpeg 前向声明
struct AVFormatContext;
struct AVCodecContext;
struct AVCodecParameters;
struct AVPacket;
struct AVFrame;
struct SwsContext;
struct AVDictionary;

#define MAX_VIDEO_PATH_LENGTH 1024

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
    
    FfmpegDecodeVideoFileWorker();
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
    uint64_t getOutputBufferPoolId() override;  // v2.0: 返回 pool_id
    
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
    
    // ============ 扩展配置接口 ============
    
    /**
     * @brief 设置输出分辨率（在open之前调用）
     */
    void setOutputResolution(int width, int height);
    
    /**
     * @brief 设置输出位深（在open之前调用）
     */
    void setOutputBitsPerPixel(int bpp);
    
    /**
     * @brief 指定解码器名称（如 "h264_taco"）
     * 
     * 重写基类方法，提供实际实现
     */
    void setDecoderName(const char* decoder_name) override;
    
    /**
     * @brief 启用/禁用硬件解码
     */
    void setHardwareDecoder(bool enable);
    
    // ============ 信息查询 ============
    
    /**
     * @brief 获取已解码帧数
     */
    int getDecodedFrames() const { return decoded_frames_.load(); }
    
    /**
     * @brief 获取解码错误数
     */
    int getDecodeErrors() const { return decode_errors_.load(); }
    
    /**
     * @brief 获取最后错误信息
     */
    std::string getLastError() const;
    
    /**
     * @brief 获取FFmpeg错误码
     */
    int getLastFFmpegError() const { return last_ffmpeg_error_; }
    
    /**
     * @brief 获取编解码器名称
     */
    const char* getCodecName() const;
    
    /**
     * @brief 打印统计信息
     */
    void printStats() const;
    
    /**
     * @brief 打印视频信息
     */
    void printVideoInfo() const;

private:
    // ============ FFmpeg 资源 ============
    AVFormatContext* format_ctx_ptr_;
    AVCodecContext* codec_ctx_ptr_;
    AVPacket* packet_ptr_;                 // 用于读取和解码的数据包
    std::map<int, std::pair<AVFrame*, AVPacket*>> frame_packet_map_;    // 用于存储解码后的帧和对应的packet
    SwsContext* sws_ctx_ptr_;              // 图像格式转换
    int video_stream_index_;
    
    // ============ 文件信息 ============
    char file_path_[MAX_VIDEO_PATH_LENGTH];
    int width_;                        // 视频原始宽度
    int height_;                       // 视频原始高度
    int output_width_;                 // 输出宽度（可能缩放）
    int output_height_;                 // 输出高度（可能缩放）
    int output_bpp_;                   // 输出位深（如 32 for ARGB888）
    int output_pixel_format_;          // 输出像素格式（如 AV_PIX_FMT_BGRA）
    
    // ============ 解码状态 ============
    int total_frames_;                 // 总帧数（估算）
    int current_frame_index_;          // 当前帧索引
    std::atomic<bool> is_open_;        // 🎯 原子变量，保证线程安全的状态检查（Worker业务层面）
    std::atomic<bool> is_ffmpeg_opened_;  // 🎯 原子变量，保证线程安全的FFmpeg资源状态检查
    bool eof_reached_;
    
    // ============ 零拷贝模式 ============
    BufferPool* zero_copy_buffer_pool_ptr_;            // 可选：零拷贝模式的BufferPool（外部提供）
    
    // ============ 解码器配置（用于特殊解码器）============
    bool use_hardware_decoder_;        // 是否使用硬件解码
    const char* decoder_name_ptr_;         // 指定解码器名称（如 "h264_taco"）
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
     * @brief 打开FFmpeg资源并初始化解码器
     */
    bool openFfmpegResources();
    
    /**
     * @brief 关闭FFmpeg资源并释放资源
     */
    void closeFfmpegResources();
    
    /**
     * @brief 查找视频流
     */
    bool findVideoStream();
    
    /**
     * @brief 初始化解码器
     */
    bool initializeDecoder();
    
    /**
     * @brief 配置特殊解码器（如 h264_taco）
     */
    bool configureSpecialDecoder();
    
    /**
     * @brief 初始化格式转换器
     */
    bool initializeSwsContext();
    
    /**
     * @brief 解码一帧
     * @return AVFrame* 解码后的帧，失败返回nullptr
     */
    AVFrame* decodeOneFrame();
    
    /**
     * @brief 将AVFrame转换为目标格式
     * @param src_frame 源帧
     * @param dest 目标地址
     * @param dest_size 目标大小
     * @return true 如果成功
     */
    bool convertFrameTo(AVFrame* src_frame, void* dest, size_t dest_size);
    
    
    /**
     * @brief 从AVFrame提取物理地址（零拷贝模式）
     * @param frame AVFrame指针
     * @return 物理地址，失败返回0
     */
    uint64_t extractPhysicalAddress(AVFrame* frame);
    
    /**
     * @brief 创建零拷贝Buffer并注入BufferPool
     * @param frame AVFrame指针（解码器内存）
     * @return Buffer指针，失败返回nullptr
     */
    Buffer* createZeroCopyBuffer(AVFrame* frame);
    
    /**
     * @brief 估算总帧数
     */
    int estimateTotalFrames();
    
    /**
     * @brief 设置错误信息
     */
    void setError(const std::string& error, int ffmpeg_error = 0);
};

#endif // FFMPEG_DECODE_VIDEO_FILE_WORKER_HPP

