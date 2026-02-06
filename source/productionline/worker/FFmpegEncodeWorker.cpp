#include "productionline/worker/FFmpegEncodeWorker.hpp"
#include "productionline/worker/RawFrameSourceFromFile.hpp"
#include "productionline/worker/RawFrameSourceFromBuffer.hpp"
#include "common/Logger.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include <cstring>
#include <climits>

// FFmpeg headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

// ============ 构造/析构 ============

FFmpegEncodeWorker::FFmpegEncodeWorker(const WorkerConfig& config)
    : WorkerBase(BufferAllocatorFactory::AllocatorType::NORMAL, config)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Encode")))
    , frame_source_(nullptr)
    , codec_ctx_ptr_(nullptr)
    , codec_options_ptr_(nullptr)
    , output_width_(config.display.width)
    , output_height_(config.display.height)
    , use_hardware_encoder_(config.encoder.enable_hardware)
    , encoder_name_(config.encoder.name.value_or(""))
    , encoded_frames_(0)
    , dropped_frames_(0)
    , current_frame_ptr_(nullptr)
    , frame_acquired_(false)
{
    LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] 构造函数开始");
    
    // 根据配置创建帧数据源
    if (config.data_source.buffer_mode) {
        // Buffer 模式：从解码 Worker 的 BufferPool 获取帧
        frame_source_ = std::make_shared<RawFrameSourceFromBuffer>(
            config.display.width,
            config.display.height,
            static_cast<AVPixelFormat>(config.encoder.input_pix_fmt)
        );
        LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] 创建 RawFrameSourceFromBuffer（Buffer 模式）");
    } else if (!config.data_source.path.empty()) {
        // 文件模式：从 YUV 文件读取帧
        frame_source_ = std::make_shared<RawFrameSourceFromFile>(
            config.data_source.path,
            config.display.width,
            config.display.height,
            static_cast<AVPixelFormat>(config.encoder.input_pix_fmt)
        );
        LOG4CPLUS_DEBUG_FMT(logger_, "[EncodeWorker] 创建 RawFrameSourceFromFile: '%s'",
                           config.data_source.path.c_str());
    } else {
        LOG4CPLUS_WARN(logger_, "[EncodeWorker] 未指定数据源，需要后续调用 setSourceBufferPool");
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
        frame_source_->close();
        return false;
    }
    
    // 5. 注册 BufferPool
    if (!registerBufferPool(BufferPoolType::ENCODE_VIDEO_OUTPUT, pool_id)) {
        LOG4CPLUS_ERROR(logger_, "[EncodeWorker] 注册 BufferPool 失败");
        frame_source_->close();
        return false;
    }
    
    // 6. 获取 Pool 信息用于日志
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    std::string actual_pool_name = pool ? pool->getName() : "Unknown";
    
    encoded_frames_ = 0;
    dropped_frames_ = 0;
    
    // 7. 日志输出
    LOG4CPLUS_INFO_FMT(logger_, "   Encoder: %s", codec_ctx_ptr_->codec->name);
    LOG4CPLUS_INFO_FMT(logger_, "   Resolution: %dx%d", output_width_, output_height_);
    LOG4CPLUS_INFO_FMT(logger_, "   Bitrate: %ld bps", codec_ctx_ptr_->bit_rate);
    LOG4CPLUS_INFO_FMT(logger_, "   GOP size: %d", codec_ctx_ptr_->gop_size);
    LOG4CPLUS_INFO_FMT(logger_, "   Framerate: %d/%d", 
                      codec_ctx_ptr_->framerate.num, codec_ctx_ptr_->framerate.den);
    LOG4CPLUS_INFO_FMT(logger_, "   BufferPool: '%s' (ID: %lu, %d buffers)",
                      actual_pool_name.c_str(), pool_id, buffer_count);
    
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
            avcodec_send_frame(codec_ctx_ptr_, nullptr);  // 发送 flush 信号
            
            AVPacket* flush_pkt = av_packet_alloc();
            while (flush_pkt && avcodec_receive_packet(codec_ctx_ptr_, flush_pkt) == 0) {
                av_packet_unref(flush_pkt);
            }
            av_packet_free(&flush_pkt);
        }
        
        // 关闭帧数据源
        if (frame_source_) {
            frame_source_->close();
        }
        
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
    LOG4CPLUS_INFO_FMT(logger_, "   Encoded frames: %d", encoded_frames_.load());
    LOG4CPLUS_INFO_FMT(logger_, "   Dropped frames: %d", dropped_frames_.load());
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
    if (!codec_ctx_ptr_) {
        return nullptr;
    }
    
    // 注意：这里需要动态分配 AVCodecParameters
    // 但为了避免内存泄漏，我们返回 nullptr
    // 调用者应该使用 avcodec_parameters_from_context() 自己获取
    return nullptr;
}

AVRational FFmpegEncodeWorker::getTimeBase() const {
    if (codec_ctx_ptr_) {
        return codec_ctx_ptr_->time_base;
    }
    return {1, 25};
}

void FFmpegEncodeWorker::printStats() const {
    LOG4CPLUS_INFO(logger_, "");
    LOG4CPLUS_INFO(logger_, "[EncodeWorker] 📊 Statistics:");
    LOG4CPLUS_INFO_FMT(logger_, "   Encoder: %s", getEncoderName());
    LOG4CPLUS_INFO_FMT(logger_, "   Resolution: %dx%d", output_width_, output_height_);
    LOG4CPLUS_INFO_FMT(logger_, "   Encoded frames: %d", encoded_frames_.load());
    LOG4CPLUS_INFO_FMT(logger_, "   Dropped frames: %d", dropped_frames_.load());
    
    uint64_t pool_id = getOutputBufferPoolId(BufferPoolType::ENCODE_VIDEO_OUTPUT);
    LOG4CPLUS_INFO_FMT(logger_, "   BufferPool ID: %lu", pool_id);
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
    codec_ctx_ptr_->gop_size = enc_config.gop_size;
    codec_ctx_ptr_->max_b_frames = enc_config.max_b_frames;
    codec_ctx_ptr_->time_base = {enc_config.framerate_den, enc_config.framerate_num};
    codec_ctx_ptr_->framerate = {enc_config.framerate_num, enc_config.framerate_den};
    codec_ctx_ptr_->pix_fmt = static_cast<AVPixelFormat>(enc_config.input_pix_fmt);
    
    // 4. 设置码率控制模式
    if (enc_config.rc_mode == 1) {  // VBR
        av_dict_set(&codec_options_ptr_, "rc-mode", "vbr", 0);
    } else if (enc_config.rc_mode == 0) {  // CBR
        av_dict_set(&codec_options_ptr_, "rc-mode", "cbr", 0);
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
        // 设置 JPEG 质量
        char quality_str[16];
        snprintf(quality_str, sizeof(quality_str), "%d", enc_config.jpeg.quality);
        av_dict_set(&codec_options_ptr_, "quality", quality_str, 0);
        
        // JPEG 通常使用 YUVJ420P
        if (codec_ctx_ptr_->pix_fmt == AV_PIX_FMT_YUV420P) {
            codec_ctx_ptr_->pix_fmt = AV_PIX_FMT_YUVJ420P;
        }
    }
    
    // 7. 打开编码器
    int ret = avcodec_open2(codec_ctx_ptr_, codec, 
                           codec_options_ptr_ ? &codec_options_ptr_ : nullptr);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "[EncodeWorker] 打开编码器失败: %s", err_buf);
        avcodec_free_context(&codec_ctx_ptr_);
        codec_ctx_ptr_ = nullptr;
        return false;
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, "[EncodeWorker] 编码器初始化成功: %s", codec->name);
    return true;
}

bool FFmpegEncodeWorker::configureTacoEncoder() {
    if (!codec_ctx_ptr_ || !codec_ctx_ptr_->priv_data) {
        return false;
    }
    
    auto& taco_config = worker_config_.encoder.taco;
    
    // 设置 profile 和 level（如果指定）
    if (taco_config.profile > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "profile", taco_config.profile, 0);
    }
    
    if (taco_config.level > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "level", taco_config.level, 0);
    }
    
    LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] TACO 编码器配置完成");
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
    
    // 从帧数据源读取一帧
    int ret = frame_source_->readRawFrame(temp_frame);
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            LOG4CPLUS_DEBUG(logger_, "[EncodeWorker] 🔄 EOF 到达");
            return FillResult::endOfStream();
        } else if (ret == AVERROR(EAGAIN)) {
            return FillResult::codecEagain();
        } else {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG4CPLUS_ERROR_FMT(logger_, "[EncodeWorker] 读取帧失败: %s", err_buf);
            return FillResult::acquireError();
        }
    }
    
    // 设置 PTS
    temp_frame->pts = encoded_frames_.load();
    
    // 发送帧到编码器
    ret = avcodec_send_frame(codec_ctx_ptr_, temp_frame);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            return FillResult::codecEagain();
        } else {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG4CPLUS_ERROR_FMT(logger_, "[EncodeWorker] avcodec_send_frame 失败: %s", err_buf);
            return FillResult::codecError();
        }
    }
    
    return FillResult::success();
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
        setLastFillStatus(FillStatus::InvalidParam);
        return FillResult::invalidParam();
    }
    
    if (!isOpen()) {
        LOG4CPLUS_ERROR(logger_, "[EncodeWorker] Worker 未打开");
        setLastFillStatus(FillStatus::NotOpen);
        return FillResult::notOpen();
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    AVPacket* packet = buffer->getAVPacket();
    if (!packet) {
        LOG4CPLUS_ERROR(logger_, "[EncodeWorker] buffer->getAVPacket() 为空");
        setLastFillStatus(FillStatus::InvalidParam);
        return FillResult::invalidParam();
    }
    
    // 步骤1：检查缓存队列是否有数据
    if (!cached_packets_.empty()) {
        AVPacket* cached_pkt = cached_packets_.front();
        cached_packets_.erase(cached_packets_.begin());
        
        av_packet_move_ref(packet, cached_pkt);
        av_packet_free(&cached_pkt);
        
        fillBufferMetadataFromPacket(packet, buffer);
        setLastFillStatus(FillStatus::Success);
        return FillResult::success();
    }
    
    // 步骤2：读取并发送帧到编码器
    AVFrame* temp_frame = av_frame_alloc();
    if (!temp_frame) {
        LOG4CPLUS_ERROR(logger_, "[EncodeWorker] 分配临时帧失败");
        setLastFillStatus(FillStatus::AllocFailed);
        return FillResult::allocFailed();
    }
    
    FillResult send_result = readAndSendFrame(temp_frame);
    av_frame_free(&temp_frame);
    
    if (!send_result.ok()) {
        setLastFillStatus(send_result.status());
        return send_result;
    }
    
    // 步骤3：接收所有编码后的 packet
    bool received_at_least_one = false;
    
    while (true) {
        AVPacket* temp_pkt = av_packet_alloc();
        if (!temp_pkt) {
            break;
        }
        
        int ret = avcodec_receive_packet(codec_ctx_ptr_, temp_pkt);
        
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0) {
            av_packet_free(&temp_pkt);
            break;
        }
        
        // 成功接收到一个 packet
        received_at_least_one = true;
        cached_packets_.push_back(temp_pkt);
    }
    
    // 步骤4：从缓存取第一个 packet 填充 buffer
    if (!cached_packets_.empty()) {
        AVPacket* first_pkt = cached_packets_.front();
        cached_packets_.erase(cached_packets_.begin());
        
        av_packet_move_ref(packet, first_pkt);
        av_packet_free(&first_pkt);
        
        fillBufferMetadataFromPacket(packet, buffer);
        setLastFillStatus(FillStatus::Success);
        return FillResult::success();
    }
    
    // 没有编码出任何 packet（可能需要更多输入帧）
    if (!received_at_least_one) {
        // 这不是错误，有些编码器需要多帧才能输出一个 packet（如 B 帧场景）
        dropped_frames_++;
        setLastFillStatus(FillStatus::CodecEagain);
        return FillResult::codecEagain();
    }
    
    setLastFillStatus(FillStatus::InternalError);
    return FillResult::internalError();
}
