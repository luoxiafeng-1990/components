#include "../../include/videoFile/TacoH264DecoderReader.hpp"
#include "../../include/buffer/BufferPoolRegistry.hpp"
#include <sys/ioctl.h>
#include <unistd.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

// DMA ioctl 定义
#ifndef FB_IOCTL_SET_DMA_INFO
#define FB_IOCTL_SET_DMA_INFO _IOW('F', 7, struct tpsfb_dma_info)
#endif

TacoH264DecoderReader::TacoH264DecoderReader()
    : format_ctx_(nullptr)
    , video_stream_idx_(-1)
    , packet_(nullptr)
    , framebuffer_fd_(-1)
    , overlay_count_(4)
    , is_open_(false)
    , width_(0)
    , height_(0)
    , total_frames_(0)
    , fps_(0.0)
    , frame_size_(0)
    , file_size_(0)
    , current_frame_index_(0)
{}

TacoH264DecoderReader::~TacoH264DecoderReader() {
    close();
}

bool TacoH264DecoderReader::open(const char* path) {
    printf("📂 Opening H.264 file: %s\n", path);
    
    if (is_open_) {
        setError("File already open");
        return false;
    }
    
    file_path_ = path;
    
    // 1. 初始化 FFmpeg（打开文件）
    if (!initializeFFmpeg(path)) {
        return false;
    }
    
    // 2. 初始化 Decoder（创建 + 注册 BufferPool）
    if (!initializeDecoder()) {
        cleanupFFmpeg();
        return false;
    }
    
    is_open_ = true;
    
    printf("✅ TacoH264DecoderReader opened successfully\n");
    printf("   Video: %dx%d, %.2f fps, %d frames\n", 
           width_, height_, fps_, total_frames_);
    printf("   Overlay pool: '%s' (%d overlays)\n", 
           getBufferPoolName().c_str(), overlay_count_);
    
    return true;
}

bool TacoH264DecoderReader::openRaw(const char* path, int width, int height, int bits_per_pixel) {
    setError("TacoH264DecoderReader does not support raw files");
    return false;
}

void TacoH264DecoderReader::close() {
    if (!is_open_) {
        return;
    }
    
    printf("🔒 Closing TacoH264DecoderReader\n");
    
    cleanupFFmpeg();
    decoder_.reset();
    
    is_open_ = false;
}

bool TacoH264DecoderReader::isOpen() const {
    return is_open_;
}

std::string TacoH264DecoderReader::getBufferPoolName() const {
    if (decoder_) {
        return decoder_->getOverlayPoolName();
    }
    return "";
}

void* TacoH264DecoderReader::getOutputBufferPool() const {
    if (decoder_) {
        return decoder_->getOverlayPool();
    }
    return nullptr;
}

bool TacoH264DecoderReader::initializeFFmpeg(const char* path) {
    // 1. 打开文件
    format_ctx_ = avformat_alloc_context();
    if (avformat_open_input(&format_ctx_, path, nullptr, nullptr) != 0) {
        setError("Cannot open video file");
        return false;
    }
    
    // 2. 查找流信息
    if (avformat_find_stream_info(format_ctx_, nullptr) < 0) {
        setError("Cannot find stream info");
        return false;
    }
    
    // 3. 查找视频流
    video_stream_idx_ = -1;
    for (unsigned int i = 0; i < format_ctx_->nb_streams; i++) {
        if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx_ = i;
            break;
        }
    }
    
    if (video_stream_idx_ == -1) {
        setError("No video stream found");
        return false;
    }
    
    // 4. 获取视频参数
    AVStream* stream = format_ctx_->streams[video_stream_idx_];
    width_ = stream->codecpar->width;
    height_ = stream->codecpar->height;
    total_frames_ = stream->nb_frames;
    file_size_ = format_ctx_->pb ? format_ctx_->pb->pos : 0;
    
    if (stream->avg_frame_rate.num > 0) {
        fps_ = av_q2d(stream->avg_frame_rate);
    } else {
        fps_ = 30.0;  // 默认
    }
    
    // 假设 NV12 格式
    frame_size_ = width_ * height_ * 3 / 2;
    
    // 5. 分配 packet
    packet_ = av_packet_alloc();
    
    printf("✅ FFmpeg initialized:\n");
    printf("   Codec: %s\n", avcodec_get_name(stream->codecpar->codec_id));
    printf("   Size: %dx%d\n", width_, height_);
    printf("   FPS: %.2f\n", fps_);
    printf("   Frames: %d\n", total_frames_);
    
    return true;
}

bool TacoH264DecoderReader::initializeDecoder() {
    // 1. 创建 TacoHardwareDecoder
    decoder_ = std::make_unique<TacoHardwareDecoder>();
    
    // 2. 配置双通道（可选）
    decoder_->configureDualChannel(
        true,  // ch0_enable
        true,  // ch1_enable
        "argb888",  // ch1_rgb_format
        "bt601"     // ch1_rgb_std
    );
    
    // 3. 配置解码器
    AVStream* stream = format_ctx_->streams[video_stream_idx_];
    DecoderConfig config;
    config.codec_id = stream->codecpar->codec_id;
    config.codec_name = "h264_taco";  // 明确指定使用 h264_taco
    config.width = width_;
    config.height = height_;
    config.pix_fmt = AV_PIX_FMT_NV12;
    config.buffer_mode = BufferAllocationMode::INJECTION;
    
    // 复制 extradata（SPS/PPS）
    if (stream->codecpar->extradata_size > 0) {
        config.extradata = stream->codecpar->extradata;
        config.extradata_size = stream->codecpar->extradata_size;
    }
    
    // 4. 初始化 Decoder + 创建 overlay BufferPool
    DecoderStatus status = decoder_->initializeWithOverlayPool(config, overlay_count_);
    if (status != DecoderStatus::OK) {
        setError("Failed to initialize Taco decoder");
        return false;
    }
    
    printf("✅ Taco decoder initialized\n");
    printf("   Decoder registered BufferPool: '%s'\n", 
           decoder_->getOverlayPoolName().c_str());
    
    return true;
}

bool TacoH264DecoderReader::readFrame(int frame_index, Buffer* buffer) {
    if (!buffer) {
        setError("Invalid buffer");
        return false;
    }
    
    if (!is_open_) {
        setError("File not open");
        return false;
    }
    
    std::lock_guard<std::mutex> lock(read_mutex_);
    
    // buffer 是从 Decoder 的 overlay BufferPool 获取的
    uint32_t overlay_id = buffer->id();
    
    printf("🎬 Decoding frame %d to overlay %u\n", frame_index, overlay_id);
    
    // 1. 读取 AVPacket
    int ret = av_read_frame(format_ctx_, packet_);
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            setError("End of file");
        } else {
            setError("Read packet failed");
        }
        return false;
    }
    
    if (packet_->stream_index != video_stream_idx_) {
        av_packet_unref(packet_);
        return readFrame(frame_index, buffer);  // 跳过非视频包
    }
    
    // 2. 发送到解码器
    if (decoder_->sendPacket(packet_) != DecoderStatus::OK) {
        av_packet_unref(packet_);
        setError("Send packet failed");
        return false;
    }
    av_packet_unref(packet_);
    
    // 3. 解码到指定的 overlay（使用对应的 AVFrame）
    DecodedFrame decoded_frame;
    DecoderStatus status = decoder_->receiveFrameToOverlay(overlay_id, decoded_frame);
    
    if (status != DecoderStatus::OK) {
        if (status == DecoderStatus::NEED_MORE_DATA) {
            // 需要更多数据，递归读取
            return readFrame(frame_index, buffer);
        }
        setError("Receive frame failed");
        return false;
    }
    
    // 4. 提取物理地址
    uint64_t phys_addr = 0;
    if (!decoder_->extractPhysicalAddress(decoded_frame, phys_addr)) {
        setError("Extract physical address failed");
        return false;
    }
    
    // 5. 设置 DMA（将物理地址绑定到这个 overlay）
    if (!setupDMA(overlay_id, phys_addr)) {
        setError("Setup DMA failed");
        return false;
    }
    
    current_frame_index_++;
    
    printf("✅ Frame %d decoded to overlay %u (phys_addr=0x%lx)\n", 
           frame_index, overlay_id, phys_addr);
    
    // 注意：不需要保存 AVFrame 指针，Decoder 内部已经管理
    // 不需要调用 decoded_frame.release()，因为 AVFrame 由 Decoder 拥有
    
    return true;
}

bool TacoH264DecoderReader::setupDMA(uint32_t overlay_id, uint64_t phys_addr) {
    if (framebuffer_fd_ < 0) {
        printf("⚠️  Warning: framebuffer_fd not set, skipping DMA setup\n");
        return true;  // 不算错误，只是警告
    }
    
    struct tpsfb_dma_info dma_info;
    dma_info.ovl_idx = overlay_id;
    dma_info.phys_addr = phys_addr;
    
    int ret = ioctl(framebuffer_fd_, FB_IOCTL_SET_DMA_INFO, &dma_info);
    if (ret < 0) {
        printf("❌ ERROR: FB_IOCTL_SET_DMA_INFO failed (overlay=%u, ret=%d, errno=%d)\n", 
               overlay_id, ret, errno);
        return false;
    }
    
    printf("   ✅ DMA configured: overlay %u → phys_addr 0x%lx\n", overlay_id, phys_addr);
    
    return true;
}

void TacoH264DecoderReader::cleanupFFmpeg() {
    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }
    
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }
}

void TacoH264DecoderReader::setError(const char* error) {
    last_error_ = error;
    printf("❌ ERROR: %s\n", error);
}

// ============ 其他 IVideoReader 接口实现（简化版）============

bool TacoH264DecoderReader::readFrameTo(Buffer& dest_buffer) {
    return readFrame(current_frame_index_, &dest_buffer);
}

bool TacoH264DecoderReader::readFrameTo(void* dest_buffer, size_t buffer_size) {
    setError("readFrameTo(void*) not supported in zero-copy mode");
    return false;
}

bool TacoH264DecoderReader::readFrameAt(int frame_index, Buffer& dest_buffer) {
    return readFrame(frame_index, &dest_buffer);
}

bool TacoH264DecoderReader::readFrameAt(int frame_index, void* dest_buffer, size_t buffer_size) {
    setError("readFrameAt(void*) not supported in zero-copy mode");
    return false;
}

bool TacoH264DecoderReader::readFrameAtThreadSafe(int frame_index, void* dest_buffer, size_t buffer_size) const {
    // 零拷贝模式不支持这个接口
    return false;
}

bool TacoH264DecoderReader::seek(int frame_index) {
    // TODO: 实现 seek 功能
    setError("Seek not implemented yet");
    return false;
}

bool TacoH264DecoderReader::seekToBegin() {
    return seek(0);
}

bool TacoH264DecoderReader::seekToEnd() {
    return seek(total_frames_ - 1);
}

bool TacoH264DecoderReader::skip(int frame_count) {
    return seek(current_frame_index_ + frame_count);
}

int TacoH264DecoderReader::getTotalFrames() const {
    return total_frames_;
}

int TacoH264DecoderReader::getCurrentFrameIndex() const {
    return current_frame_index_;
}

size_t TacoH264DecoderReader::getFrameSize() const {
    return frame_size_;
}

long TacoH264DecoderReader::getFileSize() const {
    return file_size_;
}

int TacoH264DecoderReader::getWidth() const {
    return width_;
}

int TacoH264DecoderReader::getHeight() const {
    return height_;
}

int TacoH264DecoderReader::getBytesPerPixel() const {
    return 4;  // 假设 ARGB888
}

const char* TacoH264DecoderReader::getPath() const {
    return file_path_.c_str();
}

bool TacoH264DecoderReader::hasMoreFrames() const {
    return current_frame_index_ < total_frames_;
}

bool TacoH264DecoderReader::isAtEnd() const {
    return current_frame_index_ >= total_frames_;
}

