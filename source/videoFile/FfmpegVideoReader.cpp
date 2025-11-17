#include "../../include/videoFile/FfmpegVideoReader.hpp"
#include <cstring>
#include <cstdio>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/dict.h>
#include <libswscale/swscale.h>
}

// taco_sys 接口（零拷贝模式需要）
extern "C" {
    uint64_t taco_sys_handle2_phys_addr(uint32_t handle);
}

// ============================================================================
// 构造/析构
// ============================================================================

FfmpegVideoReader::FfmpegVideoReader()
    : format_ctx_(nullptr)
    , codec_ctx_(nullptr)
    , sws_ctx_(nullptr)
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
    , eof_reached_(false)
    , buffer_pool_(nullptr)
    , supports_zero_copy_(false)
    , use_hardware_decoder_(true)  // 默认启用硬件解码
    , decoder_name_(nullptr)
    , codec_options_(nullptr)
    , decoded_frames_(0)
    , decode_errors_(0)
    , last_ffmpeg_error_(0)
{
    memset(file_path_, 0, sizeof(file_path_));
}

FfmpegVideoReader::~FfmpegVideoReader() {
    close();
}

// ============================================================================
// 打开/关闭
// ============================================================================

bool FfmpegVideoReader::open(const char* path) {
    if (!path) {
        setError("Invalid file path (nullptr)");
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 如果已经打开，先关闭
    if (is_open_) {
        closeVideo();
    }
    
    // 保存路径
    strncpy(file_path_, path, MAX_VIDEO_PATH_LENGTH - 1);
    file_path_[MAX_VIDEO_PATH_LENGTH - 1] = '\0';
    
    // 打开视频文件
    if (!openVideo()) {
        return false;
    }
    
    is_open_ = true;
    current_frame_index_ = 0;
    eof_reached_ = false;
    decoded_frames_ = 0;
    decode_errors_ = 0;
    
    printf("✅ FfmpegVideoReader: Opened '%s'\n", path);
    printf("   Resolution: %dx%d → %dx%d\n", width_, height_, output_width_, output_height_);
    printf("   Codec: %s\n", codec_ctx_->codec->name);
    printf("   Total frames (estimated): %d\n", total_frames_);
    printf("   Zero-copy: %s\n", supports_zero_copy_ ? "YES" : "NO");
    
    return true;
}

bool FfmpegVideoReader::openRaw(const char* path, int width, int height, int bits_per_pixel) {
    (void)path;
    (void)width;
    (void)height;
    (void)bits_per_pixel;
    setError("FfmpegVideoReader does not support raw video files. Use MmapVideoReader or IoUringVideoReader instead.");
    return false;
}

void FfmpegVideoReader::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeVideo();
    is_open_ = false;
}

bool FfmpegVideoReader::isOpen() const {
    return is_open_;
}

// ============================================================================
// 内部方法：打开视频
// ============================================================================

bool FfmpegVideoReader::openVideo() {
    // 1. 打开输入文件
    format_ctx_ = avformat_alloc_context();
    if (!format_ctx_) {
        setError("Failed to allocate AVFormatContext");
        return false;
    }
    
    int ret = avformat_open_input(&format_ctx_, file_path_, nullptr, nullptr);
    if (ret < 0) {
        setError("Failed to open video file", ret);
        format_ctx_ = nullptr;
        return false;
    }
    
    // 2. 读取流信息
    ret = avformat_find_stream_info(format_ctx_, nullptr);
    if (ret < 0) {
        setError("Failed to find stream info", ret);
        closeVideo();
        return false;
    }
    
    // 3. 查找视频流
    if (!findVideoStream()) {
        closeVideo();
        return false;
    }
    
    // 4. 初始化解码器
    if (!initializeDecoder()) {
        closeVideo();
        return false;
    }
    
    // 5. 估算总帧数
    total_frames_ = estimateTotalFrames();
    
    // 6. 设置输出分辨率（如果未设置，使用原始分辨率）
    if (output_width_ == 0 || output_height_ == 0) {
        output_width_ = width_;
        output_height_ = height_;
    }
    
    // 7. 检查零拷贝支持
    supports_zero_copy_ = checkZeroCopySupport();
    
    // 8. 初始化格式转换器（普通模式需要）
    if (!supports_zero_copy_) {
        if (!initializeSwsContext()) {
            closeVideo();
            return false;
        }
    }
    
    return true;
}

void FfmpegVideoReader::closeVideo() {
    // 释放格式转换器
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    
    // 释放解码器
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    
    // 释放格式上下文
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }
    
    // 释放解码器选项
    if (codec_options_) {
        av_dict_free(&codec_options_);
        codec_options_ = nullptr;
    }
    
    video_stream_index_ = -1;
    supports_zero_copy_ = false;
}

bool FfmpegVideoReader::findVideoStream() {
    video_stream_index_ = -1;
    
    for (unsigned int i = 0; i < format_ctx_->nb_streams; i++) {
        if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = (int)i;
            break;
        }
    }
    
    if (video_stream_index_ == -1) {
        setError("No video stream found in file");
        return false;
    }
    
    AVCodecParameters* codecpar = format_ctx_->streams[video_stream_index_]->codecpar;
    width_ = codecpar->width;
    height_ = codecpar->height;
    
    return true;
}

bool FfmpegVideoReader::initializeDecoder() {
    AVCodecParameters* codecpar = format_ctx_->streams[video_stream_index_]->codecpar;
    
    // 1. 查找解码器
    const AVCodec* codec = nullptr;
    
    if (decoder_name_) {
        // 用户指定了解码器名称（如 "h264_taco"）
        codec = avcodec_find_decoder_by_name(decoder_name_);
        if (!codec) {
            printf("⚠️  Warning: Specified decoder '%s' not found, trying default\n", decoder_name_);
        } else {
            printf("✅ Using specified decoder: %s\n", decoder_name_);
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
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        setError("Failed to allocate codec context");
        return false;
    }
    
    // 3. 复制参数到解码器上下文
    int ret = avcodec_parameters_to_context(codec_ctx_, codecpar);
    if (ret < 0) {
        setError("Failed to copy codec parameters", ret);
        return false;
    }
    
    // 4. 配置特殊解码器（如 h264_taco）
    if (decoder_name_ && strcmp(decoder_name_, "h264_taco") == 0) {
        if (!configureSpecialDecoder()) {
            // 配置失败不是致命错误，继续使用默认配置
            printf("⚠️  Warning: Failed to configure special decoder options\n");
        }
    }
    
    // 5. 打开解码器
    ret = avcodec_open2(codec_ctx_, codec, codec_options_ ? &codec_options_ : nullptr);
    if (ret < 0) {
        setError("Failed to open codec", ret);
        return false;
    }
    
    return true;
}

bool FfmpegVideoReader::configureSpecialDecoder() {
    // 配置 h264_taco 解码器（参考 ids_test_video3）
    if (!codec_ctx_->priv_data) {
        printf("⚠️  Warning: codec_ctx->priv_data is NULL, cannot set options\n");
        return false;
    }
    
    printf("🔧 Configuring h264_taco decoder options...\n");
    
    int ret;
    
    // 禁用重排序
    ret = av_opt_set_int(codec_ctx_->priv_data, "reorder_disable", 1, 0);
    printf("   reorder_disable=1: %s\n", ret < 0 ? "FAILED" : "OK");
    
    // 启用双通道（CH0: YUV, CH1: RGB）
    ret = av_opt_set_int(codec_ctx_->priv_data, "ch0_enable", 1, 0);
    printf("   ch0_enable=1: %s\n", ret < 0 ? "FAILED" : "OK");
    
    ret = av_opt_set_int(codec_ctx_->priv_data, "ch1_enable", 1, 0);
    printf("   ch1_enable=1: %s\n", ret < 0 ? "FAILED" : "OK");
    
    // 配置通道1（RGB输出）
    av_opt_set_int(codec_ctx_->priv_data, "ch1_crop_x", 0, 0);
    av_opt_set_int(codec_ctx_->priv_data, "ch1_crop_y", 0, 0);
    av_opt_set_int(codec_ctx_->priv_data, "ch1_crop_width", 0, 0);
    av_opt_set_int(codec_ctx_->priv_data, "ch1_crop_height", 0, 0);
    av_opt_set_int(codec_ctx_->priv_data, "ch1_scale_width", 0, 0);
    av_opt_set_int(codec_ctx_->priv_data, "ch1_scale_height", 0, 0);
    
    ret = av_opt_set_int(codec_ctx_->priv_data, "ch1_rgb", 1, 0);
    printf("   ch1_rgb=1: %s\n", ret < 0 ? "FAILED" : "OK");
    
    // 设置RGB格式为ARGB888
    ret = av_opt_set(codec_ctx_->priv_data, "ch1_rgb_format", "argb888", 0);
    printf("   ch1_rgb_format=argb888: %s\n", ret < 0 ? "FAILED" : "OK");
    
    // 设置颜色标准为BT.601
    ret = av_opt_set(codec_ctx_->priv_data, "ch1_rgb_std", "bt601", 0);
    printf("   ch1_rgb_std=bt601: %s\n", ret < 0 ? "FAILED" : "OK");
    
    return true;
}

bool FfmpegVideoReader::initializeSwsContext() {
    // 确定输出像素格式
    AVPixelFormat dst_pix_fmt;
    if (output_bpp_ == 32) {
        dst_pix_fmt = AV_PIX_FMT_BGRA;  // ARGB888 (4 bytes)
    } else if (output_bpp_ == 24) {
        dst_pix_fmt = AV_PIX_FMT_BGR24; // RGB888 (3 bytes)
    } else {
        setError("Unsupported output bits per pixel");
        return false;
    }
    
    output_pixel_format_ = dst_pix_fmt;
    
    // 创建格式转换器
    sws_ctx_ = sws_getContext(
        codec_ctx_->width, codec_ctx_->height, codec_ctx_->pix_fmt,
        output_width_, output_height_, dst_pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!sws_ctx_) {
        setError("Failed to create SwsContext");
        return false;
    }
    
    return true;
}

bool FfmpegVideoReader::checkZeroCopySupport() {
    // 零拷贝条件：
    // 1. BufferPool 已设置
    // 2. 使用特殊硬件解码器（如 h264_taco）
    // 3. 解码器输出带物理地址的 AVFrame
    
    if (!buffer_pool_) {
        return false;  // 未设置 BufferPool
    }
    
    if (!decoder_name_ || strcmp(decoder_name_, "h264_taco") != 0) {
        return false;  // 非 h264_taco 解码器
    }
    
    // h264_taco 支持零拷贝
    printf("✅ Zero-copy mode enabled (h264_taco + BufferPool)\n");
    return true;
}

uint64_t FfmpegVideoReader::extractPhysicalAddress(AVFrame* frame) {
    if (!frame || !frame->metadata) {
        return 0;
    }
    
    // 从 metadata 中读取 pool_blk_id（参考 ids_test_video3）
    AVDictionaryEntry* entry = av_dict_get(frame->metadata, "pool_blk_id", nullptr, 0);
    if (!entry) {
        return 0;
    }
    
    // 解析 block_id
    uint32_t blk_id = (uint32_t)atoi(entry->value);
    if (blk_id == 0) {
        return 0;
    }
    
    // 使用 taco_sys 接口获取物理地址
    uint64_t phys_addr = taco_sys_handle2_phys_addr(blk_id);
    
    return phys_addr;
}

Buffer* FfmpegVideoReader::createZeroCopyBuffer(AVFrame* frame) {
    if (!frame) {
        return nullptr;
    }
    
    // 1. 提取物理地址
    uint64_t phys_addr = extractPhysicalAddress(frame);
    if (phys_addr == 0) {
        printf("⚠️  Warning: Failed to extract physical address from AVFrame\n");
        return nullptr;
    }
    
    // 2. 创建 Buffer（包装解码器内存）
    size_t buffer_size = frame->width * frame->height * (output_bpp_ / 8);
    
    Buffer* buffer = new Buffer(
        0,                          // id（由 BufferPool 管理）
        frame->data[0],             // 虚拟地址
        phys_addr,                  // 物理地址
        buffer_size,                // 大小
        Buffer::Ownership::EXTERNAL // 外部拥有（解码器拥有）
    );
    
    // 3. 设置 deleter（释放时回收 AVFrame）
    // 注意：AVFrame 需要持久化，不能在这里 unref
    // 由 BufferPool 的使用者负责在使用完毕后 unref
    
    return buffer;
}

int FfmpegVideoReader::estimateTotalFrames() {
    if (!format_ctx_ || video_stream_index_ < 0) {
        return -1;
    }
    
    AVStream* stream = format_ctx_->streams[video_stream_index_];
    
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
    if (format_ctx_->duration != AV_NOPTS_VALUE && stream->avg_frame_rate.num > 0) {
        double duration_sec = format_ctx_->duration / (double)AV_TIME_BASE;
        double fps = av_q2d(stream->avg_frame_rate);
        return (int)(duration_sec * fps);
    }
    
    return -1;  // 无法估算
}

// ============================================================================
// 读取帧（核心逻辑）
// ============================================================================

AVFrame* FfmpegVideoReader::decodeOneFrame() {
    if (!is_open_ || eof_reached_) {
        return nullptr;
    }
    
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    
    if (!packet || !frame) {
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        return nullptr;
    }
    
    // 读取并解码一帧
    while (true) {
        int ret = av_read_frame(format_ctx_, packet);
        
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                eof_reached_ = true;
            } else {
                setError("Failed to read frame", ret);
                decode_errors_++;
            }
            av_packet_free(&packet);
            av_frame_free(&frame);
            return nullptr;
        }
        
        // 只处理视频流
        if (packet->stream_index != video_stream_index_) {
            av_packet_unref(packet);
            continue;
        }
        
        // 发送数据包到解码器
        ret = avcodec_send_packet(codec_ctx_, packet);
        av_packet_unref(packet);
        
        if (ret < 0) {
            setError("Failed to send packet to decoder", ret);
            decode_errors_++;
            continue;
        }
        
        // 接收解码后的帧
        ret = avcodec_receive_frame(codec_ctx_, frame);
        
        if (ret == AVERROR(EAGAIN)) {
            // 需要更多数据
            continue;
        } else if (ret == AVERROR_EOF) {
            eof_reached_ = true;
            av_packet_free(&packet);
            av_frame_free(&frame);
            return nullptr;
        } else if (ret < 0) {
            setError("Failed to receive frame from decoder", ret);
            decode_errors_++;
            av_packet_free(&packet);
            av_frame_free(&frame);
            return nullptr;
        }
        
        // 解码成功
        decoded_frames_++;
        current_frame_index_++;
        av_packet_free(&packet);
        return frame;  // 调用者负责释放
    }
}

bool FfmpegVideoReader::readFrameTo(Buffer& dest_buffer) {
    return readFrameTo(dest_buffer.data(), dest_buffer.size());
}

bool FfmpegVideoReader::readFrameTo(void* dest_buffer, size_t buffer_size) {
    if (!is_open_) {
        setError("Reader is not open");
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 零拷贝模式不应该调用这个方法
    if (supports_zero_copy_) {
        setError("Zero-copy mode: use BufferPool injection instead");
        return false;
    }
    
    // 解码一帧
    AVFrame* frame = decodeOneFrame();
    if (!frame) {
        return false;
    }
    
    // 转换并拷贝到目标buffer
    bool success = convertFrameTo(frame, dest_buffer, buffer_size);
    
    // 释放 AVFrame
    av_frame_free(&frame);
    
    return success;
}

bool FfmpegVideoReader::convertFrameTo(AVFrame* src_frame, void* dest, size_t dest_size) {
    if (!src_frame || !dest || !sws_ctx_) {
        return false;
    }
    
    size_t expected_size = output_width_ * output_height_ * (output_bpp_ / 8);
    if (dest_size < expected_size) {
        setError("Destination buffer too small");
        return false;
    }
    
    // 准备目标缓冲区参数
    uint8_t* dst_data[1] = { (uint8_t*)dest };
    int dst_linesize[1] = { output_width_ * (output_bpp_ / 8) };
    
    // 执行格式转换
    int ret = sws_scale(
        sws_ctx_,
        src_frame->data, src_frame->linesize,
        0, src_frame->height,
        dst_data, dst_linesize
    );
    
    if (ret <= 0) {
        setError("sws_scale failed");
        return false;
    }
    
    return true;
}

bool FfmpegVideoReader::readFrameAt(int frame_index, Buffer& dest_buffer) {
    if (!seek(frame_index)) {
        return false;
    }
    return readFrameTo(dest_buffer);
}

bool FfmpegVideoReader::readFrameAt(int frame_index, void* dest_buffer, size_t buffer_size) {
    if (!seek(frame_index)) {
        return false;
    }
    return readFrameTo(dest_buffer, buffer_size);
}

bool FfmpegVideoReader::readFrameAtThreadSafe(int frame_index, void* dest_buffer, size_t buffer_size) const {
    // FfmpegVideoReader 不支持线程安全的随机访问
    // （因为 seek 会修改内部状态）
    (void)frame_index;
    (void)dest_buffer;
    (void)buffer_size;
    return false;
}

// ============================================================================
// 导航操作
// ============================================================================

bool FfmpegVideoReader::seek(int frame_index) {
    if (!is_open_) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (frame_index < 0) {
        frame_index = 0;
    }
    
    // 计算时间戳
    AVStream* stream = format_ctx_->streams[video_stream_index_];
    int64_t timestamp = av_rescale_q(
        frame_index,
        av_make_q(1, (int)av_q2d(stream->avg_frame_rate)),
        stream->time_base
    );
    
    // 执行 seek
    int ret = av_seek_frame(format_ctx_, video_stream_index_, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        setError("Seek failed", ret);
        return false;
    }
    
    // 刷新解码器缓冲区
    avcodec_flush_buffers(codec_ctx_);
    
    current_frame_index_ = frame_index;
    eof_reached_ = false;
    
    return true;
}

bool FfmpegVideoReader::seekToBegin() {
    return seek(0);
}

bool FfmpegVideoReader::seekToEnd() {
    if (total_frames_ > 0) {
        return seek(total_frames_ - 1);
    }
    return false;
}

bool FfmpegVideoReader::skip(int frame_count) {
    return seek(current_frame_index_ + frame_count);
}

// ============================================================================
// 信息查询
// ============================================================================

int FfmpegVideoReader::getTotalFrames() const {
    return total_frames_;
}

int FfmpegVideoReader::getCurrentFrameIndex() const {
    return current_frame_index_;
}

size_t FfmpegVideoReader::getFrameSize() const {
    return output_width_ * output_height_ * (output_bpp_ / 8);
}

long FfmpegVideoReader::getFileSize() const {
    if (!format_ctx_) {
        return -1;
    }
    
    // 尝试从格式上下文获取
    AVIOContext* io_ctx = format_ctx_->pb;
    if (io_ctx) {
        return avio_size(io_ctx);
    }
    
    return -1;
}

int FfmpegVideoReader::getWidth() const {
    return output_width_;
}

int FfmpegVideoReader::getHeight() const {
    return output_height_;
}

int FfmpegVideoReader::getBytesPerPixel() const {
    return output_bpp_ / 8;
}

const char* FfmpegVideoReader::getPath() const {
    return file_path_;
}

bool FfmpegVideoReader::hasMoreFrames() const {
    return !eof_reached_;
}

bool FfmpegVideoReader::isAtEnd() const {
    return eof_reached_;
}

const char* FfmpegVideoReader::getReaderType() const {
    return "FfmpegVideoReader";
}

// ============================================================================
// 零拷贝模式
// ============================================================================

void FfmpegVideoReader::setBufferPool(void* pool) {
    buffer_pool_ = static_cast<BufferPool*>(pool);
    
    // 重新检查零拷贝支持
    if (is_open_) {
        supports_zero_copy_ = checkZeroCopySupport();
    }
}

// ============================================================================
// 配置接口
// ============================================================================

void FfmpegVideoReader::setOutputResolution(int width, int height) {
    if (!is_open_) {
        output_width_ = width;
        output_height_ = height;
    }
}

void FfmpegVideoReader::setOutputBitsPerPixel(int bpp) {
    if (!is_open_) {
        output_bpp_ = bpp;
    }
}

void FfmpegVideoReader::setDecoderName(const char* decoder_name) {
    if (!is_open_) {
        decoder_name_ = decoder_name;
    }
}

void FfmpegVideoReader::setHardwareDecoder(bool enable) {
    if (!is_open_) {
        use_hardware_decoder_ = enable;
    }
}

// ============================================================================
// 辅助方法
// ============================================================================

void FfmpegVideoReader::setError(const std::string& error, int ffmpeg_error) {
    last_error_ = error;
    last_ffmpeg_error_ = ffmpeg_error;
    
    if (ffmpeg_error != 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ffmpeg_error, err_buf, sizeof(err_buf));
        printf("❌ FfmpegVideoReader Error: %s (FFmpeg: %s)\n", error.c_str(), err_buf);
    } else {
        printf("❌ FfmpegVideoReader Error: %s\n", error.c_str());
    }
}

std::string FfmpegVideoReader::getLastError() const {
    return last_error_;
}

const char* FfmpegVideoReader::getCodecName() const {
    if (codec_ctx_ && codec_ctx_->codec) {
        return codec_ctx_->codec->name;
    }
    return "unknown";
}

void FfmpegVideoReader::printStats() const {
    printf("\n📊 FfmpegVideoReader Statistics:\n");
    printf("   File: %s\n", file_path_);
    printf("   Codec: %s\n", getCodecName());
    printf("   Resolution: %dx%d → %dx%d\n", width_, height_, output_width_, output_height_);
    printf("   Total frames: %d\n", total_frames_);
    printf("   Current frame: %d\n", current_frame_index_);
    printf("   Decoded frames: %d\n", decoded_frames_.load());
    printf("   Decode errors: %d\n", decode_errors_.load());
    printf("   Zero-copy: %s\n", supports_zero_copy_ ? "YES" : "NO");
    printf("   EOF: %s\n", eof_reached_ ? "YES" : "NO");
}

void FfmpegVideoReader::printVideoInfo() const {
    if (!is_open_ || !format_ctx_ || video_stream_index_ < 0) {
        printf("⚠️  Video not open\n");
        return;
    }
    
    AVStream* stream = format_ctx_->streams[video_stream_index_];
    AVCodecParameters* codecpar = stream->codecpar;
    
    printf("\n📹 Video Information:\n");
    printf("   File: %s\n", file_path_);
    printf("   Format: %s\n", format_ctx_->iformat->long_name);
    printf("   Codec: %s\n", avcodec_get_name(codecpar->codec_id));
    printf("   Resolution: %dx%d\n", codecpar->width, codecpar->height);
    printf("   FPS: %.2f\n", av_q2d(stream->avg_frame_rate));
    printf("   Duration: %.2f seconds\n", stream->duration * av_q2d(stream->time_base));
    printf("   Bit rate: %lld kbps\n", codecpar->bit_rate / 1000);
    printf("   Pixel format: %s\n", av_get_pix_fmt_name((AVPixelFormat)codecpar->format));
}


