#include "productionline/worker/FFmpegDecodeWorker.hpp"
#include "vendor/taco/decode/TacoDecoderExtension.hpp"
#include "productionline/worker/EncodedPacketSourceFromRtsp.hpp"
#include "productionline/worker/EncodedPacketSourceFromBuffer.hpp"
#include "productionline/worker/EncodedPacketSourceFromFile.hpp"
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

// 构造函数（v3.0：统一的 FFmpeg 解码 Worker，支持文件/RTSP/Buffer 模式）
FFmpegDecodeWorker::FFmpegDecodeWorker(const WorkerConfig& config)
    : WorkerBase(BufferAllocatorFactory::AllocatorType::AVFRAME, config)  // 传递 config 给父类
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Decode")))
    , packet_source_(nullptr)  // ⚠️ 数据源将在下面根据配置创建
    , codec_ctx_ptr_(nullptr)
    , output_width_(config.display.width)      // 🎯 从配置读取输出宽度
    , output_height_(config.display.height)    // 🎯 从配置读取输出高度
    , use_hardware_decoder_(config.decoder.enable_hardware)  // 🎯 从配置读取
    , decoder_name_(config.decoder.name.value_or(""))  // 🎯 从配置读取（使用 optional 的 value_or）
    , codec_options_ptr_(nullptr)
    , decoded_frames_(0)
    , dropped_frames_(0)
    , current_packet_ptr_(nullptr)  // ⭐ v2.22 新增
    , packet_acquired_(false)       // ⭐ v2.22 新增
    , flush_sent_(false)
{
    LOG4CPLUS_DEBUG(logger_, " FFmpegDecodeWorker created with config");
    
    // ⭐ v2.18 重构：根据配置创建数据源（统一在构造函数中）
    // ⭐ v2.19 修复：支持共享数据源模式
    // ⭐ v2.22 重构：数据源配置从 decoder 移至 datasource
    // ⭐ v2.23 优化：自动识别数据源类型（RTSP/文件）
    if (config.data_source.buffer_mode) {
        // Buffer 数据源模式：从 EncodedPacketSourceFromBuffer 获取 packet
        
        // ⭐ v2.19 新增：检查是否使用共享实例（MultiWorker 共享模式）
        if (config.data_source.shared_packet_source) {
            // ✅ 共享模式：使用 config 中的共享实例（ONE_TO_MANY 零拷贝）
            packet_source_ = config.data_source.shared_packet_source;
            LOG4CPLUS_INFO(logger_, "⭐ v2.22 使用共享 EncodedPacketSource（MultiWorker 共享模式）");
        } else {
            // ✅ 普通模式：创建独立的 EncodedPacketSourceFromBuffer 实例（ONE_TO_ONE）
            if (config.data_source.codec_params) {
                packet_source_ = std::make_shared<EncodedPacketSourceFromBuffer>(config.data_source.codec_params);
                LOG4CPLUS_DEBUG(logger_, "Created EncodedPacketSourceFromBuffer (v2.20: 需要调用 setSourceBufferPool 关联源 Pool)");
            } else {
                LOG4CPLUS_WARN(logger_, "buffer_mode=true but codec_params is nullptr");
            }
        }
    } else {
        // ⭐ v2.23 优化：根据路径自动识别数据源类型
        // ⭐ v2.32 修改：RTSP 也传递 max_frames
        const std::string& path = config.data_source.path;
        if (path.empty()) {
            LOG4CPLUS_WARN(logger_, "data_source.path is empty, cannot create packet source");
        } else if (path.rfind("rtsp://", 0) == 0 || path.rfind("rtsps://", 0) == 0) {
            // RTSP 流：以 rtsp:// 或 rtsps:// 开头
            packet_source_ = std::make_shared<EncodedPacketSourceFromRtsp>(path, config.data_source.max_frames);
            LOG4CPLUS_DEBUG_FMT(logger_, "Created EncodedPacketSourceFromRtsp for '%s' (max_frames=%d)", 
                               path.c_str(), config.data_source.max_frames);
        } else {
            // 文件：其他路径视为本地文件
            packet_source_ = std::make_shared<EncodedPacketSourceFromFile>(path, config.data_source.max_frames);
            LOG4CPLUS_DEBUG_FMT(logger_, "Created EncodedPacketSourceFromFile for '%s' (max_frames=%d)", 
                               path.c_str(), config.data_source.max_frames);
        }
    }
}

FFmpegDecodeWorker::~FFmpegDecodeWorker() {
    LOG4CPLUS_DEBUG(logger_, "🧹 Destroying FFmpegDecodeWorker...");
    
    // ⭐ 关键：正确的清理顺序
    //
    // 问题根源：
    // - 如果先调用 close()，再让成员变量析构
    // - 顺序就变成：关闭解码器 → 释放 AVFrame
    // - 但此时 AVFrame 可能还引用了解码器的资源，导致 free(): invalid pointer
    //
    // 正确顺序：
    // 1. 先释放缓存的帧
    // 2. 手动调用 allocator_facade_.destroyPool() 释放所有 AVFrame
    // 3. 再调用 close() 关闭解码器和数据源
    
    // 步骤1：清理缓存的帧（避免内存泄漏）
    for (AVFrame* frame : cached_frames_) {
        if (frame) {
            av_frame_free(&frame);
        }
    }
    cached_frames_.clear();
    
    // 步骤2：先清理 BufferPool 和 AVFrame（避免 free(): invalid pointer）
    if (!buffer_pool_type_map_.empty()) {
        LOG4CPLUS_DEBUG(logger_, "手动清理 BufferPool 和 AVFrame...");
        allocator_facade_.destroyPool();  // 释放所有 Pool 中的 Buffer 和 AVFrame
        clearAllBufferPools();
    }
    
    // 步骤3：再关闭解码器和数据源（此时 AVFrame 已全部释放）
    if (packet_source_ && packet_source_->isOpen()) {
        LOG4CPLUS_DEBUG(logger_, "关闭解码器和数据源...");
        close();
    }
    
    LOG4CPLUS_DEBUG(logger_, "🧹 FFmpegDecodeWorker destroyed");
}

// ============ IVideoReader 接口实现 ============

bool FFmpegDecodeWorker::open(const char* path) {
    open();
    return true;
}

bool FFmpegDecodeWorker::open() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 如果已经打开，先关闭
    if (packet_source_ && packet_source_->isOpen() && packet_source_->getDataSourceType() != IEncodedPacketSource::SourceType::BUFFER_SOURCE) {
        LOG4CPLUS_WARN(logger_, " ⚠️  Stream already open, closing previous stream");
        close();
    }
    
    // ⭐ v2.18 重构：统一处理，不区分 Buffer/RTSP/File 模式
    // 数据源应该在构造函数中已经创建
    if (!packet_source_) {
        LOG4CPLUS_ERROR(logger_, " Cannot open: packet source is nullptr. Worker must be created with WorkerConfig");
        return false;
    }

    // 每次 open 都重新开始 EOF drain 状态
    flush_sent_ = false;
    cached_frames_.clear();
    
    // 从配置读取输出参数
    int width = worker_config_.display.width;
    int height = worker_config_.display.height;
    bool is_buffer_mode = worker_config_.data_source.buffer_mode;
    
    // 打印模式信息
    if (!is_buffer_mode) {
        LOG4CPLUS_INFO(logger_, "");
        LOG4CPLUS_INFO_FMT(logger_, "📡 Opening video source: %s", worker_config_.data_source.path.c_str());
    } else {
        LOG4CPLUS_INFO(logger_, " 📦 Opening EncodedPacketSourceFromBuffer (Buffer mode)");
    }
    
    // 1. 打开数据源
    if (!packet_source_->open()) {
        LOG4CPLUS_ERROR(logger_, " Failed to open packet source");
        return false;
    }
    
    // 2. 从数据源获取编解码器参数
    const AVCodecParameters* codecpar = packet_source_->getCodecParameters();
    if (!codecpar) {
        LOG4CPLUS_ERROR(logger_, " Failed to get codec parameters from packet source");
        packet_source_->close();
        return false;
    }

    // 对于 encode->decode->display 的 Buffer pipeline，某些硬件编码器的 H264 extradata
    // 可能需要等到产生首个 packet 后才会填充（SPS/PPS）。
    // decoder 若过早初始化，可能导致 receive_frame 永远 EAGAIN/EOF（最终 0 frames）。
    if (is_buffer_mode && codecpar->extradata_size == 0) {
        constexpr int kWaitMs = 3000;
        constexpr int kSleepMs = 10;
        int waited = 0;

        LOG4CPLUS_INFO(logger_,
            " Waiting for codec extradata (SPS/PPS) to be ready...");
        while (waited < kWaitMs && codecpar->extradata_size == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
            waited += kSleepMs;
        }

        LOG4CPLUS_INFO_FMT(logger_,
            " codec extradata wait done: size=%d bytes (waited=%dms)",
            codecpar->extradata_size, waited);
    }
    
    // 3. 检查编解码器类型是否匹配
    checkCodecMismatch(codecpar->codec_id, decoder_name_);
    
    // 4. 设置输出分辨率（必须在 initializeDecoder 之前，因为解码器初始化时会打印分辨率）
    if (width == 0 || height == 0) {
        // 配置未设置，使用原始分辨率或默认值
        output_width_ = getSourceWidth() > 0 ? getSourceWidth() : 1920;
        output_height_ = getSourceHeight() > 0 ? getSourceHeight() : 1080;
        LOG4CPLUS_DEBUG_FMT(logger_, " Output resolution not set in config, using: %dx%d", 
                      output_width_, output_height_);
    } else {
        // 边界检查：验证用户配置的分辨率是否合理
        if (width < 0 || height < 0) {
            LOG4CPLUS_ERROR_FMT(logger_, " Invalid resolution: %dx%d (negative values not allowed)", 
                          width, height);
            packet_source_->close();
            return false;
        }
        if (width < MIN_RESOLUTION || height < MIN_RESOLUTION) {
            LOG4CPLUS_ERROR_FMT(logger_, " Resolution %dx%d too small (minimum: %dx%d)", 
                          width, height, MIN_RESOLUTION, MIN_RESOLUTION);
            packet_source_->close();
            return false;
        }
        if (width > MAX_RESOLUTION || height > MAX_RESOLUTION) {
            LOG4CPLUS_ERROR_FMT(logger_, " Resolution %dx%d too large (maximum: %dx%d)", 
                          width, height, MAX_RESOLUTION, MAX_RESOLUTION);
            packet_source_->close();
            return false;
        }
        
        output_width_ = width;
        output_height_ = height;
        LOG4CPLUS_DEBUG_FMT(logger_, " Output resolution from config: %dx%d", output_width_, output_height_);
    }
    
    // 5. 初始化解码器
    if (!initializeDecoder(codecpar)) {
        packet_source_->close();
        return false;
    }
    
    // 6. 生成 BufferPool 名称
    std::string pool_name;
    if (is_buffer_mode) {
        pool_name = "FFmpegDecodeWorker_BufferMode";
    } else {
        pool_name = std::string("FFmpegDecodeWorker_") + worker_config_.data_source.path;
    }
    
    uint64_t pool_id = allocator_facade_.allocatePoolWithBuffers(
        worker_config_.data_source.buffer_count,
        0,
        pool_name,
        is_buffer_mode ? "BUFFER_MODE" : "NORMAL_MODE"
    );
    
    if (pool_id == 0) {
        LOG4CPLUS_ERROR(logger_, " Failed to create BufferPool via Allocator");
        packet_source_->close();
        return false;
    }
    
    // 7. ✅ v2.18 修复：统一注册 BufferPool（Buffer 和 RTSP 模式都需要）
    if (!registerBufferPool(BufferPoolType::DECODE_VIDEO_PRIMARY, pool_id)) {
        LOG4CPLUS_ERROR(logger_, " Failed to register BufferPool");
        packet_source_->close();
        return false;
    }
    
    // 8. 从 Registry 获取 Pool 名称
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    std::string actual_pool_name = pool ? pool->getName() : "Unknown";
    
    decoded_frames_ = 0;
    dropped_frames_ = 0;
    
    // 9. 详细日志输出
    const char* mode_str = is_buffer_mode ? "Buffer mode" : 
        (packet_source_->getDataSourceType() == IEncodedPacketSource::SourceType::NETWORK_SOURCE ? "RTSP stream" : "File");
    LOG4CPLUS_DEBUG_FMT(logger_, " FFmpegDecodeWorker (%s): Opened", mode_str);
    if (!is_buffer_mode) {
        LOG4CPLUS_DEBUG_FMT(logger_, "    Source: %s", worker_config_.data_source.path.c_str());
    }
    LOG4CPLUS_DEBUG_FMT(logger_, "    Output resolution: %dx%d (%.1f bytes/pixel)", 
                  output_width_, output_height_, getOutputBytesPerPixel());
    LOG4CPLUS_DEBUG_FMT(logger_, "    Codec: %s", codec_ctx_ptr_->codec->name);
    LOG4CPLUS_DEBUG_FMT(logger_, "    BufferPool: '%s' (ID: %lu, %d buffers)", 
                  actual_pool_name.c_str(), pool_id, 
                  worker_config_.data_source.buffer_count);
    
    return true;
}

// ============ v2.13 EncodedPacketSourceFromBuffer 配置 ============

bool FFmpegDecodeWorker::setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) {
    // 检查是否是 EncodedPacketSourceFromBuffer
    auto* buffer_source = dynamic_cast<EncodedPacketSourceFromBuffer*>(packet_source_.get());
    if (!buffer_source) {
        LOG4CPLUS_WARN(logger_, "setSourceBufferPool 失败：不是 Buffer 模式");
        return false;
    }
    
    // 设置源 BufferPool
    buffer_source->setSourceBufferPool(pool_weak);
    LOG4CPLUS_DEBUG(logger_, "✅ 已设置源 BufferPool（v2.13 Pool 模式）");
    
    return true;
}

void FFmpegDecodeWorker::close() {
    // ⚠️ 注意：打开状态由数据源管理
    if (!packet_source_ || !packet_source_->isOpen()) {
        return;  // 已经关闭过了
    }
    
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        LOG4CPLUS_INFO(logger_, "");
        LOG4CPLUS_INFO(logger_, "🛑 Closing video source...");

        flush_sent_ = false;
        
        // ⭐ v2.32 修改：清理未提交的 packet（统一接口）
        if (packet_acquired_) {
            // 强制提交（避免订阅者计数永久占用）
            LOG4CPLUS_DEBUG(logger_, " Cleaning up pending packet on close");
            packet_source_->commitEncodedPacket(this);
            packet_acquired_ = false;
            current_packet_ptr_ = nullptr;
        }
        
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
    
    LOG4CPLUS_DEBUG(logger_, " Video source closed");
    LOG4CPLUS_INFO_FMT(logger_, "   success=%d failed=%d skipped=%d",
        decoded_frames_.load(), 0, dropped_frames_.load());
}

bool FFmpegDecodeWorker::isOpen() const {
    // ⚠️ 注意：打开状态从数据源获取
    if (!packet_source_) {
        return false;
    }
    return packet_source_->isOpen();
}


bool FFmpegDecodeWorker::seek(int frame_index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 1. 参数校验
    if (!packet_source_) {
        LOG4CPLUS_ERROR(logger_, " Cannot seek: packet source is nullptr");
        return false;
    }
    
    if (!packet_source_->isOpen()) {
        LOG4CPLUS_ERROR(logger_, " Cannot seek: worker is not open");
        return false;
    }
    
    // 2. 委托给数据源实现 seek（多态调用）
    //    - EncodedPacketSourceFromFile: 实现真正的 seek
    //    - EncodedPacketSourceFromRtsp: 返回 false（不支持）
    //    - EncodedPacketSourceFromBuffer: 返回 false（不支持）
    if (!packet_source_->seek(frame_index)) {
        // 根据数据源类型返回适当的日志
        if (packet_source_->getDataSourceType() == IEncodedPacketSource::SourceType::NETWORK_SOURCE) {
            LOG4CPLUS_WARN(logger_, " RTSP stream does not support seeking");
        } else if (packet_source_->getDataSourceType() == IEncodedPacketSource::SourceType::BUFFER_SOURCE) {
            LOG4CPLUS_WARN(logger_, " Buffer source does not support seeking");
        } else {
            LOG4CPLUS_ERROR(logger_, " Seek failed or not supported by packet source");
        }
        return false;
    }
    
    // 3. 清理缓存的解码帧和未提交的 packet
    for (AVFrame* frame : cached_frames_) {
        if (frame) av_frame_free(&frame);
    }
    cached_frames_.clear();

    if (packet_acquired_) {
        packet_source_->commitEncodedPacket(this);
        packet_acquired_ = false;
        current_packet_ptr_ = nullptr;
    }

    // 4. 重置解码器状态
    //    软件解码器：调用 avcodec_flush_buffers 清空内部缓冲
    //    硬件解码器（h264_taco）：跳过 flush，因为 taco 的 flush 回调会将通道
    //    置为 STOPPED 状态（state=5）且不会重新启动，导致后续 send_packet 失败。
    //    文件从 I 帧开始，解码器无需 flush 也能正确解码。
    if (codec_ctx_ptr_ && !use_hardware_decoder_) {
        avcodec_flush_buffers(codec_ctx_ptr_);
    }
    
    LOG4CPLUS_DEBUG_FMT(logger_, " Successfully seeked to frame %d", frame_index);
    return true;
}

bool FFmpegDecodeWorker::seekToBegin() {
    // 委托给 seek(0)
    return seek(0);
}

bool FFmpegDecodeWorker::seekToEnd() {
    LOG4CPLUS_WARN(logger_, " Warning: seekToEnd is not supported");
    return false;
}

bool FFmpegDecodeWorker::skip(int frame_count) {
    LOG4CPLUS_WARN(logger_, " Warning: skip is not supported for streaming sources");
    return false;
}

int FFmpegDecodeWorker::getTotalFrames() const {
    // ⭐ v2.12修改：从数据源获取（适配器模式）
    if (packet_source_) {
        return packet_source_->getTotalFrames();
    }
    return INT_MAX;
}

int FFmpegDecodeWorker::getCurrentFrameIndex() const {
    // 返回已解码帧数作为"当前索引"
    return decoded_frames_.load();
}

size_t FFmpegDecodeWorker::getFrameSize() const {
    // ✅ 使用实际解码输出格式计算（getBytesPerPixel从实际格式获取）
    return (size_t)(output_width_ * output_height_ * getOutputBytesPerPixel());
}

long FFmpegDecodeWorker::getFileSize() const {
    // ⭐ v2.12修改：从数据源获取
    if (packet_source_) {
        return packet_source_->getFileSize();
    }
    return -1;
}

int FFmpegDecodeWorker::getSourceWidth() const {
    return packet_source_ ? packet_source_->getSourceWidth() : 0;
}

int FFmpegDecodeWorker::getSourceHeight() const {
    return packet_source_ ? packet_source_->getSourceHeight() : 0;
}

int FFmpegDecodeWorker::getOutputWidth() const {
    return output_width_;
}

int FFmpegDecodeWorker::getOutputHeight() const {
    return output_height_;
}

double FFmpegDecodeWorker::getOutputBytesPerPixel(int channel) const {
    // ========== 1. TACO 硬件解码器：从 priv_data 读取 PP 配置 ==========
    if (use_hardware_decoder_ && codec_ctx_ptr_ && codec_ctx_ptr_->priv_data &&
        decoder_name_.find("taco") != std::string::npos) {
        return getTacoChannelBytesPerPixel(channel);
    }
    
    // ========== 2. 通用平台（软件解码器等）==========
    if (channel == 0) {
        // 从解码器实际输出格式获取
        if (codec_ctx_ptr_ && codec_ctx_ptr_->pix_fmt != AV_PIX_FMT_NONE) {
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(codec_ctx_ptr_->pix_fmt);
            if (desc) {
                return av_get_bits_per_pixel(desc) / 8.0;
            }
        }
        return 1.5;  // Fallback: YUV420
    }
    
    return 0.0;  // 其他通道不支持
}

// ============================================================================
// TACO 辅助函数（多通道支持）
// ============================================================================

double FFmpegDecodeWorker::getTacoChannelBytesPerPixel(int channel) const {
    int64_t value = 0;
    
    if (channel == 0) {
        // 通道0：检查是否启用
        if (av_opt_get_int(codec_ctx_ptr_->priv_data, "ch0_enable", 0, &value) < 0 || value == 0) {
            return 0.0;  // 通道未启用
        }
        
        // 通道0通常输出 YUV，从 codec_ctx_ptr_->pix_fmt 获取
        if (codec_ctx_ptr_->pix_fmt != AV_PIX_FMT_NONE) {
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(codec_ctx_ptr_->pix_fmt);
            if (desc) {
                return av_get_bits_per_pixel(desc) / 8.0;
            }
        }
        return 1.5;  // Fallback: YUV420
    }
    
    if (channel == 1) {
        // 通道1：检查是否启用
        if (av_opt_get_int(codec_ctx_ptr_->priv_data, "ch1_enable", 0, &value) < 0 || value == 0) {
            return 0.0;  // 通道未启用
        }
        
        // 检查是否是 RGB 模式
        if (av_opt_get_int(codec_ctx_ptr_->priv_data, "ch1_rgb", 0, &value) < 0 || value == 0) {
            // 不是 RGB，可能是 YUV
            return 1.5;  // 假设 YUV420
        }
        
        // 读取 RGB 格式枚举
        int64_t rgb_format = 0;
        if (av_opt_get_int(codec_ctx_ptr_->priv_data, "ch1_rgb_format", 0, &rgb_format) < 0) {
            return 4.0;  // Fallback: ARGB888
        }
        
        // 根据 RGB 格式枚举返回字节数
        OutputFormat format = mapRgbDriverValueToEnum((int)rgb_format);
        return getBytesPerPixelFromFormat(format);
    }
    
    return 0.0;  // 无效通道
}

OutputFormat FFmpegDecodeWorker::mapRgbDriverValueToEnum(int driver_value) {
    switch (driver_value) {
        case 9:  return OutputFormat::RGB_ARGB888;
        case 11: return OutputFormat::RGB_ABGR888;
        case 13: return OutputFormat::RGB_RGBA888;
        case 15: return OutputFormat::RGB_BGRA888;
        case 1:  return OutputFormat::RGB_RGB888;
        case 3:  return OutputFormat::RGB_BGR888;
        case 25: return OutputFormat::RGB_XRGB888;
        case 27: return OutputFormat::RGB_XBGR888;
        case 21: return OutputFormat::RGB_RGBX888;
        case 23: return OutputFormat::RGB_BGRX888;
        case 2:  return OutputFormat::RGB_RGB888_PLANAR;
        case 4:  return OutputFormat::RGB_BGR888_PLANAR;
        case 17: return OutputFormat::RGB_R16G16B16;
        case 19: return OutputFormat::RGB_B16G16R16;
        case 28: return OutputFormat::RGB_GBRP;
        default: return OutputFormat::RGB_ARGB888;  // 默认值
    }
}

double FFmpegDecodeWorker::getBytesPerPixelFromFormat(OutputFormat format) {
    switch (format) {
        // 8-bit RGB 有 Alpha/X 通道（4 字节/像素）
        case OutputFormat::RGB_ARGB888:
        case OutputFormat::RGB_ABGR888:
        case OutputFormat::RGB_RGBA888:
        case OutputFormat::RGB_BGRA888:
        case OutputFormat::RGB_XRGB888:
        case OutputFormat::RGB_XBGR888:
        case OutputFormat::RGB_RGBX888:
        case OutputFormat::RGB_BGRX888:
            return 4.0;
        
        // 8-bit RGB 无 Alpha 通道（3 字节/像素）
        case OutputFormat::RGB_RGB888:
        case OutputFormat::RGB_BGR888:
        case OutputFormat::RGB_RGB888_PLANAR:
        case OutputFormat::RGB_BGR888_PLANAR:
        case OutputFormat::RGB_GBRP:
            return 3.0;
        
        // 16-bit RGB（6 字节/像素）
        case OutputFormat::RGB_R16G16B16:
        case OutputFormat::RGB_B16G16R16:
            return 6.0;
        
        default:
            return 4.0;  // 默认 ARGB888
    }
}

std::string FFmpegDecodeWorker::getPath() const {
    // ⭐ v2.12修改：从数据源获取
    if (!packet_source_) {
        return std::string();
    }
    return packet_source_->getPath();
}

IDataSourceNavigator::SourceType FFmpegDecodeWorker::getDataSourceType() const {
    if (packet_source_) {
        return packet_source_->getDataSourceType();
    }
    return SourceType::NETWORK_SOURCE;  // 默认是网络流类型
}

bool FFmpegDecodeWorker::hasMoreFrames() const {
    // ⭐ v2.12修改：从数据源获取 EOF 状态
    if (!packet_source_) {
        return false;
    }
    return !packet_source_->isAtEnd();
}

bool FFmpegDecodeWorker::isAtEnd() const {
    // ⭐ v2.12修改：从数据源获取 EOF 状态
    if (!packet_source_) {
        return true;
    }
    return packet_source_->isAtEnd();
}

bool FFmpegDecodeWorker::isConnected() const {
    // 连接状态从数据源判断
    if (!packet_source_) {
        return false;
    }
    return packet_source_->isOpen();
}

// ============================================================================
// 核心功能：填充Buffer
// ============================================================================

/**
 * @brief 从 AVFrame 填充 Buffer 的元数据
 * @param frame_ptr AVFrame 指针（必须已填充数据）
 * @param buffer Buffer 指针（用于存储元数据）
 * @return true 成功设置元数据，false 失败
 */
bool FFmpegDecodeWorker::fillBufferMetadataFromFrame(AVFrame* frame_ptr, Buffer* buffer) {
    // ⭐ 硬件解码器：提取物理内存地址
    if (!decoder_name_.empty() && use_hardware_decoder_) {
        if (!extractHardwareAddressFromMetadata(frame_ptr, buffer)) {
            LOG4CPLUS_ERROR_FMT(logger_, " Hardware decoder '%s': Failed to extract physical address",
                         decoder_name_.c_str());
            // ⚠️ 容错处理，打印日志但继续执行
        }
    }
    
    // ⭐ 设置虚拟地址
    buffer->setVirtualAddress(frame_ptr->data[0]);
    
    // ⭐ 计算并设置帧大小
    int actual_frame_size = av_image_get_buffer_size(
        (AVPixelFormat)frame_ptr->format,
        frame_ptr->width,
        frame_ptr->height,
        1  // alignment
    );
    
    if (actual_frame_size > 0) {
        buffer->setSize(actual_frame_size);
        LOG_TRACE_FMT(" Updated buffer size to actual frame size: %d bytes", actual_frame_size);
    } else {
        LOG4CPLUS_ERROR_FMT(logger_, " Failed to get frame buffer size: %d", actual_frame_size);
    }
    
    // ⭐ 设置图像元数据（格式、宽高、linesize 等）
    buffer->setImageMetadataFromAVFrame(frame_ptr);
    
    // ⭐ v2.26新增：保存 PTS（用于多通道帧对齐）
    buffer->setPts(frame_ptr->pts);
    
    // ⭐ 更新统计计数器
    decoded_frames_++;
    
    return true;
}

/**
 * @brief 从数据源读取 packet 并发送到解码器
 * @param packet_ptr AVPacket 指针（File/RTSP 模式使用）
 * @return FillResult 结果对象
 * 
 * v2.32 重构：统一使用 acquireEncodedPacket 接口
 * - Buffer 共享模式：忽略 packet_ptr，返回借用指针
 * - File/RTSP 模式：往 packet_ptr 填充数据（零拷贝）
 * 
 * v2.33 变更：返回类型从 bool 改为 FillResult
 */
FillResult FFmpegDecodeWorker::readAndSendPacket(AVPacket* packet_ptr) {
    // ⭐ v2.32 统一接口：所有模式都使用 acquireEncodedPacket
    // - File/RTSP 模式：传入 packet_ptr，数据填充到里面
    // - Buffer 共享模式：传入 nullptr（或 packet_ptr 会被忽略），返回借用指针
    
    if (!packet_acquired_) {
        auto acquire_result = packet_source_->acquireEncodedPacket(packet_ptr, this);
        
        if (!acquire_result.ok()) {
            return FillResult::fromAcquire(acquire_result);
        }
        
        current_packet_ptr_ = acquire_result.packet();
        packet_acquired_ = true;
    }
    
    // ========== 发送到解码器 ==========
    int ret = avcodec_send_packet(codec_ctx_ptr_, current_packet_ptr_);
    
    // ⏳ EAGAIN 处理：解码器内部缓冲区已满，延迟后重试发送
    if (ret == AVERROR(EAGAIN)) {
        LOG4CPLUS_WARN(logger_, 
            " avcodec_send_packet: decoder buffer full (EAGAIN), "
            "will retry after short delay...");
        
        constexpr int kMaxRetries = 3;
        constexpr int kRetryDelayMs = 10;
        
        for (int i = 0; i < kMaxRetries; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
            ret = avcodec_send_packet(codec_ctx_ptr_, current_packet_ptr_);
            
            if (ret == 0) {
                LOG4CPLUS_INFO_FMT(logger_, 
                    " avcodec_send_packet: retry #%d succeeded, packet consumed", i + 1);
                return FillResult::success();
            }
            if (ret != AVERROR(EAGAIN)) {
                break;  // 遇到其他错误，跳出重试进入后续错误处理
            }
        }
        
        if (ret == AVERROR(EAGAIN)) {
            // ❌ 重试耗尽仍然 EAGAIN，返回发送失败错误，由调用者决定是否丢弃该帧
            LOG4CPLUS_ERROR_FMT(logger_, 
                " avcodec_send_packet: still EAGAIN after %d retries, "
                "returning sendPacketFailed to caller", kMaxRetries);
            return FillResult::fromCodec(CodecSendResult::sendFailed());
        }
        // ret 已变为其他错误码，fall through 到下方映射
    }
    
    // v2.35 重构：与 Acquire 层保持一致，直接使用工厂方法映射
    if (ret == 0) {
        return FillResult::success();
    }
    
    // 错误码映射（与 PacketAcquireResult 在 acquireEncodedPacket 中的风格一致）
    using Result = CodecSendResult;
    if (ret == AVERROR_EOF)     return FillResult::fromCodec(Result::eof());
    if (ret == AVERROR(EINVAL)) return FillResult::fromCodec(Result::invalidState());
    if (ret == AVERROR(ENOMEM)) return FillResult::fromCodec(Result::allocFailed());
    
    // 其他未识别错误
    char err_buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, err_buf, sizeof(err_buf));
    LOG4CPLUS_ERROR_FMT(logger_, 
        " ERROR: avcodec_send_packet: decode error (ret=%d, %s)", ret, err_buf);
    return FillResult::fromCodec(Result::decodeError());
}

/**
 * @brief 填充 Buffer（解码一帧）
 * 
 * v2.33 变更：返回类型从 bool 改为 FillResult
 */
FillResult FFmpegDecodeWorker::fillBuffer(int frame_index, Buffer* buffer) {
    (void)frame_index;  // 未使用
    
    // ========== 参数校验 ==========
    if (!buffer) {
        LOG4CPLUS_ERROR(logger_, " ERROR: buffer is nullptr");
        return FillResult::invalidParam();
    }
    
    if (!packet_source_ || !packet_source_->isOpen()) {
        LOG4CPLUS_ERROR(logger_, " ERROR: Worker is not open");
        return FillResult::notOpen();
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    AVFrame* frame_ptr = buffer->getAVFrame();
    if (!frame_ptr) {
        LOG4CPLUS_ERROR(logger_, " ERROR: buffer->getAVFrame() is nullptr");
        return FillResult::invalidParam();
    }
    
    AVPacket* packet_ptr = buffer->getAVPacket();
    if (!packet_ptr) {
        LOG4CPLUS_ERROR(logger_, " ERROR: buffer->getAVPacket() is nullptr");
        return FillResult::invalidParam();
    }
    
    // ========== 步骤1: 检查缓存队列 ==========
    if (!cached_frames_.empty()) {
        AVFrame* cached_frame = cached_frames_.front();
        cached_frames_.erase(cached_frames_.begin());
        
        av_frame_move_ref(frame_ptr, cached_frame);
        av_frame_free(&cached_frame);
        
        fillBufferMetadataFromFrame(frame_ptr, buffer);
        return FillResult::success();
    }
    
    // ========== 步骤2: 读取并发送 packet ==========
    FillResult send_result = readAndSendPacket(packet_ptr);
    if (!send_result.ok()) {
        // Acquire 层 EOF：需要对 codec 做一次 drain，不能直接退出，否则会出现 0 frames 但管线结束。
        if (send_result.isAcquireEof()) {
            // 尝试 flush（send_packet(NULL)），但不因为 flush 失败就提前返回。
            // taco/部分 codec 可能会打印 "no stream to decode, skip flush" 并返回错误，
            // 但仍可能存在延迟帧需要通过 receive_frame drain 出来。
            if (!flush_sent_) {
                (void)avcodec_send_packet(codec_ctx_ptr_, nullptr);
                flush_sent_ = true;
            }

            // ========== drain: receive_frame 直到拿到一帧或 EAGAIN/EOF ==========
            bool decoded_at_least_one = false;
            CodecSendResult receive_result = CodecSendResult::success();

            while (true) {
                AVFrame* temp_frame = av_frame_alloc();
                if (!temp_frame) {
                    receive_result = CodecSendResult::allocFailed();
                    break;
                }

                int ret = avcodec_receive_frame(codec_ctx_ptr_, temp_frame);

                if (ret == 0) {
                    decoded_at_least_one = true;
                    cached_frames_.push_back(temp_frame);
                    continue;
                }

                // 失败：释放临时帧，映射错误码
                av_frame_free(&temp_frame);

                if (ret == AVERROR(EAGAIN)) {
                    receive_result = CodecSendResult::eagain();
                } else if (ret == AVERROR_EOF) {
                    receive_result = CodecSendResult::eof();
                } else if (ret == AVERROR(EINVAL)) {
                    receive_result = CodecSendResult::invalidState();
                } else {
                    receive_result = CodecSendResult::receiveError();
                }
                break;
            }

            if (!decoded_at_least_one) {
                // drain 阶段无帧：
                // - 如果 codec 返回 EAGAIN，说明可能仍有“延迟帧”尚未准备好（尤其是硬件/部分 codec）
                //   这里必须返回 eagain 触发 kRetry，让后续 fillBuffer 再次 drain。
                // - 其余情况（如真正 flush 完成的 eof / 或其它错误）按 receive_result 原样返回。
                return FillResult::fromCodec(receive_result);
            }

            // ========== 从缓存取第一帧填充 buffer ==========
            if (cached_frames_.empty()) {
                return FillResult::internalError();
            }

            AVFrame* first_frame = cached_frames_.front();
            cached_frames_.erase(cached_frames_.begin());

            av_frame_move_ref(buffer->getAVFrame(), first_frame);
            av_frame_free(&first_frame);
            fillBufferMetadataFromFrame(buffer->getAVFrame(), buffer);
            return FillResult::success();
        }

        return send_result;
    }
   
    // ========== 步骤3: 循环读取所有解码的帧到缓存 ==========
    bool decoded_at_least_one = false;
    CodecSendResult receive_result = CodecSendResult::success();
    
    while (true) {
        AVFrame* temp_frame = av_frame_alloc();
        if (!temp_frame) {
            receive_result = CodecSendResult::allocFailed();
            break;
        }
        
        int ret = avcodec_receive_frame(codec_ctx_ptr_, temp_frame);
        
        if (ret == 0) {
            // ✅ 成功解码一帧
            decoded_at_least_one = true;
            cached_frames_.push_back(temp_frame);
            continue;
        }
        
        // 失败：释放临时帧，映射错误码
        av_frame_free(&temp_frame);
        
        if (ret == AVERROR(EAGAIN)) {
            receive_result = CodecSendResult::eagain();
        } else if (ret == AVERROR_EOF) {
            // 解码器缓存帧耗尽，等同于需要更多输入（不是文件 EOF）
            receive_result = CodecSendResult::eagain();
        } else if (ret == AVERROR(EINVAL)) {
            // codec 未正确打开或类型不匹配（编程错误）
            LOG4CPLUS_ERROR(logger_, 
                " ERROR: avcodec_receive_frame: EINVAL - codec not opened or is an encoder");
            receive_result = CodecSendResult::invalidState();
        } else if (ret == AVERROR_INPUT_CHANGED) {
            // 解码参数在帧间发生变化（需要 AV_CODEC_FLAG_DROPCHANGED）
            LOG4CPLUS_WARN(logger_, 
                " WARN: avcodec_receive_frame: input parameters changed between frames");
            receive_result = CodecSendResult::receiveError();
        } else {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG4CPLUS_ERROR_FMT(logger_, 
                " ERROR: avcodec_receive_frame: unknown error (ret=%d, %s)", ret, err_buf);
            receive_result = CodecSendResult::receiveError();
        }
        break;
    }
    
    // ========== 步骤4: 检查是否成功解码 ==========
    if (!decoded_at_least_one) {
        // 根据 receive_result 的真实原因返回：
        // - eagain: 正常，解码器需要更多输入
        // - receiveError/allocFailed: 真正的错误
        
        // v2.32 统一：调用接口的 cancel 方法
        // - Buffer 共享模式：重置 Worker 状态，允许重试
        // - File/RTSP 模式：默认空实现，无需操作
        if (!worker_config_.data_source.deferred_commit) {
            // 注意：当 receive_result 为 eagain 时，packet 已经被 avcodec_send_packet 消费；
            // 共享模式必须 commit 以推进共享版本，否则 fetch 线程可能卡在同一 version。
            if (receive_result.isEagain()) {
                packet_source_->commitEncodedPacket(this);
            } else {
                packet_source_->cancelEncodedPacket(this);
            }
        }
        packet_acquired_ = false;
        current_packet_ptr_ = nullptr;
        return FillResult::fromCodec(receive_result);
    }
    
    // ========== 步骤5: 成功解码，提交（释放）==========
    // v2.32 统一：调用接口的 commit 方法
    // - Buffer 共享模式：递减订阅者计数，最后一个订阅者触发 Buffer 释放
    // - File/RTSP 模式：默认返回 true，无需操作
    if (packet_acquired_) {
        if (!worker_config_.data_source.deferred_commit) {
            packet_source_->commitEncodedPacket(this);
        }
        
        // 重置状态
        packet_acquired_ = false;
        current_packet_ptr_ = nullptr;
    }
    
    // ========== 步骤6: 从缓存取第一帧填充 buffer ==========
    if (cached_frames_.empty()) {
        return FillResult::internalError();  // 不应该到这里，逻辑错误
    }
    
    AVFrame* first_frame = cached_frames_.front();
    cached_frames_.erase(cached_frames_.begin());
    
    av_frame_move_ref(frame_ptr, first_frame);
    av_frame_free(&first_frame);
    
    fillBufferMetadataFromFrame(frame_ptr, buffer);
    return FillResult::success();
}

// ============================================================================
// 提供原材料（BufferPool）
// ============================================================================

// ============ 特有接口 ============

const AVCodecParameters* FFmpegDecodeWorker::getCodecParameters() const {
    if (!packet_source_) {
        return nullptr;
    }
    return packet_source_->getCodecParameters();
}

AVRational FFmpegDecodeWorker::getTimeBase() const {
    if (!packet_source_) {
        return {1, 25};  // 默认值
    }
    
    // 从数据源获取编解码器参数
    const AVCodecParameters* codecpar = packet_source_->getCodecParameters();
    if (!codecpar) {
        return {1, 25};  // 默认值
    }
    
    // 通常使用帧率的倒数作为时间基
    // 这里返回一个通用的时间基（可以根据实际需求调整）
    return {1, 25};  // 默认25fps
}

const char* FFmpegDecodeWorker::getCodecName() const {
    if (codec_ctx_ptr_ && codec_ctx_ptr_->codec) {
        return codec_ctx_ptr_->codec->name;
    }
    return "unknown";
}

void FFmpegDecodeWorker::printStats() const {
    LOG4CPLUS_INFO(logger_, "");
    LOG4CPLUS_INFO(logger_, " 📊 Statistics:");
    std::string path = packet_source_ ? packet_source_->getPath() : std::string();
    LOG4CPLUS_INFO_FMT(logger_, "    Codec: %s", getCodecName());
    LOG4CPLUS_INFO_FMT(logger_, "    Resolution: %dx%d → %dx%d",
        getSourceWidth(), getSourceHeight(), output_width_, output_height_);
    LOG4CPLUS_INFO_FMT(logger_, "    Source: %s", path.empty() ? "(Buffer mode)" : path.c_str());
    LOG4CPLUS_INFO_FMT(logger_, "    success=%d failed=%d skipped=%d",
        decoded_frames_.load(), 0, dropped_frames_.load());

    SourceType type = getDataSourceType();
    if (type == SourceType::FILE_SOURCE) {
        LOG4CPLUS_INFO_FMT(logger_, "    Total frames: %d", packet_source_ ? packet_source_->getTotalFrames() : -1);
        LOG4CPLUS_INFO_FMT(logger_, "    EOF: %s", packet_source_ && packet_source_->isAtEnd() ? "YES" : "NO");
    } else if (type == SourceType::NETWORK_SOURCE) {
        LOG4CPLUS_INFO_FMT(logger_, "    Connected: %s", isConnected() ? "Yes" : "No");
    }

    uint64_t pool_id = getOutputBufferPoolId(BufferPoolType::DECODE_VIDEO_PRIMARY);
    LOG4CPLUS_INFO_FMT(logger_, "    BufferPool ID: %lu", pool_id);
}

// ============ 内部实现 ============

bool FFmpegDecodeWorker::initializeDecoder(const AVCodecParameters* codec_params) {
    // ⭐ v2.12修改：codec_params 必须提供（从 packet_source_ 获取）
    if (!codec_params) {
        LOG4CPLUS_ERROR(logger_, " Cannot initialize decoder: codec_params is nullptr");
        return false;
    }
    const AVCodecParameters* codecpar = codec_params;
    
    // 1. 查找解码器
    const AVCodec* codec = nullptr;
    
    if (!decoder_name_.empty()) {
        // ⭐ 用户指定了解码器名称（如 "h264_taco"）
        codec = avcodec_find_decoder_by_name(decoder_name_.c_str());
        if (!codec) {
            LOG4CPLUS_WARN_FMT(logger_, " ⚠️ Warning: Specified decoder '%s' not found", decoder_name_.c_str());
        } else {
            LOG4CPLUS_DEBUG_FMT(logger_, " Using specified decoder: %s", decoder_name_.c_str());
            
            // ⭐ v2.18 配置冲突检测：用户要求软件解码，但指定了硬件解码器
            if (!use_hardware_decoder_ && isHardwareDecoder(codec)) {
                LOG4CPLUS_WARN(logger_, "╔═══════════════════════════════════════════════════════════════╗");
                LOG4CPLUS_WARN(logger_, "║  ⚠️  Configuration Conflict Detected                        ║");
                LOG4CPLUS_WARN(logger_, "╚═══════════════════════════════════════════════════════════════╝");
                LOG4CPLUS_WARN_FMT(logger_, "  Requested: Software decoding (use_hardware_decoder_=false)");
                LOG4CPLUS_WARN_FMT(logger_, "  But specified decoder '%s' is a hardware decoder", decoder_name_.c_str());
                LOG4CPLUS_WARN(logger_, "");
                LOG4CPLUS_WARN(logger_, "  💡 Resolution: Ignoring decoder name, searching for software decoder...");
                LOG4CPLUS_WARN(logger_, "╚═══════════════════════════════════════════════════════════════╝");
                
                // ✅ 重置 codec，让后续逻辑自动查找软件解码器（满足用户核心需求）
                codec = nullptr;
            }
        }
    }
    
    if (!codec) {
        if (!use_hardware_decoder_) {
            // ⭐ v2.18 用户要求软件解码：查找纯软件解码器
            LOG4CPLUS_INFO(logger_, " Searching for pure software decoder...");
            codec = findPureSoftwareDecoder(codecpar->codec_id);
            if (!codec) {
                LOG4CPLUS_ERROR(logger_, " No pure software decoder available for this codec!");
                return false;
            }
            LOG4CPLUS_INFO_FMT(logger_, " ✅ Using software decoder: %s", codec->name);
        } else {
            // 硬件解码或自动选择：使用 FFmpeg 默认行为
            codec = avcodec_find_decoder(codecpar->codec_id);
            if (!codec) {
                LOG4CPLUS_ERROR(logger_, " Decoder not found for codec");
                return false;
            }
            
            // 日志：显示选择的解码器类型
            if (isHardwareDecoder(codec)) {
                LOG4CPLUS_INFO_FMT(logger_, " Auto-selected hardware decoder: %s", codec->name);
            } else {
                LOG4CPLUS_INFO_FMT(logger_, " Auto-selected software decoder: %s", codec->name);
            }
        }
    }
    
    // 2. 分配解码器上下文
    codec_ctx_ptr_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_ptr_) {
        LOG4CPLUS_ERROR(logger_, " Failed to allocate codec context");
        return false;
    }
    
    // 3. 复制参数到解码器上下文
    int ret = avcodec_parameters_to_context(codec_ctx_ptr_, codecpar);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, " Failed to copy codec parameters (FFmpeg: %s)", err_buf);
        avcodec_free_context(&codec_ctx_ptr_);
        codec_ctx_ptr_ = nullptr;
        return false;
    }
    
    // 4. 配置特殊解码器（如 h264_taco）
    if (decoder_name_ == "h264_taco") {
        if (!configureSpecialDecoder()) {
            LOG4CPLUS_ERROR(logger_, " ERROR: Failed to configure special decoder options");
            avcodec_free_context(&codec_ctx_ptr_);
            codec_ctx_ptr_ = nullptr;
            return false;
        }
    }
    
    // 5. 打开解码器
    ret = avcodec_open2(codec_ctx_ptr_, codec, codec_options_ptr_ ? &codec_options_ptr_ : nullptr);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, " Failed to open codec (FFmpeg: %s)", err_buf);
        avcodec_free_context(&codec_ctx_ptr_);
        codec_ctx_ptr_ = nullptr;
        return false;
    }
    
    LOG4CPLUS_DEBUG(logger_, " Initialized decoder");
    LOG4CPLUS_INFO_FMT(logger_, "   Codec: %s", codec_ctx_ptr_->codec->name);
    LOG4CPLUS_INFO_FMT(logger_, "   Stream resolution: %dx%d", codec_ctx_ptr_->width, codec_ctx_ptr_->height);
    LOG4CPLUS_INFO_FMT(logger_, "   Output resolution: %dx%d", output_width_, output_height_);
    
    return true;
}

bool FFmpegDecodeWorker::configureSpecialDecoder() {
    // 配置 h264_taco 解码器（从 worker_config_ 读取配置）
    if (!codec_ctx_ptr_->priv_data) {
        LOG4CPLUS_WARN_FMT(logger_, "  Warning: codec_ctx->priv_data is NULL, cannot set options");
        return false;
    }
    
    TacoConfig* taco_ptr = tacoDecoderConfig(worker_config_.decoder);
    if (!taco_ptr) {
        LOG4CPLUS_ERROR(logger_, " configureSpecialDecoder: no TACO vendor config (decoder.vendor)");
        return false;
    }
    TacoConfig& taco = *taco_ptr;
    
    LOG4CPLUS_DEBUG_FMT(logger_, " Configuring h264_taco decoder options from config...");
    
    int ret;
    
    // 禁用重排序（从 config 读取）
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "reorder_disable", 
                         taco.reorder_disable ? 1 : 0, 0);
    LOG4CPLUS_DEBUG_FMT(logger_, "    reorder_disable=%d: %s", taco.reorder_disable ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    // 启用通道（从 config 读取）
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_enable", 
                         taco.ch0_enable ? 1 : 0, 0);
    LOG4CPLUS_DEBUG_FMT(logger_, "    ch0_enable=%d: %s", taco.ch0_enable ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_enable", 
                         taco.ch1_enable ? 1 : 0, 0);
    LOG4CPLUS_DEBUG_FMT(logger_, "    ch1_enable=%d: %s", taco.ch1_enable ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    // ========== 通道0配置 ==========
    
    // 配置通道0裁剪参数（从 config 读取）
    if (taco.ch0_crop_width > 0 && taco.ch0_crop_height > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_x", taco.ch0_crop_x, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_y", taco.ch0_crop_y, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_width", taco.ch0_crop_width, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_height", taco.ch0_crop_height, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "    ch0_crop: (%d, %d, %d, %d)", 
               taco.ch0_crop_x, taco.ch0_crop_y, 
               taco.ch0_crop_width, taco.ch0_crop_height);
    }
    
    // 配置通道0缩放参数（从 config 读取）
    if (taco.ch0_scale_width > 0 && taco.ch0_scale_height > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_scale_width", taco.ch0_scale_width, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_scale_height", taco.ch0_scale_height, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "    ch0_scale: (%d, %d)", taco.ch0_scale_width, taco.ch0_scale_height);
    }
    
    // ========== 通道1配置 ==========
    
    // 配置通道1裁剪参数（从 config 读取）
    if (taco.ch1_crop_width > 0 && taco.ch1_crop_height > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_x", taco.ch1_crop_x, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_y", taco.ch1_crop_y, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_width", taco.ch1_crop_width, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_height", taco.ch1_crop_height, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "    ch1_crop: (%d, %d, %d, %d)", 
               taco.ch1_crop_x, taco.ch1_crop_y, 
               taco.ch1_crop_width, taco.ch1_crop_height);
    }
    
    // ⭐ 配置通道1缩放参数（从 config 读取）
    // ⚠️ TACO 硬件限制：只能缩小，不能放大
    if (taco.ch1_scale_width > 0 && taco.ch1_scale_height > 0) {
        // 验证缩放配置是否超出原始分辨率
        int orig_width = getSourceWidth();
        int orig_height = getSourceHeight();
        if (taco.ch1_scale_width > orig_width || taco.ch1_scale_height > orig_height) {
            LOG4CPLUS_WARN(logger_, "═══════════════════════════════════════════════════════════════");
            LOG4CPLUS_WARN(logger_, "  ⚠️  TACO 硬件缩放限制：只能缩小，不能放大");
            LOG4CPLUS_WARN(logger_, "═══════════════════════════════════════════════════════════════");
            LOG4CPLUS_WARN_FMT(logger_, "  原始分辨率: %dx%d", orig_width, orig_height);
            LOG4CPLUS_WARN_FMT(logger_, "  请求分辨率: %dx%d (超出限制)", 
                         taco.ch1_scale_width, taco.ch1_scale_height);
            LOG4CPLUS_WARN_FMT(logger_, "  自动回退：使用原始分辨率 %dx%d", orig_width, orig_height);
            LOG4CPLUS_WARN(logger_, "═══════════════════════════════════════════════════════════════");
            
            // 清除缩放配置，使用原始分辨率
            taco.ch1_scale_width = 0;
            taco.ch1_scale_height = 0;
        } else {
            // 配置有效，设置缩放参数
            av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_scale_width", taco.ch1_scale_width, 0);
            av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_scale_height", taco.ch1_scale_height, 0);
            LOG4CPLUS_DEBUG_FMT(logger_, "    ch1_scale: (%d, %d)", taco.ch1_scale_width, taco.ch1_scale_height);
        }
    }
    
    // 配置通道1 RGB（从 config 读取）
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_rgb", 
                         taco.ch1_rgb ? 1 : 0, 0);
    LOG4CPLUS_DEBUG_FMT(logger_, "    ch1_rgb=%d: %s", taco.ch1_rgb ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    // ⭐ v2.17: 设置 RGB 格式（使用整型枚举）
    if (taco.ch1_rgb && taco.ch1_rgb_format > 0) {
        ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_rgb_format", 
                             taco.ch1_rgb_format, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "    ch1_rgb_format=%d: %s", taco.ch1_rgb_format, 
               ret < 0 ? "FAILED" : "OK");
    }
    
    // ⭐ v2.17: 设置颜色标准（使用整型枚举）
    if (taco.ch1_rgb && taco.ch1_rgb_std > 0) {
        ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_rgb_std", 
                             taco.ch1_rgb_std, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "    ch1_rgb_std=%d: %s", taco.ch1_rgb_std, 
               ret < 0 ? "FAILED" : "OK");
    }
    
    return true;
}

// ============================================================================
// 硬件解码器元数据提取（重写基类虚函数）
// ============================================================================

bool FFmpegDecodeWorker::extractHardwareAddressFromMetadata(AVFrame* frame, Buffer* buffer) {
    // ⭐ 职责：从 AVFrame 中提取硬件解码器的物理内存地址
    // 
    // 设计原则：
    // 1. 此函数只在 decoder_name_ 非空 && use_hardware_decoder_==true 时被调用
    // 2. 不同硬件解码器有不同的提取方式
    // 3. 提取失败返回 false，调用者会报错并终止解码
    
    if (!frame || !buffer) {
        LOG4CPLUS_ERROR(logger_, " extractHardwareAddressFromMetadata: Invalid parameters");
        return false;
    }
    
    if (decoder_name_.find("taco") != std::string::npos) {
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
                    LOG4CPLUS_ERROR_FMT(logger_, " TACO: Failed to convert blk_id=%u to physical address", blk_id);
                    return false;
                }
            }
        }
        
        // ❌ TACO 解码器但没有 metadata（异常情况）
        LOG4CPLUS_ERROR(logger_, " TACO: AVFrame->metadata is missing or no 'pool_blk_id' entry");
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
    //     // QSV 特定逻辑：从 AVFrame 的 data[3] 获取 mfxFrameSurface1*
    //     // mfxFrameSurface1* surface = (mfxFrameSurface1*)frame->data[3];
    //     // buffer->setPhysicalAddress((uint64_t)surface->Data.MemId);
    //     // return true;
    // }
    
    // ⭐ v2.18 改进：软件解码器不需要物理地址
    if (decoder_name_.empty() || !use_hardware_decoder_) {
        // 软件解码器，不需要物理地址
        LOG4CPLUS_DEBUG(logger_, " Software decoder: No hardware address needed");
        return true;  // ✅ 软件解码器返回 true（不是错误）
    }
    
    // 未识别的硬件解码器
    LOG4CPLUS_ERROR_FMT(logger_, " Unknown hardware decoder '%s', cannot extract physical address", 
                 decoder_name_.c_str());
    return false;
}




AVPixelFormat FFmpegDecodeWorker::getSourcePixelFormat() const {
    return packet_source_ ? packet_source_->getSourcePixelFormat() : AV_PIX_FMT_NONE;
}
