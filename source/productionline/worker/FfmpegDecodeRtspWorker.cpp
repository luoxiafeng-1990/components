#include "productionline/worker/FfmpegDecodeRtspWorker.hpp"
#include "productionline/worker/RtspPacketSource.hpp"
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
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include "taco_sys_api.h"
}

// ============ 构造/析构 ============

// 构造函数（v2.12修改：必须通过配置创建，与 FfmpegDecodeVideoFileWorker 保持一致）
FfmpegDecodeRtspWorker::FfmpegDecodeRtspWorker(const WorkerConfig& config)
    : WorkerBase(BufferAllocatorFactory::AllocatorType::AVFRAME, config)  // 传递 config 给父类
    , packet_source_(nullptr)  // ⚠️ 数据源将在 open() 时根据 RTSP URL 创建
    , codec_ctx_ptr_(nullptr)
    , output_width_(0)
    , output_height_(0)
    , use_hardware_decoder_(config.decoder.enable_hardware)  // 🎯 从配置读取
    , decoder_name_(config.decoder.name.value_or(""))  // 🎯 从配置读取（使用 optional 的 value_or）
    , codec_options_ptr_(nullptr)
    , decoded_frames_(0)
    , dropped_frames_(0)
{
    LOG_DEBUG("[Worker] FfmpegDecodeRtspWorker created with config");
}

FfmpegDecodeRtspWorker::~FfmpegDecodeRtspWorker() {
    LOG_DEBUG("🧹 Destroying FfmpegDecodeRtspWorker...");
    close();
}

// ============ IVideoReader 接口实现 ============

bool FfmpegDecodeRtspWorker::open(const char* path) {
    open();
    return true;
}

bool FfmpegDecodeRtspWorker::open() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 如果已经打开，先关闭
    if (packet_source_ && packet_source_->isOpen()) {
        LOG_WARN("[Worker] ⚠️  Stream already open, closing previous stream");
        close();
    }
    
    // ✅ 从 worker_config_ 读取所有参数
    const char* rtsp_url = worker_config_.data_source.path.c_str();
    int width = worker_config_.display.width;
    int height = worker_config_.display.height;
    int bits_per_pixel = worker_config_.display.bits_per_pixel;
    
    // 验证参数
    if (!rtsp_url || strlen(rtsp_url) == 0) {
        setError("RTSP URL not configured in worker_config_.data_source.path");
        return false;
    }
    
    if (width == 0 || height == 0 || bits_per_pixel == 0) {
        setError("Display resolution and bits_per_pixel must be configured for RTSP stream");
        LOG_ERROR_FMT("[Worker] ❌ Current config: %dx%d@%dbpp", width, height, bits_per_pixel);
        LOG_ERROR("[Worker]    Please set worker_config_.display.width/height/bits_per_pixel");
        return false;
    }
    
    // 保存输出参数（运行时状态）
    output_width_ = width;
    output_height_ = height;
    
    LOG_INFO("");
    LOG_INFO_FMT("📡 Opening RTSP stream: %s", rtsp_url);
    LOG_INFO_FMT("   Output resolution: %dx%d@%dbpp", width, height, bits_per_pixel);
    
    // ⭐ 创建 RTSP 数据源
    packet_source_ = std::make_unique<RtspPacketSource>(std::string(rtsp_url));
    LOG_DEBUG_FMT("[Worker] Created RtspPacketSource for '%s'", rtsp_url);
    
    // 1. 打开数据源
    if (!packet_source_->open()) {
        setError("Failed to open RTSP packet source");
        return false;
    }
    
    // 2. 从数据源获取编解码器参数
    const AVCodecParameters* codecpar = packet_source_->getCodecParameters();
    if (!codecpar) {
        setError("Failed to get codec parameters from packet source");
        packet_source_->close();
        return false;
    }
    
    // 3. 检查编解码器类型是否匹配（v2.11）
    checkCodecMismatch(codecpar->codec_id, decoder_name_);
    
    // 4. 初始化解码器（使用从数据源获取的 codec_params）
    if (!initializeDecoder(codecpar)) {
        packet_source_->close();
        return false;
    }
    
    // 5. 🎯 Worker职责：在open()时自动创建BufferPool（通过调用Allocator）
    // 计算帧大小
    size_t frame_size = output_width_ * output_height_ * (bits_per_pixel / 8);
    if (frame_size == 0) {
        setError("Invalid frame size, cannot create BufferPool");
        packet_source_->close();
        return false;
    }
    
    // v2.0: allocatePoolWithBuffers 返回 pool_id
    uint64_t pool_id = allocator_facade_.allocatePoolWithBuffers(
        worker_config_.data_source.buffer_count,
        frame_size,
        std::string("FfmpegDecodeRtspWorker_") + std::string(rtsp_url),
        "RTSP"
    );
    
    if (pool_id == 0) {
        setError("Failed to create BufferPool via Allocator");
        packet_source_->close();
        return false;
    }
    
    // v2.0 新设计：注册为主视频解码输出
    if (!registerBufferPool(BufferPoolType::DECODE_VIDEO_PRIMARY, pool_id)) {
        setError("Failed to register BufferPool");
        packet_source_->close();
        return false;
    }
    
    // v2.0: 从 Registry 获取 Pool 名称（返回 weak_ptr）
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    std::string pool_name = pool ? pool->getName() : "Unknown";
    
    decoded_frames_ = 0;
    dropped_frames_ = 0;
    
    LOG_DEBUG("[Worker] RTSP stream opened successfully");
    LOG_DEBUG_FMT("[Worker]    Resolution: %dx%d", output_width_, output_height_);
    LOG_DEBUG_FMT("[Worker]    Codec: %s", codec_ctx_ptr_->codec->name);
    LOG_DEBUG_FMT("[Worker]    BufferPool: '%s' (ID: %lu, %d buffers, %zu bytes each)", 
           pool_name.c_str(), pool_id, worker_config_.data_source.buffer_count, frame_size);
    
    return true;
}

void FfmpegDecodeRtspWorker::close() {
    // ⚠️ 注意：打开状态由数据源管理
    if (!packet_source_ || !packet_source_->isOpen()) {
        return;  // 已经关闭过了
    }
    
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        LOG_INFO("");
        LOG_INFO("🛑 Closing RTSP stream...");
        
        // ⭐ v2.12新增：关闭数据源
        if (packet_source_) {
            packet_source_->close();
        }
        
        // 释放解码器
        if (codec_ctx_ptr_) {
            avcodec_free_context(&codec_ctx_ptr_);
            codec_ctx_ptr_ = nullptr;
        }
        
        // 释放解码器选项
        if (codec_options_ptr_) {
            av_dict_free(&codec_options_ptr_);
            codec_options_ptr_ = nullptr;
        }
        
        // ⭐ 清除所有 BufferPool 注册（标记不再使用）
        clearAllBufferPools();
    }
    
    LOG_DEBUG("[Worker] RTSP stream closed");
    LOG_INFO_FMT("   Decoded frames: %d", decoded_frames_.load());
    LOG_INFO_FMT("   Dropped frames: %d", dropped_frames_.load());
}

bool FfmpegDecodeRtspWorker::isOpen() const {
    // ⚠️ 注意：打开状态从数据源获取
    if (!packet_source_) {
        return false;
    }
    return packet_source_->isOpen();
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
    // ⭐ v2.12修改：从数据源获取（适配器模式）
    if (packet_source_) {
        return packet_source_->getTotalFrames();
    }
    return INT_MAX;
}

int FfmpegDecodeRtspWorker::getCurrentFrameIndex() const {
    // 返回已解码帧数作为"当前索引"
    return decoded_frames_.load();
}

size_t FfmpegDecodeRtspWorker::getFrameSize() const {
    // ✅ 使用实际解码输出格式计算（getBytesPerPixel从实际格式获取）
    return (size_t)(output_width_ * output_height_ * getBytesPerPixel());
}

long FfmpegDecodeRtspWorker::getFileSize() const {
    // ⭐ v2.12修改：从数据源获取
    if (packet_source_) {
        return packet_source_->getFileSize();
    }
    return -1;
}

int FfmpegDecodeRtspWorker::getWidth() const {
    return output_width_;
}

int FfmpegDecodeRtspWorker::getHeight() const {
    return output_height_;
}

double FfmpegDecodeRtspWorker::getBytesPerPixel() const {
    // 1️⃣ 优先：从解码器实际输出格式计算（最准确）
    if (codec_ctx_ptr_ && codec_ctx_ptr_->pix_fmt != AV_PIX_FMT_NONE) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(codec_ctx_ptr_->pix_fmt);
        if (desc) {
            int bits_per_pixel = av_get_bits_per_pixel(desc);
            return bits_per_pixel / 8.0;  // 返回浮点数，支持1.5字节等
        }
    }
    
    // 2️⃣ Fallback：从 worker_config_.decoder.taco 的格式枚举推断（⭐ v2.17）
    if (worker_config_.decoder.taco.ch1_rgb) {
        // RGB 模式：根据 ch1_rgb_format 整型枚举推断
        int rgb_fmt = worker_config_.decoder.taco.ch1_rgb_format;
        
        // RGB 8-bit 有 Alpha 通道（4 字节/像素）
        if (rgb_fmt == 9 || rgb_fmt == 10 || rgb_fmt == 11 || rgb_fmt == 12 ||  // argb888/abgr888/bgra888/rgba888
            rgb_fmt == 21 || rgb_fmt == 22) {  // xrgb888/xbgr888
            return 4.0;
        }
        // RGB 8-bit 无 Alpha 通道（3 字节/像素）
        else if (rgb_fmt == 1 || rgb_fmt == 3) {  // rgb888/bgr888
            return 3.0;
        }
        // RGB 16-bit（6 字节/像素）
        else if (rgb_fmt == 2 || rgb_fmt == 4) {  // r16g16b16/b16g16r16
            return 6.0;
        }
        // 默认 ARGB888（4 字节/像素）
        return 4.0;
    } else {
        // YUV 模式：格式由解码器自动决定，无配置字段
        // 默认假设 YUV420（最常见，1.5 字节/像素）
        LOG_WARN("[Worker] getBytesPerPixel() fallback: assuming YUV420 (1.5 bytes/pixel)");
        return 1.5;
    }
}

const char* FfmpegDecodeRtspWorker::getPath() const {
    // ⭐ v2.12修改：从数据源获取
    if (!packet_source_) {
        return nullptr;
    }
    
    // 返回 RTSP URL
    static thread_local std::string cached_path;
    cached_path = packet_source_->getFilePath();
    return cached_path.empty() ? nullptr : cached_path.c_str();
}

bool FfmpegDecodeRtspWorker::hasMoreFrames() const {
    // ⭐ v2.12修改：从数据源获取 EOF 状态
    if (!packet_source_) {
        return false;
    }
    return !packet_source_->isEof();
}

bool FfmpegDecodeRtspWorker::isAtEnd() const {
    // ⭐ v2.12修改：从数据源获取 EOF 状态
    if (!packet_source_) {
        return true;
    }
    return packet_source_->isEof();
}

bool FfmpegDecodeRtspWorker::isConnected() const {
    // 连接状态从数据源判断
    if (!packet_source_) {
        return false;
    }
    return packet_source_->isOpen();
}

// ============================================================================
// 核心功能：填充Buffer
// ============================================================================

bool FfmpegDecodeRtspWorker::fillBuffer(int frame_index, Buffer* buffer) {
    if (!buffer) {
        LOG_ERROR("[Worker] ERROR: buffer is nullptr");
        return false;
    }
    
    if (!packet_source_->isOpen()) {
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
    
    // 步骤1.1: ⭐ v2.8新增：从 Buffer 获取关联的 AVPacket*
    AVPacket* packet_ptr = buffer->getAVPacket();
    if (!packet_ptr) {
        LOG_ERROR("[Worker] ERROR: buffer->getAVPacket() is nullptr");
        return false;
    }
    
    // ⭐ v2.12新增：使用数据源抽象读取 packet
    if (!packet_source_) {
        LOG_ERROR("[Worker] ERROR: packet_source_ is nullptr");
        return false;
    }
    
    // 步骤2: 从数据源读取 packet（数据源已处理重试和流过滤）
    int read_ret = packet_source_->readPacket(packet_ptr);
    
    if (read_ret < 0) {
        if (read_ret == AVERROR_EOF) {
            LOG_DEBUG("🔄 RTSP EOF reached");
            av_packet_unref(packet_ptr);
            return false;
        } else {
            // 其他错误：记录错误并返回
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(read_ret, err_buf, sizeof(err_buf));
            LOG_ERROR_FMT("[Worker] ERROR: readPacket failed: %d (%s)", read_ret, err_buf);
            av_packet_unref(packet_ptr);
            return false;
        }
    }
    
    // 步骤3: 数据源已经过滤了非视频流，直接使用packet
    
    // 步骤4: 发送 packet 到解码器（参考 FfmpegDecodeVideoFileWorker）
    int ret = avcodec_send_packet(codec_ctx_ptr_, packet_ptr);
    
    // 🔧 修复：无论成功与否，都要释放packet引用
    // avcodec_send_packet 会复制数据，不再需要原始packet
    av_packet_unref(packet_ptr);
    
    if (ret < 0) {
        LOG_ERROR_FMT("[Worker] ERROR: avcodec_send_packet failed: %d", ret);
        return false;
    }
    
    bool recv_frm = false;

    // 🎯 在循环前创建临时 AVFrame（只创建一次）
    AVFrame* temp_frame = av_frame_alloc();
    if (!temp_frame) {
        LOG_ERROR("[Worker] ERROR: Failed to allocate temporary AVFrame");
        return false;
    }

    // 步骤5: 🎯 循环调用 receive_frame，直到成功或需要更多数据
    while (true) {
        // 🎯 使用临时 AVFrame 进行解码
        ret = avcodec_receive_frame(codec_ctx_ptr_, temp_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0) {
            break;
        }

        // ✅ 成功解码到临时 AVFrame！

        // 🎯 使用 move 操作将数据转移到 buffer 的 AVFrame
        av_frame_move_ref(frame_ptr, temp_frame);

        // ⭐ v2.13新增：通过虚函数提取硬件物理地址（与 FileWorker 保持一致）
        if (!decoder_name_.empty()) {
            // 使用了硬件解码器，尝试提取物理地址
            if (!extractHardwareAddressFromMetadata(frame_ptr, buffer)) {
                // ❌ 硬件解码时提取失败是错误
                LOG_ERROR_FMT("[Worker] Hardware decoder '%s': Failed to extract physical address",
                             decoder_name_.c_str());
                av_frame_free(&temp_frame);  // 🔧 清理临时 AVFrame
                return false;
            }
        }

        // ⭐ v2.7改进：先更新虚拟地址为实际数据地址（frame->data[0]）
        buffer->setVirtualAddress(frame_ptr->data[0]);

        // ⭐ v2.6新增：从AVFrame设置图像元数据到Buffer
        buffer->setImageMetadataFromAVFrame(frame_ptr);
        decoded_frames_++;
        recv_frm = true;

        // 🎯 处理完一帧后立即退出循环，避免覆盖
        break;
    }

    // 🔧 在循环结束后清理临时 AVFrame
    av_frame_free(&temp_frame);
    return recv_frm;
}

// ============================================================================
// 提供原材料（BufferPool）
// ============================================================================

// ============ RTSP 特有接口 ============

std::string FfmpegDecodeRtspWorker::getLastError() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return last_error_;
}

const AVCodecParameters* FfmpegDecodeRtspWorker::getCodecParameters() const {
    if (!packet_source_) {
        return nullptr;
    }
    return packet_source_->getCodecParameters();
}

AVRational FfmpegDecodeRtspWorker::getTimeBase() const {
    if (!packet_source_) {
        return {1, 25};  // 默认值
    }
    
    // 从数据源获取编解码器参数
    const AVCodecParameters* codecpar = packet_source_->getCodecParameters();
    if (!codecpar) {
        return {1, 25};  // 默认值
    }
    
    // 对于 RTSP 流，通常使用帧率的倒数作为时间基
    // 这里返回一个通用的时间基（可以根据实际需求调整）
    return {1, 25};  // 默认25fps
}

void FfmpegDecodeRtspWorker::printStats() const {
    LOG_INFO("");
    LOG_INFO("📊 FfmpegDecodeRtspWorker Statistics:");
    // ⭐ v2.12修改：从数据源获取 RTSP URL
    std::string rtsp_url = packet_source_ ? packet_source_->getFilePath() : std::string();
    LOG_INFO_FMT("   RTSP URL: %s", rtsp_url.empty() ? "(Not Set)" : rtsp_url.c_str());
    LOG_INFO_FMT("   Connected: %s", isConnected() ? "Yes" : "No");
    LOG_INFO_FMT("   Decoded frames: %d", decoded_frames_.load());
    LOG_INFO_FMT("   Dropped frames: %d", dropped_frames_.load());
    
    uint64_t pool_id = getOutputBufferPoolId(BufferPoolType::DECODE_VIDEO_PRIMARY);
    LOG_INFO_FMT("   BufferPool ID: %lu", pool_id);
}

// ============ 内部实现 ============

bool FfmpegDecodeRtspWorker::initializeDecoder(const AVCodecParameters* codec_params) {
    // ⭐ v2.12修改：codec_params 必须提供（从 packet_source_ 获取）
    if (!codec_params) {
        setError("Cannot initialize decoder: codec_params is nullptr");
        return false;
    }
    const AVCodecParameters* codecpar = codec_params;
    
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
    
    LOG_DEBUG("[Worker] Initialized decoder");
    LOG_INFO_FMT("   Codec: %s", codec_ctx_ptr_->codec->name);
    LOG_INFO_FMT("   Stream resolution: %dx%d", codec_ctx_ptr_->width, codec_ctx_ptr_->height);
    LOG_INFO_FMT("   Output resolution: %dx%d", output_width_, output_height_);
    
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
    
    // ========== 通道0配置 ==========
    
    // 配置通道0裁剪参数（从 config 读取）
    if (taco.ch0_crop_width > 0 && taco.ch0_crop_height > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_x", taco.ch0_crop_x, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_y", taco.ch0_crop_y, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_width", taco.ch0_crop_width, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_height", taco.ch0_crop_height, 0);
        LOG_DEBUG_FMT("[Worker]    ch0_crop: (%d, %d, %d, %d)", 
               taco.ch0_crop_x, taco.ch0_crop_y, 
               taco.ch0_crop_width, taco.ch0_crop_height);
    }
    
    // 配置通道0缩放参数（从 config 读取）
    if (taco.ch0_scale_width > 0 && taco.ch0_scale_height > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_scale_width", taco.ch0_scale_width, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_scale_height", taco.ch0_scale_height, 0);
        LOG_DEBUG_FMT("[Worker]    ch0_scale: (%d, %d)", taco.ch0_scale_width, taco.ch0_scale_height);
    }
    
    // ========== 通道1配置 ==========
    
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
    
    // ⭐ v2.17: 设置 RGB 格式（使用整型枚举）
    if (taco.ch1_rgb && taco.ch1_rgb_format > 0) {
        ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_rgb_format", 
                             taco.ch1_rgb_format, 0);
        LOG_DEBUG_FMT("[Worker]    ch1_rgb_format=%d: %s", taco.ch1_rgb_format, 
               ret < 0 ? "FAILED" : "OK");
    }
    
    // ⭐ v2.17: 设置颜色标准（使用整型枚举）
    if (taco.ch1_rgb && taco.ch1_rgb_std > 0) {
        ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_rgb_std", 
                             taco.ch1_rgb_std, 0);
        LOG_DEBUG_FMT("[Worker]    ch1_rgb_std=%d: %s", taco.ch1_rgb_std, 
               ret < 0 ? "FAILED" : "OK");
    }
    
    return true;
}

void FfmpegDecodeRtspWorker::setError(const std::string& error, int ffmpeg_error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    last_error_ = error;
    
    if (ffmpeg_error != 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ffmpeg_error, err_buf, sizeof(err_buf));
        LOG_ERROR_FMT("[Worker] FfmpegDecodeRtspWorker Error: %s (FFmpeg: %s)", error.c_str(), err_buf);
    } else {
        LOG_ERROR_FMT("[Worker] FfmpegDecodeRtspWorker Error: %s", error.c_str());
    }
}

// ============================================================================
// 硬件解码器元数据提取（重写基类虚函数）
// ============================================================================

bool FfmpegDecodeRtspWorker::extractHardwareAddressFromMetadata(AVFrame* frame, Buffer* buffer) {
    // ⭐ 职责：从 AVFrame 中提取硬件解码器的物理内存地址
    // 
    // 设计原则：
    // 1. 此函数只在使用硬件解码器时调用（decoder_name_ 非空）
    // 2. 不同硬件解码器有不同的提取方式
    // 3. 提取失败返回 false，调用者会报错并终止解码
    
    if (!frame || !buffer) {
        LOG_ERROR("[Worker] extractHardwareAddressFromMetadata: Invalid parameters");
        return false;
    }
    
    // ========== h264_taco 硬件解码器 ==========
    if (decoder_name_ == "h264_taco") {
        // TACO 特定逻辑：从 metadata 中提取 pool_blk_id，转换为物理地址
        uint64_t phys_addr = 0;
        uint32_t blk_id = 0;
        
        if (frame->metadata) {
            AVDictionaryEntry* entry = av_dict_get(frame->metadata, "pool_blk_id", NULL, 0);
            if (entry) {
                blk_id = (uint32_t)atoi(entry->value);
                phys_addr = taco_sys_handle2_phys_addr(blk_id);
                
                if (phys_addr != 0) {
                    // ✅ 成功提取物理地址
                    buffer->setPhysicalAddress(phys_addr);
                    return true;
                } else {
                    // ❌ blk_id 有效，但转换失败
                    LOG_ERROR_FMT("[Worker] TACO: Failed to convert blk_id=%u to physical address", blk_id);
                    return false;
                }
            }
        }
        
        // ❌ TACO 解码器但没有 metadata（异常情况）
        LOG_ERROR("[Worker] TACO: AVFrame->metadata is missing or no 'pool_blk_id' entry");
        return false;
    }
    
    // ========== 其他硬件解码器（扩展点）==========
    // 
    // 示例：NVIDIA CUDA 解码器
    // if (decoder_name_ == "h264_cuvid") {
    //     // CUDA 特定逻辑：从 AVFrame 的 data[0] 获取设备内存指针
    //     // CUdeviceptr cuda_ptr = (CUdeviceptr)frame->data[0];
    //     // buffer->setPhysicalAddress((uint64_t)cuda_ptr);
    //     // return true;
    // }
    //
    // 示例：Intel QSV 解码器
    // if (decoder_name_ == "h264_qsv") {
    //     // QSV 特定逻辑：从 AVFrame 的 data[3] 获取 mfxFrameSurface1 指针
    //     // mfxFrameSurface1* surface = (mfxFrameSurface1*)frame->data[3];
    //     // buffer->setPhysicalAddress((uint64_t)surface->Data.MemId);
    //     // return true;
    // }
    
    // 未知硬件解码器或软件解码器
    LOG_WARN_FMT("[Worker] Decoder '%s': No hardware address extraction implemented", 
                 decoder_name_.c_str());
    return false;
}




int FfmpegDecodeRtspWorker::getSourceWidth() const {
    return packet_source_ ? packet_source_->getSourceWidth() : 0;
}

int FfmpegDecodeRtspWorker::getSourceHeight() const {
    return packet_source_ ? packet_source_->getSourceHeight() : 0;
}

AVPixelFormat FfmpegDecodeRtspWorker::getSourcePixelFormat() const {
    return packet_source_ ? packet_source_->getSourcePixelFormat() : AV_PIX_FMT_NONE;
}
