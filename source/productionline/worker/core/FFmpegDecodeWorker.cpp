#include "productionline/worker/core/FFmpegDecodeWorker.hpp"
#include "vendor/contracts/DecoderVendorExtension.hpp"
#include "productionline/worker/datasource/encodeddata/EncodedPacketSourceFromRtsp.hpp"
#include "productionline/worker/datasource/encodeddata/EncodedPacketSourceFromBuffer.hpp"
#include "productionline/worker/datasource/encodeddata/EncodedPacketSourceFromFile.hpp"
#include "common/Logger.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include <string.h>
#include <algorithm>
#include <chrono>
#include <climits>  // for INT_MAX

// FFmpeg headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include "ta_sys_api.h"
}

static constexpr size_t MAX_CACHED_FRAMES = 4;

// ============ 构造/析构 ============

// 构造函数（v3.0：统一的 FFmpeg 解码 Worker，支持文件/RTSP/Buffer 模式）
FFmpegDecodeWorker::FFmpegDecodeWorker(const WorkerConfig& config)
    : WorkerBase(BufferPoolBuilderFactory::AllocatorType::AVFRAME, config)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Decode")))
    , datasource_(nullptr)  // ⚠️ 数据源将在下面根据配置创建
    , codec_ctx_ptr_(nullptr)
    , output_width_(config.decoder.vendor ? config.decoder.vendor->getOutputWidth() : 0)
    , output_height_(config.decoder.vendor ? config.decoder.vendor->getOutputHeight() : 0)
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
            datasource_ = config.data_source.shared_packet_source;
            LOG4CPLUS_INFO(logger_, "⭐ v2.22 使用共享 EncodedPacketSource（MultiWorker 共享模式）");
        } else {
            // ✅ 普通模式：创建独立的 EncodedPacketSourceFromBuffer 实例（ONE_TO_ONE）
            if (config.data_source.codec_params) {
                datasource_ = std::make_shared<EncodedPacketSourceFromBuffer>(config.data_source.codec_params);
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
            datasource_ = std::make_shared<EncodedPacketSourceFromRtsp>(path, config.data_source.max_frames);
            LOG4CPLUS_DEBUG_FMT(logger_, "Created EncodedPacketSourceFromRtsp for '%s' (max_frames=%d)", 
                               path.c_str(), config.data_source.max_frames);
        } else {
            // 文件：其他路径视为本地文件
            datasource_ = std::make_shared<EncodedPacketSourceFromFile>(path, config.data_source.max_frames);
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
    // 2. 手动调用 builder_->destroyPool() 释放所有 AVFrame
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
        builder_->destroyPool();  // 释放所有 Pool 中的 Buffer 和 AVFrame
        clearAllBufferPools();
    }
    
    // 步骤3：再关闭解码器和数据源（此时 AVFrame 已全部释放）
    if (datasource_ && datasource_->isOpen()) {
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
    if (datasource_ && datasource_->isOpen() && datasource_->getDataSourceType() != IEncodedPacketSource::SourceType::BUFFER_SOURCE) {
        LOG4CPLUS_WARN(logger_, " ⚠️  Stream already open, closing previous stream");
        close();
    }
    
    // ⭐ v2.18 重构：统一处理，不区分 Buffer/RTSP/File 模式
    // 数据源应该在构造函数中已经创建
    if (!datasource_) {
        LOG4CPLUS_ERROR(logger_, " Cannot open: packet source is nullptr. Worker must be created with WorkerConfig");
        return false;
    }

    // 每次 open 都重新开始 EOF drain 状态
    flush_sent_ = false;
    cached_frames_.clear();
    
    // 从 vendor extension 获取 PP 输出分辨率
    int width = output_width_;
    int height = output_height_;
    bool is_buffer_mode = worker_config_.data_source.buffer_mode;
    
    // 打印模式信息
    if (!is_buffer_mode) {
        LOG4CPLUS_INFO(logger_, "");
        LOG4CPLUS_INFO_FMT(logger_, "📡 Opening video source: %s", worker_config_.data_source.path.c_str());
    } else {
        LOG4CPLUS_INFO(logger_, " 📦 Opening EncodedPacketSourceFromBuffer (Buffer mode)");
    }
    
    // 1. 打开数据源
    if (!datasource_->open()) {
        LOG4CPLUS_ERROR(logger_, " Failed to open packet source");
        return false;
    }
    
    // 2. 从数据源获取编解码器参数
    const AVCodecParameters* codecpar = datasource_->getCodecParameters();
    if (!codecpar) {
        LOG4CPLUS_ERROR(logger_, " Failed to get codec parameters from packet source");
        datasource_->close();
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
            datasource_->close();
            return false;
        }
        if (width < MIN_RESOLUTION || height < MIN_RESOLUTION) {
            LOG4CPLUS_ERROR_FMT(logger_, " Resolution %dx%d too small (minimum: %dx%d)", 
                          width, height, MIN_RESOLUTION, MIN_RESOLUTION);
            datasource_->close();
            return false;
        }
        if (width > MAX_RESOLUTION || height > MAX_RESOLUTION) {
            LOG4CPLUS_ERROR_FMT(logger_, " Resolution %dx%d too large (maximum: %dx%d)", 
                          width, height, MAX_RESOLUTION, MAX_RESOLUTION);
            datasource_->close();
            return false;
        }
        
        output_width_ = width;
        output_height_ = height;
        LOG4CPLUS_DEBUG_FMT(logger_, " Output resolution from config: %dx%d", output_width_, output_height_);
    }
    
    // 5. 初始化解码器
    if (!initializeDecoder(codecpar)) {
        datasource_->close();
        return false;
    }
    
    // 6. 防御性处理：buffer_count 必须 > 0，否则使用默认值
    //    ⭐ v2.78 修复：当 consumer 以 buffer_mode 创建时，原 config 的 buffer_count
    //    可能为 0（DataSourceConfig 默认值）。旧的 configureSpecialDecoder() 中有隐式
    //    处理，重构后该路径被移除，导致 allocatePoolWithBuffers(0, ...) 失败。
    //    与 FFmpegEncodeWorker 保持一致的防御逻辑。
    int buffer_count = worker_config_.data_source.buffer_count;
    if (buffer_count <= 0) {
        buffer_count = WorkerConfig::DataSourceConfig::kDefaultBufferCount;
        LOG4CPLUS_DEBUG_FMT(logger_, " buffer_count was %d, using default: %d",
                      worker_config_.data_source.buffer_count, buffer_count);
    }
    
    // 7. 生成 BufferPool 名称
    std::string pool_name;
    if (is_buffer_mode) {
        pool_name = "FFmpegDecodeWorker_BufferMode";
    } else {
        pool_name = std::string("FFmpegDecodeWorker_") + worker_config_.data_source.path;
    }
    
    uint64_t pool_id = builder_->allocatePoolWithBuffers(
        buffer_count,
        0,
        pool_name,
        is_buffer_mode ? "BUFFER_MODE" : "NORMAL_MODE"
    );
    
    if (pool_id == 0) {
        LOG4CPLUS_ERROR(logger_, " Failed to create BufferPool via Allocator");
        datasource_->close();
        return false;
    }
    
    // 7. ✅ v2.18 修复：统一注册 BufferPool（Buffer 和 RTSP 模式都需要）
    if (!registerBufferPool(BufferPoolType::DECODE_VIDEO_PRIMARY, pool_id)) {
        LOG4CPLUS_ERROR(logger_, " Failed to register BufferPool");
        datasource_->close();
        return false;
    }
    
    // 8. 从 Registry 获取 Pool 名称
    auto pool_weak = ComponentTopology::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    std::string actual_pool_name = pool ? pool->getName() : "Unknown";
    
    decoded_frames_ = 0;
    dropped_frames_ = 0;
    
    // 9. 详细日志输出
    const char* mode_str = is_buffer_mode ? "Buffer mode" : 
        (datasource_->getDataSourceType() == IEncodedPacketSource::SourceType::NETWORK_SOURCE ? "RTSP stream" : "File");
    LOG4CPLUS_DEBUG_FMT(logger_, " FFmpegDecodeWorker (%s): Opened", mode_str);
    if (!is_buffer_mode) {
        LOG4CPLUS_DEBUG_FMT(logger_, "    Source: %s", worker_config_.data_source.path.c_str());
    }
    LOG4CPLUS_DEBUG_FMT(logger_, "    Output resolution: %dx%d (%.1f bytes/pixel)", 
                  output_width_, output_height_, getOutputBytesPerPixel());
    LOG4CPLUS_DEBUG_FMT(logger_, "    Codec: %s", codec_ctx_ptr_->codec->name);
    LOG4CPLUS_DEBUG_FMT(logger_, "    BufferPool: '%s' (ID: %lu, %d buffers)", 
                  actual_pool_name.c_str(), pool_id, 
                  buffer_count);
    
    return true;
}

// ============ v2.13 EncodedPacketSourceFromBuffer 配置 ============

bool FFmpegDecodeWorker::setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) {
    // 检查是否是 EncodedPacketSourceFromBuffer
    auto* buffer_source = dynamic_cast<EncodedPacketSourceFromBuffer*>(datasource_.get());
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
    if (!datasource_ || !datasource_->isOpen()) {
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
            datasource_->commitEncodedPacket(this);
            packet_acquired_ = false;
            current_packet_ptr_ = nullptr;
        }
        
        // ⭐ v2.12新增：关闭数据源
        if (datasource_) {
            datasource_->close();
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
    if (!datasource_) {
        return false;
    }
    return datasource_->isOpen();
}


bool FFmpegDecodeWorker::seek(int frame_index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 1. 参数校验
    if (!datasource_) {
        LOG4CPLUS_ERROR(logger_, " Cannot seek: packet source is nullptr");
        return false;
    }
    
    if (!datasource_->isOpen()) {
        LOG4CPLUS_ERROR(logger_, " Cannot seek: worker is not open");
        return false;
    }
    
    // 2. 委托给数据源实现 seek（多态调用）
    //    - EncodedPacketSourceFromFile: 实现真正的 seek
    //    - EncodedPacketSourceFromRtsp: 返回 false（不支持）
    //    - EncodedPacketSourceFromBuffer: 返回 false（不支持）
    if (!datasource_->seek(frame_index)) {
        // 根据数据源类型返回适当的日志
        if (datasource_->getDataSourceType() == IEncodedPacketSource::SourceType::NETWORK_SOURCE) {
            LOG4CPLUS_WARN(logger_, " RTSP stream does not support seeking");
        } else if (datasource_->getDataSourceType() == IEncodedPacketSource::SourceType::BUFFER_SOURCE) {
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
        datasource_->commitEncodedPacket(this);
        packet_acquired_ = false;
        current_packet_ptr_ = nullptr;
    }

    // 4. 重置解码器状态
    bool was_draining = flush_sent_;
    flush_sent_ = false;

    if (codec_ctx_ptr_ && !use_hardware_decoder_) {
        // 软件解码器：avcodec_flush_buffers 清空内部缓冲并退出 draining mode
        avcodec_flush_buffers(codec_ctx_ptr_);
    } else if (codec_ctx_ptr_ && use_hardware_decoder_ && was_draining) {
        // 硬件解码器（h264_taco）在 draining mode（已发送过 NULL packet）：
        //   avcodec_flush_buffers 会将 taco 通道置为 STOPPED（state=5）且不重启，
        //   所以必须关闭并重建 codec context 来彻底重置解码器。
        LOG4CPLUS_DEBUG(logger_, " Hardware decoder was in draining mode, re-initializing codec...");
        const AVCodecParameters* codecpar = datasource_->getCodecParameters();
        avcodec_free_context(&codec_ctx_ptr_);
        codec_ctx_ptr_ = nullptr;
        if (!initializeDecoder(codecpar)) {
            LOG4CPLUS_ERROR(logger_, " Failed to re-initialize hardware decoder after seek");
            return false;
        }
        LOG4CPLUS_DEBUG(logger_, " Hardware decoder re-initialized successfully");
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
    if (datasource_) {
        return datasource_->getTotalFrames();
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
    if (datasource_) {
        return datasource_->getFileSize();
    }
    return -1;
}

int FFmpegDecodeWorker::getSourceWidth() const {
    return datasource_ ? datasource_->getSourceWidth() : 0;
}

int FFmpegDecodeWorker::getSourceHeight() const {
    return datasource_ ? datasource_->getSourceHeight() : 0;
}

int FFmpegDecodeWorker::getOutputWidth() const {
    return output_width_;
}

int FFmpegDecodeWorker::getOutputHeight() const {
    return output_height_;
}

double FFmpegDecodeWorker::getOutputBytesPerPixel(int channel) const {
    // 1. 硬件解码器：通过厂商扩展多态获取（不依赖具体厂商名称）
    if (use_hardware_decoder_ && codec_ctx_ptr_ && codec_ctx_ptr_->priv_data &&
        worker_config_.decoder.vendor) {
        double bpp = worker_config_.decoder.vendor->getChannelBytesPerPixel(
            channel, codec_ctx_ptr_->priv_data, codec_ctx_ptr_->pix_fmt);
        if (bpp > 0.0) return bpp;
    }

    // 2. 通用平台（软件解码器等）
    if (channel == 0) {
        if (codec_ctx_ptr_ && codec_ctx_ptr_->pix_fmt != AV_PIX_FMT_NONE) {
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(codec_ctx_ptr_->pix_fmt);
            if (desc) {
                return av_get_bits_per_pixel(desc) / 8.0;
            }
        }
        return 1.5;  // Fallback: YUV420
    }

    return 0.0;
}

std::string FFmpegDecodeWorker::getPath() const {
    // ⭐ v2.12修改：从数据源获取
    if (!datasource_) {
        return std::string();
    }
    return datasource_->getPath();
}

IDataSourceNavigator::SourceType FFmpegDecodeWorker::getDataSourceType() const {
    if (datasource_) {
        return datasource_->getDataSourceType();
    }
    return SourceType::NETWORK_SOURCE;  // 默认是网络流类型
}

bool FFmpegDecodeWorker::hasMoreFrames() const {
    // ⭐ v2.12修改：从数据源获取 EOF 状态
    if (!datasource_) {
        return false;
    }
    return !datasource_->isAtEnd();
}

bool FFmpegDecodeWorker::isAtEnd() const {
    // ⭐ v2.12修改：从数据源获取 EOF 状态
    if (!datasource_) {
        return true;
    }
    return datasource_->isAtEnd();
}

bool FFmpegDecodeWorker::isConnected() const {
    // 连接状态从数据源判断
    if (!datasource_) {
        return false;
    }
    return datasource_->isOpen();
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
            LOG4CPLUS_WARN_FMT(logger_, " Hardware decoder '%s': Physical address not available (fallback to virtual)",
                         decoder_name_.c_str());
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
        auto acquire_result = datasource_->acquireEncodedPacket(packet_ptr, this);
        
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
            packet_acquired_ = false;
            current_packet_ptr_ = nullptr;
            return FillResult::fromCodec(CodecSendResult::sendFailed());
        }
        // ret 已变为其他错误码，fall through 到下方映射
    }
    
    // v2.35 重构：与 Acquire 层保持一致，直接使用工厂方法映射
    if (ret == 0) {
        return FillResult::success();
    }
    
    // 错误码映射（与 PacketAcquireResult 在 acquireEncodedPacket 中的风格一致）
    // 注意：所有错误路径必须重置 packet_acquired_，否则下一轮 fillBuffer 会跳过
    // acquireEncodedPacket 而使用已释放的 current_packet_ptr_，导致死循环。
    using Result = CodecSendResult;
    packet_acquired_ = false;
    current_packet_ptr_ = nullptr;
    if (ret == AVERROR_EOF)     return FillResult::fromCodec(Result::eof());
    if (ret == AVERROR(EINVAL)) return FillResult::fromCodec(Result::invalidState());
    if (ret == AVERROR(ENOMEM)) return FillResult::fromCodec(Result::allocFailed());
    
    // 其他未识别错误
    char err_buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, err_buf, sizeof(err_buf));
    LOG4CPLUS_ERROR_FMT(logger_, 
        " ERROR: avcodec_send_packet: decode error (ret=%d, %s)", ret, err_buf);
    packet_acquired_ = false;
    current_packet_ptr_ = nullptr;
    if (ret == AVERROR_EXTERNAL) {
        LOG4CPLUS_ERROR(logger_,
            " FATAL: hardware driver returned AVERROR_EXTERNAL, marking driver as dead");
        driver_fatal_ = true;
    }
    return FillResult::fromCodec(Result::decodeError());
}

/**
 * @brief 从解码器收取所有就绪帧到缓存，取第一帧填充 buffer
 *
 * 统一了"正常解码"和"drain"两个路径中相同的 receive_frame 循环逻辑。
 */
FillResult FFmpegDecodeWorker::receiveAndFillBuffer(AVFrame* frame_ptr, Buffer* buffer) {
    bool decoded_at_least_one = false;
    CodecSendResult receive_result = CodecSendResult::success();

    while (true) {
        if (cached_frames_.size() >= MAX_CACHED_FRAMES) {
            receive_result = CodecSendResult::eagain();
            break;
        }

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

        av_frame_free(&temp_frame);

        if (ret == AVERROR(EAGAIN)) {
            receive_result = CodecSendResult::eagain();
        } else if (ret == AVERROR_EOF) {
            receive_result = CodecSendResult::eof();
        } else if (ret == AVERROR(EINVAL)) {
            LOG4CPLUS_ERROR(logger_,
                " ERROR: avcodec_receive_frame: EINVAL - codec not opened or is an encoder");
            receive_result = CodecSendResult::invalidState();
        } else if (ret == AVERROR_INPUT_CHANGED) {
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

    if (!decoded_at_least_one) {
        return FillResult::fromCodec(receive_result);
    }

    if (cached_frames_.empty()) {
        return FillResult::internalError();
    }

    AVFrame* first_frame = cached_frames_.front();
    cached_frames_.erase(cached_frames_.begin());

    av_frame_move_ref(frame_ptr, first_frame);
    av_frame_free(&first_frame);

    fillBufferMetadataFromFrame(frame_ptr, buffer);
    return FillResult::success();
}

/**
 * @brief 填充 Buffer（解码一帧）
 * 
 * v2.33 变更：返回类型从 bool 改为 FillResult
 */
FillResult FFmpegDecodeWorker::fillBuffer(int frame_index, Buffer* buffer) {
    (void)frame_index;  // 未使用
    
    if (driver_fatal_) {
        return FillResult::fromCodec(CodecSendResult::decodeError());
    }
    
    // ========== 参数校验 ==========
    if (!buffer) {
        LOG4CPLUS_ERROR(logger_, " ERROR: buffer is nullptr");
        return FillResult::invalidParam();
    }
    
    if (!datasource_ || !datasource_->isOpen()) {
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
            return receiveAndFillBuffer(buffer->getAVFrame(), buffer);
        }

        return send_result;
    }
   
    // ========== 步骤3: 从解码器收取帧并填充 buffer ==========
    FillResult recv_result = receiveAndFillBuffer(frame_ptr, buffer);
    
    if (!recv_result.ok()) {
        // 无帧可填：处理 packet 生命周期
        if (!worker_config_.data_source.deferred_commit) {
            // 注意：当 codec 返回 eagain 时，packet 已经被 avcodec_send_packet 消费；
            // 共享模式必须 commit 以推进共享版本，否则 fetch 线程可能卡在同一 version。
            if (recv_result.codecCause() == CodecStatus::Eagain) {
                datasource_->commitEncodedPacket(this);
            } else {
                datasource_->cancelEncodedPacket(this);
            }
        }
        packet_acquired_ = false;
        current_packet_ptr_ = nullptr;
        return recv_result;
    }
    
    // ========== 步骤4: 成功解码，提交 packet ==========
    if (packet_acquired_) {
        if (!worker_config_.data_source.deferred_commit) {
            datasource_->commitEncodedPacket(this);
        }
        packet_acquired_ = false;
        current_packet_ptr_ = nullptr;
    }
    
    return recv_result;
}

// ============================================================================
// 提供原材料（BufferPool）
// ============================================================================

// ============ 特有接口 ============

const AVCodecParameters* FFmpegDecodeWorker::getCodecParameters() const {
    if (!datasource_) {
        return nullptr;
    }
    return datasource_->getCodecParameters();
}

AVRational FFmpegDecodeWorker::getTimeBase() const {
    if (!datasource_) {
        return {1, 25};  // 默认值
    }
    
    // 从数据源获取编解码器参数
    const AVCodecParameters* codecpar = datasource_->getCodecParameters();
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

// ============ 内部实现 ============

bool FFmpegDecodeWorker::initializeDecoder(const AVCodecParameters* codec_params) {
    // ⭐ v2.12修改：codec_params 必须提供（从 datasource_ 获取）
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
    
    // 4. 配置硬件解码器厂商扩展（vendor-agnostic）
    //    判定条件：实际打开的 codec 是硬件解码器 + 配置了厂商扩展
    //    ⭐ v3.1: 通过 IDecoderVendorExtension::applyToCodecContext() 委托，
    //          Worker 核心不再包含任何厂商特有的 PP 配置逻辑
    if (isHardwareDecoder(codec) && worker_config_.decoder.vendor) {
        if (!worker_config_.decoder.vendor->applyToCodecContext(
                codec_ctx_ptr_->priv_data,
                codec_ctx_ptr_->width,
                codec_ctx_ptr_->height)) {
            LOG4CPLUS_ERROR(logger_, " ERROR: Vendor decoder extension rejected configuration");
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

// configureSpecialDecoder() has been removed.
// PP configuration logic migrated to IDecoderVendorExtension::applyToCodecContext()
// (see TacoDecoderExtension.cpp)



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
        
        // TACO 解码器但没有 metadata（MJPEG 等部分编解码器可能不提供）
        if (decoder_name_.find("mjpeg") != std::string::npos) {
            LOG4CPLUS_DEBUG(logger_, " TACO MJPEG: No pool_blk_id metadata (expected for MJPEG)");
        } else {
            LOG4CPLUS_WARN_FMT(logger_, " TACO %s: AVFrame->metadata is missing or no 'pool_blk_id' entry",
                decoder_name_.c_str());
        }
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
    return datasource_ ? datasource_->getSourcePixelFormat() : AV_PIX_FMT_NONE;
}
