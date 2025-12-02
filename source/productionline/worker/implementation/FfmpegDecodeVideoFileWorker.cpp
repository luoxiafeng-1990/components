#include "productionline/worker/implementation/FfmpegDecodeVideoFileWorker.hpp"
#include "buffer/BufferPoolRegistry.hpp"
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

FfmpegDecodeVideoFileWorker::FfmpegDecodeVideoFileWorker()
    : WorkerBase(BufferAllocatorFactory::AllocatorType::AVFRAME)  // 🎯 只需传递类型！
    , format_ctx_ptr_(nullptr)
    , codec_ctx_ptr_(nullptr)
    , packet_ptr_(nullptr)          // 🎯 新增：packet 指针
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
    , eof_reached_(false)
    , zero_copy_buffer_pool_ptr_(nullptr)
    , use_hardware_decoder_(true)  // 默认启用硬件解码
    , decoder_name_ptr_("h264_taco")
    , codec_options_ptr_(nullptr)
    , decoded_frames_(0)
    , decode_errors_(0)
    , last_ffmpeg_error_(0)
{
    memset(file_path_, 0, sizeof(file_path_));
    // 🎯 父类已经创建好 AVFRAME 类型的 allocator_facade_，无需任何初始化代码
}

FfmpegDecodeVideoFileWorker::~FfmpegDecodeVideoFileWorker() {
    close();
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
    
    // 🎯 Worker职责：在open()时自动创建BufferPool（通过调用Allocator）
    // 计算帧大小（在openVideo()后，output_width_和output_height_已设置）
    size_t frame_size = output_width_ * output_height_ * output_bpp_ / 8;
    if (frame_size == 0) {
        setError("Invalid frame size, cannot create BufferPool");
        closeVideo();
        return false;
    }
    
    int buffer_count = 1;  // 默认创建4个Buffer
    
    // v2.0: allocatePoolWithBuffers 返回 pool_id
    buffer_pool_id_ = allocator_facade_.allocatePoolWithBuffers(
        buffer_count,
        frame_size,
        std::string("FfmpegDecodeVideoFileWorker_") + std::string(path),
        "Video"
    );
    
    if (buffer_pool_id_ == 0) {
        setError("Failed to create BufferPool via Allocator");
        closeVideo();
        return false;
    }
    
    // v2.0: 从 Registry 获取 Pool 名称（返回 weak_ptr）
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(buffer_pool_id_);
    auto pool = pool_weak.lock();
    std::string pool_name = pool ? pool->getName() : "Unknown";
    
    is_open_ = true;
    current_frame_index_ = 0;
    eof_reached_ = false;
    decoded_frames_ = 0;
    decode_errors_ = 0;
    
    printf("✅ FfmpegDecodeVideoFileWorker: Opened '%s'\n", path);
    printf("   Resolution: %dx%d → %dx%d\n", width_, height_, output_width_, output_height_);
    printf("   Codec: %s\n", codec_ctx_ptr_->codec->name);
    printf("   Total frames (estimated): %d\n", total_frames_);
    printf("   BufferPool: '%s' (ID: %lu, %d buffers, %zu bytes each)\n", 
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // v2.0: 主动清理 BufferPool
    if (buffer_pool_id_ != 0) {
        allocator_facade_.destroyPool(buffer_pool_id_);
        buffer_pool_id_ = 0;
    }
    
    closeVideo();
    is_open_ = false;
}

bool FfmpegDecodeVideoFileWorker::isOpen() const {
    return is_open_;
}

// ============================================================================
// 内部方法：打开视频
// ============================================================================

bool FfmpegDecodeVideoFileWorker::openVideo() {
    // 1. 打开输入文件
    format_ctx_ptr_ = avformat_alloc_context();
    if (!format_ctx_ptr_) {
        setError("Failed to allocate AVFormatContext");
        return false;
    }
    
    int ret = avformat_open_input(&format_ctx_ptr_, file_path_, nullptr, nullptr);
    if (ret < 0) {
        setError("Failed to open video file", ret);
        format_ctx_ptr_ = nullptr;
        return false;
    }
    
    // 2. 读取流信息
    ret = avformat_find_stream_info(format_ctx_ptr_, nullptr);
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
   
    // 8. 🎯 分配 AVPacket（用于 fillBuffer）
    packet_ptr_ = av_packet_alloc();
    if (!packet_ptr_) {
        setError("Failed to allocate AVPacket");
        closeVideo();
        return false;
    }
    
    return true;
}

void FfmpegDecodeVideoFileWorker::closeVideo() {
    // 释放 AVPacket
    if (packet_ptr_) {
        av_packet_free(&packet_ptr_);
        packet_ptr_ = nullptr;
    }
    
    // 释放格式转换器
    if (sws_ctx_ptr_) {
        sws_freeContext(sws_ctx_ptr_);
        sws_ctx_ptr_ = nullptr;
    }
    
    // 释放解码器
    if (codec_ctx_ptr_) {
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
    
    if (decoder_name_ptr_) {
        // 用户指定了解码器名称（如 "h264_taco"）
        codec = avcodec_find_decoder_by_name(decoder_name_ptr_);
        if (!codec) {
            printf("⚠️  Warning: Specified decoder '%s' not found, trying default\n", decoder_name_ptr_);
        } else {
            printf("✅ Using specified decoder: %s\n", decoder_name_ptr_);
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
    if (decoder_name_ptr_ && strcmp(decoder_name_ptr_, "h264_taco") == 0) {
        if (!configureSpecialDecoder()) {
            // 🔧 修复：配置失败是致命错误，必须返回
            printf("❌ ERROR: Failed to configure special decoder options\n");
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
    // 配置 h264_taco 解码器（参考 ids_test_video3）
    if (!codec_ctx_ptr_->priv_data) {
        printf("⚠️  Warning: codec_ctx->priv_data is NULL, cannot set options\n");
        // 🔧 修复：不要在这里释放，由调用者处理
        // avcodec_free_context(&codec_ctx_ptr_);  // ❌ 删除，由调用者处理
        return false;
    }
    
    printf("🔧 Configuring h264_taco decoder options...\n");
    
    int ret;
    
    // 禁用重排序
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "reorder_disable", 1, 0);
    printf("   reorder_disable=1: %s\n", ret < 0 ? "FAILED" : "OK");
    
    // 启用双通道（CH0: YUV, CH1: RGB）
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_enable", 1, 0);
    printf("   ch0_enable=1: %s\n", ret < 0 ? "FAILED" : "OK");
    
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_enable", 1, 0);
    printf("   ch1_enable=1: %s\n", ret < 0 ? "FAILED" : "OK");
    
    // 配置通道1（RGB输出）
    av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_x", 0, 0);
    av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_y", 0, 0);
    av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_width", 0, 0);
    av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_height", 0, 0);
    av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_scale_width", 0, 0);
    av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_scale_height", 0, 0);
    
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_rgb", 1, 0);
    printf("   ch1_rgb=1: %s\n", ret < 0 ? "FAILED" : "OK");
    
    // 设置RGB格式为ARGB888
    ret = av_opt_set(codec_ctx_ptr_->priv_data, "ch1_rgb_format", "argb888", 0);
    printf("   ch1_rgb_format=argb888: %s\n", ret < 0 ? "FAILED" : "OK");
    
    // 设置颜色标准为BT.601
    ret = av_opt_set(codec_ctx_ptr_->priv_data, "ch1_rgb_std", "bt601", 0);
    printf("   ch1_rgb_std=bt601: %s\n", ret < 0 ? "FAILED" : "OK");
    
    return true;
}

bool FfmpegDecodeVideoFileWorker::initializeSwsContext() {
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
    sws_ctx_ptr_ = sws_getContext(
        codec_ctx_ptr_->width, codec_ctx_ptr_->height, codec_ctx_ptr_->pix_fmt,
        output_width_, output_height_, dst_pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!sws_ctx_ptr_) {
        setError("Failed to create SwsContext");
        return false;
    }
    
    return true;
}


uint64_t FfmpegDecodeVideoFileWorker::extractPhysicalAddress(AVFrame* frame) {
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

Buffer* FfmpegDecodeVideoFileWorker::createZeroCopyBuffer(AVFrame* frame) {
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

bool FfmpegDecodeVideoFileWorker::convertFrameTo(AVFrame* src_frame, void* dest, size_t dest_size) {
    if (!src_frame || !dest || !sws_ctx_ptr_) {
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
        sws_ctx_ptr_,
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


// ============================================================================
// 导航操作
// ============================================================================

bool FfmpegDecodeVideoFileWorker::seek(int frame_index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    closeVideo();
    openVideo();
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
    return file_path_;
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
        printf("❌ ERROR: buffer is nullptr\n");
        return false;
    }
    
    if (!is_open_) {
        printf("❌ ERROR: Worker is not open\n");
        return false;
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 步骤1: 从 Buffer 获取预分配的 AVFrame*
    AVFrame* frame_ptr = (AVFrame*)buffer->getVirtualAddress();
    if (!frame_ptr) {
        printf("❌ ERROR: buffer->getVirtualAddress() is nullptr\n");
        return false;
    }
    
    // 步骤2: 读取一个 packet（参考 ids_test_video3:2240）
    int read_ret = av_read_frame(format_ctx_ptr_, packet_ptr_);
    
    if (read_ret < 0) {
        if (read_ret == AVERROR_EOF) {
            printf("🔄 EOF reached, restarting...\n");
            // 在 seek 前先清理 packet 状态
            av_packet_unref(packet_ptr_);
            // 重新 seek 到开头
            if (!seek(0)) {
                printf("❌ ERROR: seek to begin failed\n");
                return false;
            }
            eof_reached_ = true;
            return false;
        } else {
            printf("❌ ERROR: av_read_frame failed: %d\n", read_ret);
            return false;
        }
    }
    
    // 步骤3: 检查是否是视频流
    if (packet_ptr_->stream_index != video_stream_index_) {
        // 🔧 修复：不是视频流的packet需要释放，然后继续读取下一个
        av_packet_unref(packet_ptr_);
        return false;  // 让调用者再次调用以读取下一个packet
    }
    
    // 步骤4: 发送 packet 到解码器（参考 ids_test_video3:2270）
    int ret = avcodec_send_packet(codec_ctx_ptr_, packet_ptr_);
    
    // 🔧 修复：无论成功与否，都要释放packet引用
    // avcodec_send_packet 会复制数据，不再需要原始packet
    av_packet_unref(packet_ptr_);
    
    if (ret < 0) {
        printf("❌ ERROR: avcodec_send_packet failed: %d\n", ret);
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
            printf("⚠️  Warning: Failed to extract physical address\n");
            return false;
        }
        
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
// 配置接口
// ============================================================================

void FfmpegDecodeVideoFileWorker::setOutputResolution(int width, int height) {
    if (!is_open_) {
        output_width_ = width;
        output_height_ = height;
    }
}

void FfmpegDecodeVideoFileWorker::setOutputBitsPerPixel(int bpp) {
    if (!is_open_) {
        output_bpp_ = bpp;
    }
}

void FfmpegDecodeVideoFileWorker::setDecoderName(const char* decoder_name) {
    if (!is_open_) {
        decoder_name_ptr_ = decoder_name;
    }
}

void FfmpegDecodeVideoFileWorker::setHardwareDecoder(bool enable) {
    if (!is_open_) {
        use_hardware_decoder_ = enable;
    }
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
        printf("❌ FfmpegDecodeVideoFileWorker Error: %s (FFmpeg: %s)\n", error.c_str(), err_buf);
    } else {
        printf("❌ FfmpegDecodeVideoFileWorker Error: %s\n", error.c_str());
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
    printf("\n📊 FfmpegDecodeVideoFileWorker Statistics:\n");
    printf("   File: %s\n", file_path_);
    printf("   Codec: %s\n", getCodecName());
    printf("   Resolution: %dx%d → %dx%d\n", width_, height_, output_width_, output_height_);
    printf("   Total frames: %d\n", total_frames_);
    printf("   Current frame: %d\n", current_frame_index_);
    printf("   Decoded frames: %d\n", decoded_frames_.load());
    printf("   Decode errors: %d\n", decode_errors_.load());
    printf("   EOF: %s\n", eof_reached_ ? "YES" : "NO");
}

void FfmpegDecodeVideoFileWorker::printVideoInfo() const {
    if (!is_open_ || !format_ctx_ptr_ || video_stream_index_ < 0) {
        printf("⚠️  Video not open\n");
        return;
    }
    
    AVStream* stream = format_ctx_ptr_->streams[video_stream_index_];
    AVCodecParameters* codecpar = stream->codecpar;
    
    printf("\n📹 Video Information:\n");
    printf("   File: %s\n", file_path_);
    printf("   Format: %s\n", format_ctx_ptr_->iformat->long_name);
    printf("   Codec: %s\n", avcodec_get_name(codecpar->codec_id));
    printf("   Resolution: %dx%d\n", codecpar->width, codecpar->height);
    printf("   FPS: %.2f\n", av_q2d(stream->avg_frame_rate));
    printf("   Duration: %.2f seconds\n", stream->duration * av_q2d(stream->time_base));
    printf("   Bit rate: %ld kbps\n", (long)(codecpar->bit_rate / 1000));
    printf("   Pixel format: %s\n", av_get_pix_fmt_name((AVPixelFormat)codecpar->format));
}


