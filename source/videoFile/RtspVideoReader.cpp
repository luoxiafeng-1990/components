#include "../../include/videoFile/RtspVideoReader.hpp"
#include "../../include/buffer/BufferPool.hpp"
#include "../../include/buffer/BufferHandle.hpp"
#include <stdio.h>
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
}

// ============ 构造/析构 ============

RtspVideoReader::RtspVideoReader()
    : format_ctx_(nullptr)
    , codec_ctx_(nullptr)
    , sws_ctx_(nullptr)
    , video_stream_index_(-1)
    , width_(0)
    , height_(0)
    , output_pixel_format_(AV_PIX_FMT_BGRA)
    , running_(false)
    , connected_(false)
    , write_index_(0)
    , read_index_(0)
    , buffer_pool_(nullptr)
    , decoded_frames_(0)
    , dropped_frames_(0)
    , is_open_(false)
    , eof_reached_(false)
{
    rtsp_url_[0] = '\0';
    
    // 初始化内部缓冲区（30帧）
    internal_buffer_.resize(30);
    for (auto& slot : internal_buffer_) {
        slot.filled = false;
        slot.timestamp = 0;
    }
    
    printf("🎬 RtspVideoReader created\n");
}

RtspVideoReader::~RtspVideoReader() {
    printf("🧹 Destroying RtspVideoReader...\n");
    close();
}

// ============ IVideoReader 接口实现 ============

bool RtspVideoReader::open(const char* path) {
    printf("❌ ERROR: RTSP stream requires explicit format specification\n");
    printf("   Please use: openRaw(rtsp_url, width, height, bits_per_pixel)\n");
    return false;
}

bool RtspVideoReader::openRaw(const char* path, int width, int height, int bits_per_pixel) {
    if (is_open_) {
        printf("⚠️  Warning: Stream already open, closing previous stream\n");
        close();
    }
    
    strncpy(rtsp_url_, path, MAX_RTSP_PATH_LENGTH - 1);
    rtsp_url_[MAX_RTSP_PATH_LENGTH - 1] = '\0';
    
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
            printf("❌ ERROR: Unsupported bits_per_pixel: %d\n", bits_per_pixel);
            return false;
    }
    
    printf("\n📡 Opening RTSP stream: %s\n", rtsp_url_);
    printf("   Output resolution: %dx%d\n", width_, height_);
    printf("   Bits per pixel: %d\n", bits_per_pixel);
    printf("   Reader: RtspVideoReader (FFmpeg)\n");
    
    // 预分配内部缓冲区
    size_t frame_size = width_ * height_ * (bits_per_pixel / 8);
    for (auto& slot : internal_buffer_) {
        slot.data.resize(frame_size);
        slot.filled = false;
    }
    
    // 连接RTSP流
    if (!connectRTSP()) {
        return false;
    }
    
    // 启动解码线程
    running_ = true;
    decode_thread_ = std::thread(&RtspVideoReader::decodeThreadFunc, this);
    
    is_open_ = true;
    
    printf("✅ RTSP stream opened successfully\n");
    return true;
}

void RtspVideoReader::close() {
    if (!is_open_) {
        return;
    }
    
    printf("\n🛑 Closing RTSP stream...\n");
    
    // 停止解码线程
    running_ = false;
    buffer_cv_.notify_all();
    
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }
    
    // 断开RTSP连接
    disconnectRTSP();
    
    is_open_ = false;
    connected_ = false;
    
    printf("✅ RTSP stream closed\n");
    printf("   Decoded frames: %d\n", decoded_frames_.load());
    printf("   Dropped frames: %d\n", dropped_frames_.load());
}

bool RtspVideoReader::isOpen() const {
    return is_open_;
}

bool RtspVideoReader::readFrameTo(Buffer& dest_buffer) {
    return readFrameTo(dest_buffer.getVirtualAddress(), dest_buffer.size());
}

bool RtspVideoReader::readFrameTo(void* dest_buffer, size_t buffer_size) {
    // 如果处于零拷贝模式，这个接口不应该被使用
    if (buffer_pool_) {
        // 零拷贝模式：数据已经直接注入BufferPool
        // 这里返回true表示"操作成功"，但实际上不做任何事
        return true;
    }
    
    // 传统模式：从内部缓冲区拷贝
    return copyFromInternalBuffer(dest_buffer, buffer_size);
}

bool RtspVideoReader::readFrameAt(int frame_index, Buffer& dest_buffer) {
    // RTSP流不支持随机访问
    printf("⚠️  Warning: RTSP stream does not support random access (readFrameAt)\n");
    return readFrameTo(dest_buffer);
}

bool RtspVideoReader::readFrameAt(int frame_index, void* dest_buffer, size_t buffer_size) {
    // RTSP流不支持随机访问
    return readFrameTo(dest_buffer, buffer_size);
}

bool RtspVideoReader::readFrameAtThreadSafe(int frame_index, void* dest_buffer, size_t buffer_size) const {
    // RTSP流不支持随机访问，忽略frame_index
    return const_cast<RtspVideoReader*>(this)->readFrameTo(dest_buffer, buffer_size);
}

bool RtspVideoReader::seek(int frame_index) {
    printf("⚠️  Warning: RTSP stream does not support seeking\n");
    return false;
}

bool RtspVideoReader::seekToBegin() {
    printf("⚠️  Warning: RTSP stream does not support seeking\n");
    return false;
}

bool RtspVideoReader::seekToEnd() {
    printf("⚠️  Warning: RTSP stream does not support seeking\n");
    return false;
}

bool RtspVideoReader::skip(int frame_count) {
    printf("⚠️  Warning: RTSP stream does not support frame skipping\n");
    return false;
}

int RtspVideoReader::getTotalFrames() const {
    // RTSP 实时流是无限的，返回一个很大的值以适配 VideoProducer 的接口
    // 这样可以通过边界检查 (frame_index >= total_frames_)，同时不影响实际使用
    // 注意：RTSP 流并不依赖这个值，只是为了接口兼容性
    return INT_MAX;
}

int RtspVideoReader::getCurrentFrameIndex() const {
    // 返回已解码帧数作为"当前索引"
    return decoded_frames_.load();
}

size_t RtspVideoReader::getFrameSize() const {
    return width_ * height_ * getBytesPerPixel();
}

long RtspVideoReader::getFileSize() const {
    // RTSP流没有文件大小概念
    return -1;
}

int RtspVideoReader::getWidth() const {
    return width_;
}

int RtspVideoReader::getHeight() const {
    return height_;
}

int RtspVideoReader::getBytesPerPixel() const {
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

const char* RtspVideoReader::getPath() const {
    return rtsp_url_;
}

bool RtspVideoReader::hasMoreFrames() const {
    // 只要连接着且未到达EOF，就有更多帧
    return connected_.load() && !eof_reached_.load();
}

bool RtspVideoReader::isAtEnd() const {
    return eof_reached_.load();
}

const char* RtspVideoReader::getReaderType() const {
    return "RtspVideoReader";
}

void RtspVideoReader::setBufferPool(void* pool) {
    buffer_pool_ = reinterpret_cast<BufferPool*>(pool);
    if (buffer_pool_) {
        printf("🚀 RtspVideoReader: Zero-copy mode enabled\n");
    } else {
        printf("📦 RtspVideoReader: Traditional buffering mode\n");
    }
}

// ============ RTSP 特有接口 ============

std::string RtspVideoReader::getLastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void RtspVideoReader::printStats() const {
    printf("\n📊 RtspVideoReader Statistics:\n");
    printf("   Connected: %s\n", connected_.load() ? "Yes" : "No");
    printf("   Decoded frames: %d\n", decoded_frames_.load());
    printf("   Dropped frames: %d\n", dropped_frames_.load());
    printf("   Zero-copy mode: %s\n", buffer_pool_ ? "Enabled" : "Disabled");
}

// ============ 内部实现 ============

bool RtspVideoReader::connectRTSP() {
    // 1. 分配格式上下文
    format_ctx_ = avformat_alloc_context();
    if (!format_ctx_) {
        setError("Failed to allocate AVFormatContext");
        return false;
    }
    
    // 2. 设置RTSP选项（超时、传输协议等）
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);  // 使用TCP传输
    av_dict_set(&options, "stimeout", "5000000", 0);    // 5秒超时
    av_dict_set(&options, "max_delay", "500000", 0);    // 最大延迟0.5秒
    
    // 3. 打开RTSP流
    int ret = avformat_open_input(&format_ctx_, rtsp_url_, nullptr, &options);
    av_dict_free(&options);
    
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        setError(std::string("Failed to open RTSP stream: ") + errbuf);
        avformat_free_context(format_ctx_);
        format_ctx_ = nullptr;
        return false;
    }
    
    // 4. 获取流信息
    ret = avformat_find_stream_info(format_ctx_, nullptr);
    if (ret < 0) {
        setError("Failed to find stream information");
        avformat_close_input(&format_ctx_);
        return false;
    }
    
    // 5. 查找视频流
    video_stream_index_ = -1;
    for (unsigned int i = 0; i < format_ctx_->nb_streams; i++) {
        if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = i;
            break;
        }
    }
    
    if (video_stream_index_ == -1) {
        setError("No video stream found in RTSP source");
        avformat_close_input(&format_ctx_);
        return false;
    }
    
    // 6. 获取解码器
    AVCodecParameters* codecpar = format_ctx_->streams[video_stream_index_]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        setError("Codec not found");
        avformat_close_input(&format_ctx_);
        return false;
    }
    
    // 7. 分配解码器上下文
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        setError("Failed to allocate codec context");
        avformat_close_input(&format_ctx_);
        return false;
    }
    
    // 8. 复制编解码器参数
    ret = avcodec_parameters_to_context(codec_ctx_, codecpar);
    if (ret < 0) {
        setError("Failed to copy codec parameters");
        avcodec_free_context(&codec_ctx_);
        avformat_close_input(&format_ctx_);
        return false;
    }
    
    // 9. 打开解码器
    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        setError("Failed to open codec");
        avcodec_free_context(&codec_ctx_);
        avformat_close_input(&format_ctx_);
        return false;
    }
    
    // 10. 初始化格式转换上下文
    sws_ctx_ = sws_getContext(
        codec_ctx_->width, codec_ctx_->height, codec_ctx_->pix_fmt,
        width_, height_, (AVPixelFormat)output_pixel_format_,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!sws_ctx_) {
        setError("Failed to initialize SwsContext");
        avcodec_free_context(&codec_ctx_);
        avformat_close_input(&format_ctx_);
        return false;
    }
    
    connected_ = true;
    
    printf("✅ Connected to RTSP stream\n");
    printf("   Codec: %s\n", codec->long_name);
    printf("   Stream resolution: %dx%d\n", codec_ctx_->width, codec_ctx_->height);
    printf("   Output resolution: %dx%d\n", width_, height_);
    
    return true;
}

void RtspVideoReader::disconnectRTSP() {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }
    
    video_stream_index_ = -1;
    connected_ = false;
}

void RtspVideoReader::decodeThreadFunc() {
    printf("🚀 RTSP decode thread started\n");
    
    while (running_) {
        // 解码一帧
        AVFrame* frame = decodeOneFrame();
        if (!frame) {
            // 解码失败或超时
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        if (buffer_pool_) {
            // ✨ 零拷贝模式：直接注入BufferPool
            
            // 分配目标buffer（临时，用于转换）
            size_t frame_size = width_ * height_ * getBytesPerPixel();
            std::unique_ptr<uint8_t[]> temp_buffer(new uint8_t[frame_size]);
            
            // 转换格式到临时buffer
            uint8_t* dest_data[1] = { temp_buffer.get() };
            int dest_linesize[1] = { width_ * getBytesPerPixel() };
            
            sws_scale(sws_ctx_,
                     frame->data, frame->linesize, 0, frame->height,
                     dest_data, dest_linesize);
            
            // 包装为BufferHandle并注入
            auto handle = std::make_unique<BufferHandle>(
                temp_buffer.release(),  // 转移所有权
                0,  // 物理地址（暂时不可用）
                frame_size,
                [](void* ptr) {
                    // Deleter: 释放临时buffer
                    delete[] reinterpret_cast<uint8_t*>(ptr);
                }
            );
            
            buffer_pool_->injectFilledBuffer(std::move(handle));
            decoded_frames_++;
            
        } else {
            // 传统模式：存储到内部缓冲区
            storeToInternalBuffer(frame);
            decoded_frames_++;
        }
        
        // 释放AVFrame
        av_frame_free(&frame);
    }
    
    printf("🏁 RTSP decode thread finished\n");
}

AVFrame* RtspVideoReader::decodeOneFrame() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    
    if (!packet || !frame) {
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        return nullptr;
    }
    
    // 读取包
    int ret = av_read_frame(format_ctx_, packet);
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            eof_reached_ = true;
        }
        av_packet_free(&packet);
        av_frame_free(&frame);
        return nullptr;
    }
    
    // 只处理视频流的包
    if (packet->stream_index != video_stream_index_) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        return nullptr;
    }
    
    // 发送包到解码器
    ret = avcodec_send_packet(codec_ctx_, packet);
    av_packet_free(&packet);
    
    if (ret < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    
    // 接收解码后的帧
    ret = avcodec_receive_frame(codec_ctx_, frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    
    return frame;  // 调用者负责释放
}

void RtspVideoReader::storeToInternalBuffer(AVFrame* frame) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    FrameSlot& slot = internal_buffer_[write_index_];
    
    // 转换格式并存储
    uint8_t* dest_data[1] = { slot.data.data() };
    int dest_linesize[1] = { width_ * getBytesPerPixel() };
    
    sws_scale(sws_ctx_,
             frame->data, frame->linesize, 0, frame->height,
             dest_data, dest_linesize);
    
    slot.filled = true;
    slot.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    
    // 移动写入索引
    write_index_ = (write_index_ + 1) % internal_buffer_.size();
    
    // 如果缓冲区满了，丢弃最老的帧
    if (write_index_ == read_index_) {
        read_index_ = (read_index_ + 1) % internal_buffer_.size();
        dropped_frames_++;
    }
    
    buffer_cv_.notify_one();
}

bool RtspVideoReader::copyFromInternalBuffer(void* dest, size_t size) {
    std::unique_lock<std::mutex> lock(buffer_mutex_);
    
    // 等待有可用帧（最多等待100ms）
    auto timeout = std::chrono::milliseconds(100);
    if (!buffer_cv_.wait_for(lock, timeout, [this] {
        return internal_buffer_[read_index_].filled || !running_;
    })) {
        return false;  // 超时
    }
    
    if (!running_) {
        return false;  // 已停止
    }
    
    FrameSlot& slot = internal_buffer_[read_index_];
    if (!slot.filled) {
        return false;
    }
    
    // 拷贝数据
    size_t copy_size = std::min(size, slot.data.size());
    memcpy(dest, slot.data.data(), copy_size);
    
    // 标记为已消费
    slot.filled = false;
    read_index_ = (read_index_ + 1) % internal_buffer_.size();
    
    return true;
}

void RtspVideoReader::setError(const std::string& error) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = error;
    printf("❌ RtspVideoReader Error: %s\n", error.c_str());
}

uint64_t RtspVideoReader::getAVFramePhysicalAddress(AVFrame* frame) {
    // 对于软件解码的AVFrame，通常无法获取物理地址
    // 硬件解码器（如VAAPI、NVDEC）可能提供物理地址
    // 这里返回0表示不可用
    (void)frame;
    return 0;
}


