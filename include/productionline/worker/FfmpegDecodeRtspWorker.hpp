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
 * - 支持两种工作模式：
 *   1. 传统模式：内部缓冲 + fillBuffer拷贝
 *   2. 零拷贝模式：直接注入BufferPool（Worker自动创建）
 * 
 * 特点：
 * - 实时流处理（无总帧数概念）
 * - 自动重连机制
 * - 线程安全的帧访问
 * - 支持硬件加速解码（可选）
 * 
 * 使用方式：
 * ```cpp
 * FfmpegDecodeRtspWorker worker;
 * worker.open("rtsp://192.168.1.100:8554/stream", width, height, bpp);
 * // Worker在open()时自动创建BufferPool（如果需要）
 * std::unique_ptr<BufferPool> pool = worker.getOutputBufferPool();
 * Buffer buffer(frame_size);
 * worker.fillBuffer(0, &buffer);
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
    
    // ============ 解码线程 ============
    std::thread decode_thread_;
    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    
    // ============ 内部帧缓冲（传统模式）============
    struct FrameSlot {
        std::vector<uint8_t> data;     // 帧数据
        bool filled;                   // 是否已填充
        uint64_t timestamp;            // 时间戳
    };
    std::vector<FrameSlot> internal_buffer_;  // 环形缓冲区（默认30帧）
    int write_index_;                  // 写入索引
    int read_index_;                   // 读取索引
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    
    // ============ 零拷贝模式 ============
    BufferPool* buffer_pool_ptr_;          // 可选：零拷贝模式的BufferPool
    
    // ============ 统计信息 ============
    std::atomic<int> decoded_frames_;
    std::atomic<int> dropped_frames_;
    
    // ============ 状态 ============
    bool is_open_;
    std::atomic<bool> eof_reached_;    // 流结束标志
    
    // ============ 错误处理 ============
    std::string last_error_;
    mutable std::mutex error_mutex_;
    
    // ============ 内部辅助方法 ============
    
    /**
     * 连接 RTSP 流并初始化解码器
     */
    bool connectRTSP();
    
    /**
     * 断开 RTSP 连接并释放资源
     */
    void disconnectRTSP();
    
    /**
     * 解码线程主函数
     */
    void decodeThreadFunc();
    
    /**
     * 从RTSP接收并解码一帧
     * @return AVFrame* 解码后的帧，失败返回nullptr
     */
    AVFrame* decodeOneFrame();
    
    /**
     * 将AVFrame转换为目标格式并存储
     * @param frame 源帧
     * @param dest 目标地址
     * @param dest_size 目标大小
     * @return true 如果成功
     */
    bool convertAndStore(AVFrame* frame, void* dest, size_t dest_size);
    
    /**
     * 存储帧到内部缓冲区（传统模式）
     */
    void storeToInternalBuffer(AVFrame* frame);
    
    /**
     * 从内部缓冲区拷贝帧（传统模式）
     */
    bool copyFromInternalBuffer(void* dest, size_t size);
    
    /**
     * 设置错误信息
     */
    void setError(const std::string& error);
    
    /**
     * 获取AVFrame的物理地址（如果可用）
     */
    uint64_t getAVFramePhysicalAddress(AVFrame* frame);
};

#endif // FFMPEG_DECODE_RTSP_WORKER_HPP

