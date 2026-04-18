#include "productionline/worker/core/FFmpegEncodeWorker.hpp"
#include "productionline/worker/datasource/RawFrameSourceFromFile.hpp"
#include "productionline/worker/datasource/RawFrameSourceFromBuffer.hpp"
#include "common/Logger.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include <cstring>
#include <climits>

// FFmpeg headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

// ============ 构造/析构 ============

FFmpegEncodeWorker::FFmpegEncodeWorker(const WorkerConfig& config)
    : WorkerBase(BufferAllocatorFactory::AllocatorType::AVFRAME, config)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Encode")))
    , frame_source_(nullptr)
    , codec_ctx_ptr_(nullptr)
    , codec_options_ptr_(nullptr)
    , out_codec_params_(nullptr)
    , codec_params_extradata_ready_(false)
    , output_width_(config.encoder.width)
    , output_height_(config.encoder.height)
    , use_hardware_encoder_(config.encoder.enable_hardware)
    , encoder_name_(config.encoder.name.value_or(""))
    , encoded_frames_(0)
    , dropped_frames_(0)
    , current_frame_ptr_(nullptr)
    , frame_acquired_(false)
    , input_frame_(nullptr)
{
    LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] 构造函数开始");
    
    // 根据配置创建帧数据源
    if (config.data_source.shared_raw_frame_source) {
        frame_source_ = config.data_source.shared_raw_frame_source;
        LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] 使用外部注入的帧源（直接模式）");
    } else if (config.data_source.buffer_mode) {
        frame_source_ = std::make_shared<RawFrameSourceFromBuffer>(
            config.encoder.width,
            config.encoder.height,
            static_cast<AVPixelFormat>(config.encoder.input_pix_fmt)
        );
        LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] 创建 RawFrameSourceFromBuffer（Buffer 模式）");
    } else if (!config.data_source.path.empty()) {
        // 文件模式：从 YUV 文件读取帧（可与编码输出分辨率不同，读帧用 raw_*，编码前 swscale）
        int fw = config.encoder.width;
        int fh = config.encoder.height;
        if (config.data_source.raw_frame_width > 0 && config.data_source.raw_frame_height > 0) {
            fw = config.data_source.raw_frame_width;
            fh = config.data_source.raw_frame_height;
        }
        frame_source_ = std::make_shared<RawFrameSourceFromFile>(
            config.data_source.path,
            fw,
            fh,
            static_cast<AVPixelFormat>(config.encoder.input_pix_fmt)
        );
        LOG4CPLUS_DEBUG_FMT(logger_, "[EncodeWorker] 创建 RawFrameSourceFromFile: '%s'",
                           config.data_source.path.c_str());
    } else {
        LOG4CPLUS_WARN(logger_, "[EncodeWorker] 未指定数据源，需要后续调用 setSourceBufferPool");
    }
    
    if (frame_source_ && !config.data_source.buffer_mode && !config.data_source.path.empty()) {
        const int sw = frame_source_->getFrameWidth();
        const int sh = frame_source_->getFrameHeight();
        if (output_width_ > 0 && output_height_ > 0 && (sw != output_width_ || sh != output_height_)) {
            input_scale_needed_ = true;
        }
    }

    LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] 构造函数完成");
}

FFmpegEncodeWorker::~FFmpegEncodeWorker() {
    LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] 🧹 析构函数开始");
    
    // 清理缓存的 packet
    for (AVPacket* pkt : cached_packets_) {
        if (pkt) {
            av_packet_free(&pkt);
        }
    }
    cached_packets_.clear();
    
    // 先清理 BufferPool
    if (!buffer_pool_type_map_.empty()) {
        LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] 手动清理 BufferPool...");
        allocator_facade_.destroyPool();
        clearAllBufferPools();
    }
    
    // 再关闭编码器和数据源
    if (frame_source_ && frame_source_->isOpen()) {
        LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] 关闭编码器和数据源...");
        close();
    } else if (input_frame_) {
        av_frame_free(&input_frame_);
        input_frame_ = nullptr;
    }
    
    LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] 🧹 析构函数完成");
}

// ============ IDataSourceNavigator 接口实现 ============

bool FFmpegEncodeWorker::open(const char* path) {
    (void)path;
    return open();
}

bool FFmpegEncodeWorker::open() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 如果已经打开，先关闭
    if (frame_source_ && frame_source_->isOpen()) {
        LOG4CPLUS_WARN(logger_, "[EncodeWorker] ⚠️ 已打开，先关闭");
        close();
    }
    
    // 检查帧数据源
    if (!frame_source_) {
        LOG4CPLUS_ERROR(logger_, "[EncodeWorker] 帧数据源未设置");
        return false;
    }
    
    LOG4CPLUS_INFO(logger_, "");
    LOG4CPLUS_INFO(logger_, "🎬 Opening encoder...");
    
    // 1. 打开帧数据源
    if (!frame_source_->open()) {
        LOG4CPLUS_ERROR(logger_, "[EncodeWorker] 打开帧数据源失败");
        return false;
    }
    
    // 2. 获取输入帧信息
    if (output_width_ == 0) {
        output_width_ = frame_source_->getFrameWidth();
    }
    if (output_height_ == 0) {
        output_height_ = frame_source_->getFrameHeight();
    }
    
    if (output_width_ <= 0 || output_height_ <= 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "[EncodeWorker] 无效的分辨率: %dx%d",
                           output_width_, output_height_);
        frame_source_->close();
        return false;
    }
    
    // 3. 初始化编码器
    if (!initializeEncoder()) {
        LOG4CPLUS_ERROR(logger_, "[EncodeWorker] 初始化编码器失败");
        frame_source_->close();
        return false;
    }

    // 3.1 输出码流参数（MultiWorker：encoder → decoder 需 getCodecParameters() 非空）
    if (!syncOutputCodecParameters()) {
        LOG4CPLUS_ERROR(logger_, "[EncodeWorker] 同步输出 codec 参数失败");
        if (codec_ctx_ptr_) {
            avcodec_free_context(&codec_ctx_ptr_);
            codec_ctx_ptr_ = nullptr;
        }
        frame_source_->close();
        return false;
    }

    codec_params_extradata_ready_ = (out_codec_params_ &&
        out_codec_params_->extradata &&
        out_codec_params_->extradata_size > 0);
    
    // 4. 创建输出 BufferPool
    bool is_buffer_mode = worker_config_.data_source.buffer_mode;
    std::string pool_name = is_buffer_mode ? 
        "FFmpegEncodeWorker_BufferMode" :
        "FFmpegEncodeWorker_" + worker_config_.data_source.path;
    
    int buffer_count = worker_config_.data_source.buffer_count;
    if (buffer_count <= 0) {
        buffer_count = 32;  // 编码器默认 Buffer 数量
    }
    
    uint64_t pool_id = allocator_facade_.allocatePoolWithBuffers(
        buffer_count,
        0,  // 初始大小，后续会动态调整
        pool_name,
        is_buffer_mode ? "BUFFER_MODE" : "NORMAL_MODE"
    );
    
    if (pool_id == 0) {
        LOG4CPLUS_ERROR(logger_, "[EncodeWorker] 创建 BufferPool 失败");
        freeOutputCodecParameters();
        if (codec_ctx_ptr_) {
            avcodec_free_context(&codec_ctx_ptr_);
            codec_ctx_ptr_ = nullptr;
        }
        frame_source_->close();
        return false;
    }
    
    // 5. 注册 BufferPool
    if (!registerBufferPool(BufferPoolType::ENCODE_VIDEO_OUTPUT, pool_id)) {
        LOG4CPLUS_ERROR(logger_, "[EncodeWorker] 注册 BufferPool 失败");
        freeOutputCodecParameters();
        if (codec_ctx_ptr_) {
            avcodec_free_context(&codec_ctx_ptr_);
            codec_ctx_ptr_ = nullptr;
        }
        frame_source_->close();
        return false;
    }
    
    // 6. 获取 Pool 信息用于日志
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    std::string actual_pool_name = pool ? pool->getName() : "Unknown";
    
    encoded_frames_ = 0;
    dropped_frames_ = 0;
    
    // 7. 日志输出（与 FFmpegDecodeWorker::initializeDecoder 对齐：Codec / Stream / Output 分辨率）
    LOG4CPLUS_INFO_FMT(logger_, "   Codec: %s", codec_ctx_ptr_->codec->name);
    LOG4CPLUS_INFO_FMT(logger_, "   Stream resolution: %dx%d", getSourceWidth(), getSourceHeight());
    LOG4CPLUS_INFO_FMT(logger_, "   Output resolution: %dx%d", output_width_, output_height_);
    LOG4CPLUS_INFO_FMT(logger_, "   Bitrate: %ld bps", codec_ctx_ptr_->bit_rate);
    LOG4CPLUS_INFO_FMT(logger_, "   GOP size: %d", codec_ctx_ptr_->gop_size);
    LOG4CPLUS_INFO_FMT(logger_, "   Framerate: %d/%d",
                      codec_ctx_ptr_->framerate.num, codec_ctx_ptr_->framerate.den);
    LOG4CPLUS_INFO_FMT(logger_, "   BufferPool: '%s' (ID: %lu, %d buffers)",
                      actual_pool_name.c_str(), pool_id, buffer_count);
    
    // 文件模式：仅分配 input_frame 壳子，buffer 在首帧 readRawFrame 时 Lazy 分配（backup 逻辑）
    if (!worker_config_.data_source.buffer_mode && frame_source_) {
        input_frame_ = av_frame_alloc();
        if (!input_frame_) {
            LOG4CPLUS_ERROR(logger_, "[EncodeWorker] 分配 input_frame 结构体失败");
            unregisterBufferPool(BufferPoolType::ENCODE_VIDEO_OUTPUT);
            allocator_facade_.destroyPool();
            freeOutputCodecParameters();
            if (codec_ctx_ptr_) {
                avcodec_free_context(&codec_ctx_ptr_);
                codec_ctx_ptr_ = nullptr;
            }
            frame_source_->close();
            return false;
        }
        input_frame_->format = codec_ctx_ptr_->pix_fmt;
        if (input_scale_needed_) {
            input_frame_->width  = frame_source_->getFrameWidth();
            input_frame_->height = frame_source_->getFrameHeight();
        } else {
            input_frame_->width  = output_width_;
            input_frame_->height = output_height_;
        }
        LOG4CPLUS_DEBUG(logger_,
            "[EncodeWorker] input_frame 已就绪，YUV buffer 延迟到首帧 readRawFrame（64 对齐由数据源分配）");

        if (input_scale_needed_) {
            scaled_frame_ = av_frame_alloc();
            if (!scaled_frame_) {
                LOG4CPLUS_ERROR(logger_, "[EncodeWorker] 分配 scaled_frame 失败");
                av_frame_free(&input_frame_);
                input_frame_ = nullptr;
                unregisterBufferPool(BufferPoolType::ENCODE_VIDEO_OUTPUT);
                allocator_facade_.destroyPool();
                freeOutputCodecParameters();
                avcodec_free_context(&codec_ctx_ptr_);
                codec_ctx_ptr_ = nullptr;
                frame_source_->close();
                return false;
            }
            scaled_frame_->format = codec_ctx_ptr_->pix_fmt;
            scaled_frame_->width  = output_width_;
            scaled_frame_->height = output_height_;
            if (av_frame_get_buffer(scaled_frame_, 64) < 0) {
                LOG4CPLUS_ERROR(logger_, "[EncodeWorker] scaled_frame av_frame_get_buffer 失败");
                av_frame_free(&scaled_frame_);
                scaled_frame_ = nullptr;
                av_frame_free(&input_frame_);
                input_frame_ = nullptr;
                unregisterBufferPool(BufferPoolType::ENCODE_VIDEO_OUTPUT);
                allocator_facade_.destroyPool();
                freeOutputCodecParameters();
                avcodec_free_context(&codec_ctx_ptr_);
                codec_ctx_ptr_ = nullptr;
                frame_source_->close();
                return false;
            }
            const AVPixelFormat src_pf =
                static_cast<AVPixelFormat>(worker_config_.encoder.input_pix_fmt);
            sws_ctx_ = sws_getContext(
                frame_source_->getFrameWidth(),
                frame_source_->getFrameHeight(),
                src_pf,
                output_width_,
                output_height_,
                codec_ctx_ptr_->pix_fmt,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr);
            if (!sws_ctx_) {
                LOG4CPLUS_ERROR(logger_, "[EncodeWorker] sws_getContext 失败（编码前缩放）");
                av_frame_free(&scaled_frame_);
                scaled_frame_ = nullptr;
                av_frame_free(&input_frame_);
                input_frame_ = nullptr;
                unregisterBufferPool(BufferPoolType::ENCODE_VIDEO_OUTPUT);
                allocator_facade_.destroyPool();
                freeOutputCodecParameters();
                avcodec_free_context(&codec_ctx_ptr_);
                codec_ctx_ptr_ = nullptr;
                frame_source_->close();
                return false;
            }
            LOG4CPLUS_INFO_FMT(logger_,
                "[EncodeWorker] 编码前缩放: %dx%d -> %dx%d",
                frame_source_->getFrameWidth(),
                frame_source_->getFrameHeight(),
                output_width_,
                output_height_);
        }
    }
    
    return true;
}

void FFmpegEncodeWorker::close() {
    if (!frame_source_ || !frame_source_->isOpen()) {
        return;
    }
    
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        LOG4CPLUS_INFO(logger_, "");
        LOG4CPLUS_INFO(logger_, "🛑 Closing encoder...");
        
        if (scaled_frame_) {
            av_frame_free(&scaled_frame_);
            scaled_frame_ = nullptr;
        }
        if (sws_ctx_) {
            sws_freeContext(sws_ctx_);
            sws_ctx_ = nullptr;
        }
        if (input_frame_) {
            av_frame_free(&input_frame_);
            input_frame_ = nullptr;
        }
        
        // 清理缓存的 packet
        for (AVPacket* pkt : cached_packets_) {
            if (pkt) {
                av_packet_free(&pkt);
            }
        }
        cached_packets_.clear();
        
        // Buffer 模式清理
        if (worker_config_.data_source.buffer_mode && frame_acquired_) {
            auto* buffer_source = dynamic_cast<RawFrameSourceFromBuffer*>(frame_source_.get());
            if (buffer_source) {
                buffer_source->commitRawFrame(this);
            }
            frame_acquired_ = false;
            current_frame_ptr_ = nullptr;
        }
        
        // 刷新编码器（获取剩余的 packet）
        if (codec_ctx_ptr_) {
            int flush_ret = avcodec_send_frame(codec_ctx_ptr_, nullptr);  // 发送 flush 信号
            if (flush_ret < 0 && flush_ret != AVERROR_EOF) {
                // AVERROR_EOF 表示已经在 draining 模式（重复 flush），可忽略
                char err_buf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(flush_ret, err_buf, sizeof(err_buf));
                LOG4CPLUS_WARN_FMT(logger_, 
                    "[EncodeWorker] close: avcodec_send_frame(nullptr) failed (ret=%d, %s)", 
                    flush_ret, err_buf);
            }
            
            AVPacket* flush_pkt = av_packet_alloc();
            if (flush_pkt) {
                while (true) {
                    int recv_ret = avcodec_receive_packet(codec_ctx_ptr_, flush_pkt);
                    if (recv_ret == 0) {
                        av_packet_unref(flush_pkt);
                        continue;
                    }
                    // EAGAIN / EOF 是正常退出
                    if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
                        break;
                    }
                    // 其他错误：记录日志后退出
                    char err_buf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(recv_ret, err_buf, sizeof(err_buf));
                    LOG4CPLUS_WARN_FMT(logger_, 
                        "[EncodeWorker] close: avcodec_receive_packet failed (ret=%d, %s)", 
                        recv_ret, err_buf);
                    break;
                }
            }
            av_packet_free(&flush_pkt);
        }
        
        // 关闭帧数据源
        if (frame_source_) {
            frame_source_->close();
        }
        
        freeOutputCodecParameters();

        // 释放编码器
        if (codec_ctx_ptr_) {
            avcodec_free_context(&codec_ctx_ptr_);
            codec_ctx_ptr_ = nullptr;
        }
        
        // 释放编码器选项
        if (codec_options_ptr_) {
            av_dict_free(&codec_options_ptr_);
            codec_options_ptr_ = nullptr;
        }
        
        // 清除 BufferPool 注册
        clearAllBufferPools();
    }
    
    LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] 编码器已关闭");
    // 与 FFmpegDecodeWorker::close() 一致：success=成功帧数 failed=失败帧数 skipped=丢弃/跳过计数
    LOG4CPLUS_INFO_FMT(logger_, "   success=%d failed=%d skipped=%d",
        encoded_frames_.load(), 0, dropped_frames_.load());
}

bool FFmpegEncodeWorker::isOpen() const {
    return frame_source_ && frame_source_->isOpen() && codec_ctx_ptr_ != nullptr;
}

bool FFmpegEncodeWorker::seek(int frame_index) {
    if (frame_source_) {
        return frame_source_->seek(frame_index);
    }
    return false;
}

bool FFmpegEncodeWorker::seekToBegin() {
    return seek(0);
}

bool FFmpegEncodeWorker::seekToEnd() {
    LOG4CPLUS_WARN(logger_, "[EncodeWorker] seekToEnd 不支持");
    return false;
}

bool FFmpegEncodeWorker::skip(int frame_count) {
    if (frame_source_) {
        return frame_source_->skip(frame_count);
    }
    return false;
}

int FFmpegEncodeWorker::getTotalFrames() const {
    return frame_source_ ? frame_source_->getTotalFrames() : -1;
}

int FFmpegEncodeWorker::getCurrentFrameIndex() const {
    return encoded_frames_.load();
}

size_t FFmpegEncodeWorker::getFrameSize() const {
    // 编码后的帧大小不固定，返回平均估计值
    if (codec_ctx_ptr_ && codec_ctx_ptr_->bit_rate > 0) {
        // 平均每帧大小 ≈ 码率 / 帧率 / 8
        double fps = (double)codec_ctx_ptr_->framerate.num / codec_ctx_ptr_->framerate.den;
        if (fps > 0) {
            return static_cast<size_t>(codec_ctx_ptr_->bit_rate / fps / 8);
        }
    }
    return 0;
}

long FFmpegEncodeWorker::getFileSize() const {
    return frame_source_ ? frame_source_->getFileSize() : -1;
}

std::string FFmpegEncodeWorker::getPath() const {
    return frame_source_ ? frame_source_->getPath() : "";
}

bool FFmpegEncodeWorker::hasMoreFrames() const {
    return frame_source_ && frame_source_->hasMoreFrames();
}

bool FFmpegEncodeWorker::isAtEnd() const {
    return !frame_source_ || frame_source_->isAtEnd();
}

IDataSourceNavigator::SourceType FFmpegEncodeWorker::getDataSourceType() const {
    return frame_source_ ? frame_source_->getDataSourceType() : SourceType::FILE_SOURCE;
}

// ============ Worker 输出属性 ============

int FFmpegEncodeWorker::getSourceWidth() const {
    return frame_source_ ? frame_source_->getSourceWidth() : 0;
}

int FFmpegEncodeWorker::getSourceHeight() const {
    return frame_source_ ? frame_source_->getSourceHeight() : 0;
}

AVPixelFormat FFmpegEncodeWorker::getSourcePixelFormat() const {
    return frame_source_ ? frame_source_->getSourcePixelFormat() : AV_PIX_FMT_NONE;
}

int FFmpegEncodeWorker::getOutputWidth() const {
    return output_width_;
}

int FFmpegEncodeWorker::getOutputHeight() const {
    return output_height_;
}

double FFmpegEncodeWorker::getOutputBytesPerPixel(int channel) const {
    (void)channel;
    // 编码输出是压缩数据，没有固定的"每像素字节数"概念
    // 返回 0 表示这是压缩数据
    return 0.0;
}

// ============ 编码器特有接口 ============

bool FFmpegEncodeWorker::setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) {
    auto* buffer_source = dynamic_cast<RawFrameSourceFromBuffer*>(frame_source_.get());
    if (!buffer_source) {
        LOG4CPLUS_WARN(logger_, "[EncodeWorker] setSourceBufferPool 失败：不是 Buffer 模式");
        return false;
    }
    
    buffer_source->setSourceBufferPool(pool_weak);
    LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] ✅ 已设置源 BufferPool");
    return true;
}

const char* FFmpegEncodeWorker::getEncoderName() const {
    if (codec_ctx_ptr_ && codec_ctx_ptr_->codec) {
        return codec_ctx_ptr_->codec->name;
    }
    return "unknown";
}

const AVCodecParameters* FFmpegEncodeWorker::getCodecParameters() const {
    return out_codec_params_;
}

AVRational FFmpegEncodeWorker::getTimeBase() const {
    if (codec_ctx_ptr_) {
        return codec_ctx_ptr_->time_base;
    }
    return {1, 25};
}

void FFmpegEncodeWorker::printStats() const {
    // 与 FFmpegDecodeWorker::printStats() 字段顺序与统计行格式一致
    LOG4CPLUS_INFO(logger_, "");
    LOG4CPLUS_INFO(logger_, " 📊 Statistics:");
    std::string path = getPath();
    LOG4CPLUS_INFO_FMT(logger_, "    Codec: %s", getEncoderName());
    LOG4CPLUS_INFO_FMT(logger_, "    Resolution: %dx%d → %dx%d",
        getSourceWidth(), getSourceHeight(), output_width_, output_height_);
    LOG4CPLUS_INFO_FMT(logger_, "    Source: %s", path.empty() ? "(Buffer mode)" : path.c_str());
    LOG4CPLUS_INFO_FMT(logger_, "    success=%d failed=%d skipped=%d",
        encoded_frames_.load(), 0, dropped_frames_.load());

    SourceType type = getDataSourceType();
    if (type == SourceType::FILE_SOURCE) {
        LOG4CPLUS_INFO_FMT(logger_, "    Total frames: %d", getTotalFrames());
        LOG4CPLUS_INFO_FMT(logger_, "    EOF: %s", isAtEnd() ? "YES" : "NO");
    }

    uint64_t pool_id = getOutputBufferPoolId(BufferPoolType::ENCODE_VIDEO_OUTPUT);
    LOG4CPLUS_INFO_FMT(logger_, "    BufferPool ID: %lu", pool_id);
}

// ============ 内部方法 ============

bool FFmpegEncodeWorker::initializeEncoder() {
    // 1. 查找编码器
    const AVCodec* codec = nullptr;
    
    if (!encoder_name_.empty()) {
        // 用户指定了编码器名称
        codec = avcodec_find_encoder_by_name(encoder_name_.c_str());
        if (!codec) {
            LOG4CPLUS_WARN_FMT(logger_, "[EncodeWorker] ⚠️ 指定的编码器 '%s' 未找到",
                             encoder_name_.c_str());
        } else {
            LOG4CPLUS_DEBUG_FMT(logger_, "[EncodeWorker] 使用指定编码器: %s",
                              encoder_name_.c_str());
        }
    }
    
    if (!codec) {
        // 自动选择编码器
        if (use_hardware_encoder_) {
            // 尝试 TACO 硬件编码器
            codec = avcodec_find_encoder_by_name("h264_taco");
            if (!codec) {
                codec = avcodec_find_encoder_by_name("hevc_taco");
            }
        }
        
        if (!codec) {
            // 使用软件编码器
            codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        }
        
        if (!codec) {
            LOG4CPLUS_ERROR(logger_, "[EncodeWorker] 找不到可用的编码器");
            return false;
        }
        
        LOG4CPLUS_DEBUG_FMT(logger_, "[EncodeWorker] 自动选择编码器: %s", codec->name);
    }
    
    // 2. 分配编码器上下文
    codec_ctx_ptr_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_ptr_) {
        LOG4CPLUS_ERROR(logger_, "[EncodeWorker] 分配编码器上下文失败");
        return false;
    }
    
    // 3. 设置编码参数
    auto& enc_config = worker_config_.encoder;
    
    codec_ctx_ptr_->width = output_width_;
    codec_ctx_ptr_->height = output_height_;
    codec_ctx_ptr_->bit_rate = enc_config.bit_rate;
    // TACO：CQP 时 bit_rate 仅作 HRD/码控结构占位，不能为 0（易触发 H264EncSetRateCtrl -3）
    if (enc_config.rc_mode == 2 && codec_ctx_ptr_->bit_rate <= 0) {
        codec_ctx_ptr_->bit_rate = 4000000;
        LOG4CPLUS_DEBUG_FMT(
            logger_,
            "[EncodeWorker] CQP：bit_rate 由 0 放宽为名义 4000000 bps（与 VencPlugin 预定义一致时可由上层的 EncoderConfig 传入）");
    }
    codec_ctx_ptr_->gop_size = enc_config.gop_size;
    codec_ctx_ptr_->max_b_frames = enc_config.max_b_frames;
    codec_ctx_ptr_->time_base = {enc_config.framerate_den, enc_config.framerate_num};
    codec_ctx_ptr_->framerate = {enc_config.framerate_num, enc_config.framerate_den};
    codec_ctx_ptr_->pix_fmt = static_cast<AVPixelFormat>(enc_config.input_pix_fmt);
    
    // 4. 设置码率控制模式
    // TACO h264_taco：rc-mode 走表达式求值，已注册的符号为 cbr / vbr 等；CQP 与
    // WorkerConfig::encoder.rc_mode 数值一致，应使用枚举整型字符串 "2"，不可用 "cqp"（会报 Undefined constant 'cqp'）
    if (enc_config.rc_mode == 1) {  // VBR
        av_dict_set(&codec_options_ptr_, "rc-mode", "vbr", 0);
    } else if (enc_config.rc_mode == 0) {  // CBR
        av_dict_set(&codec_options_ptr_, "rc-mode", "cbr", 0);
    } else if (enc_config.rc_mode == 2) {  // CQP
        if (codec && strstr(codec->name, "taco")) {
            av_dict_set(&codec_options_ptr_, "rc-mode", "2", 0);
            int qp = enc_config.cqp_qp;
            if (qp < 1) {
                qp = 1;
            }
            if (qp > 51) {
                qp = 51;
            }
            char qp_buf[16];
            snprintf(qp_buf, sizeof(qp_buf), "%d", qp);
            av_dict_set(&codec_options_ptr_, "qp", qp_buf, 0);
            LOG4CPLUS_DEBUG_FMT(
                logger_, "[EncodeWorker] TACO CQP: rc-mode=2 qp=%s", qp_buf);
        } else {
            LOG4CPLUS_WARN_FMT(
                logger_,
                "[EncodeWorker] rc_mode=CQP(2) 仅对 TACO 硬件编码器(h264_taco/hevc_taco)下发，当前 codec=%s，已跳过",
                codec ? codec->name : "(null)");
        }
    }
    
    // 5. TACO 编码器特定配置
    if (encoder_name_.find("taco") != std::string::npos) {
        if (!configureTacoEncoder()) {
            LOG4CPLUS_WARN(logger_, "[EncodeWorker] TACO 编码器配置失败，继续使用默认设置");
        }
    }
    
    // 6. JPEG 编码器配置
    if (encoder_name_.find("jpeg") != std::string::npos ||
        encoder_name_.find("mjpeg") != std::string::npos) {
        char quality_str[16];
        snprintf(quality_str, sizeof(quality_str), "%d", enc_config.jpeg.quality);
        av_dict_set(&codec_options_ptr_, "quality", quality_str, 0);
        
        // 硬件 jpeg_taco 直接接受 NV12，软件 mjpeg 需要 YUVJ420P
        bool is_hw_jpeg = (encoder_name_.find("taco") != std::string::npos);
        if (!is_hw_jpeg) {
            if (codec_ctx_ptr_->pix_fmt == AV_PIX_FMT_YUV420P ||
                codec_ctx_ptr_->pix_fmt == AV_PIX_FMT_NV12 ||
                codec_ctx_ptr_->pix_fmt == AV_PIX_FMT_NV21) {
                codec_ctx_ptr_->pix_fmt = AV_PIX_FMT_YUVJ420P;
            }
        }
    }
    
    // 7. 打开编码器
    fprintf(stderr, "[EncodeWorker] avcodec_open2: codec=%s pix_fmt=%d(%s) %dx%d\n",
            codec->name, codec_ctx_ptr_->pix_fmt,
            av_get_pix_fmt_name(codec_ctx_ptr_->pix_fmt) ? av_get_pix_fmt_name(codec_ctx_ptr_->pix_fmt) : "?",
            codec_ctx_ptr_->width, codec_ctx_ptr_->height);

    int ret = avcodec_open2(codec_ctx_ptr_, codec, 
                           codec_options_ptr_ ? &codec_options_ptr_ : nullptr);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "[EncodeWorker] 打开编码器失败: %s", err_buf);
        fprintf(stderr, "[EncodeWorker] 打开编码器 '%s' 失败: %s\n", codec->name, err_buf);
        avcodec_free_context(&codec_ctx_ptr_);
        codec_ctx_ptr_ = nullptr;
        return false;
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "[EncodeWorker] 编码器初始化成功: %s", codec->name);
    fprintf(stderr, "[EncodeWorker] 编码器 '%s' 初始化成功\n", codec->name);
    return true;
}

void FFmpegEncodeWorker::freeOutputCodecParameters() {
    if (out_codec_params_) {
        avcodec_parameters_free(&out_codec_params_);
        out_codec_params_ = nullptr;
    }
}

bool FFmpegEncodeWorker::syncOutputCodecParameters() {
    if (!codec_ctx_ptr_) {
        return false;
    }

    // 重要：不要反复 free/realloc out_codec_params_，
    // 否则 MultiWorker 中持有该指针的解码侧可能拿到悬垂指针。
    if (!out_codec_params_) {
        out_codec_params_ = avcodec_parameters_alloc();
        if (!out_codec_params_) {
            LOG4CPLUS_ERROR(logger_, "[EncodeWorker] avcodec_parameters_alloc 失败");
            return false;
        }
    }
    int ret = avcodec_parameters_from_context(out_codec_params_, codec_ctx_ptr_);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "[EncodeWorker] avcodec_parameters_from_context 失败: %s", err_buf);
        return false;
    }
    LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] 已同步输出 AVCodecParameters（供下游解码订阅）");
    return true;
}

bool FFmpegEncodeWorker::configureTacoEncoder() {
    if (!codec_ctx_ptr_) {
        return false;
    }
    if (worker_config_.encoder.vendor &&
        worker_config_.encoder.vendor->applyToCodecContext(codec_ctx_ptr_->priv_data)) {
        LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] TACO 编码器配置完成");
        return true;
    }

    LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] 无厂商编码器扩展或应用失败");
    return true;
}

/**
 * @brief 读取并发送帧到编码器
 * 
 * v2.33 变更：返回类型从 bool 改为 FillResult
 */
FillResult FFmpegEncodeWorker::readAndSendFrame(AVFrame* temp_frame) {
    if (!frame_source_ || !temp_frame) {
        return FillResult::notOpen();
    }
    
    // 从帧数据源读取一帧（Acquire 层）
    int ret = frame_source_->readRawFrame(temp_frame);
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] 🔄 EOF 到达");
            return FillResult::fromAcquire(PacketAcquireResult::eof());
        } else if (ret == AVERROR(EAGAIN)) {
            return FillResult::fromAcquire(PacketAcquireResult::again());
        } else {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG4CPLUS_ERROR_FMT(logger_, "[EncodeWorker] 读取帧失败: %s", err_buf);
            return FillResult::fromAcquire(PacketAcquireResult::unknownError());
        }
    }
    
    // 直接模式：消费者的 buffer 直接使用，不经过 temp_frame
    AVFrame* encode_frame = temp_frame;
    auto* buf_src = dynamic_cast<RawFrameSourceFromBuffer*>(frame_source_.get());
    if (buf_src) {
        AVFrame* df = buf_src->getDirectFrame();
        if (df) encode_frame = df;
    }
    
    // 发送帧到编码器（Codec 层）
    using Result = CodecSendResult;
    if (input_scale_needed_ && scaled_frame_ && sws_ctx_) {
        const int src_h = frame_source_->getFrameHeight();
        const int lines = sws_scale(
            sws_ctx_,
            encode_frame->data,
            encode_frame->linesize,
            0,
            src_h,
            scaled_frame_->data,
            scaled_frame_->linesize);
        if (lines <= 0) {
            LOG4CPLUS_ERROR_FMT(logger_, "[EncodeWorker] sws_scale 失败 (lines=%d)", lines);
            return FillResult::fromCodec(Result::encodeError());
        }
        scaled_frame_->pts = encoded_frames_.load();
        ret = avcodec_send_frame(codec_ctx_ptr_, scaled_frame_);
    } else {
        encode_frame->pts = encoded_frames_.load();
        ret = avcodec_send_frame(codec_ctx_ptr_, encode_frame);
    }
    
    if (ret == 0) {
        return FillResult::success();
    }
    
    if (ret == AVERROR_EOF)     return FillResult::fromCodec(Result::eof());
    if (ret == AVERROR(EAGAIN)) return FillResult::fromCodec(Result::eagain());
    if (ret == AVERROR(EINVAL)) return FillResult::fromCodec(Result::invalidState());
    if (ret == AVERROR(ENOMEM)) return FillResult::fromCodec(Result::allocFailed());
    
    char err_buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, err_buf, sizeof(err_buf));
    LOG4CPLUS_ERROR_FMT(logger_, "[EncodeWorker] avcodec_send_frame: encode error (ret=%d, %s)", 
        ret, err_buf);
    return FillResult::fromCodec(Result::encodeError());
}

bool FFmpegEncodeWorker::fillBufferMetadataFromPacket(AVPacket* packet, Buffer* buffer) {
    if (!packet || !buffer) {
        return false;
    }
    
    // 设置 Buffer 大小
    buffer->setSize(packet->size);
    
    // 设置虚拟地址（指向 packet 数据）
    buffer->setVirtualAddress(packet->data);
    
    // 更新统计
    encoded_frames_++;
    
    return true;
}

// ============ 核心功能：fillBuffer ============

/**
 * @brief 填充 Buffer（编码一帧）
 * 
 * v2.33 变更：返回类型从 bool 改为 FillResult
 */
FillResult FFmpegEncodeWorker::fillBuffer(int frame_index, Buffer* buffer) {
    (void)frame_index;
    
    // 参数校验
    if (!buffer) {
        LOG4CPLUS_ERROR(logger_, "[EncodeWorker] buffer 为空");
        return FillResult::invalidParam();
    }
    
    if (!isOpen()) {
        LOG4CPLUS_ERROR(logger_, "[EncodeWorker] Worker 未打开");
        return FillResult::notOpen();
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    AVPacket* packet = buffer->getAVPacket();
    if (!packet) {
        LOG4CPLUS_ERROR(logger_, "[EncodeWorker] buffer->getAVPacket() 为空");
        return FillResult::invalidParam();
    }
    
    // 步骤1：检查缓存队列是否有数据
    if (!cached_packets_.empty()) {
        AVPacket* cached_pkt = cached_packets_.front();
        cached_packets_.erase(cached_packets_.begin());
        
        av_packet_move_ref(packet, cached_pkt);
        av_packet_free(&cached_pkt);
        
        fillBufferMetadataFromPacket(packet, buffer);
        return FillResult::success();
    }
    
    // 步骤2：读取并发送帧（文件模式复用 input_frame_，与 backup 一致，避免每帧 av_frame_get_buffer 峰值）
    FillResult send_result = FillResult::success();
    if (input_frame_) {
        send_result = readAndSendFrame(input_frame_);
    } else {
        AVFrame* temp_frame = av_frame_alloc();
        if (!temp_frame) {
            LOG4CPLUS_ERROR(logger_, "[EncodeWorker] 分配临时帧失败");
            return FillResult::fromCodec(CodecSendResult::allocFailed());
        }
        send_result = readAndSendFrame(temp_frame);
        av_frame_free(&temp_frame);
    }
    if (!send_result.ok()) {
        if (input_frame_) av_frame_unref(input_frame_);
        return send_result;
    }
    
    // 步骤3：接收所有编码后的 packet
    bool received_at_least_one = false;
    CodecSendResult receive_result = CodecSendResult::success();
    
    while (true) {
        AVPacket* temp_pkt = av_packet_alloc();
        if (!temp_pkt) {
            receive_result = CodecSendResult::allocFailed();
            break;
        }
        
        int ret = avcodec_receive_packet(codec_ctx_ptr_, temp_pkt);
        
        if (ret == 0) {
            // ✅ 成功接收到一个 packet
            received_at_least_one = true;
            cached_packets_.push_back(temp_pkt);
            continue;
        }
        
        // 失败：释放临时 packet，映射错误码
        av_packet_free(&temp_pkt);
        
        if (ret == AVERROR(EAGAIN)) {
            // 正常：编码器需要更多输入帧
            receive_result = CodecSendResult::eagain();
        } else if (ret == AVERROR_EOF) {
            // 编码器 flush 完成，等同于需要更多输入
            receive_result = CodecSendResult::eagain();
        } else if (ret == AVERROR(EINVAL)) {
            // codec 未打开或类型不匹配（编程错误）
            LOG4CPLUS_ERROR(logger_, 
                "[EncodeWorker] avcodec_receive_packet: EINVAL - codec not opened or is a decoder");
            receive_result = CodecSendResult::invalidState();
        } else {
            // 其他未识别错误
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG4CPLUS_ERROR_FMT(logger_, 
                "[EncodeWorker] avcodec_receive_packet: unknown error (ret=%d, %s)", ret, err_buf);
            receive_result = CodecSendResult::encodeError();
        }
        break;
    }
    
    if (input_frame_) {
        av_frame_unref(input_frame_);
    }
    
    // 步骤4：从缓存取第一个 packet 填充 buffer
    if (!cached_packets_.empty()) {
        AVPacket* first_pkt = cached_packets_.front();
        cached_packets_.erase(cached_packets_.begin());
        
        av_packet_move_ref(packet, first_pkt);
        av_packet_free(&first_pkt);
        
        fillBufferMetadataFromPacket(packet, buffer);

        // 首个 packet 产生后，部分硬件编码器才会填充 extradata（SPS/PPS 等）。
        // 让解码侧能够在 open() 阶段拿到完整 codecpar。
        if (!codec_params_extradata_ready_) {
            (void)syncOutputCodecParameters();
            codec_params_extradata_ready_ = (out_codec_params_ &&
                out_codec_params_->extradata &&
                out_codec_params_->extradata_size > 0);
        }
        return FillResult::success();
    }
    
    // 没有编码出任何 packet（可能需要更多输入帧）
    if (!received_at_least_one) {
        // 根据 receive_result 的真实原因返回：
        // - eagain: 正常，编码器需要更多输入帧（如 B 帧场景）
        // - invalidState/encodeError/allocFailed: 真正的错误
        if (receive_result.isEagain()) {
            dropped_frames_++;
        }
        return FillResult::fromCodec(receive_result);
    }
    
    return FillResult::internalError();
}
