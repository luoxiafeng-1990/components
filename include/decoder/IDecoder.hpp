#ifndef IDECODER_HPP
#define IDECODER_HPP

#include "../buffer/Buffer.hpp"
#include "../buffer/BufferPool.hpp"
#include <stddef.h>
#include <cstdint>
#include <functional>

// 直接使用 FFmpeg 标准类型（对齐大厂实践）
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

/**
 * DecoderStatus - 解码器状态码
 * 
 * 参考 Chromium VideoDecoder 的设计
 * 提供清晰的状态反馈，而不是简单的 true/false
 */
enum class DecoderStatus {
    OK = 0,                    // 成功
    ABORTED,                   // 操作被中止
    DECODE_ERROR,              // 解码错误
    UNSUPPORTED_CONFIG,        // 不支持的配置
    OUT_OF_MEMORY,             // 内存不足
    PLATFORM_ERROR,            // 平台相关错误
    NEED_MORE_DATA,            // 需要更多输入数据 (对应 AVERROR(EAGAIN))
    END_OF_STREAM,             // 流结束 (对应 AVERROR_EOF)
    BUFFER_FULL,               // 解码器缓冲区满
    NOT_INITIALIZED            // 解码器未初始化
};

/**
 * BufferAllocationMode - Buffer分配模式
 * 
 * 定义解码器如何管理输出buffer的内存
 */
enum class BufferAllocationMode {
    /**
     * INTERNAL - FFmpeg内部分配
     * - FFmpeg自己管理内存
     * - 需要拷贝到BufferPool（有一次memcpy）
     * - 简单，兼容性最好
     * - 适合：原型开发、非性能关键场景
     */
    INTERNAL,
    
    /**
     * ZERO_COPY - 零拷贝模式（推荐）
     * - 通过 get_buffer2 让FFmpeg直接使用BufferPool的Buffer
     * - FFmpeg直接解码到BufferPool的内存
     * - 真正的零拷贝！
     * - 适合：高性能场景、实时处理
     * - 要求：BufferPool必须预分配且buffer大小足够
     */
    ZERO_COPY,
    
    /**
     * INJECTION - 注入模式
     * - 解码到临时缓冲区
     * - 创建BufferHandle包装AVFrame
     * - 动态注入到BufferPool
     * - 适合：硬件解码、GPU内存、不确定大小的场景
     * - 类似 RtspVideoReader 的工作方式
     */
    INJECTION
};

/**
 * HardwareAccelConfig - 硬件加速配置
 * 
 * 对齐 FFmpeg 的硬件加速架构
 */
struct HardwareAccelConfig {
    AVHWDeviceType device_type;        // 硬件设备类型 (如 AV_HWDEVICE_TYPE_VAAPI)
    AVPixelFormat hw_pix_fmt;          // 硬件像素格式
    AVBufferRef* device_ctx;           // 设备上下文（可选）
    
    HardwareAccelConfig()
        : device_type(AV_HWDEVICE_TYPE_NONE)
        , hw_pix_fmt(AV_PIX_FMT_NONE)
        , device_ctx(nullptr)
    {}
};

/**
 * DecoderConfig - 解码器配置
 * 
 * 对齐 AVCodecContext 的设计，包含完整的配置参数
 * 参考：FFmpeg、Chromium、GStreamer 的最佳实践
 */
struct DecoderConfig {
    // ============ 基本参数 ============
    AVCodecID codec_id;                // 编解码器 ID (如 AV_CODEC_ID_H264)
    const char* codec_name;            // 编解码器名称（可选，优先使用 codec_id）
    
    // ============ 视频参数 ============
    int width;                         // 视频宽度（像素）
    int height;                        // 视频高度（像素）
    AVPixelFormat pix_fmt;             // 像素格式（直接使用FFmpeg标准！）
    AVRational time_base;              // 时间基准
    AVRational framerate;              // 帧率
    AVRational sample_aspect_ratio;    // 样本宽高比
    
    // ============ 编码参数 ============
    const uint8_t* extradata;          // 额外数据 (SPS/PPS/VPS 等)
    int extradata_size;                // 额外数据大小
    int profile;                       // Profile (如 FF_PROFILE_H264_HIGH)
    int level;                         // Level
    int64_t bit_rate;                  // 比特率（bps）
    
    // ============ 性能参数 ============
    int thread_count;                  // 线程数（0=自动）
    int thread_type;                   // 线程类型（FF_THREAD_FRAME/FF_THREAD_SLICE）
    
    // ============ 硬件加速 ============
    HardwareAccelConfig hwaccel;       // 硬件加速配置
    
    // ============ 缓冲区选项 ============
    bool low_delay;                    // 低延迟模式（禁用B帧重排序）
    int max_b_frames;                  // 最大B帧数
    
    // ============ Buffer管理（核心！）============
    BufferAllocationMode buffer_mode;  // Buffer分配模式
    BufferPool* buffer_pool;           // 关联的BufferPool
    size_t buffer_alignment;           // 内存对齐（字节，默认32，某些硬件要求64/128）
    
    // 默认构造函数
    DecoderConfig()
        : codec_id(AV_CODEC_ID_NONE)
        , codec_name(nullptr)
        , width(0)
        , height(0)
        , pix_fmt(AV_PIX_FMT_NONE)
        , time_base({0, 0})
        , framerate({0, 0})
        , sample_aspect_ratio({0, 0})
        , extradata(nullptr)
        , extradata_size(0)
        , profile(FF_PROFILE_UNKNOWN)
        , level(FF_LEVEL_UNKNOWN)
        , bit_rate(0)
        , thread_count(0)
        , thread_type(0)
        , hwaccel()
        , low_delay(false)
        , max_b_frames(0)
        , buffer_mode(BufferAllocationMode::ZERO_COPY)  // 默认零拷贝
        , buffer_pool(nullptr)
        , buffer_alignment(32)
    {}
};

/**
 * DecodedFrame - 解码后的帧
 * 
 * 设计理念：
 * - 不重复造轮子，直接使用 FFmpeg 的 AVFrame（包含所有元数据）
 * - 只添加我们系统特定的 Buffer 管理
 * - 清晰的所有权语义
 */
struct DecodedFrame {
    AVFrame* av_frame;                 // FFmpeg原生帧（包含所有元数据）
    Buffer* buffer;                    // 关联的Buffer（可能为nullptr）
    bool owns_av_frame;                // 是否拥有av_frame（决定析构时是否释放）
    
    // ============ 便捷访问器（避免每次都写 av_frame->xxx）============
    
    inline int64_t pts() const { 
        return av_frame ? av_frame->pts : AV_NOPTS_VALUE; 
    }
    
    inline int64_t dts() const { 
        return av_frame ? av_frame->pkt_dts : AV_NOPTS_VALUE; 
    }
    
    inline int width() const { 
        return av_frame ? av_frame->width : 0; 
    }
    
    inline int height() const { 
        return av_frame ? av_frame->height : 0; 
    }
    
    inline AVPixelFormat pix_fmt() const { 
        return av_frame ? (AVPixelFormat)av_frame->format : AV_PIX_FMT_NONE; 
    }
    
    inline bool key_frame() const { 
        return av_frame ? av_frame->key_frame : false; 
    }
    
    inline int linesize(int plane = 0) const {
        return av_frame ? av_frame->linesize[plane] : 0;
    }
    
    // ============ 数据访问（统一接口，无论来自哪里）============
    
    /**
     * 获取数据指针
     * - 优先使用 buffer（来自BufferPool）
     * - 回退到 av_frame（FFmpeg内部分配）
     */
    inline void* data(int plane = 0) const {
        if (buffer) return buffer->data();              // 来自BufferPool
        if (av_frame) return av_frame->data[plane];     // 来自FFmpeg
        return nullptr;
    }
    
    /**
     * 获取数据大小
     */
    inline size_t size() const {
        if (buffer) return buffer->size();
        // 计算AVFrame的buffer大小
        if (av_frame) {
            int ret = av_image_get_buffer_size(
                (AVPixelFormat)av_frame->format,
                av_frame->width, 
                av_frame->height, 
                1  // align
            );
            return ret > 0 ? (size_t)ret : 0;
        }
        return 0;
    }
    
    /**
     * 获取物理地址（如果有）
     */
    inline uint64_t physicalAddress() const {
        return buffer ? buffer->getPhysicalAddress() : 0;
    }
    
    // ============ 构造与析构 ============
    
    DecodedFrame()
        : av_frame(nullptr)
        , buffer(nullptr)
        , owns_av_frame(false)
    {}
    
    // 禁用拷贝（避免所有权混乱）
    DecodedFrame(const DecodedFrame&) = delete;
    DecodedFrame& operator=(const DecodedFrame&) = delete;
    
    // 支持移动
    DecodedFrame(DecodedFrame&& other) noexcept
        : av_frame(other.av_frame)
        , buffer(other.buffer)
        , owns_av_frame(other.owns_av_frame)
    {
        other.av_frame = nullptr;
        other.buffer = nullptr;
        other.owns_av_frame = false;
    }
    
    DecodedFrame& operator=(DecodedFrame&& other) noexcept {
        if (this != &other) {
            release();
            av_frame = other.av_frame;
            buffer = other.buffer;
            owns_av_frame = other.owns_av_frame;
            other.av_frame = nullptr;
            other.buffer = nullptr;
            other.owns_av_frame = false;
        }
        return *this;
    }
    
    /**
     * 显式释放资源
     * 
     * 注意：
     * - 只释放 av_frame（如果拥有）
     * - buffer 由 BufferPool 管理，调用者负责归还
     */
    void release() {
        if (owns_av_frame && av_frame) {
            av_frame_free(&av_frame);
            av_frame = nullptr;
        }
        buffer = nullptr;  // 不释放，由BufferPool管理
        owns_av_frame = false;
    }
    
    ~DecodedFrame() {
        // 析构时不自动释放，避免意外
        // 调用者应该显式调用 release()
    }
};

/**
 * IDecoder - 解码器抽象接口
 * 
 * 设计原则：
 * - 对齐 FFmpeg 的 send/receive 模式（专业标准）
 * - 同时提供简化接口（便捷性）
 * - 清晰的状态反馈（DecoderStatus）
 * - 支持多种 buffer 管理模式
 */
class IDecoder {
public:
    virtual ~IDecoder() = default;
    
    // ============ 初始化与生命周期 ============
    
    /**
     * 初始化解码器
     * @param config 解码器配置
     * @return DecoderStatus
     */
    virtual DecoderStatus initialize(const DecoderConfig& config) = 0;
    
    /**
     * 关闭解码器，释放所有资源
     */
    virtual void close() = 0;
    
    /**
     * 检查是否已初始化
     */
    virtual bool isInitialized() const = 0;
    
    // ============ 解码操作（FFmpeg send/receive 模式）============
    
    /**
     * 发送编码数据包到解码器
     * 
     * @param packet AVPacket指针，nullptr表示flush（获取缓冲帧）
     * @return DecoderStatus
     *         - OK: 成功
     *         - BUFFER_FULL: 解码器缓冲区满，需要先调用receiveFrame
     *         - DECODE_ERROR: 解码错误
     * 
     * 说明：
     * - 对应 avcodec_send_packet()
     * - send 只是提交数据，不一定立即产生输出
     * - 需要循环调用 receiveFrame 直到返回 NEED_MORE_DATA
     * - 发送 nullptr 表示刷新解码器（获取延迟帧）
     */
    virtual DecoderStatus sendPacket(AVPacket* packet) = 0;
    
    /**
     * 从解码器接收解码后的帧
     * 
     * @param out_frame 输出帧（调用者负责 release）
     * @return DecoderStatus
     *         - OK: 成功接收一帧
     *         - NEED_MORE_DATA: 需要更多输入数据（调用 sendPacket）
     *         - END_OF_STREAM: 流结束（flush完成）
     *         - DECODE_ERROR: 解码错误
     * 
     * 说明：
     * - 对应 avcodec_receive_frame()
     * - 应该循环调用直到返回 NEED_MORE_DATA
     * - 对于有B帧的编解码器，一个packet可能产生多个frame
     * - out_frame.buffer 在零拷贝模式下直接指向BufferPool的Buffer
     */
    virtual DecoderStatus receiveFrame(DecodedFrame& out_frame) = 0;
    
    /**
     * 简化接口：一次性解码（send + receive）
     * 
     * @param packet 输入数据包
     * @param out_frame 输出帧
     * @return DecoderStatus
     * 
     * 说明：
     * - 内部调用 sendPacket + receiveFrame
     * - 适合简单场景，但不如 send/receive 灵活
     * - 不适合有B帧的编解码器（可能丢帧）
     */
    virtual DecoderStatus decode(AVPacket* packet, DecodedFrame& out_frame) = 0;
    
    /**
     * 刷新解码器缓冲区
     * 
     * @param out_frame 输出帧
     * @return DecoderStatus
     *         - OK: 成功获取一帧
     *         - END_OF_STREAM: 没有更多缓冲帧
     * 
     * 说明：
     * - 等价于 sendPacket(nullptr) + 循环 receiveFrame
     * - 在文件结束时调用，获取延迟输出的帧
     * - 循环调用直到返回 END_OF_STREAM
     */
    virtual DecoderStatus flush(DecodedFrame& out_frame) = 0;
    
    /**
     * 重置解码器状态（清空缓冲区）
     * 
     * @return DecoderStatus
     * 
     * 说明：
     * - 对应 avcodec_flush_buffers()
     * - 清空内部缓冲区，但保持配置
     * - 用于 seek 后重新解码
     */
    virtual DecoderStatus reset() = 0;
    
    // ============ 信息查询 ============
    
    /**
     * 获取解码器配置
     */
    virtual const DecoderConfig& getConfig() const = 0;
    
    /**
     * 获取编解码器名称
     */
    virtual const char* getCodecName() const = 0;
    
    /**
     * 检查是否启用硬件加速
     */
    virtual bool isHardwareAccelerated() const = 0;
    
    /**
     * 获取解码器类型名称（用于调试）
     */
    virtual const char* getDecoderType() const = 0;
    
    /**
     * 获取最后一次错误信息
     */
    virtual const char* getLastError() const = 0;
    
    /**
     * 获取详细的FFmpeg错误码
     */
    virtual int getLastFFmpegError() const = 0;
};

#endif // IDECODER_HPP
