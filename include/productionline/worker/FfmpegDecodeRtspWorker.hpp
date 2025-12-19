#ifndef FFMPEG_DECODE_RTSP_WORKER_HPP
#define FFMPEG_DECODE_RTSP_WORKER_HPP

#include "productionline/worker/WorkerBase.hpp"
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
struct AVFormatContext;
struct AVCodecContext;
struct AVCodecParameters;
struct AVPacket;
struct AVFrame;
struct SwsContext;

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
 *         DecoderConfigBuilder().useH264Taco().build()
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
    
    FfmpegDecodeRtspWorker();
    FfmpegDecodeRtspWorker(const WorkerConfig& config);  // v2.2: 配置构造函数
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
    uint64_t getOutputBufferPoolId() override;
    
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
    bool isConnected() const { return connected_.load(); }
    
    /**
     * 获取最后错误信息
     */
    std::string getLastError() const;
    
    /**
     * 打印统计信息
     */
    void printStats() const;

private:
    // ============ FFmpeg 资源 ============
    AVFormatContext* format_ctx_ptr_;
    AVCodecContext* codec_ctx_ptr_;
    SwsContext* sws_ctx_ptr_;              // 图像格式转换
    int video_stream_index_;
    
    // ============ RTSP 连接信息 ============
    std::string rtsp_url_;            // RTSP URL（使用 std::string 更安全）
    int width_;                        // 输出宽度
    int height_;                       // 输出高度
    int output_pixel_format_;          // 输出像素格式（如AV_PIX_FMT_BGRA）
    int output_bpp_;                   // 输出每像素位数
    
    // ============ 解码器配置（v2.2新增）============
    bool use_hardware_decoder_;        // 是否使用硬件解码
    std::string decoder_name_;         // 指定解码器名称（如 "h264_taco"），空字符串表示自动选择
    struct AVDictionary* codec_options_ptr_;  // 解码器选项（用于 h264_taco 配置）
    
    // ============ 统计信息 ============
    std::atomic<int> decoded_frames_;
    std::atomic<int> dropped_frames_;
    
    // ============ 状态 ============
    std::atomic<bool> connected_;
    bool is_open_;
    std::atomic<bool> eof_reached_;    // 流结束标志
    
    // ============ 线程安全 ============
    mutable std::recursive_mutex mutex_;  // 使用递归锁避免死锁
    
    // ============ 错误处理 ============
    std::string last_error_;
    mutable std::mutex error_mutex_;
    
    // ============ 内部辅助方法 ============
    
    /**
     * 连接 RTSP 流
     */
    bool connectRTSP();
    
    /**
     * 断开 RTSP 连接并释放资源
     */
    void disconnectRTSP();
    
    /**
     * 查找视频流
     */
    bool findVideoStream();
    
    /**
     * 初始化解码器（支持硬件解码和配置）
     */
    bool initializeDecoder();
    
    /**
     * 配置特殊解码器（如 h264_taco）
     * @return true 如果成功
     */
    bool configureSpecialDecoder();
    
    /**
     * 设置错误信息
     */
    void setError(const std::string& error, int ffmpeg_error = 0);
};

#endif // FFMPEG_DECODE_RTSP_WORKER_HPP

