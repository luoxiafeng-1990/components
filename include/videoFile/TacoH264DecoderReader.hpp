#ifndef TACO_H264_DECODER_READER_HPP
#define TACO_H264_DECODER_READER_HPP

#include "IVideoReader.hpp"
#include "../decoder/TacoHardwareDecoder.hpp"
#include "../buffer/BufferPool.hpp"
#include <memory>
#include <string>
#include <mutex>

// 前向声明 FFmpeg 结构
struct AVFormatContext;
struct AVPacket;

// DMA 配置结构体
struct tpsfb_dma_info {
    uint32_t ovl_idx;      // overlay 索引
    uint64_t phys_addr;    // 物理地址
};

/**
 * TacoH264DecoderReader - Taco 硬件解码型 VideoReader
 * 
 * 职责：
 * - 读取 H.264 文件（通过 AVFormatContext）
 * - 硬件解码（通过 TacoHardwareDecoder）
 * - Overlay ID 管理（Decoder 内部 BufferPool）
 * - DMA 设置（零拷贝显示）
 * 
 * 特性：
 * - 完全自包含：读取 + 解码 + overlay 管理
 * - 零拷贝：数据在 taco_sys 内存，通过 DMA 显示
 * - 注册机制：Decoder 自己注册 BufferPool 到全局注册表
 * - 对外透明：VideoProducer 只知道 IVideoReader 接口
 * 
 * 数据流：
 * 1. open() → 初始化 FFmpeg + Decoder + BufferPool（Decoder 注册）
 * 2. readFrame() → 读取packet → 解码 → DMA设置 → 返回成功
 * 3. 外部通过 getOutputBufferPool() 获取 Decoder 的 overlay pool
 * 4. 显示线程从 pool 获取 overlay ID，调用 FBIOPAN_DISPLAY
 * 
 * 架构：
 * ```
 * TacoH264DecoderReader
 * ├─ AVFormatContext (读取H.264文件)
 * ├─ TacoHardwareDecoder
 * │   ├─ AVCodecContext (h264_taco解码)
 * │   ├─ BufferPool (overlay ID池，已注册)
 * │   └─ AVFrame[0-3] (预分配)
 * └─ framebuffer_fd (用于DMA设置)
 * ```
 */
class TacoH264DecoderReader : public IVideoReader {
public:
    TacoH264DecoderReader();
    ~TacoH264DecoderReader() override;
    
    // 禁止拷贝
    TacoH264DecoderReader(const TacoH264DecoderReader&) = delete;
    TacoH264DecoderReader& operator=(const TacoH264DecoderReader&) = delete;
    
    // ============ 配置接口 ============
    
    /**
     * @brief 设置 framebuffer 文件描述符（在 open 之前调用）
     * @param fd framebuffer fd
     */
    void setFramebufferFd(int fd) { framebuffer_fd_ = fd; }
    
    /**
     * @brief 设置 overlay 数量（在 open 之前调用）
     * @param count overlay 数量（1-4）
     */
    void setOverlayCount(int count) { overlay_count_ = count; }
    
    /**
     * @brief 获取 BufferPool 名称（用于外部访问）
     * @return BufferPool 注册名称
     */
    std::string getBufferPoolName() const;
    
    // ============ IVideoReader 接口实现 ============
    
    bool open(const char* path) override;
    bool openRaw(const char* path, int width, int height, int bits_per_pixel) override;
    void close() override;
    bool isOpen() const override;
    
    bool requiresExternalBuffer() const override {
        // 零拷贝模式，不需要外部 buffer
        return false;
    }
    
    bool readFrameTo(Buffer& dest_buffer) override;
    bool readFrameTo(void* dest_buffer, size_t buffer_size) override;
    bool readFrameAt(int frame_index, Buffer& dest_buffer) override;
    bool readFrameAt(int frame_index, void* dest_buffer, size_t buffer_size) override;
    bool readFrameAtThreadSafe(int frame_index, void* dest_buffer, size_t buffer_size) const override;
    
    /**
     * @brief 🆕 读取并填充一帧（统一接口）
     * @param frame_index 帧索引
     * @param buffer 输出 Buffer（从 Decoder 的 BufferPool 获取）
     * @return 成功返回 true
     * 
     * 内部流程：
     * 1. buffer->id() 是 overlay ID
     * 2. 读取 AVPacket
     * 3. 解码到 AVFrame[overlay_id]（复用）
     * 4. 提取物理地址
     * 5. 设置 DMA（将物理地址绑定到 overlay）
     * 6. 返回成功（buffer 由调用者提交到 filled 队列）
     */
    bool readFrame(int frame_index, Buffer* buffer) override;
    
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
    
    const char* getReaderType() const override { 
        return "TacoH264DecoderReader"; 
    }
    
    /**
     * @brief 🆕 获取输出 BufferPool（Decoder 的 overlay pool）
     */
    void* getOutputBufferPool() const override;
    
private:
    // ============ FFmpeg 上下文（读取文件）============
    
    AVFormatContext* format_ctx_;        // 格式上下文
    int video_stream_idx_;               // 视频流索引
    AVPacket* packet_;                   // 数据包
    
    // ============ 解码器（Decoder自己管理BufferPool）============
    
    std::unique_ptr<TacoHardwareDecoder> decoder_;  // Taco 硬件解码器
    
    // ============ 配置 ============
    
    std::string file_path_;              // 文件路径
    int framebuffer_fd_;                 // framebuffer fd
    int overlay_count_;                  // overlay 数量
    bool is_open_;                       // 是否已打开
    
    // 视频参数
    int width_;
    int height_;
    int total_frames_;
    double fps_;
    size_t frame_size_;
    long file_size_;
    
    // ============ 帧索引管理 ============
    
    int current_frame_index_;            // 当前帧索引
    mutable std::mutex read_mutex_;      // 读取锁
    
    // ============ 内部辅助函数 ============
    
    /**
     * @brief 初始化 FFmpeg（打开文件，查找流）
     */
    bool initializeFFmpeg(const char* path);
    
    /**
     * @brief 初始化 Decoder（创建 + 注册 BufferPool）
     */
    bool initializeDecoder();
    
    /**
     * @brief 读取一个 AVPacket
     */
    bool readPacket(AVPacket* packet);
    
    /**
     * @brief 设置 DMA
     */
    bool setupDMA(uint32_t overlay_id, uint64_t phys_addr);
    
    /**
     * @brief 清理 FFmpeg 资源
     */
    void cleanupFFmpeg();
    
    /**
     * @brief 设置错误信息
     */
    void setError(const char* error);
    
    // 错误信息
    std::string last_error_;
};

#endif // TACO_H264_DECODER_READER_HPP

