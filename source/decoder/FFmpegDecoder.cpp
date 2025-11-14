#include "../../include/decoder/FFmpegDecoder.hpp"
#include <cstdio>
#include <cstring>

// FFmpeg头文件
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

FFmpegDecoder::FFmpegDecoder()
    : codec_ctx_(nullptr)
    , codec_(nullptr)
    , sws_ctx_(nullptr)
    , config_()
    , is_initialized_(false)
    , is_flushing_(false)
    , codec_name_()
    , last_error_()
    , last_ffmpeg_error_(0)
    , buffer_mode_(BufferAllocationMode::ZERO_COPY)
    , buffer_pool_(nullptr)
    , buffer_alignment_(32)
    , buffer_map_()
{
}

FFmpegDecoder::~FFmpegDecoder() {
    close();
}

DecoderStatus FFmpegDecoder::initialize(const DecoderConfig& config) {
    if (is_initialized_) {
        setError("Decoder is already initialized");
        return DecoderStatus::DECODE_ERROR;
    }
    
    config_ = config;
    buffer_mode_ = config.buffer_mode;
    buffer_pool_ = config.buffer_pool;
    buffer_alignment_ = config.buffer_alignment;
    
    // 初始化FFmpeg解码器
    DecoderStatus status = initializeFFmpeg();
    if (status != DecoderStatus::OK) {
        cleanupFFmpeg();
        return status;
    }
    
    is_initialized_ = true;
    
    printf("✅ FFmpegDecoder initialized:\n");
    printf("   Codec: %s\n", codec_name_.c_str());
    printf("   Resolution: %dx%d\n", config_.width, config_.height);
    printf("   Pixel format: %s\n", av_get_pix_fmt_name(config_.pix_fmt));
    printf("   Buffer mode: %s\n", 
           buffer_mode_ == BufferAllocationMode::ZERO_COPY ? "ZERO_COPY" :
           buffer_mode_ == BufferAllocationMode::INJECTION ? "INJECTION" : "INTERNAL");
    if (buffer_mode_ == BufferAllocationMode::ZERO_COPY) {
        printf("   ⚡ Zero-copy enabled: FFmpeg -> BufferPool\n");
    }
    
    return DecoderStatus::OK;
}

void FFmpegDecoder::close() {
    cleanupFFmpeg();
    is_initialized_ = false;
    is_flushing_ = false;
    buffer_map_.clear();
}

bool FFmpegDecoder::isInitialized() const {
    return is_initialized_;
}

DecoderStatus FFmpegDecoder::sendPacket(AVPacket* packet) {
    if (!is_initialized_ || !codec_ctx_) {
        setError("Decoder is not initialized");
        return DecoderStatus::NOT_INITIALIZED;
    }
    
    int ret = avcodec_send_packet(codec_ctx_, packet);
    
    if (ret == 0) {
        if (!packet) {
            is_flushing_ = true;  // 开始flush
        }
        return DecoderStatus::OK;
    }
    
    return convertFFmpegError(ret);
}

DecoderStatus FFmpegDecoder::receiveFrame(DecodedFrame& out_frame) {
    if (!is_initialized_ || !codec_ctx_) {
        setError("Decoder is not initialized");
        return DecoderStatus::NOT_INITIALIZED;
    }
    
    // 分配AVFrame
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        setError("Failed to allocate AVFrame");
        return DecoderStatus::OUT_OF_MEMORY;
    }
    
    // 从解码器接收帧
    int ret = avcodec_receive_frame(codec_ctx_, frame);
    
    if (ret == 0) {
        // 成功接收一帧
        out_frame.av_frame = frame;
        out_frame.owns_av_frame = true;
        
        // 零拷贝模式：从AVFrame获取关联的Buffer
        if (buffer_mode_ == BufferAllocationMode::ZERO_COPY) {
            if (frame->buf[0] && frame->data[0]) {
                // 从buffer_map_中查找对应的Buffer
                auto it = buffer_map_.find(frame->data[0]);
                if (it != buffer_map_.end()) {
                    out_frame.buffer = it->second;
                } else {
                    printf("⚠️  Warning: Buffer not found in map for frame data %p\n", 
                           frame->data[0]);
                }
            }
        }
        
        return DecoderStatus::OK;
    }
    
    // 失败，释放frame
    av_frame_free(&frame);
    
    if (ret == AVERROR_EOF) {
        is_flushing_ = false;  // flush完成
        return DecoderStatus::END_OF_STREAM;
    }
    
    return convertFFmpegError(ret);
}

DecoderStatus FFmpegDecoder::decode(AVPacket* packet, DecodedFrame& out_frame) {
    // 简化接口：send + receive
    DecoderStatus status = sendPacket(packet);
    if (status != DecoderStatus::OK) {
        return status;
    }
    
    return receiveFrame(out_frame);
}

DecoderStatus FFmpegDecoder::flush(DecodedFrame& out_frame) {
    if (!is_initialized_ || !codec_ctx_) {
        setError("Decoder is not initialized");
        return DecoderStatus::NOT_INITIALIZED;
    }
    
    // 如果还没开始flush，先发送flush信号
    if (!is_flushing_) {
        DecoderStatus status = sendPacket(nullptr);
        if (status != DecoderStatus::OK) {
            return status;
        }
    }
    
    // 接收缓冲的帧
    return receiveFrame(out_frame);
}

DecoderStatus FFmpegDecoder::reset() {
    if (!is_initialized_ || !codec_ctx_) {
        setError("Decoder is not initialized");
        return DecoderStatus::NOT_INITIALIZED;
    }
    
    avcodec_flush_buffers(codec_ctx_);
    is_flushing_ = false;
    
    return DecoderStatus::OK;
}

const DecoderConfig& FFmpegDecoder::getConfig() const {
    return config_;
}

const char* FFmpegDecoder::getCodecName() const {
    return codec_name_.c_str();
}

bool FFmpegDecoder::isHardwareAccelerated() const {
    return config_.hwaccel.device_type != AV_HWDEVICE_TYPE_NONE;
}

const char* FFmpegDecoder::getDecoderType() const {
    return "FFmpegDecoder";
}

const char* FFmpegDecoder::getLastError() const {
    return last_error_.c_str();
}

int FFmpegDecoder::getLastFFmpegError() const {
    return last_ffmpeg_error_;
}

// ============ 私有辅助函数 ============

void FFmpegDecoder::setError(const char* error_msg, int ffmpeg_error) {
    if (error_msg) {
        last_error_ = error_msg;
        last_ffmpeg_error_ = ffmpeg_error;
        
        if (ffmpeg_error != 0) {
            char ffmpeg_error_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ffmpeg_error, ffmpeg_error_buf, sizeof(ffmpeg_error_buf));
            printf("❌ FFmpegDecoder Error: %s (FFmpeg: %s)\n", error_msg, ffmpeg_error_buf);
        } else {
            printf("❌ FFmpegDecoder Error: %s\n", error_msg);
        }
    }
}

DecoderStatus FFmpegDecoder::initializeFFmpeg() {
    // 查找解码器（优先使用 codec_id）
    if (config_.codec_id != AV_CODEC_ID_NONE) {
        codec_ = avcodec_find_decoder(config_.codec_id);
        if (codec_) {
            codec_name_ = codec_->name;
        }
    } else if (config_.codec_name) {
        codec_ = avcodec_find_decoder_by_name(config_.codec_name);
        if (codec_) {
            codec_name_ = config_.codec_name;
        }
    }
    
    if (!codec_) {
        setError("Failed to find decoder");
        return DecoderStatus::UNSUPPORTED_CONFIG;
    }
    
    // 创建解码器上下文
    codec_ctx_ = avcodec_alloc_context3(codec_);
    if (!codec_ctx_) {
        setError("Failed to allocate decoder context");
        return DecoderStatus::OUT_OF_MEMORY;
    }
    
    // 设置解码器参数
    codec_ctx_->width = config_.width;
    codec_ctx_->height = config_.height;
    codec_ctx_->pix_fmt = config_.pix_fmt;
    codec_ctx_->thread_count = config_.thread_count;
    
    if (config_.thread_type != 0) {
        codec_ctx_->thread_type = config_.thread_type;
    }
    
    if (config_.time_base.num != 0 && config_.time_base.den != 0) {
        codec_ctx_->time_base = config_.time_base;
    }
    
    if (config_.framerate.num != 0 && config_.framerate.den != 0) {
        codec_ctx_->framerate = config_.framerate;
    }
    
    if (config_.extradata && config_.extradata_size > 0) {
        codec_ctx_->extradata = (uint8_t*)av_mallocz(config_.extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (codec_ctx_->extradata) {
            memcpy(codec_ctx_->extradata, config_.extradata, config_.extradata_size);
            codec_ctx_->extradata_size = config_.extradata_size;
        }
    }
    
    // 零拷贝模式：设置自定义buffer分配器
    if (buffer_mode_ == BufferAllocationMode::ZERO_COPY) {
        if (!buffer_pool_) {
            setError("Zero-copy mode requires BufferPool");
            return DecoderStatus::UNSUPPORTED_CONFIG;
        }
        
        // 关键：注册 get_buffer2 回调
        codec_ctx_->opaque = this;  // 传递this指针
        codec_ctx_->get_buffer2 = getBufferCallback;
        
        printf("🔗 Zero-copy: get_buffer2 callback registered\n");
    }
    
    // 硬件加速
    if (config_.hwaccel.device_type != AV_HWDEVICE_TYPE_NONE) {
        if (config_.hwaccel.device_ctx) {
            codec_ctx_->hw_device_ctx = av_buffer_ref(config_.hwaccel.device_ctx);
        }
        codec_ctx_->pix_fmt = config_.hwaccel.hw_pix_fmt;
    }
    
    // 打开解码器
    int ret = avcodec_open2(codec_ctx_, codec_, nullptr);
    if (ret < 0) {
        setError("Failed to open decoder", ret);
        return DecoderStatus::DECODE_ERROR;
    }
    
    return DecoderStatus::OK;
}

DecoderStatus FFmpegDecoder::initializeScaler() {
    // TODO: 如果需要像素格式转换，在这里初始化 SwsContext
    // 目前假设输出格式与解码器输出格式一致
    return DecoderStatus::OK;
}

void FFmpegDecoder::cleanupFFmpeg() {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    
    if (codec_ctx_) {
        if (codec_ctx_->extradata) {
            av_freep(&codec_ctx_->extradata);
        }
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    
    codec_ = nullptr;
}

DecoderStatus FFmpegDecoder::convertFFmpegError(int ffmpeg_error) {
    switch (ffmpeg_error) {
        case AVERROR(EAGAIN):
            return DecoderStatus::NEED_MORE_DATA;
        
        case AVERROR_EOF:
            return DecoderStatus::END_OF_STREAM;
        
        case AVERROR(ENOMEM):
            setError("Out of memory", ffmpeg_error);
            return DecoderStatus::OUT_OF_MEMORY;
        
        case AVERROR(EINVAL):
            setError("Invalid argument", ffmpeg_error);
            return DecoderStatus::UNSUPPORTED_CONFIG;
        
        default:
            setError("FFmpeg decode error", ffmpeg_error);
            return DecoderStatus::DECODE_ERROR;
    }
}

size_t FFmpegDecoder::calculateFrameSize(int width, int height, AVPixelFormat format) const {
    int ret = av_image_get_buffer_size(format, width, height, buffer_alignment_);
    return ret > 0 ? (size_t)ret : 0;
}

// ============ 零拷贝核心实现 ============

int FFmpegDecoder::getBufferCallback(AVCodecContext* ctx, AVFrame* frame, int flags) {
    // 从 opaque 获取 FFmpegDecoder 实例
    FFmpegDecoder* decoder = static_cast<FFmpegDecoder*>(ctx->opaque);
    
    if (!decoder) {
        // 回退到默认分配器
        return avcodec_default_get_buffer2(ctx, frame, flags);
    }
    
    return decoder->allocateBuffer(frame, flags);
}

void FFmpegDecoder::releaseBufferCallback(void* opaque, uint8_t* data) {
    FFmpegDecoder* decoder = static_cast<FFmpegDecoder*>(opaque);
    
    if (decoder) {
        decoder->deallocateBuffer(data);
    }
}

int FFmpegDecoder::allocateBuffer(AVFrame* frame, int flags) {
    if (!buffer_pool_) {
        return avcodec_default_get_buffer2(codec_ctx_, frame, flags);
    }
    
    // 检查帧尺寸
    int ret = av_image_check_size(frame->width, frame->height, 0, codec_ctx_);
    if (ret < 0) {
        setError("Invalid frame size", ret);
        return ret;
    }
    
    // 计算所需buffer大小
    size_t buffer_size = calculateFrameSize(
        frame->width, 
        frame->height, 
        (AVPixelFormat)frame->format
    );
    
    if (buffer_size == 0) {
        setError("Failed to calculate buffer size");
        return AVERROR(EINVAL);
    }
    
    // 从BufferPool获取空闲buffer
    Buffer* buffer = buffer_pool_->acquireFree();
    if (!buffer) {
        printf("⚠️  BufferPool full, falling back to default allocator\n");
        return avcodec_default_get_buffer2(codec_ctx_, frame, flags);
    }
    
    // 检查buffer大小
    if (buffer->size() < buffer_size) {
        printf("❌ Buffer too small: need %zu, got %zu\n", buffer_size, buffer->size());
        buffer_pool_->releaseFilled(buffer);
        return AVERROR(ENOMEM);
    }
    
    // 填充AVFrame的data和linesize
    ret = av_image_fill_arrays(
        frame->data, 
        frame->linesize,
        static_cast<uint8_t*>(buffer->data()),
        (AVPixelFormat)frame->format,
        frame->width, 
        frame->height,
        buffer_alignment_
    );
    
    if (ret < 0) {
        setError("Failed to fill frame arrays", ret);
        buffer_pool_->releaseFilled(buffer);
        return ret;
    }
    
    // 创建AVBuffer，关联我们的Buffer
    frame->buf[0] = av_buffer_create(
        static_cast<uint8_t*>(buffer->data()),
        buffer->size(),
        releaseBufferCallback,  // 释放回调
        this,                   // opaque: 传递decoder指针
        0                       // flags
    );
    
    if (!frame->buf[0]) {
        setError("Failed to create AVBuffer");
        buffer_pool_->releaseFilled(buffer);
        return AVERROR(ENOMEM);
    }
    
    // 追踪Buffer（用于后续查找）
    buffer_map_[frame->data[0]] = buffer;
    
    frame->extended_data = frame->data;
    
    printf("🔗 Zero-copy: Buffer #%u allocated for frame (%dx%d, %zu bytes)\n",
           buffer->id(), frame->width, frame->height, buffer_size);
    
    return 0;
}

void FFmpegDecoder::deallocateBuffer(uint8_t* data) {
    auto it = buffer_map_.find(data);
    if (it != buffer_map_.end()) {
        Buffer* buffer = it->second;
        printf("🔄 Zero-copy: Buffer #%u released by FFmpeg\n", buffer->id());
        
        // 注意：不在这里归还给BufferPool
        // 因为用户可能还在使用（通过DecodedFrame）
        // 用户负责调用 pool->releaseFilled(buffer)
        
        buffer_map_.erase(it);
    }
}
