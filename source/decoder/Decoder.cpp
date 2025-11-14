#include "../../include/decoder/Decoder.hpp"
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
}

Decoder::Decoder(DecoderFactory::DecoderType type)
    : decoder_(nullptr)
    , type_(type)
    , config_()
    , is_open_(false)
    , last_error_()
{
}

Decoder::~Decoder() {
    close();
}

Decoder::Decoder(Decoder&& other) noexcept
    : decoder_(std::move(other.decoder_))
    , type_(other.type_)
    , config_(other.config_)
    , is_open_(other.is_open_)
    , last_error_(std::move(other.last_error_))
{
    other.is_open_ = false;
}

Decoder& Decoder::operator=(Decoder&& other) noexcept {
    if (this != &other) {
        close();
        
        decoder_ = std::move(other.decoder_);
        type_ = other.type_;
        config_ = other.config_;
        is_open_ = other.is_open_;
        last_error_ = std::move(other.last_error_);
        
        other.is_open_ = false;
    }
    return *this;
}

bool Decoder::setDecoderType(DecoderFactory::DecoderType type) {
    if (is_open_) {
        setError("Cannot change decoder type while decoder is open");
        return false;
    }
    
    type_ = type;
    decoder_.reset();
    return true;
}

bool Decoder::setCodec(AVCodecID codec_id) {
    if (is_open_) {
        setError("Cannot change codec while decoder is open");
        return false;
    }
    
    config_.codec_id = codec_id;
    config_.codec_name = avcodec_get_name(codec_id);
    return true;
}

bool Decoder::setCodec(const char* codec_name) {
    if (is_open_) {
        setError("Cannot change codec while decoder is open");
        return false;
    }
    
    if (!codec_name) {
        setError("Codec name is null");
        return false;
    }
    
    // 尝试从名称获取codec_id
    const AVCodec* codec = avcodec_find_decoder_by_name(codec_name);
    if (codec) {
        config_.codec_id = codec->id;
    } else {
        config_.codec_id = AV_CODEC_ID_NONE;
    }
    
    config_.codec_name = codec_name;
    return true;
}

bool Decoder::setOutputFormat(int width, int height, AVPixelFormat pix_fmt) {
    if (is_open_) {
        setError("Cannot change output format while decoder is open");
        return false;
    }
    
    if (width <= 0 || height <= 0) {
        setError("Invalid output dimensions");
        return false;
    }
    
    if (pix_fmt == AV_PIX_FMT_NONE) {
        setError("Invalid pixel format");
        return false;
    }
    
    config_.width = width;
    config_.height = height;
    config_.pix_fmt = pix_fmt;
    return true;
}

bool Decoder::setThreadCount(int thread_count) {
    if (is_open_) {
        setError("Cannot change thread count while decoder is open");
        return false;
    }
    
    if (thread_count < 0) {
        setError("Invalid thread count");
        return false;
    }
    
    config_.thread_count = thread_count;
    return true;
}

bool Decoder::setBufferMode(BufferAllocationMode mode) {
    if (is_open_) {
        setError("Cannot change buffer mode while decoder is open");
        return false;
    }
    
    config_.buffer_mode = mode;
    return true;
}

bool Decoder::attachBufferPool(BufferPool* pool) {
    if (is_open_) {
        setError("Cannot attach BufferPool while decoder is open");
        return false;
    }
    
    config_.buffer_pool = pool;
    return true;
}

bool Decoder::setExtraData(const uint8_t* data, int size) {
    if (is_open_) {
        setError("Cannot set extradata while decoder is open");
        return false;
    }
    
    config_.extradata = data;
    config_.extradata_size = size;
    return true;
}

bool Decoder::setTimeBase(AVRational time_base) {
    if (is_open_) {
        setError("Cannot set time base while decoder is open");
        return false;
    }
    
    config_.time_base = time_base;
    return true;
}

DecoderStatus Decoder::open() {
    if (is_open_) {
        setError("Decoder is already open");
        return DecoderStatus::DECODE_ERROR;
    }
    
    // 验证配置
    if (!validateConfig()) {
        return DecoderStatus::UNSUPPORTED_CONFIG;
    }
    
    // 创建解码器实例
    if (!decoder_) {
        decoder_ = DecoderFactory::createDecoder(type_);
        if (!decoder_) {
            setError("Failed to create decoder instance");
            return DecoderStatus::PLATFORM_ERROR;
        }
    }
    
    // 初始化解码器
    DecoderStatus status = decoder_->initialize(config_);
    if (status != DecoderStatus::OK) {
        setError(decoder_->getLastError());
        return status;
    }
    
    is_open_ = true;
    
    printf("✅ Decoder opened:\n");
    printf("   Type: %s\n", DecoderFactory::getDecoderTypeName(type_));
    printf("   Codec: %s (ID=%d)\n", config_.codec_name, config_.codec_id);
    printf("   Output: %dx%d, %s\n", 
           config_.width, config_.height,
           av_get_pix_fmt_name(config_.pix_fmt));
    printf("   Buffer mode: %s\n",
           config_.buffer_mode == BufferAllocationMode::ZERO_COPY ? "ZERO_COPY" :
           config_.buffer_mode == BufferAllocationMode::INJECTION ? "INJECTION" : "INTERNAL");
    
    return DecoderStatus::OK;
}

void Decoder::close() {
    if (decoder_) {
        decoder_->close();
    }
    is_open_ = false;
}

bool Decoder::isOpen() const {
    return is_open_ && decoder_ && decoder_->isInitialized();
}

DecoderStatus Decoder::reset() {
    if (!is_open_ || !decoder_) {
        setError("Decoder is not open");
        return DecoderStatus::NOT_INITIALIZED;
    }
    
    return decoder_->reset();
}

DecoderStatus Decoder::sendPacket(AVPacket* packet) {
    if (!is_open_ || !decoder_) {
        setError("Decoder is not open");
        return DecoderStatus::NOT_INITIALIZED;
    }
    
    return decoder_->sendPacket(packet);
}

DecoderStatus Decoder::receiveFrame(DecodedFrame& out_frame) {
    if (!is_open_ || !decoder_) {
        setError("Decoder is not open");
        return DecoderStatus::NOT_INITIALIZED;
    }
    
    return decoder_->receiveFrame(out_frame);
}

DecoderStatus Decoder::decode(AVPacket* packet, DecodedFrame& out_frame) {
    if (!is_open_ || !decoder_) {
        setError("Decoder is not open");
        return DecoderStatus::NOT_INITIALIZED;
    }
    
    return decoder_->decode(packet, out_frame);
}

DecoderStatus Decoder::flush(DecodedFrame& out_frame) {
    if (!is_open_ || !decoder_) {
        setError("Decoder is not open");
        return DecoderStatus::NOT_INITIALIZED;
    }
    
    return decoder_->flush(out_frame);
}

const DecoderConfig& Decoder::getConfig() const {
    return config_;
}

const char* Decoder::getCodecName() const {
    if (decoder_) {
        return decoder_->getCodecName();
    }
    return config_.codec_name;
}

DecoderFactory::DecoderType Decoder::getDecoderType() const {
    return type_;
}

bool Decoder::isHardwareAccelerated() const {
    if (decoder_) {
        return decoder_->isHardwareAccelerated();
    }
    return config_.hwaccel.device_type != AV_HWDEVICE_TYPE_NONE;
}

const char* Decoder::getLastError() const {
    if (!last_error_.empty()) {
        return last_error_.c_str();
    }
    if (decoder_) {
        return decoder_->getLastError();
    }
    return "No error";
}

int Decoder::getLastFFmpegError() const {
    if (decoder_) {
        return decoder_->getLastFFmpegError();
    }
    return 0;
}

IDecoder* Decoder::getUnderlyingDecoder() {
    return decoder_.get();
}

void Decoder::setError(const char* error_msg) {
    if (error_msg) {
        last_error_ = error_msg;
        printf("❌ Decoder Error: %s\n", error_msg);
    }
}

bool Decoder::validateConfig() const {
    // 检查codec
    if (config_.codec_id == AV_CODEC_ID_NONE && 
        (!config_.codec_name || strlen(config_.codec_name) == 0)) {
        const_cast<Decoder*>(this)->setError("Codec is not set");
        return false;
    }
    
    // 检查分辨率
    if (config_.width <= 0 || config_.height <= 0) {
        const_cast<Decoder*>(this)->setError("Output dimensions are not set");
        return false;
    }
    
    // 检查像素格式
    if (config_.pix_fmt == AV_PIX_FMT_NONE) {
        const_cast<Decoder*>(this)->setError("Output pixel format is not set");
        return false;
    }
    
    // 检查零拷贝模式的要求
    if (config_.buffer_mode == BufferAllocationMode::ZERO_COPY) {
        if (!config_.buffer_pool) {
            const_cast<Decoder*>(this)->setError("Zero-copy mode requires BufferPool");
            return false;
        }
    }
    
    return true;
}
