#include "productionline/worker/FfmpegDecodeVideoFileWorker.hpp"
#include "common/Logger.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include <cstring>
#include <cstdio>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>  // 用于 av_strerror
#include <libswscale/swscale.h>
#include "taco_sys_api.h"
}

// ============================================================================
// 构造/析构
// ============================================================================

// 默认构造函数（向后兼容）
FfmpegDecodeVideoFileWorker::FfmpegDecodeVideoFileWorker()
    : WorkerBase(BufferAllocatorFactory::AllocatorType::AVFRAME)
    , format_ctx_ptr_(nullptr)
    , codec_ctx_ptr_(nullptr)
    , sws_ctx_ptr_(nullptr)
    , video_stream_index_(-1)
    , width_(0)
    , height_(0)
    , output_width_(0)
    , output_height_(0)
    , output_bpp_(32)  // 默认ARGB888
    , output_pixel_format_(AV_PIX_FMT_BGRA)
    , total_frames_(-1)
    , current_frame_index_(0)
    , is_open_(false)
    , is_ffmpeg_opened_(false)
    , eof_reached_(false)
    , zero_copy_buffer_pool_ptr_(nullptr)
    , use_hardware_decoder_(true)  // 默认启用硬件解码
    , decoder_name_()              // 默认自动选择（空字符串）
    , codec_options_ptr_(nullptr)
    , decoded_frames_(0)
    , decode_errors_(0)
    , last_ffmpeg_error_(0)
{
    // file_path_ 使用 std::string，无需手动初始化
}

// 配置构造函数（v2.2新增）
FfmpegDecodeVideoFileWorker::FfmpegDecodeVideoFileWorker(const WorkerConfig& config)
    : WorkerBase(BufferAllocatorFactory::AllocatorType::AVFRAME, config)
    , format_ctx_ptr_(nullptr)
    , codec_ctx_ptr_(nullptr)
    , sws_ctx_ptr_(nullptr)
    , video_stream_index_(-1)
    , width_(0)
    , height_(0)
    , output_width_(0)
    , output_height_(0)
    , output_bpp_(32)
    , output_pixel_format_(AV_PIX_FMT_BGRA)
    , total_frames_(-1)
    , current_frame_index_(0)
    , is_open_(false)
    , is_ffmpeg_opened_(false)
    , eof_reached_(false)
    , zero_copy_buffer_pool_ptr_(nullptr)
    , use_hardware_decoder_(config.decoder.enable_hardware)  // 🎯 从配置读取
    , decoder_name_(config.decoder.name.value_or(""))  // 🎯 从配置读取（使用 optional 的 value_or）
    , codec_options_ptr_(nullptr)
    , decoded_frames_(0)
    , decode_errors_(0)
    , last_ffmpeg_error_(0)
{
    // file_path_ 使用 std::string，无需手动初始化
}

FfmpegDecodeVideoFileWorker::~FfmpegDecodeVideoFileWorker() {
    LOG_DEBUG("[FfmpegDecodeVideoFileWorker] 析构函数开始");
    
    // ⭐ 关键修改：正确的清理顺序
    //
    // 问题根源：
    // - 成员变量的析构永远在析构函数体执行完毕后
    // - 如果在函数体内先调用 closeMediaSource()，再让成员变量析构
    // - 顺序就变成：关闭解码器 → 释放 AVFrame
    // - 但此时 AVFrame 可能还引用了解码器的资源，导致 free(): invalid pointer
    //
    // 正确顺序：
    // 1. 手动调用 allocator_facade_.destroyPool() 先释放所有 AVFrame
    // 2. 再调用 close() 关闭解码器
    // 3. 成员变量自动析构（但 Pool 已清理，destroyPool() 幂等性保证不会重复释放）
    
    // 步骤1：先清理 BufferPool 和 AVFrame
    if (buffer_pool_id_ != 0) {
        LOG_DEBUG("[FfmpegDecodeVideoFileWorker] 手动清理 BufferPool 和 AVFrame...");
        allocator_facade_.destroyPool();  // 释放所有 Pool 中的 Buffer 和 AVFrame
        buffer_pool_id_ = 0;
    }
    
    // 步骤2：再关闭解码器（此时 AVFrame 已全部释放）
    if (is_open_.load(std::memory_order_acquire)) {
        LOG_DEBUG("[FfmpegDecodeVideoFileWorker] 关闭解码器...");
        close();  // 只关闭解码器，不再清理 Pool（已在上面清理）
    }
    
    LOG_DEBUG("[FfmpegDecodeVideoFileWorker] 析构函数体结束");
    
    // ⭐ 函数体执行完毕后，成员变量自动析构
    // ⭐ 但由于 Pool 已经手动清理，allocator_facade_.destroyPool() 会因为幂等性直接返回
}

// ============================================================================
// 打开/关闭
// ============================================================================

bool FfmpegDecodeVideoFileWorker::open(const char* path) {
    if (!path) {
        setError("Invalid file path (nullptr)");
        return false;
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 如果已经打开，先关闭
    if (is_open_.load(std::memory_order_acquire)) {
        closeMediaSource();
    }
    
    // 保存路径（使用 std::string 自动管理）
    file_path_ = path;
    
    // 打开FFmpeg资源
    if (!openMediaSource()) {
        return false;
    }
    
    // 🎯 Worker职责：在open()时自动创建BufferPool（通过调用Allocator）
    // 计算帧大小（在openMediaSource()后，output_width_和output_height_已设置）
    size_t frame_size = output_width_ * output_height_ * output_bpp_ / 8;
    if (frame_size == 0) {
        setError("Invalid frame size, cannot create BufferPool");
        closeMediaSource();
        return false;
    }
    
    int buffer_count = 16;  // 默认创建4个Buffer
    
    // v2.0: allocatePoolWithBuffers 返回 pool_id
    buffer_pool_id_ = allocator_facade_.allocatePoolWithBuffers(
        buffer_count,
        frame_size,
        std::string("FfmpegDecodeVideoFileWorker_") + std::string(path),
        "Video"
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
    
    is_open_.store(true, std::memory_order_release);
    current_frame_index_ = 0;
    eof_reached_ = false;
    decoded_frames_ = 0;
    decode_errors_ = 0;
    
    LOG_DEBUG_FMT("[Worker] FfmpegDecodeVideoFileWorker: Opened '%s'", path);
    LOG_DEBUG_FMT("[Worker]    Resolution: %dx%d → %dx%d", width_, height_, output_width_, output_height_);
    LOG_DEBUG_FMT("[Worker]    Codec: %s", codec_ctx_ptr_->codec->name);
    LOG_DEBUG_FMT("[Worker]    Total frames (estimated): %d", total_frames_);
    LOG_DEBUG_FMT("[Worker]    BufferPool: '%s' (ID: %lu, %d buffers, %zu bytes each)", 
           pool_name.c_str(), buffer_pool_id_, buffer_count, frame_size);
    
    return true;
}

bool FfmpegDecodeVideoFileWorker::open(const char* path, int width, int height, int bits_per_pixel) {
    // FfmpegDecodeVideoFileWorker 忽略 width/height/bpp 参数，自动检测格式
    (void)width;
    (void)height;
    (void)bits_per_pixel;
    return open(path);
}

void FfmpegDecodeVideoFileWorker::close() {
    // 🎯 原子检查并设置：如果 is_open_ 是 true，则设置为 false
    // 返回值表示是否成功设置（即之前是 true）
    bool expected = true;
    if (!is_open_.compare_exchange_strong(expected, false,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        // is_open_ 已经是 false，说明已经关闭过了，直接返回
        return;
    }
    
    // 🎯 只有第一个线程能执行到这里（is_open_ 从 true 变为 false）
    // 此时 is_open_ == false，其他线程调用 close() 会直接返回
    
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        // ⭐ 关键修改：Worker 只负责业务逻辑（关闭解码器）
        //    BufferPool 和 AVFrame 的清理由 allocator_facade_ 析构时自动处理
        //
        // 资源释放顺序（析构时）：
        // 1. closeMediaSource() - 释放解码器、格式上下文（业务资源）
        // 2. ~allocator_facade_() - 释放 BufferPool 和 AVFrame（底层内存资源）
        //
        // 设计原则：
        // - Worker::close() 只负责业务逻辑清理
        // - Allocator::~Allocator() 负责内存资源清理
        // - 符合单一职责原则和 RAII 原则
        closeMediaSource();
        
        // ⭐ 清除 buffer_pool_id_（标记不再使用）
        buffer_pool_id_ = 0;
    }
    
    // is_open_ 已经在上面设置为 false，不需要再次设置
}

bool FfmpegDecodeVideoFileWorker::isOpen() const {
    return is_open_.load(std::memory_order_acquire);
}

// ============================================================================
// 内部方法：打开FFmpeg资源
// ============================================================================

bool FfmpegDecodeVideoFileWorker::openMediaSource() {
    // 🎯 重置FFmpeg资源状态标志
    is_ffmpeg_opened_.store(false, std::memory_order_release);
    
    // 1. 打开输入文件
    format_ctx_ptr_ = avformat_alloc_context();
    if (!format_ctx_ptr_) {
        setError("Failed to allocate AVFormatContext");
        return false;
    }
    
    int ret = avformat_open_input(&format_ctx_ptr_, file_path_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        setError("Failed to open video file", ret);
        format_ctx_ptr_ = nullptr;
        return false;
    }
    
    // 2. 读取流信息
    ret = avformat_find_stream_info(format_ctx_ptr_, nullptr);
    if (ret < 0) {
        setError("Failed to find stream info", ret);
        closeMediaSource();
        return false;
    }
    
    // 3. 查找视频流
    if (!findVideoStream()) {
        closeMediaSource();
        return false;
    }
    
    // 4. 初始化解码器
    if (!initializeDecoder()) {
        closeMediaSource();
        return false;
    }
    
    // 5. 估算总帧数
    total_frames_ = estimateTotalFrames();
    
    // 6. 设置输出分辨率（如果未设置，使用原始分辨率）
    if (output_width_ == 0 || output_height_ == 0) {
        output_width_ = width_;
        output_height_ = height_;
    }
   
    // 🎯 成功打开FFmpeg资源，设置标志位
    is_ffmpeg_opened_.store(true, std::memory_order_release);
    
    return true;
}

void FfmpegDecodeVideoFileWorker::closeMediaSource() {
    // 🎯 原子检查并设置：如果 is_ffmpeg_opened_ 是 true，则设置为 false
    // 返回值表示是否成功设置（即之前是 true）
    bool expected = true;
    if (!is_ffmpeg_opened_.compare_exchange_strong(expected, false,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
        // is_ffmpeg_opened_ 已经是 false，说明已经关闭过了，直接返回
        return;
    }
    
    // 🎯 只有第一个线程能执行到这里（is_ffmpeg_opened_ 从 true 变为 false）
    // 此时 is_ffmpeg_opened_ == false，其他线程调用 closeMediaSource() 会直接返回
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 释放格式转换器
    if (sws_ctx_ptr_) {
        sws_freeContext(sws_ctx_ptr_);
        sws_ctx_ptr_ = nullptr;
    }
    
    // 释放解码器
    if (codec_ctx_ptr_) {
        // 🔧 临时注释：TACO解码器可能在flush时损坏内部指针
        avcodec_free_context(&codec_ctx_ptr_);
        codec_ctx_ptr_ = nullptr;
    }
    
    // 释放格式上下文
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
}

bool FfmpegDecodeVideoFileWorker::findVideoStream() {
    video_stream_index_ = -1;
    
    for (unsigned int i = 0; i < format_ctx_ptr_->nb_streams; i++) {
        if (format_ctx_ptr_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = (int)i;
            break;
        }
    }
    
    if (video_stream_index_ == -1) {
        setError("No video stream found in file");
        return false;
    }
    
    AVCodecParameters* codecpar = format_ctx_ptr_->streams[video_stream_index_]->codecpar;
    width_ = codecpar->width;
    height_ = codecpar->height;
    
    return true;
}

bool FfmpegDecodeVideoFileWorker::initializeDecoder() {
    AVCodecParameters* codecpar = format_ctx_ptr_->streams[video_stream_index_]->codecpar;
    
    // 1. 查找解码器
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
        codec_ctx_ptr_ = nullptr;  // 🔧 置空防止 double free
        return false;
    }
    
    // 4. 配置特殊解码器（如 h264_taco）
    if (decoder_name_ == "h264_taco") {
        if (!configureSpecialDecoder()) {
            // 🔧 修复：配置失败是致命错误，必须返回
            LOG_ERROR_FMT("[Worker] ERROR: Failed to configure special decoder options");
            avcodec_free_context(&codec_ctx_ptr_);
            codec_ctx_ptr_ = nullptr;  // 🔧 置空防止 double free
            return false;
        }
    }
    
    // 5. 打开解码器
    ret = avcodec_open2(codec_ctx_ptr_, codec, codec_options_ptr_ ? &codec_options_ptr_ : nullptr);
    if (ret < 0) {
        setError("Failed to open codec", ret);
        avcodec_free_context(&codec_ctx_ptr_);
        codec_ctx_ptr_ = nullptr;  // 🔧 置空防止 double free
        return false;
    }
    
    return true;
}

bool FfmpegDecodeVideoFileWorker::configureSpecialDecoder() {
    // 配置 h264_taco 解码器（从 worker_config_ 读取配置）
    if (!codec_ctx_ptr_->priv_data) {
        LOG_WARN_FMT("[Worker]  Warning: codec_ctx->priv_data is NULL, cannot set options");
        return false;
    }
    
    // 🎯 从 worker_config_ 获取 taco 配置
    const auto& taco = worker_config_.decoder.taco;
    
    LOG_DEBUG_FMT("[Worker] Configuring h264_taco decoder options from config...");
    
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


int FfmpegDecodeVideoFileWorker::estimateTotalFrames() {
    if (!format_ctx_ptr_ || video_stream_index_ < 0) {
        return -1;
    }
    
    AVStream* stream = format_ctx_ptr_->streams[video_stream_index_];
    
    // 方法1：从流的 nb_frames 获取
    if (stream->nb_frames > 0) {
        return (int)stream->nb_frames;
    }
    
    // 方法2：根据时长和帧率估算
    if (stream->duration != AV_NOPTS_VALUE && stream->avg_frame_rate.num > 0) {
        double duration_sec = stream->duration * av_q2d(stream->time_base);
        double fps = av_q2d(stream->avg_frame_rate);
        return (int)(duration_sec * fps);
    }
    
    // 方法3：根据文件大小和比特率估算（不太准确）
    if (format_ctx_ptr_->duration != AV_NOPTS_VALUE && stream->avg_frame_rate.num > 0) {
        double duration_sec = format_ctx_ptr_->duration / (double)AV_TIME_BASE;
        double fps = av_q2d(stream->avg_frame_rate);
        return (int)(duration_sec * fps);
    }
    
    return -1;  // 无法估算
}

// ============================================================================
// 导航操作
// ============================================================================

bool FfmpegDecodeVideoFileWorker::seek(int frame_index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    close();
    open(file_path_.c_str());
    return true;
}

bool FfmpegDecodeVideoFileWorker::seekToBegin() {
    return seek(0);
}

bool FfmpegDecodeVideoFileWorker::seekToEnd() {
    if (total_frames_ > 0) {
        return seek(total_frames_ - 1);
    }
    return false;
}

bool FfmpegDecodeVideoFileWorker::skip(int frame_count) {
    return seek(current_frame_index_ + frame_count);
}

// ============================================================================
// 信息查询
// ============================================================================

int FfmpegDecodeVideoFileWorker::getTotalFrames() const {
    return total_frames_;
}

int FfmpegDecodeVideoFileWorker::getCurrentFrameIndex() const {
    return current_frame_index_;
}

size_t FfmpegDecodeVideoFileWorker::getFrameSize() const {
    return output_width_ * output_height_ * (output_bpp_ / 8);
}

long FfmpegDecodeVideoFileWorker::getFileSize() const {
    if (!format_ctx_ptr_) {
        return -1;
    }
    
    // 尝试从格式上下文获取
    AVIOContext* io_ctx = format_ctx_ptr_->pb;
    if (io_ctx) {
        return avio_size(io_ctx);
    }
    
    return -1;
}

int FfmpegDecodeVideoFileWorker::getWidth() const {
    return output_width_;
}

int FfmpegDecodeVideoFileWorker::getHeight() const {
    return output_height_;
}

int FfmpegDecodeVideoFileWorker::getBytesPerPixel() const {
    return output_bpp_ / 8;
}

const char* FfmpegDecodeVideoFileWorker::getPath() const {
    return file_path_.c_str();
}

bool FfmpegDecodeVideoFileWorker::hasMoreFrames() const {
    return !eof_reached_;
}

bool FfmpegDecodeVideoFileWorker::isAtEnd() const {
    return eof_reached_;
}

// ============================================================================
// 核心功能：填充Buffer
// ============================================================================

bool FfmpegDecodeVideoFileWorker::fillBuffer(int frame_index, Buffer* buffer) {
    if (!buffer) {
        LOG_ERROR_FMT("[Worker] ERROR: buffer is nullptr");
        return false;
    }
    
    if (!is_open_.load(std::memory_order_acquire)) {
        LOG_ERROR_FMT("[Worker] ERROR: Worker is not open");
        return false;
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 步骤1: ⭐ v2.7改进：从 Buffer 获取关联的 AVFrame*
    AVFrame* frame_ptr = buffer->getAVFrame();
    if (!frame_ptr) {
        LOG_ERROR_FMT("[Worker] ERROR: buffer->getAVFrame() is nullptr");
        return false;
    }
    
    // 步骤1.1: ⭐ v2.8新增：从 Buffer 获取关联的 AVPacket*
    AVPacket* packet_ptr = buffer->getAVPacket();
    if (!packet_ptr) {
        LOG_ERROR_FMT("[Worker] ERROR: buffer->getAVPacket() is nullptr");
        return false;
    }
    
    // 步骤2: 读取一个 packet（参考 ids_test_video3:2240）
    // 🔧 修复：对于损坏帧，在内部循环尝试读取，而不是返回 false
    const int AVERROR_INVALIDDATA_VALUE = -1094995529;  // AVERROR(0x41444e49)
    const int MAX_CORRUPTED_RETRIES = 10;  // 最大重试次数，避免无限循环
    
    int corrupted_retries = 0;
    int read_ret;
    
    while (true) {
        read_ret = av_read_frame(format_ctx_ptr_, packet_ptr);
        
        if (read_ret < 0) {
            if (read_ret == AVERROR_EOF) {
                LOG_DEBUG("🔄 EOF reached");
                // 🔧 修复：Worker 不应该决定是否循环，只设置 EOF 标志并返回 false
                // 循环逻辑由 ProductionLine 根据 loop_ 变量控制
                av_packet_unref(packet_ptr);
                eof_reached_ = true;
                return false;
            } else if (read_ret == AVERROR_INVALIDDATA_VALUE) {
                // 🔧 修复：遇到损坏帧时，在内部循环跳过，继续读取下一个 packet
                corrupted_retries++;
                if (corrupted_retries <= MAX_CORRUPTED_RETRIES) {
                    LOG_WARN_FMT("[Worker]  WARNING: Corrupted packet detected (attempt %d/%d), skipping...\n", 
                           corrupted_retries, MAX_CORRUPTED_RETRIES);
                    av_packet_unref(packet_ptr);
                    // 继续循环，尝试读取下一个 packet
                    continue;
                } else {
                    // 连续多次都是损坏帧，可能文件确实损坏严重，返回失败
                    LOG_ERROR_FMT("[Worker] ERROR: Too many corrupted packets (%d), giving up\n", corrupted_retries);
                    av_packet_unref(packet_ptr);
                    return false;
                }
            } else {
                // 其他错误（非 EOF，非损坏帧）：记录错误并返回
                char err_buf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(read_ret, err_buf, sizeof(err_buf));
                LOG_ERROR_FMT("[Worker] ERROR: av_read_frame failed: %d (%s)\n", read_ret, err_buf);
                av_packet_unref(packet_ptr);
                return false;
            }
        } else {
            // 成功读取到 packet，退出循环
            break;
        }
    }
    
    // 步骤3: 检查是否是视频流
    if (packet_ptr->stream_index != video_stream_index_) {
        // 🔧 修复：不是视频流的packet需要释放，然后继续读取下一个
        av_packet_unref(packet_ptr);
        return false;  // 让调用者再次调用以读取下一个packet
    }
    
    // 步骤4: 发送 packet 到解码器（参考 ids_test_video3:2270）
    int ret = avcodec_send_packet(codec_ctx_ptr_, packet_ptr);
    
    // 🔧 修复：无论成功与否，都要释放packet引用
    // avcodec_send_packet 会复制数据，不再需要原始packet
    av_packet_unref(packet_ptr);
    
    if (ret < 0) {
        LOG_ERROR_FMT("[Worker] ERROR: avcodec_send_packet failed: %d", ret);
        return false;
    }
    bool recv_frm = false;
    // 步骤5: 🎯 循环调用 receive_frame，直到成功或需要更多数据（参考 ids_test_video3:2276-2354）
    while (true) {
        ret = avcodec_receive_frame(codec_ctx_ptr_, frame_ptr);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0) {
            break;
        } 
        // ✅ 成功！提取物理地址（参考 ids_test_video3:2314-2338）
        uint64_t phys_addr = 0;
        uint32_t blk_id = 0;
        if (frame_ptr->metadata) {
            AVDictionaryEntry* entry = av_dict_get(frame_ptr->metadata, "pool_blk_id", NULL, 0);
            if (entry) {
                blk_id = (uint32_t)atoi(entry->value);
                phys_addr = taco_sys_handle2_phys_addr(blk_id);
                
                // 🎯 保存物理地址到 Buffer
                buffer->setPhysicalAddress(phys_addr);
            }
        }
        
        if (phys_addr == 0) {
            LOG_WARN_FMT("[Worker]  Warning: Failed to extract physical address");
            return false;
        }
        
        // ⭐ v2.7改进：先更新虚拟地址为实际数据地址（frame->data[0]）
        buffer->setVirtualAddress(frame_ptr->data[0]);
        
        // ⭐ v2.6新增：从AVFrame设置图像元数据到Buffer
        buffer->setImageMetadataFromAVFrame(frame_ptr);
        decoded_frames_++;
        current_frame_index_++;
        recv_frm = true;
    }
    return recv_frm;
}

// ============================================================================
// 提供原材料（BufferPool）
// ============================================================================

uint64_t FfmpegDecodeVideoFileWorker::getOutputBufferPoolId() {
    // v2.0: 使用基类的实现（返回 pool_id）
    return WorkerBase::getOutputBufferPoolId();
}

// ============================================================================
// 辅助方法
// ============================================================================

void FfmpegDecodeVideoFileWorker::setError(const std::string& error, int ffmpeg_error) {
    last_error_ = error;
    last_ffmpeg_error_ = ffmpeg_error;
    
    if (ffmpeg_error != 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ffmpeg_error, err_buf, sizeof(err_buf));
        LOG_ERROR_FMT("[Worker] FfmpegDecodeVideoFileWorker Error: %s (FFmpeg: %s)\n", error.c_str(), err_buf);
    } else {
        LOG_ERROR_FMT("[Worker] FfmpegDecodeVideoFileWorker Error: %s", error.c_str());
    }
}

std::string FfmpegDecodeVideoFileWorker::getLastError() const {
    return last_error_;
}

const char* FfmpegDecodeVideoFileWorker::getCodecName() const {
    if (codec_ctx_ptr_ && codec_ctx_ptr_->codec) {
        return codec_ctx_ptr_->codec->name;
    }
    return "unknown";
}

void FfmpegDecodeVideoFileWorker::printStats() const {
    LOG_INFO("\n[Worker] 📊 FfmpegDecodeVideoFileWorker Statistics:");
    LOG_INFO_FMT("[Worker]    File: %s", file_path_.c_str());
    LOG_INFO_FMT("[Worker]    Codec: %s", getCodecName());
    LOG_INFO_FMT("[Worker]    Resolution: %dx%d → %dx%d", width_, height_, output_width_, output_height_);
    LOG_INFO_FMT("[Worker]    Total frames: %d", total_frames_);
    LOG_INFO_FMT("[Worker]    Current frame: %d", current_frame_index_);
    LOG_INFO_FMT("[Worker]    Decoded frames: %d", decoded_frames_.load());
    LOG_INFO_FMT("[Worker]    Decode errors: %d", decode_errors_.load());
    LOG_INFO_FMT("[Worker]    EOF: %s", eof_reached_ ? "YES" : "NO");
}

