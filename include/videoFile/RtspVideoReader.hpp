#ifndef RTSP_VIDEO_READER_HPP
#define RTSP_VIDEO_READER_HPP

#include "IVideoReader.hpp"
#include "../buffer/Buffer.hpp"
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
 * RtspVideoReader - 基于 FFmpeg 的 RTSP 视频流读取器
 * 
 * 功能：
 * - 连接 RTSP 视频流并解码
 * - 支持两种工作模式：
 *   1. 传统模式：内部缓冲 + readFrameAtThreadSafe拷贝
 *   2. 零拷贝模式：直接注入BufferPool（需调用setBufferPool）
 * 
 * 特点：
 * - 实时流处理（无总帧数概念）
 * - 自动重连机制
 * - 线程安全的帧访问
 * - 支持硬件加速解码（可选）
 * 
 * 使用方式：
 * ```cpp
 * // 方式1：传统模式
 * RtspVideoReader reader;
 * reader.open("rtsp://192.168.1.100:8554/stream");
 * Buffer buffer(frame_size);
 * reader.readFrameTo(buffer);
 * 
 * // 方式2：零拷贝模式（推荐）
 * RtspVideoReader reader;
 * reader.setBufferPool(&pool);  // 启用零拷贝
 * reader.open("rtsp://...");
 * // reader内部直接注入pool，无需手动readFrameTo
 * ```
 */
class RtspVideoReader : public IVideoReader {
private:
    // ============ FFmpeg 资源 ============
    AVFormatContext* format_ctx_;
    AVCodecContext* codec_ctx_;
    SwsContext* sws_ctx_;              // 图像格式转换
    int video_stream_index_;
    
    // ============ RTSP 连接信息 ============
    char rtsp_url_[MAX_RTSP_PATH_LENGTH];
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
    BufferPool* buffer_pool_;          // 可选：零拷贝模式的BufferPool
    
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

public:
    // ============ 构造/析构 ============
    
    RtspVideoReader();
    virtual ~RtspVideoReader();
    
    // 禁止拷贝
    RtspVideoReader(const RtspVideoReader&) = delete;
    RtspVideoReader& operator=(const RtspVideoReader&) = delete;
    
    // ============ IVideoReader 接口实现 ============
    
    bool open(const char* path) override;
    bool openRaw(const char* path, int width, int height, int bits_per_pixel) override;
    void close() override;
    bool isOpen() const override;
    
    bool readFrameTo(Buffer& dest_buffer) override;
    bool readFrameTo(void* dest_buffer, size_t buffer_size) override;
    bool readFrameAt(int frame_index, Buffer& dest_buffer) override;
    bool readFrameAt(int frame_index, void* dest_buffer, size_t buffer_size) override;
    bool readFrameAtThreadSafe(int frame_index, void* dest_buffer, size_t buffer_size) const override;
    
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
    
    const char* getReaderType() const override;
    
    // ============ 零拷贝模式支持 ============
    
    /**
     * 设置BufferPool（启用零拷贝模式）
     * @param pool BufferPool指针（void*避免头文件依赖）
     */
    void setBufferPool(void* pool) override;
    
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
};

#endif // RTSP_VIDEO_READER_HPP

