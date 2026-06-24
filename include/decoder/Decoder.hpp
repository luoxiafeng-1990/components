#ifndef DECODER_HPP
#define DECODER_HPP

#include "IDecoder.hpp"
#include "DecoderFactory.hpp"
#include "../buffer/BufferPool.hpp"
#include <memory>
#include <string>

/**
 * Decoder - 解码器门面类
 * 
 * 提供统一、简洁的解码器使用接口
 * 内部持有具体解码器实例，封装所有实现细节
 * 
 * 设计模式：门面模式（Facade Pattern）
 * - 为复杂的子系统提供简单统一的接口
 * - 隐藏内部实现细节
 * - 降低客户端使用复杂度
 * 
 * 优势：
 * - 使用简单：一个类搞定所有解码需求
 * - 接口稳定：内部实现变化不影响客户端
 * - 易于维护：集中管理解码器生命周期
 * 
 * 使用示例（零拷贝模式）：
 * ```cpp
 * // 创建BufferPool
 * BufferPool pool("Decoder_Pool", "Decoder", 10);
 * 
 * // 创建解码器
 * Decoder decoder(DecoderFactory::DecoderType::FFMPEG);
 * 
 * // 配置（使用FFmpeg原生类型）
 * decoder.setCodec(AV_CODEC_ID_H264);
 * decoder.setOutputFormat(1920, 1080, AV_PIX_FMT_NV12);
 * decoder.setBufferMode(BufferAllocationMode::ZERO_COPY);
 * decoder.attachBufferPool(&pool);
 * 
 * // 初始化
 * if (decoder.open() != DecoderStatus::OK) {
 *     printf("Failed: %s\n", decoder.getLastError());
 *     return;
 * }
 * 
 * // 解码（零拷贝）
 * AVPacket* packet = ...;
 * decoder.sendPacket(packet);
 * 
 * DecodedFrame frame;
 * while (decoder.receiveFrame(frame) == DecoderStatus::OK) {
 *     // frame.buffer 直接指向BufferPool的Buffer！
 *     display.displayBufferByDMA(frame.buffer);
 *     
 *     frame.release();
 *     pool.releaseFilled(frame.buffer);
 * }
 * 
 * decoder.close();
 * ```
 */
class Decoder {
public:
    // ============ 构造与析构 ============
    
    /**
     * 构造函数
     * @param type 解码器类型（默认FFMPEG）
     */
    explicit Decoder(DecoderFactory::DecoderType type = DecoderFactory::DecoderType::FFMPEG);
    
    /**
     * 析构函数（自动关闭解码器）
     */
    ~Decoder();
    
    // 禁用拷贝
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    
    // 支持移动
    Decoder(Decoder&& other) noexcept;
    Decoder& operator=(Decoder&& other) noexcept;
    
    // ============ 配置接口 ============
    
    /**
     * 设置解码器类型（必须在open之前）
     */
    bool setDecoderType(DecoderFactory::DecoderType type);
    
    /**
     * 设置编解码器（通过ID，推荐）
     */
    bool setCodec(AVCodecID codec_id);
    
    /**
     * 设置编解码器（通过名称）
     */
    bool setCodec(const char* codec_name);
    
    /**
     * 设置输出格式（直接使用FFmpeg类型）
     */
    bool setOutputFormat(int width, int height, AVPixelFormat pix_fmt);
    
    /**
     * 设置线程数
     */
    bool setThreadCount(int thread_count);
    
    /**
     * 设置Buffer分配模式
     */
    bool setBufferMode(BufferAllocationMode mode);
    
    /**
     * 关联BufferPool（零拷贝模式必需）
     */
    bool attachBufferPool(BufferPool* pool);
    
    /**
     * 设置extradata（SPS/PPS等）
     */
    bool setExtraData(const uint8_t* data, int size);
    
    /**
     * 设置时间基准
     */
    bool setTimeBase(AVRational time_base);
    
    // ============ 生命周期管理 ============
    
    /**
     * 初始化并打开解码器
     */
    DecoderStatus open();
    
    /**
     * 关闭解码器
     */
    void close();
    
    /**
     * 检查是否已打开
     */
    bool isOpen() const;
    
    /**
     * 重置解码器
     */
    DecoderStatus reset();
    
    // ============ 解码操作 ============
    
    /**
     * 发送数据包（FFmpeg标准send/receive模式）
     */
    DecoderStatus sendPacket(AVPacket* packet);
    
    /**
     * 接收解码帧（FFmpeg标准send/receive模式）
     */
    DecoderStatus receiveFrame(DecodedFrame& out_frame);
    
    /**
     * 简化接口：一次性解码
     */
    DecoderStatus decode(AVPacket* packet, DecodedFrame& out_frame);
    
    /**
     * 刷新解码器缓冲区
     */
    DecoderStatus flush(DecodedFrame& out_frame);
    
    // ============ 信息查询 ============
    
    const DecoderConfig& getConfig() const;
    const char* getCodecName() const;
    DecoderFactory::DecoderType getDecoderType() const;
    bool isHardwareAccelerated() const;
    const char* getLastError() const;
    int getLastFFmpegError() const;
    
    /**
     * 获取底层解码器实例（高级用户）
     */
    IDecoder* getUnderlyingDecoder();

private:
    std::unique_ptr<IDecoder> decoder_;      // 具体解码器实例
    DecoderFactory::DecoderType type_;       // 解码器类型
    DecoderConfig config_;                   // 配置参数
    bool is_open_;                           // 是否已打开
    std::string last_error_;                 // 最后一次错误信息
    
    // 内部辅助函数
    void setError(const char* error_msg);
    bool validateConfig() const;
};

#endif // DECODER_HPP
