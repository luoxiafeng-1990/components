#ifndef FFMPEG_DECODER_HPP
#define FFMPEG_DECODER_HPP

#include "IDecoder.hpp"
#include "../buffer/BufferPool.hpp"
#include <string>
#include <map>

// 前向声明FFmpeg结构体
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVCodec;
struct SwsContext;
struct AVBufferRef;

/**
 * FFmpegDecoder - 基于FFmpeg的解码器实现
 * 
 * 特性：
 * - 软件解码，跨平台兼容性好
 * - 支持多线程解码
 * - 支持像素格式转换（通过 libswscale）
 * - **支持零拷贝模式**（通过 get_buffer2 回调直接使用 BufferPool）
 * - 支持硬件加速（VA-API、NVDEC等，通过hwaccel）
 * 
 * 零拷贝工作原理：
 * 1. 初始化时注册 get_buffer2 回调
 * 2. FFmpeg 需要buffer时调用我们的回调
 * 3. 回调从 BufferPool 获取空闲 Buffer
 * 4. FFmpeg 直接解码到这个 Buffer
 * 5. 解码完成后，Buffer 通过 DecodedFrame 返回给用户
 * 6. 用户使用完后，归还给 BufferPool
 * 
 * 参考：Chromium、GStreamer 的零拷贝实现
 */
class FFmpegDecoder : public IDecoder {
public:
    FFmpegDecoder();
    ~FFmpegDecoder() override;
    
    // 禁用拷贝
    FFmpegDecoder(const FFmpegDecoder&) = delete;
    FFmpegDecoder& operator=(const FFmpegDecoder&) = delete;
    
    // ============ IDecoder接口实现 ============
    
    DecoderStatus initialize(const DecoderConfig& config) override;
    void close() override;
    bool isInitialized() const override;
    
    DecoderStatus sendPacket(AVPacket* packet) override;
    DecoderStatus receiveFrame(DecodedFrame& out_frame) override;
    DecoderStatus decode(AVPacket* packet, DecodedFrame& out_frame) override;
    DecoderStatus flush(DecodedFrame& out_frame) override;
    DecoderStatus reset() override;
    
    const DecoderConfig& getConfig() const override;
    const char* getCodecName() const override;
    bool isHardwareAccelerated() const override;
    const char* getDecoderType() const override;
    const char* getLastError() const override;
    int getLastFFmpegError() const override;

private:
    // ============ FFmpeg 上下文 ============
    AVCodecContext* codec_ctx_;      // 解码器上下文
    const AVCodec* codec_;           // 编解码器
    SwsContext* sws_ctx_;            // 像素格式转换上下文（如果需要）
    
    // ============ 配置与状态 ============
    DecoderConfig config_;           // 解码器配置
    bool is_initialized_;            // 是否已初始化
    bool is_flushing_;               // 是否正在flush
    std::string codec_name_;         // 编解码器名称
    std::string last_error_;         // 最后一次错误信息
    int last_ffmpeg_error_;          // 最后一次FFmpeg错误码
    
    // ============ Buffer管理 ============
    BufferAllocationMode buffer_mode_;   // Buffer分配模式
    BufferPool* buffer_pool_;            // 关联的BufferPool
    size_t buffer_alignment_;            // 内存对齐
    
    // Buffer追踪（用于零拷贝模式的生命周期管理）
    // key: AVFrame的data指针, value: Buffer指针
    std::map<uint8_t*, Buffer*> buffer_map_;
    
    // ============ 内部辅助函数 ============
    
    /**
     * 设置错误信息
     */
    void setError(const char* error_msg, int ffmpeg_error = 0);
    
    /**
     * 初始化FFmpeg解码器
     */
    DecoderStatus initializeFFmpeg();
    
    /**
     * 初始化像素格式转换器（如果需要）
     */
    DecoderStatus initializeScaler();
    
    /**
     * 清理FFmpeg资源
     */
    void cleanupFFmpeg();
    
    /**
     * 转换FFmpeg错误码到DecoderStatus
     */
    DecoderStatus convertFFmpegError(int ffmpeg_error);
    
    /**
     * 计算帧大小
     */
    size_t calculateFrameSize(int width, int height, AVPixelFormat format) const;
    
    // ============ 零拷贝核心：get_buffer2 回调 ============
    
    /**
     * FFmpeg的get_buffer2回调（静态函数）
     * 
     * 这是零拷贝的核心实现！
     * 让FFmpeg使用我们的BufferPool分配内存
     */
    static int getBufferCallback(AVCodecContext* ctx, AVFrame* frame, int flags);
    
    /**
     * Buffer释放回调（静态函数）
     * 
     * 当FFmpeg不再使用buffer时调用
     * 我们在这里进行清理和追踪
     */
    static void releaseBufferCallback(void* opaque, uint8_t* data);
    
    /**
     * 实例方法：分配buffer
     */
    int allocateBuffer(AVFrame* frame, int flags);
    
    /**
     * 实例方法：释放buffer
     */
    void deallocateBuffer(uint8_t* data);
};

#endif // FFMPEG_DECODER_HPP
