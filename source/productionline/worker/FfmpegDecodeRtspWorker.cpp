#include "productionline/worker/FfmpegDecodeRtspWorker.hpp"
#include "common/Logger.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/NormalAllocator.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include <string.h>
#include <chrono>
#include <climits>  // for INT_MAX

// FFmpeg headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include "taco_sys_api.h"
}

// ============ 构造/析构 ============

FfmpegDecodeRtspWorker::FfmpegDecodeRtspWorker()
    : WorkerBase(BufferAllocatorFactory::AllocatorType::AVFRAME)  // 🎯 只需传递类型！
    , format_ctx_ptr_(nullptr)
    , codec_ctx_ptr_(nullptr)
    , sws_ctx_ptr_(nullptr)
    , video_stream_index_(-1)
    , width_(0)
    , height_(0)
    , output_pixel_format_(AV_PIX_FMT_BGRA)
    , output_bpp_(32)  // 默认ARGB888
    , use_hardware_decoder_(true)  // 默认启用硬件解码
    , decoder_name_()              // 默认自动选择（空字符串）
    , codec_options_ptr_(nullptr)
    , decoded_frames_(0)
    , dropped_frames_(0)
    , connected_(false)
    , is_open_(false)
    , eof_reached_(false)
{
    // rtsp_url_ 使用 std::string，无需手动初始化
    
    // 🎯 父类已经创建好 AVFRAME 类型的 allocator_facade_，无需任何初始化代码
    
    LOG_DEBUG("[Worker] FfmpegDecodeRtspWorker created");
}

// v2.2: 配置构造函数（新增）
FfmpegDecodeRtspWorker::FfmpegDecodeRtspWorker(const WorkerConfig& config)
    : WorkerBase(BufferAllocatorFactory::AllocatorType::AVFRAME, config)  // 传递 config 给父类
    , format_ctx_ptr_(nullptr)
    , codec_ctx_ptr_(nullptr)
    , sws_ctx_ptr_(nullptr)
    , video_stream_index_(-1)
    , width_(0)
    , height_(0)
    , output_pixel_format_(AV_PIX_FMT_BGRA)
    , output_bpp_(32)
    , use_hardware_decoder_(config.decoder.enable_hardware)  // 🎯 从配置读取
    , decoder_name_(config.decoder.name.value_or(""))  // 🎯 从配置读取（使用 optional 的 value_or）
    , codec_options_ptr_(nullptr)
    , decoded_frames_(0)
    , dropped_frames_(0)
    , connected_(false)
    , is_open_(false)
    , eof_reached_(false)
{
    // rtsp_url_ 使用 std::string，无需手动初始化
    
    LOG_DEBUG("[Worker] FfmpegDecodeRtspWorker created (with config)");
}

FfmpegDecodeRtspWorker::~FfmpegDecodeRtspWorker() {
    LOG_DEBUG("🧹 Destroying FfmpegDecodeRtspWorker...");
    close();
}

// ============ IVideoReader 接口实现 ============

bool FfmpegDecodeRtspWorker::open(const char* path) {
    LOG_ERROR("[Worker] ERROR: RTSP stream requires explicit format specification");
    LOG_ERROR("   Please use: open(rtsp_url, width, height, bits_per_pixel)");
    return false;
}

bool FfmpegDecodeRtspWorker::open(const char* path, int width, int height, int bits_per_pixel) {
    if (is_open_) {
        LOG_WARN_FMT("[Worker]  Warning: Stream already open, closing previous stream");
        close();
    }
    
    rtsp_url_ = path;  // 使用 std::string 自动管理
    
    width_ = width;
    height_ = height;
    
    // 根据 bits_per_pixel 确定输出格式
    switch (bits_per_pixel) {
        case 24:
            output_pixel_format_ = AV_PIX_FMT_BGR24;
            break;
        case 32:
            output_pixel_format_ = AV_PIX_FMT_BGRA;
            break;
        default:
            LOG_ERROR_FMT("[Worker] ERROR: Unsupported bits_per_pixel: %d", bits_per_pixel);
            return false;
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    output_bpp_ = bits_per_pixel;
    
    LOG_INFO("");
    LOG_INFO_FMT("📡 Opening RTSP stream: %s", rtsp_url_.c_str());
    LOG_INFO_FMT("   Output resolution: %dx%d", width_, height_);
    LOG_INFO_FMT("   Bits per pixel: %d", bits_per_pixel);
    
    // 连接RTSP流并初始化解码器
    if (!openMediaSource()) {
        return false;
    }
    
    // 🎯 Worker职责：在open()时自动创建BufferPool（通过调用Allocator）
    // 计算帧大小
    size_t frame_size = width_ * height_ * (bits_per_pixel / 8);
    if (frame_size == 0) {
        setError("Invalid frame size, cannot create BufferPool");
        closeMediaSource();
        return false;
    }
    
    int buffer_count = 4;  // RTSP流建议4-8个Buffer
    
    // v2.0: allocatePoolWithBuffers 返回 pool_id
    buffer_pool_id_ = allocator_facade_.allocatePoolWithBuffers(
        buffer_count,
        frame_size,
        std::string("FfmpegDecodeRtspWorker_") + std::string(path),
        "RTSP"
    );
    
    if (buffer_pool_id_ == 0) {
        setError("Failed to create BufferPool via Allocator");
        closeMediaSource();
        return false;
    }
    
    // v2.0: 从 Registry 获取 Pool 名称（返回 weak_ptr）
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(buffer_pool_id_);
    auto pool = pool_weak.lock();
    std::string pool_name = pool ? pool->getName() : "Unknown";
    
    is_open_ = true;
    eof_reached_ = false;
    decoded_frames_ = 0;
    dropped_frames_ = 0;
    
    LOG_DEBUG("[Worker] RTSP stream opened successfully");
    LOG_DEBUG_FMT("[Worker]    Resolution: %dx%d", width_, height_);
    LOG_DEBUG_FMT("[Worker]    Codec: %s", codec_ctx_ptr_->codec->name);
    LOG_DEBUG_FMT("[Worker]    BufferPool: '%s' (ID: %lu, %d buffers, %zu bytes each)", 
           pool_name.c_str(), buffer_pool_id_, buffer_count, frame_size);
    
    return true;
}

void FfmpegDecodeRtspWorker::close() {
    if (!is_open_) {
        return;
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    LOG_INFO("");
    LOG_INFO("🛑 Closing RTSP stream...");
    
    // v2.0: BufferPool 生命周期由 Allocator 管理，Worker 不需要调用 destroyPool
    // Allocator 析构时会自动清理所有 Pool
    buffer_pool_id_ = 0;  // 只清除ID，不调用destroyPool
    
    // 断开RTSP连接并释放资源
    closeMediaSource();
    
    is_open_ = false;
    connected_ = false;
    
    LOG_DEBUG("[Worker] RTSP stream closed");
    LOG_INFO_FMT("   Decoded frames: %d", decoded_frames_.load());
    LOG_INFO_FMT("   Dropped frames: %d", dropped_frames_.load());
}

bool FfmpegDecodeRtspWorker::isOpen() const {
    return is_open_;
}


bool FfmpegDecodeRtspWorker::seek(int frame_index) {
    LOG_WARN("[Worker]  Warning: RTSP stream does not support seeking");
    return false;
}

bool FfmpegDecodeRtspWorker::seekToBegin() {
    LOG_WARN("[Worker]  Warning: RTSP stream does not support seeking");
    return false;
}

bool FfmpegDecodeRtspWorker::seekToEnd() {
    LOG_WARN("[Worker]  Warning: RTSP stream does not support seeking");
    return false;
}

bool FfmpegDecodeRtspWorker::skip(int frame_count) {
    LOG_WARN("[Worker]  Warning: RTSP stream does not support frame skipping");
    return false;
}

int FfmpegDecodeRtspWorker::getTotalFrames() const {
    // RTSP 实时流是无限的，返回一个很大的值以适配 VideoProducer 的接口
    // 这样可以通过边界检查 (frame_index >= total_frames_)，同时不影响实际使用
    // 注意：RTSP 流并不依赖这个值，只是为了接口兼容性
    return INT_MAX;
}

int FfmpegDecodeRtspWorker::getCurrentFrameIndex() const {
    // 返回已解码帧数作为"当前索引"
    return decoded_frames_.load();
}

size_t FfmpegDecodeRtspWorker::getFrameSize() const {
    return width_ * height_ * getBytesPerPixel();
}

long FfmpegDecodeRtspWorker::getFileSize() const {
    // RTSP流没有文件大小概念
    return -1;
}

int FfmpegDecodeRtspWorker::getWidth() const {
    return width_;
}

int FfmpegDecodeRtspWorker::getHeight() const {
    return height_;
}

int FfmpegDecodeRtspWorker::getBytesPerPixel() const {
    switch (output_pixel_format_) {
        case AV_PIX_FMT_BGR24:
            return 3;
        case AV_PIX_FMT_BGRA:
        case AV_PIX_FMT_RGBA:
            return 4;
        default:
            return 4;
    }
}

const char* FfmpegDecodeRtspWorker::getPath() const {
    return rtsp_url_.c_str();
}

bool FfmpegDecodeRtspWorker::hasMoreFrames() const {
    // 只要连接着且未到达EOF，就有更多帧
    return connected_.load() && !eof_reached_.load();
}

bool FfmpegDecodeRtspWorker::isAtEnd() const {
    return eof_reached_.load();
}

// ============================================================================
// 核心功能：填充Buffer
// ============================================================================

bool FfmpegDecodeRtspWorker::fillBuffer(int frame_index, Buffer* buffer) {
    if (!buffer) {
        LOG_ERROR("[Worker] ERROR: buffer is nullptr");
        return false;
    }
    
    if (!is_open_) {
        LOG_ERROR("[Worker] ERROR: Worker is not open");
        return false;
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 步骤1: ⭐ v2.7改进：从 Buffer 获取关联的 AVFrame*
    AVFrame* frame_ptr = buffer->getAVFrame();
    if (!frame_ptr) {
        LOG_ERROR("[Worker] ERROR: buffer->getAVFrame() is nullptr");
        return false;
    }
    
    // 步骤2: 读取 packet（循环读取直到是视频流）
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        setError("Failed to allocate AVPacket");
        return false;
    }
    
    while (true) {
        int ret = av_read_frame(format_ctx_ptr_, packet);
        if (ret < 0) {
            av_packet_free(&packet);
            packet = nullptr;
            if (ret == AVERROR_EOF) {
                eof_reached_ = true;
                LOG_DEBUG("[Worker] RTSP EOF reached");
            } else {
                char errbuf[128];
                av_strerror(ret, errbuf, sizeof(errbuf));
                setError(std::string("av_read_frame failed: ") + errbuf, ret);
            }
            return false;
        }
        
        if (packet->stream_index == video_stream_index_) {
            break;  // 找到视频流
        }
        av_packet_unref(packet);
    }
    
    // 步骤3: 发送 packet 到解码器
    int ret = avcodec_send_packet(codec_ctx_ptr_, packet);
    av_packet_unref(packet);
    av_packet_free(&packet);
    
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        setError(std::string("avcodec_send_packet failed: ") + errbuf, ret);
        return false;
    }
    
    // 步骤4: 接收解码后的帧（循环直到成功）
    while (true) {
        ret = avcodec_receive_frame(codec_ctx_ptr_, frame_ptr);
        if (ret == 0) {
            // ✅ 成功解码
            break;
        } else if (ret == AVERROR(EAGAIN)) {
            // 需要更多数据，返回false让调用者再次调用
            return false;
        } else if (ret == AVERROR_EOF) {
            eof_reached_ = true;
            LOG_DEBUG("[Worker] Decoder EOF reached");
            return false;
        } else {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            setError(std::string("avcodec_receive_frame failed: ") + errbuf, ret);
            return false;
        }
    }
    
    // 步骤5: 提取物理地址（零拷贝模式）
    uint64_t phys_addr = 0;
    uint32_t blk_id = 0;
    
    if (frame_ptr->metadata) {
        AVDictionaryEntry* entry = av_dict_get(frame_ptr->metadata, "pool_blk_id", NULL, 0);
        if (entry) {
            blk_id = (uint32_t)atoi(entry->value);
            phys_addr = taco_sys_handle2_phys_addr(blk_id);
            buffer->setPhysicalAddress(phys_addr);
        }
    }
    
    if (phys_addr == 0) {
        LOG_WARN("[Worker]  Warning: Failed to extract physical address");
    }
    
    // 步骤6: ⭐ v2.7改进：先更新虚拟地址为实际数据地址（frame->data[0]）
    buffer->setVirtualAddress(frame_ptr->data[0]);
    
    // 步骤7: 设置图像元数据（v2.6新增）
    buffer->setImageMetadataFromAVFrame(frame_ptr);
    
    decoded_frames_++;
    return true;
}

// ============================================================================
// 提供原材料（BufferPool）
// ============================================================================

uint64_t FfmpegDecodeRtspWorker::getOutputBufferPoolId() {
    // 使用基类的实现（返回 pool_id）
    return WorkerBase::getOutputBufferPoolId();
}

// ============ RTSP 特有接口 ============

std::string FfmpegDecodeRtspWorker::getLastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void FfmpegDecodeRtspWorker::printStats() const {
    LOG_INFO("");
    LOG_INFO("📊 FfmpegDecodeRtspWorker Statistics:");
    LOG_INFO_FMT("   RTSP URL: %s", rtsp_url_.c_str());
    LOG_INFO_FMT("   Connected: %s", connected_.load() ? "Yes" : "No");
    LOG_INFO_FMT("   Decoded frames: %d", decoded_frames_.load());
    LOG_INFO_FMT("   Dropped frames: %d", dropped_frames_.load());
    LOG_INFO_FMT("   BufferPool ID: %lu", buffer_pool_id_);
}

// ============ 内部实现 ============

bool FfmpegDecodeRtspWorker::openMediaSource() {
    // 1. 分配格式上下文
    format_ctx_ptr_ = avformat_alloc_context();
    if (!format_ctx_ptr_) {
        setError("Failed to allocate AVFormatContext");
        return false;
    }
    
    // 2. 设置RTSP选项（超时、传输协议等）
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);  // 使用TCP传输
    av_dict_set(&options, "stimeout", "5000000", 0);    // 5秒超时
    av_dict_set(&options, "max_delay", "500000", 0);    // 最大延迟0.5秒
    
    // 3. 打开RTSP流
    int ret = avformat_open_input(&format_ctx_ptr_, rtsp_url_.c_str(), nullptr, &options);
    av_dict_free(&options);
    
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        setError(std::string("Failed to open RTSP stream: ") + errbuf);
        avformat_free_context(format_ctx_ptr_);
        format_ctx_ptr_ = nullptr;
        return false;
    }
    
    // 4. 获取流信息
    ret = avformat_find_stream_info(format_ctx_ptr_, nullptr);
    if (ret < 0) {
        setError("Failed to find stream information");
        avformat_close_input(&format_ctx_ptr_);
        return false;
    }
    
    // 5. 查找视频流
    if (!findVideoStream()) {
        avformat_close_input(&format_ctx_ptr_);
        return false;
    }
    
    // 6. 初始化解码器（支持配置）
    if (!initializeDecoder()) {
        avformat_close_input(&format_ctx_ptr_);
        return false;
    }
    
    // 10. 初始化格式转换上下文
    sws_ctx_ptr_ = sws_getContext(
        codec_ctx_ptr_->width, codec_ctx_ptr_->height, codec_ctx_ptr_->pix_fmt,
        width_, height_, (AVPixelFormat)output_pixel_format_,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!sws_ctx_ptr_) {
        setError("Failed to initialize SwsContext");
        avcodec_free_context(&codec_ctx_ptr_);
        avformat_close_input(&format_ctx_ptr_);
        return false;
    }
    
    connected_ = true;
    
    LOG_DEBUG("[Worker] Opened RTSP media source");
    LOG_INFO_FMT("   Codec: %s", codec_ctx_ptr_->codec->name);
    LOG_INFO_FMT("   Stream resolution: %dx%d", codec_ctx_ptr_->width, codec_ctx_ptr_->height);
    LOG_INFO_FMT("   Output resolution: %dx%d", width_, height_);
    
    return true;
}

void FfmpegDecodeRtspWorker::closeMediaSource() {
    if (sws_ctx_ptr_) {
        sws_freeContext(sws_ctx_ptr_);
        sws_ctx_ptr_ = nullptr;
    }
    
    if (codec_ctx_ptr_) {
        avcodec_free_context(&codec_ctx_ptr_);
        codec_ctx_ptr_ = nullptr;
    }
    
    if (format_ctx_ptr_) {
        avformat_close_input(&format_ctx_ptr_);
        format_ctx_ptr_ = nullptr;
    }
    
    // 释放解码器选项
    if (codec_options_ptr_) {
        av_dict_free(&codec_options_ptr_);
        codec_options_ptr_ = nullptr;
    }
    
    video_stream_index_ = -1;
    connected_ = false;
}

bool FfmpegDecodeRtspWorker::findVideoStream() {
    video_stream_index_ = -1;
    
    for (unsigned int i = 0; i < format_ctx_ptr_->nb_streams; i++) {
        if (format_ctx_ptr_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = (int)i;
            break;
        }
    }
    
    if (video_stream_index_ == -1) {
        setError("No video stream found in RTSP source");
        return false;
    }
    
    return true;
}

bool FfmpegDecodeRtspWorker::initializeDecoder() {
    AVCodecParameters* codecpar = format_ctx_ptr_->streams[video_stream_index_]->codecpar;
    
    // 1. 查找解码器（支持指定名称）
    const AVCodec* codec = nullptr;
    
    if (!decoder_name_.empty()) {
        // 用户指定了解码器名称（如 "h264_taco"）
        codec = avcodec_find_decoder_by_name(decoder_name_.c_str());
        if (!codec) {
            LOG_WARN_FMT("[Worker]  Warning: Specified decoder '%s' not found, trying default", decoder_name_.c_str());
        } else {
            LOG_DEBUG_FMT("[Worker] Using specified decoder: %s", decoder_name_.c_str());
        }
    }
    
    if (!codec) {
        // 使用默认解码器
        codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) {
            setError("Decoder not found for codec");
            return false;
        }
    }
    
    // 2. 分配解码器上下文
    codec_ctx_ptr_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_ptr_) {
        setError("Failed to allocate codec context");
        return false;
    }
    
    // 3. 复制参数到解码器上下文
    int ret = avcodec_parameters_to_context(codec_ctx_ptr_, codecpar);
    if (ret < 0) {
        setError("Failed to copy codec parameters", ret);
        avcodec_free_context(&codec_ctx_ptr_);
        codec_ctx_ptr_ = nullptr;
        return false;
    }
    
    // 4. 配置特殊解码器（如 h264_taco）
    if (decoder_name_ == "h264_taco") {
        if (!configureSpecialDecoder()) {
            LOG_ERROR("[Worker] ERROR: Failed to configure special decoder options");
            avcodec_free_context(&codec_ctx_ptr_);
            codec_ctx_ptr_ = nullptr;
            return false;
        }
    }
    
    // 5. 打开解码器
    ret = avcodec_open2(codec_ctx_ptr_, codec, codec_options_ptr_ ? &codec_options_ptr_ : nullptr);
    if (ret < 0) {
        setError("Failed to open codec", ret);
        avcodec_free_context(&codec_ctx_ptr_);
        codec_ctx_ptr_ = nullptr;
        return false;
    }
    
    return true;
}

bool FfmpegDecodeRtspWorker::configureSpecialDecoder() {
    // 配置 h264_taco 解码器（从 worker_config_ 读取配置）
    if (!codec_ctx_ptr_->priv_data) {
        LOG_WARN("[Worker]  Warning: codec_ctx->priv_data is NULL, cannot set options");
        return false;
    }
    
    // 🎯 从 worker_config_ 获取 taco 配置
    const auto& taco = worker_config_.decoder.taco;
    
    LOG_DEBUG("[Worker] Configuring h264_taco decoder options from config...");
    
    int ret;
    
    // 禁用重排序（从 config 读取）
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "reorder_disable", 
                         taco.reorder_disable ? 1 : 0, 0);
    LOG_DEBUG_FMT("[Worker]    reorder_disable=%d: %s", taco.reorder_disable ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    // 启用通道（从 config 读取）
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_enable", 
                         taco.ch0_enable ? 1 : 0, 0);
    LOG_DEBUG_FMT("[Worker]    ch0_enable=%d: %s", taco.ch0_enable ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_enable", 
                         taco.ch1_enable ? 1 : 0, 0);
    LOG_DEBUG_FMT("[Worker]    ch1_enable=%d: %s", taco.ch1_enable ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    // 配置通道1裁剪参数（从 config 读取）
    if (taco.ch1_crop_width > 0 && taco.ch1_crop_height > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_x", taco.ch1_crop_x, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_y", taco.ch1_crop_y, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_width", taco.ch1_crop_width, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_height", taco.ch1_crop_height, 0);
        LOG_DEBUG_FMT("[Worker]    ch1_crop: (%d, %d, %d, %d)", 
               taco.ch1_crop_x, taco.ch1_crop_y, 
               taco.ch1_crop_width, taco.ch1_crop_height);
    }
    
    // 配置通道1缩放参数（从 config 读取）
    if (taco.ch1_scale_width > 0 && taco.ch1_scale_height > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_scale_width", taco.ch1_scale_width, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_scale_height", taco.ch1_scale_height, 0);
        LOG_DEBUG_FMT("[Worker]    ch1_scale: (%d, %d)", taco.ch1_scale_width, taco.ch1_scale_height);
    }
    
    // 配置通道1 RGB（从 config 读取）
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_rgb", 
                         taco.ch1_rgb ? 1 : 0, 0);
    LOG_DEBUG_FMT("[Worker]    ch1_rgb=%d: %s", taco.ch1_rgb ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    // 设置RGB格式（从 config 读取）
    if (taco.ch1_rgb && !taco.ch1_rgb_format.empty()) {
        ret = av_opt_set(codec_ctx_ptr_->priv_data, "ch1_rgb_format", 
                         taco.ch1_rgb_format.c_str(), 0);
        LOG_DEBUG_FMT("[Worker]    ch1_rgb_format=%s: %s", taco.ch1_rgb_format.c_str(), 
               ret < 0 ? "FAILED" : "OK");
    }
    
    // 设置颜色标准（从 config 读取）
    if (taco.ch1_rgb && !taco.ch1_rgb_std.empty()) {
        ret = av_opt_set(codec_ctx_ptr_->priv_data, "ch1_rgb_std", 
                         taco.ch1_rgb_std.c_str(), 0);
        LOG_DEBUG_FMT("[Worker]    ch1_rgb_std=%s: %s", taco.ch1_rgb_std.c_str(), 
               ret < 0 ? "FAILED" : "OK");
    }
    
    return true;
}

void FfmpegDecodeRtspWorker::setError(const std::string& error, int ffmpeg_error) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = error;
    
    if (ffmpeg_error != 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ffmpeg_error, err_buf, sizeof(err_buf));
        LOG_ERROR_FMT("[Worker] FfmpegDecodeRtspWorker Error: %s (FFmpeg: %s)", error.c_str(), err_buf);
    } else {
        LOG_ERROR_FMT("[Worker] FfmpegDecodeRtspWorker Error: %s", error.c_str());
    }
}



