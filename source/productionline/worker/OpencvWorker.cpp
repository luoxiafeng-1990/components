#include "productionline/worker/OpencvWorker.hpp"
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
OpencvWorker::OpencvWorker(const WorkerConfig& config)
    : WorkerBase(BufferAllocatorFactory::AllocatorType::MAT, config)  // 传递 config 给父类
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
    , current_packet_ptr_(nullptr)
    , packet_acquired_(false)
{
    LOG4CPLUS_DEBUG(logger_, "[Worker] OpencvWorker created with config");
    
    if (config.data_source.buffer_mode) {
        if (config.data_source.shared_packet_source) {
            packet_source_ = config.data_source.shared_packet_source;
            LOG4CPLUS_INFO(logger_, "⭐ v2.22 使用共享 EncodedPacketSource（MultiWorker 共享模式）");
        } else {
            if (config.data_source.codec_params) {
                packet_source_ = std::make_shared<EncodedPacketSourceFromBuffer>(config.data_source.codec_params);
                LOG4CPLUS_DEBUG(logger_, "Created EncodedPacketSourceFromBuffer (v2.20: 需要调用 setSourceBufferPool 关联源 Pool)");
            } else {
                LOG4CPLUS_WARN(logger_, "buffer_mode=true but codec_params is nullptr");
            }
        }
    } else {
        const std::string& path = config.data_source.path;
        if (path.empty()) {
            LOG4CPLUS_WARN(logger_, "data_source.path is empty, cannot create packet source");
        } else if (path.rfind("rtsp://", 0) == 0 || path.rfind("rtsps://", 0) == 0) {
            packet_source_ = std::make_shared<EncodedPacketSourceFromRtsp>(path);
            LOG4CPLUS_DEBUG_FMT(logger_, "Created EncodedPacketSourceFromRtsp for '%s'", path.c_str());
        } else {
            packet_source_ = std::make_shared<EncodedPacketSourceFromFile>(path);
            LOG4CPLUS_DEBUG_FMT(logger_, "Created EncodedPacketSourceFromFile for '%s'", path.c_str());
        }
    }
}

OpencvWorker::~OpencvWorker() {
    LOG4CPLUS_DEBUG(logger_, "🧹 Destroying OpencvWorker...");
    
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
    
    LOG4CPLUS_DEBUG(logger_, "🧹 OpencvWorker destroyed");
}

bool OpencvWorker::open(const char* path) {
    open();
    return true;
}

bool OpencvWorker::open() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 如果已经打开，先关闭
    if (packet_source_ && packet_source_->isOpen() && packet_source_->getDataSourceType() != IEncodedPacketSource::SourceType::BUFFER_SOURCE) {
        LOG4CPLUS_WARN(logger_, "[Worker] ⚠️  Stream already open, closing previous stream");
        close();
    }
    
    if (!packet_source_) {
        LOG4CPLUS_ERROR(logger_, "[Worker] Cannot open: packet source is nullptr. Worker must be created with WorkerConfig");
        return false;
    }
    
    // 从配置读取输出参数
    int width = worker_config_.display.width;
    int height = worker_config_.display.height;
    bool is_buffer_mode = worker_config_.data_source.buffer_mode;
    
    // 打印模式信息
    if (!is_buffer_mode) {
        LOG4CPLUS_INFO(logger_, "");
        LOG4CPLUS_INFO_FMT(logger_, "📡 Opening video source: %s", worker_config_.data_source.path.c_str());
    } else {
        LOG4CPLUS_INFO(logger_, "[Worker] 📦 Opening EncodedPacketSourceFromBuffer (Buffer mode)");
    }
    
    // 1. 打开数据源
    if (!packet_source_->open()) {
        LOG4CPLUS_ERROR(logger_, "[Worker] Failed to open packet source");
        return false;
    }
    
    // 2. 从数据源获取编解码器参数
    const AVCodecParameters* codecpar = packet_source_->getCodecParameters();
    if (!codecpar) {
        LOG4CPLUS_ERROR(logger_, "[Worker] Failed to get codec parameters from packet source");
        packet_source_->close();
        return false;
    }
    
    // 3. 检查编解码器类型是否匹配
    checkCodecMismatch(codecpar->codec_id, decoder_name_);
    
    // 4. 设置输出分辨率（必须在 initializeDecoder 之前，因为解码器初始化时会打印分辨率）
    if (width == 0 || height == 0) {
        // 配置未设置，使用原始分辨率或默认值
        output_width_ = getSourceWidth() > 0 ? getSourceWidth() : 1920;
        output_height_ = getSourceHeight() > 0 ? getSourceHeight() : 1080;
        LOG4CPLUS_DEBUG_FMT(logger_, "[Worker] Output resolution not set in config, using: %dx%d", 
                      output_width_, output_height_);
    } else {        
        output_width_ = width;
        output_height_ = height;
        LOG4CPLUS_DEBUG_FMT(logger_, "[Worker] Output resolution from config: %dx%d", output_width_, output_height_);
    }
    
    // 5. 初始化解码器
    if (!initializeDecoder(codecpar)) {
        packet_source_->close();
        return false;
    }
    
    // 6. 生成 BufferPool 名称
    std::string pool_name;
    if (is_buffer_mode) {
        pool_name = "OpencvWorker_BufferMode";
    } else {
        pool_name = std::string("OpencvWorker_") + worker_config_.data_source.path;
    }
    
    uint64_t pool_id = allocator_facade_.allocatePoolWithBuffers(
        worker_config_.data_source.buffer_count,
        0,
        pool_name,
        is_buffer_mode ? "BUFFER_MODE" : "NORMAL_MODE"
    );
    
    if (pool_id == 0) {
        LOG4CPLUS_ERROR(logger_, "[Worker] Failed to create BufferPool via Allocator");
        packet_source_->close();
        return false;
    }
    
    // 7. ✅ v2.18 修复：统一注册 BufferPool（Buffer 和 RTSP 模式都需要）
    if (!registerBufferPool(BufferPoolType::DECODE_VIDEO_PRIMARY, pool_id)) {
        LOG4CPLUS_ERROR(logger_, "[Worker] Failed to register BufferPool");
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
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker] OpencvWorker (%s): Opened", mode_str);
    if (!is_buffer_mode) {
        LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    Source: %s", worker_config_.data_source.path.c_str());
    }
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    Output resolution: %dx%d (%.1f bytes/pixel)", 
                  output_width_, output_height_, getOutputBytesPerPixel());
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    Codec: %s", codec_ctx_ptr_->codec->name);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    BufferPool: '%s' (ID: %lu, %d buffers)", 
                  actual_pool_name.c_str(), pool_id, 
                  worker_config_.data_source.buffer_count);
    
    return true;
}

/*
bool OpencvWorker::setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) {
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
*/

void OpencvWorker::close() {
    // ⚠️ 注意：打开状态由数据源管理
    if (!packet_source_ || !packet_source_->isOpen()) {
        return;  // 已经关闭过了
    }
    
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        LOG4CPLUS_INFO(logger_, "");
        LOG4CPLUS_INFO(logger_, "🛑 Closing video source...");
        
        // ⭐ v2.22 新增：清理未提交的 packet（Buffer 模式）
        if (worker_config_.data_source.buffer_mode && packet_acquired_) {
            auto ps = std::dynamic_pointer_cast<EncodedPacketSourceFromBuffer>(packet_source_);
            if (ps) {
                // 强制提交（避免订阅者计数永久占用）
                LOG4CPLUS_DEBUG(logger_, "[Worker] Cleaning up pending packet on close");
                ps->commitEncodedPacket(this);
            }
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
    
    LOG4CPLUS_DEBUG(logger_, "[Worker] Video source closed");
    LOG4CPLUS_INFO_FMT(logger_, "   Decoded frames: %d", decoded_frames_.load());
    LOG4CPLUS_INFO_FMT(logger_, "   Dropped frames: %d", dropped_frames_.load());
}

bool OpencvWorker::isOpen() const {
    // ⚠️ 注意：打开状态从数据源获取
    if (!packet_source_) {
        return false;
    }
    return packet_source_->isOpen();
}


bool OpencvWorker::seek(int frame_index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 1. 参数校验
    if (!packet_source_) {
        LOG4CPLUS_ERROR(logger_, "[Worker] Cannot seek: packet source is nullptr");
        return false;
    }
    
    if (!packet_source_->isOpen()) {
        LOG4CPLUS_ERROR(logger_, "[Worker] Cannot seek: worker is not open");
        return false;
    }
    
    // 2. 委托给数据源实现 seek（多态调用）
    //    - EncodedPacketSourceFromFile: 实现真正的 seek
    //    - EncodedPacketSourceFromRtsp: 返回 false（不支持）
    //    - EncodedPacketSourceFromBuffer: 返回 false（不支持）
    if (!packet_source_->seek(frame_index)) {
        // 根据数据源类型返回适当的日志
        if (packet_source_->getDataSourceType() == IEncodedPacketSource::SourceType::NETWORK_SOURCE) {
            LOG4CPLUS_WARN(logger_, "[Worker] RTSP stream does not support seeking");
        } else if (packet_source_->getDataSourceType() == IEncodedPacketSource::SourceType::BUFFER_SOURCE) {
            LOG4CPLUS_WARN(logger_, "[Worker] Buffer source does not support seeking");
        } else {
            LOG4CPLUS_ERROR(logger_, "[Worker] Seek failed or not supported by packet source");
        }
        return false;
    }
    
    // 3. seek 成功后，清理解码器状态（flush内部缓冲区）
    if (codec_ctx_ptr_) {
        avcodec_flush_buffers(codec_ctx_ptr_);
    }
    
    // ⚠️ 注意：EOF 状态由数据源的 seek() 自动重置，不需要手动重置
    
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker] Successfully seeked to frame %d", frame_index);
    return true;
}

bool OpencvWorker::seekToBegin() {
    // 委托给 seek(0)
    return seek(0);
}

bool OpencvWorker::seekToEnd() {
    LOG4CPLUS_WARN(logger_, "[Worker] Warning: seekToEnd is not supported");
    return false;
}

bool OpencvWorker::skip(int frame_count) {
    LOG4CPLUS_WARN(logger_, "[Worker] Warning: skip is not supported for streaming sources");
    return false;
}

int OpencvWorker::getTotalFrames() const {
    // ⭐ v2.12修改：从数据源获取（适配器模式）
    if (packet_source_) {
        return packet_source_->getTotalFrames();
    }
    return INT_MAX;
}

int OpencvWorker::getCurrentFrameIndex() const {
    // 返回已解码帧数作为"当前索引"
    return decoded_frames_.load();
}

size_t OpencvWorker::getFrameSize() const {
    // ✅ 使用实际解码输出格式计算（getBytesPerPixel从实际格式获取）
    return (size_t)(output_width_ * output_height_ * getOutputBytesPerPixel());
}

long OpencvWorker::getFileSize() const {
    // ⭐ v2.12修改：从数据源获取
    if (packet_source_) {
        return packet_source_->getFileSize();
    }
    return -1;
}

int OpencvWorker::getSourceWidth() const {
    return packet_source_ ? packet_source_->getSourceWidth() : 0;
}

int OpencvWorker::getSourceHeight() const {
    return packet_source_ ? packet_source_->getSourceHeight() : 0;
}

int OpencvWorker::getOutputWidth() const {
    return output_width_;
}

int OpencvWorker::getOutputHeight() const {
    return output_height_;
}

double OpencvWorker::getOutputBytesPerPixel(int channel) const {
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

double OpencvWorker::getTacoChannelBytesPerPixel(int channel) const {
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

OutputFormat OpencvWorker::mapRgbDriverValueToEnum(int driver_value) {
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

double OpencvWorker::getBytesPerPixelFromFormat(OutputFormat format) {
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

std::string OpencvWorker::getPath() const {
    // ⭐ v2.12修改：从数据源获取
    if (!packet_source_) {
        return std::string();
    }
    return packet_source_->getPath();
}

IDataSourceNavigator::SourceType OpencvWorker::getDataSourceType() const {
    if (packet_source_) {
        return packet_source_->getDataSourceType();
    }
    return SourceType::NETWORK_SOURCE;  // 默认是网络流类型
}

bool OpencvWorker::hasMoreFrames() const {
    // ⭐ v2.12修改：从数据源获取 EOF 状态
    if (!packet_source_) {
        return false;
    }
    return !packet_source_->isAtEnd();
}

bool OpencvWorker::isAtEnd() const {
    // ⭐ v2.12修改：从数据源获取 EOF 状态
    if (!packet_source_) {
        return true;
    }
    return packet_source_->isAtEnd();
}

bool OpencvWorker::isConnected() const {
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
bool OpencvWorker::fillBufferMetadataFromFrame(AVFrame* frame_ptr, Buffer* buffer) {
    // ⭐ 硬件解码器：提取物理内存地址
    if (!decoder_name_.empty() && use_hardware_decoder_) {
        if (!extractHardwareAddressFromMetadata(frame_ptr, buffer)) {
            LOG4CPLUS_ERROR_FMT(logger_, "[Worker] Hardware decoder '%s': Failed to extract physical address",
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
        LOG_TRACE_FMT("[Worker] Updated buffer size to actual frame size: %d bytes", actual_frame_size);
    } else {
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] Failed to get frame buffer size: %d", actual_frame_size);
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
 * @param packet_ptr AVPacket 指针（RTSP/文件模式使用）
 * @return true 成功发送 packet 到解码器，false 失败或 EOF
 * 
 * ⭐ v2.22 修改：
 *   - Buffer 模式的逻辑已移至 fillBuffer() 中
 *   - 此函数仅用于 RTSP/文件模式
 */
bool OpencvWorker::readAndSendPacket(AVPacket* packet_ptr) {
    if (worker_config_.data_source.buffer_mode) {
        auto ps = std::dynamic_pointer_cast<EncodedPacketSourceFromBuffer>(packet_source_);
        if (!ps) {
            LOG4CPLUS_ERROR(logger_, "[Worker] ERROR: Failed to cast to EncodedPacketSourceFromBuffer");
            return false;
        }
        
        // ⭐ v2.22 新增：只在未获取时才尝试获取
        if (!packet_acquired_) {
            current_packet_ptr_ = ps->acquireEncodedPacket(this);  // ✅ 传递 this 指针
            
            if (!current_packet_ptr_) {
                // EOF 或 已获取过当前版本（需要等待新 buffer）
                return false;
            }
            
            packet_acquired_ = true;
        }
        
        // ========== 步骤3: 发送到解码器 ==========
        int ret = avcodec_send_packet(codec_ctx_ptr_, current_packet_ptr_);
        
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            // ❌ 发送失败，取消当前获取
            LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ERROR: avcodec_send_packet failed: %d", ret);
            ps->cancelEncodedPacket(this);
            packet_acquired_ = false;
            current_packet_ptr_ = nullptr;
            return false;
        }

        return true;
    }
    
    // ========== 文件/RTSP 模式：使用 readPacket ==========
    // ⭐ 损坏帧重试机制
    const int AVERROR_INVALIDDATA_VALUE = -1094995529;  // AVERROR(0x41444e49)
    const int MAX_CORRUPTED_RETRIES = 10;  // 最大重试次数，避免无限循环
    
    int corrupted_retries = 0;
    int read_ret;
    
    while (true) {
        // 使用数据源抽象读取 packet
        read_ret = packet_source_->readEncodedPacket(packet_ptr);
        
        if (read_ret < 0) {
            if (read_ret == AVERROR_EOF) {
                LOG4CPLUS_DEBUG(logger_, "🔄 EOF reached");
                av_packet_unref(packet_ptr);
                return false;
            } else if (read_ret == AVERROR_INVALIDDATA_VALUE) {
                // 🔧 遇到损坏帧时，在内部循环跳过，继续读取下一个 packet
                corrupted_retries++;
                if (corrupted_retries <= MAX_CORRUPTED_RETRIES) {
                    LOG4CPLUS_WARN_FMT(logger_, "[Worker] WARNING: Corrupted packet detected (attempt %d/%d), skipping...", 
                           corrupted_retries, MAX_CORRUPTED_RETRIES);
                    av_packet_unref(packet_ptr);
                    // 继续循环，尝试读取下一个 packet
                    continue;
                } else {
                    // 连续多次都是损坏帧，可能文件确实损坏严重，返回失败
                    LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ERROR: Too many corrupted packets (%d), giving up", corrupted_retries);
                    av_packet_unref(packet_ptr);
                    return false;
                }
            } else {
                // 其他错误（非 EOF，非损坏帧）：记录错误并返回
                char err_buf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(read_ret, err_buf, sizeof(err_buf));
                LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ERROR: readEncodedPacket failed: %d (%s)", read_ret, err_buf);
                av_packet_unref(packet_ptr);
                return false;
            }
        } else {
            // 成功读取到 packet，退出循环
            break;
        }
    }
    
    // ⭐ 视频流索引检查（仅文件模式需要）
    if (auto* file_source = dynamic_cast<EncodedPacketSourceFromFile*>(packet_source_.get())) {
        (void)file_source;  // 仅用于类型检查
        if (packet_ptr->stream_index != packet_source_->getVideoStreamIndex()) {
            // 不是视频流的 packet 需要释放，然后继续读取下一个
            av_packet_unref(packet_ptr);
            return false;  // 让调用者再次调用以读取下一个 packet
        }
    }
    // RTSP/Buffer 模式：packet 已经是视频流，不需要检查
    
    // 发送 packet 到解码器
    int ret = avcodec_send_packet(codec_ctx_ptr_, packet_ptr);
    
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ERROR: avcodec_send_packet failed: %d (%s)", ret, err_buf);
        return false;
    }
    
    return true;
}

bool OpencvWorker::fillBuffer(int frame_index, Buffer* buffer) {
    // ========== 参数校验 ==========
    if (!buffer) {
        LOG4CPLUS_ERROR(logger_, "[Worker] ERROR: buffer is nullptr");
        return false;
    }
    
    if (!packet_source_->isOpen()) {
        LOG4CPLUS_ERROR(logger_, "[Worker] ERROR: Worker is not open");
        return false;
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    AVFrame* frame_ptr = buffer->getAVFrame();
    if (!frame_ptr) {
        LOG4CPLUS_ERROR(logger_, "[Worker] ERROR: buffer->getAVFrame() is nullptr");
        return false;
    }
    
    AVPacket* packet_ptr = buffer->getAVPacket();
    if (!packet_ptr) {
        LOG4CPLUS_ERROR(logger_, "[Worker] ERROR: buffer->getAVPacket() is nullptr");
        return false;
    }
    
    if (!packet_source_) {
        LOG4CPLUS_ERROR(logger_, "[Worker] ERROR: packet_source_ is nullptr");
        return false;
    }
    
    // ========== 步骤1: 检查缓存队列 ==========
    if (!cached_frames_.empty()) {
        AVFrame* cached_frame = cached_frames_.front();
        cached_frames_.erase(cached_frames_.begin());
        
        av_frame_move_ref(frame_ptr, cached_frame);
        av_frame_free(&cached_frame);
        
        return fillBufferMetadataFromFrame(frame_ptr, buffer);
    }
    
    if (!readAndSendPacket(packet_ptr)) {
        return false;
    }
   
    // ========== 步骤2: 循环读取所有解码的帧到缓存 ==========
    bool decoded_at_least_one = false;
    
    while (true) {
        AVFrame* temp_frame = av_frame_alloc();
        if (!temp_frame) {
            break;
        }
        
        int ret = avcodec_receive_frame(codec_ctx_ptr_, temp_frame);
        
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0) {
            av_frame_free(&temp_frame);
            break;
        }
        
        // ✅ 成功解码一帧
        decoded_at_least_one = true;
        cached_frames_.push_back(temp_frame);
    }
    
    // ========== 步骤5: 检查是否成功解码 ==========
    if (!decoded_at_least_one) {
        // ❌ 没有解码出帧
        
        if (worker_config_.data_source.buffer_mode) {
            // ⭐ v2.22 新增：Buffer 模式 - 取消获取（下次重试）
            auto ps = std::dynamic_pointer_cast<EncodedPacketSourceFromBuffer>(packet_source_);
            if (ps) {
                ps->cancelEncodedPacket(this);
            }
            packet_acquired_ = false;
            current_packet_ptr_ = nullptr;
            return false;
        }
    }
    
    // ========== 步骤6: 成功解码，提交（释放）==========
    if (worker_config_.data_source.buffer_mode && packet_acquired_) {
        auto ps = std::dynamic_pointer_cast<EncodedPacketSourceFromBuffer>(packet_source_);
        if (ps) {
            // ⭐ v2.22 新增：成功处理，提交释放
            ps->commitEncodedPacket(this);
        }
        
        // 重置状态
        packet_acquired_ = false;
        current_packet_ptr_ = nullptr;
    }
    
    // ========== 步骤7: 从缓存取第一帧填充 buffer ==========
    if (cached_frames_.empty()) {
        return false;  // 不应该到这里
    }
    
    AVFrame* first_frame = cached_frames_.front();
    cached_frames_.erase(cached_frames_.begin());
    
    av_frame_move_ref(frame_ptr, first_frame);
    av_frame_free(&first_frame);
    
    return fillBufferMetadataFromFrame(frame_ptr, buffer);
}

// ============================================================================
// 提供原材料（BufferPool）
// ============================================================================

// ============ 特有接口 ============

const AVCodecParameters* OpencvWorker::getCodecParameters() const {
    if (!packet_source_) {
        return nullptr;
    }
    return packet_source_->getCodecParameters();
}

AVRational OpencvWorker::getTimeBase() const {
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

const char* OpencvWorker::getCodecName() const {
    if (codec_ctx_ptr_ && codec_ctx_ptr_->codec) {
        return codec_ctx_ptr_->codec->name;
    }
    return "unknown";
}

void OpencvWorker::printStats() const {
    LOG4CPLUS_INFO(logger_, "");
    LOG4CPLUS_INFO(logger_, "[Worker] 📊 Statistics:");
    
    // 1. 通用信息
    std::string path = packet_source_ ? packet_source_->getPath() : std::string();
    LOG4CPLUS_INFO_FMT(logger_, "[Worker]    Source: %s", path.empty() ? "(Buffer Mode)" : path.c_str());
    LOG4CPLUS_INFO_FMT(logger_, "[Worker]    Codec: %s", getCodecName());
    LOG4CPLUS_INFO_FMT(logger_, "[Worker]    Resolution: %dx%d → %dx%d", getSourceWidth(), getSourceHeight(), output_width_, output_height_);
    LOG4CPLUS_INFO_FMT(logger_, "[Worker]    Decoded frames: %d", decoded_frames_.load());
    
    // 2. 根据数据源类型显示特定信息
    SourceType type = getDataSourceType();
    if (type == SourceType::FILE_SOURCE) {
        LOG4CPLUS_INFO_FMT(logger_, "[Worker]    Total frames: %d", packet_source_ ? packet_source_->getTotalFrames() : -1);
        LOG4CPLUS_INFO_FMT(logger_, "[Worker]    EOF: %s", packet_source_ && packet_source_->isAtEnd() ? "YES" : "NO");
    } else if (type == SourceType::NETWORK_SOURCE) {
        LOG4CPLUS_INFO_FMT(logger_, "[Worker]    Connected: %s", isConnected() ? "Yes" : "No");
        LOG4CPLUS_INFO_FMT(logger_, "[Worker]    Dropped frames: %d", dropped_frames_.load());
    } else if (type == SourceType::BUFFER_SOURCE) {
        LOG4CPLUS_INFO_FMT(logger_, "[Worker]    Dropped frames: %d", dropped_frames_.load());
    }
    
    // 3. BufferPool 信息（通用）
    uint64_t pool_id = getOutputBufferPoolId(BufferPoolType::DECODE_VIDEO_PRIMARY);
    LOG4CPLUS_INFO_FMT(logger_, "[Worker]    BufferPool ID: %lu", pool_id);
}

// ============ 内部实现 ============

bool OpencvWorker::initializeDecoder(const AVCodecParameters* codec_params) {
    // ⭐ v2.12修改：codec_params 必须提供（从 packet_source_ 获取）
    if (!codec_params) {
        LOG4CPLUS_ERROR(logger_, "[Worker] Cannot initialize decoder: codec_params is nullptr");
        return false;
    }
    const AVCodecParameters* codecpar = codec_params;
    
    // 1. 查找解码器
    const AVCodec* codec = nullptr;
    
    if (!decoder_name_.empty()) {
        // ⭐ 用户指定了解码器名称（如 "h264_taco"）
        codec = avcodec_find_decoder_by_name(decoder_name_.c_str());
        if (!codec) {
            LOG4CPLUS_WARN_FMT(logger_, "[Worker] ⚠️ Warning: Specified decoder '%s' not found", decoder_name_.c_str());
        } else {
            LOG4CPLUS_DEBUG_FMT(logger_, "[Worker] Using specified decoder: %s", decoder_name_.c_str());
            
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
            LOG4CPLUS_INFO(logger_, "[Worker] Searching for pure software decoder...");
            codec = findPureSoftwareDecoder(codecpar->codec_id);
            if (!codec) {
                LOG4CPLUS_ERROR(logger_, "[Worker] No pure software decoder available for this codec!");
                return false;
            }
            LOG4CPLUS_INFO_FMT(logger_, "[Worker] ✅ Using software decoder: %s", codec->name);
        } else {
            // 硬件解码或自动选择：使用 FFmpeg 默认行为
            codec = avcodec_find_decoder(codecpar->codec_id);
            if (!codec) {
                LOG4CPLUS_ERROR(logger_, "[Worker] Decoder not found for codec");
                return false;
            }
            
            // 日志：显示选择的解码器类型
            if (isHardwareDecoder(codec)) {
                LOG4CPLUS_INFO_FMT(logger_, "[Worker] Auto-selected hardware decoder: %s", codec->name);
            } else {
                LOG4CPLUS_INFO_FMT(logger_, "[Worker] Auto-selected software decoder: %s", codec->name);
            }
        }
    }
    
    // 2. 分配解码器上下文
    codec_ctx_ptr_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_ptr_) {
        LOG4CPLUS_ERROR(logger_, "[Worker] Failed to allocate codec context");
        return false;
    }
    
    // 3. 复制参数到解码器上下文
    int ret = avcodec_parameters_to_context(codec_ctx_ptr_, codecpar);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] Failed to copy codec parameters (FFmpeg: %s)", err_buf);
        avcodec_free_context(&codec_ctx_ptr_);
        codec_ctx_ptr_ = nullptr;
        return false;
    }
    
    // 4. 配置特殊解码器（如 h264_taco）
    if (decoder_name_ == "h264_taco") {
        if (!configureSpecialDecoder()) {
            LOG4CPLUS_ERROR(logger_, "[Worker] ERROR: Failed to configure special decoder options");
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
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] Failed to open codec (FFmpeg: %s)", err_buf);
        avcodec_free_context(&codec_ctx_ptr_);
        codec_ctx_ptr_ = nullptr;
        return false;
    }
    
    LOG4CPLUS_DEBUG(logger_, "[Worker] Initialized decoder");
    LOG4CPLUS_INFO_FMT(logger_, "   Codec: %s", codec_ctx_ptr_->codec->name);
    LOG4CPLUS_INFO_FMT(logger_, "   Stream resolution: %dx%d", codec_ctx_ptr_->width, codec_ctx_ptr_->height);
    LOG4CPLUS_INFO_FMT(logger_, "   Output resolution: %dx%d", output_width_, output_height_);
    
    return true;
}

bool OpencvWorker::configureSpecialDecoder() {
    // 配置 h264_taco 解码器（从 worker_config_ 读取配置）
    if (!codec_ctx_ptr_->priv_data) {
        LOG4CPLUS_WARN_FMT(logger_, "[Worker]  Warning: codec_ctx->priv_data is NULL, cannot set options");
        return false;
    }
    
    // 🎯 从 worker_config_ 获取 taco 配置（非 const，可能需要修改）
    auto& taco = worker_config_.decoder.taco;
    
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker] Configuring h264_taco decoder options from config...");
    
    int ret;
    
    // 禁用重排序（从 config 读取）
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "reorder_disable", 
                         taco.reorder_disable ? 1 : 0, 0);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    reorder_disable=%d: %s", taco.reorder_disable ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    // 启用通道（从 config 读取）
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_enable", 
                         taco.ch0_enable ? 1 : 0, 0);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    ch0_enable=%d: %s", taco.ch0_enable ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_enable", 
                         taco.ch1_enable ? 1 : 0, 0);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    ch1_enable=%d: %s", taco.ch1_enable ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    // ========== 通道0配置 ==========
    
    // 配置通道0裁剪参数（从 config 读取）
    if (taco.ch0_crop_width > 0 && taco.ch0_crop_height > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_x", taco.ch0_crop_x, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_y", taco.ch0_crop_y, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_width", taco.ch0_crop_width, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_height", taco.ch0_crop_height, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    ch0_crop: (%d, %d, %d, %d)", 
               taco.ch0_crop_x, taco.ch0_crop_y, 
               taco.ch0_crop_width, taco.ch0_crop_height);
    }
    
    // 配置通道0缩放参数（从 config 读取）
    if (taco.ch0_scale_width > 0 && taco.ch0_scale_height > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_scale_width", taco.ch0_scale_width, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_scale_height", taco.ch0_scale_height, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    ch0_scale: (%d, %d)", taco.ch0_scale_width, taco.ch0_scale_height);
    }
    
    // ========== 通道1配置 ==========
    
    // 配置通道1裁剪参数（从 config 读取）
    if (taco.ch1_crop_width > 0 && taco.ch1_crop_height > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_x", taco.ch1_crop_x, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_y", taco.ch1_crop_y, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_width", taco.ch1_crop_width, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_height", taco.ch1_crop_height, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    ch1_crop: (%d, %d, %d, %d)", 
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
            LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    ch1_scale: (%d, %d)", taco.ch1_scale_width, taco.ch1_scale_height);
        }
    }
    
    // 配置通道1 RGB（从 config 读取）
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_rgb", 
                         taco.ch1_rgb ? 1 : 0, 0);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    ch1_rgb=%d: %s", taco.ch1_rgb ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    // ⭐ v2.17: 设置 RGB 格式（使用整型枚举）
    if (taco.ch1_rgb && taco.ch1_rgb_format > 0) {
        ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_rgb_format", 
                             taco.ch1_rgb_format, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    ch1_rgb_format=%d: %s", taco.ch1_rgb_format, 
               ret < 0 ? "FAILED" : "OK");
    }
    
    // ⭐ v2.17: 设置颜色标准（使用整型枚举）
    if (taco.ch1_rgb && taco.ch1_rgb_std > 0) {
        ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_rgb_std", 
                             taco.ch1_rgb_std, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    ch1_rgb_std=%d: %s", taco.ch1_rgb_std, 
               ret < 0 ? "FAILED" : "OK");
    }
    
    return true;
}

// ============================================================================
// 硬件解码器元数据提取（重写基类虚函数）
// ============================================================================

bool OpencvWorker::extractHardwareAddressFromMetadata(AVFrame* frame, Buffer* buffer) {
    // ⭐ 职责：从 AVFrame 中提取硬件解码器的物理内存地址
    // 
    // 设计原则：
    // 1. 此函数只在 decoder_name_ 非空 && use_hardware_decoder_==true 时被调用
    // 2. 不同硬件解码器有不同的提取方式
    // 3. 提取失败返回 false，调用者会报错并终止解码
    
    if (!frame || !buffer) {
        LOG4CPLUS_ERROR(logger_, "[Worker] extractHardwareAddressFromMetadata: Invalid parameters");
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
                    LOG4CPLUS_ERROR_FMT(logger_, "[Worker] TACO: Failed to convert blk_id=%u to physical address", blk_id);
                    return false;
                }
            }
        }
        
        // ❌ TACO 解码器但没有 metadata（异常情况）
        LOG4CPLUS_ERROR(logger_, "[Worker] TACO: AVFrame->metadata is missing or no 'pool_blk_id' entry");
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
        LOG4CPLUS_DEBUG(logger_, "[Worker] Software decoder: No hardware address needed");
        return true;  // ✅ 软件解码器返回 true（不是错误）
    }
    
    // 未识别的硬件解码器
    LOG4CPLUS_ERROR_FMT(logger_, "[Worker] Unknown hardware decoder '%s', cannot extract physical address", 
                 decoder_name_.c_str());
    return false;
}




AVPixelFormat OpencvWorker::getSourcePixelFormat() const {
    return packet_source_ ? packet_source_->getSourcePixelFormat() : AV_PIX_FMT_NONE;
}
