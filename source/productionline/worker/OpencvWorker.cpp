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
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Opencv")))
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
    LOG4CPLUS_DEBUG(logger_, "OpencvWorker created with config");
    
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
        LOG4CPLUS_WARN(logger_, "⚠️  Stream already open, closing previous stream");
        close();
    }
    
    if (!packet_source_) {
        LOG4CPLUS_ERROR(logger_, "Cannot open: packet source is nullptr. Worker must be created with WorkerConfig");
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
        LOG4CPLUS_INFO(logger_, "📦 Opening EncodedPacketSourceFromBuffer (Buffer mode)");
    }
    
    // 1. 打开数据源
    if (!packet_source_->open()) {
        LOG4CPLUS_ERROR(logger_, "Failed to open packet source");
        return false;
    }
    
    // 2. 从数据源获取编解码器参数
    const AVCodecParameters* codecpar = packet_source_->getCodecParameters();
    if (!codecpar) {
        LOG4CPLUS_ERROR(logger_, "Failed to get codec parameters from packet source");
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
        LOG4CPLUS_DEBUG_FMT(logger_, "Output resolution not set in config, using: %dx%d", 
                      output_width_, output_height_);
    } else {        
        output_width_ = width;
        output_height_ = height;
        LOG4CPLUS_DEBUG_FMT(logger_, "Output resolution from config: %dx%d", output_width_, output_height_);
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
        LOG4CPLUS_ERROR(logger_, "Failed to create BufferPool via Allocator");
        packet_source_->close();
        return false;
    }
    
    // 7. ✅ v2.18 修复：统一注册 BufferPool（Buffer 和 RTSP 模式都需要）
    if (!registerBufferPool(BufferPoolType::DECODE_VIDEO_PRIMARY, pool_id)) {
        LOG4CPLUS_ERROR(logger_, "Failed to register BufferPool");
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
    LOG4CPLUS_DEBUG_FMT(logger_, "OpencvWorker (%s): Opened", mode_str);
    if (!is_buffer_mode) {
        LOG4CPLUS_DEBUG_FMT(logger_, "   Source: %s", worker_config_.data_source.path.c_str());
    }
    LOG4CPLUS_DEBUG_FMT(logger_, "   Output resolution: %dx%d (%.1f bytes/pixel)", 
                  output_width_, output_height_, getOutputBytesPerPixel());
    LOG4CPLUS_DEBUG_FMT(logger_, "   Codec: %s", codec_ctx_ptr_->codec->name);
    LOG4CPLUS_DEBUG_FMT(logger_, "   BufferPool: '%s' (ID: %lu, %d buffers)", 
                  actual_pool_name.c_str(), pool_id, 
                  worker_config_.data_source.buffer_count);
    
    return true;
}

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
                LOG4CPLUS_DEBUG(logger_, "Cleaning up pending packet on close");
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
    
    LOG4CPLUS_DEBUG(logger_, "Video source closed");
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
        LOG4CPLUS_ERROR(logger_, "Cannot seek: packet source is nullptr");
        return false;
    }
    
    if (!packet_source_->isOpen()) {
        LOG4CPLUS_ERROR(logger_, "Cannot seek: worker is not open");
        return false;
    }
    
    // 2. 委托给数据源实现 seek（多态调用）
    //    - EncodedPacketSourceFromFile: 实现真正的 seek
    //    - EncodedPacketSourceFromRtsp: 返回 false（不支持）
    //    - EncodedPacketSourceFromBuffer: 返回 false（不支持）
    if (!packet_source_->seek(frame_index)) {
        // 根据数据源类型返回适当的日志
        if (packet_source_->getDataSourceType() == IEncodedPacketSource::SourceType::NETWORK_SOURCE) {
            LOG4CPLUS_WARN(logger_, "RTSP stream does not support seeking");
        } else if (packet_source_->getDataSourceType() == IEncodedPacketSource::SourceType::BUFFER_SOURCE) {
            LOG4CPLUS_WARN(logger_, "Buffer source does not support seeking");
        } else {
            LOG4CPLUS_ERROR(logger_, "Seek failed or not supported by packet source");
        }
        return false;
    }
    
    // 3. seek 成功后，清理解码器状态（flush内部缓冲区）
    if (codec_ctx_ptr_) {
        avcodec_flush_buffers(codec_ctx_ptr_);
    }
    
    // ⚠️ 注意：EOF 状态由数据源的 seek() 自动重置，不需要手动重置
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Successfully seeked to frame %d", frame_index);
    return true;
}

bool OpencvWorker::seekToBegin() {
    // 委托给 seek(0)
    return seek(0);
}

bool OpencvWorker::seekToEnd() {
    LOG4CPLUS_WARN(logger_, "Warning: seekToEnd is not supported");
    return false;
}

bool OpencvWorker::skip(int frame_count){
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
    if (!packet_source_) {
        return false;
    }
    return !packet_source_->isAtEnd();
}

bool OpencvWorker::isAtEnd() const {
    if (!packet_source_) {
        return true;
    }
    return packet_source_->isAtEnd();
}

bool OpencvWorker::isConnected() const {
    if (!packet_source_) {
        return false;
    }
    return packet_source_->isOpen();
}

bool OpencvWorker::fillBufferMetadataFromFrame(AVFrame* frame_ptr, Buffer* buffer, cv::Mat* mat) {
    // ⭐ 硬件解码器：提取物理内存地址
    if (!decoder_name_.empty() && use_hardware_decoder_) {
        if (!extractHardwareAddressFromMetadata(frame_ptr, buffer)) {
            LOG4CPLUS_ERROR_FMT(logger_, "Hardware decoder '%s': Failed to extract physical address",
                         decoder_name_.c_str());
            // ⚠️ 容错处理，打印日志但继续执行
        }
    }

    // ⭐ 根据是否使用Mat选择不同的处理方式
    if (mat) {
        // ========== Mat模式：存储Mat并设置Mat的大小 ==========
        buffer->setMat(mat);

        // 计算并设置Mat的大小
        size_t mat_size = mat->total() * mat->elemSize();
        buffer->setSize(mat_size);

        // 可选：设置虚拟地址为Mat的data指针（用于某些需要data指针的场景）
        buffer->setVirtualAddress(mat->data);
    } else {
        // ========== AVFrame模式：使用原来的逻辑 ==========
        // 设置虚拟地址
        buffer->setVirtualAddress(frame_ptr->data[0]);

        // 计算并设置帧大小
        int actual_frame_size = av_image_get_buffer_size(
            (AVPixelFormat)frame_ptr->format,
            frame_ptr->width,
            frame_ptr->height,
            1  // alignment
        );

        if (actual_frame_size > 0) {
            buffer->setSize(actual_frame_size);
            LOG_TRACE_FMT("Updated buffer size to actual frame size: %d bytes", actual_frame_size);
        } else {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to get frame buffer size: %d", actual_frame_size);
        }
    }

    // ⭐ 设置图像元数据（格式、宽高、linesize 等）
    buffer->setImageMetadataFromAVFrame(frame_ptr);

    // ⭐ v2.26新增：保存 PTS（用于多通道帧对齐）
    buffer->setPts(frame_ptr->pts);

    // ⭐ 更新统计计数器
    decoded_frames_++;

    return true;
}

FillResult OpencvWorker::readAndSendPacket(AVPacket* packet_ptr) {
    // ⭐ v2.32 统一接口：所有模式都使用 acquireEncodedPacket
    if (!packet_acquired_) {
        auto acquire_result = packet_source_->acquireEncodedPacket(packet_ptr, this);

        if (acquire_result.ok()) {
            // ✅ 成功获取
            current_packet_ptr_ = acquire_result.packet();
            packet_acquired_ = true;

        } else if (acquire_result.isEof()) {
            // 📍 EOF：数据流正常结束
            LOG4CPLUS_DEBUG(logger_, "acquireEncodedPacket: EOF reached");
            setLastFillStatus(FillStatus::EndOfStream);
            return FillResult::endOfStream();

        } else if (acquire_result.shouldRetry()) {
            // ⏳ AGAIN：需要重试
            if (acquire_result.status() == AcquireStatus::Again) {
                setLastFillStatus(FillStatus::DataPending);
                return FillResult::dataPending();
            }
            setLastFillStatus(FillStatus::NonVideoPacket);
            return FillResult::nonVideoPacket();

        } else {
            // ❌ 错误
            LOG4CPLUS_ERROR_FMT(logger_, "acquireEncodedPacket failed: %s",
                               acquire_result.statusString());
            setLastFillStatus(FillStatus::AcquireError);
            return FillResult::acquireError();
        }
    }

    // ========== 发送到解码器 ==========
    int ret = avcodec_send_packet(codec_ctx_ptr_, current_packet_ptr_);

    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        // ❌ 发送失败
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "ERROR: avcodec_send_packet failed: %d (%s)", ret, err_buf);

        if (!worker_config_.data_source.deferred_commit) {
            packet_source_->cancelEncodedPacket(this);
        }
        packet_acquired_ = false;
        current_packet_ptr_ = nullptr;
        setLastFillStatus(FillStatus::CodecError);
        return FillResult::codecError();
    }

    return FillResult::success();
}

FillResult OpencvWorker::fillBuffer(int frame_index, Buffer* buffer) {
    (void)frame_index;  // 未使用

    // ========== 参数校验 ==========
    if (!buffer) {
        LOG4CPLUS_ERROR(logger_, "ERROR: buffer is nullptr");
        setLastFillStatus(FillStatus::InvalidParam);
        return FillResult::invalidParam();
    }

    if (!packet_source_ || !packet_source_->isOpen()) {
        LOG4CPLUS_ERROR(logger_, "ERROR: Worker is not open");
        setLastFillStatus(FillStatus::NotOpen);
        return FillResult::notOpen();
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    AVPacket* packet_ptr = buffer->getAVPacket();
    if (!packet_ptr) {
        LOG4CPLUS_ERROR(logger_, "ERROR: buffer->getAVPacket() is nullptr");
        setLastFillStatus(FillStatus::InvalidParam);
        return FillResult::invalidParam();
    }

    // ========== 步骤1: 检查缓存队列 ==========
    if (!cached_frames_.empty()) {
        AVFrame* cached_frame = cached_frames_.front();
        cached_frames_.erase(cached_frames_.begin());

        // 转换AVFrame到Mat
        //cv::Mat* mat = convertAVFrameToMat(cached_frame);
        cv::Mat* mat = new cv::Mat(cached_frame);
        if (mat->empty()) {
            LOG4CPLUS_ERROR(logger_, "Failed to convert cached AVFrame to Mat");
            delete mat;
            av_frame_free(&cached_frame);
            setLastFillStatus(FillStatus::InternalError);
            return FillResult::internalError();
        }

        // 使用fillBufferMetadataFromFrame统一设置所有元数据
        fillBufferMetadataFromFrame(cached_frame, buffer, mat);

        buffer->setAVFrame(cached_frame);

        setLastFillStatus(FillStatus::Success);
        return FillResult::success();
    }

    // ========== 步骤2: 读取并发送 packet ==========
    FillResult send_result = readAndSendPacket(packet_ptr);
    if (!send_result.ok()) {
        // readAndSendPacket() 已经设置了 lastFillStatus_，直接返回
        return send_result;
    }

    // ========== 步骤3: 循环读取所有解码的帧到缓存 ==========
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

    // ========== 步骤4: 检查是否成功解码 ==========
    if (!decoded_at_least_one) {
        // ❌ 没有解码出帧（EAGAIN：解码器需要更多输入才能输出）
        if (!worker_config_.data_source.deferred_commit) {
            packet_source_->cancelEncodedPacket(this);
        }
        packet_acquired_ = false;
        current_packet_ptr_ = nullptr;
        setLastFillStatus(FillStatus::CodecEagain);
        return FillResult::codecEagain();
    }

    // ========== 步骤5: 成功解码，提交（释放）==========
    if (packet_acquired_) {
        if (!worker_config_.data_source.deferred_commit) {
            packet_source_->commitEncodedPacket(this);
        }

        // 重置状态
        packet_acquired_ = false;
        current_packet_ptr_ = nullptr;
    }

    AVFrame* first_frame = cached_frames_.front();
    cached_frames_.erase(cached_frames_.begin());

    // 转换AVFrame到Mat，这是tacosdk自定义的接口
    // 注意avframe和mat之间是零拷贝的，底层内存是共享的，所以当av_frame_free(avframe)时mat数据会被销毁
    cv::Mat* mat = new cv::Mat(first_frame);
    if (mat->empty()) {
        LOG4CPLUS_ERROR(logger_, "Failed to convert AVFrame to Mat");
        delete mat;
        av_frame_free(&first_frame);
        setLastFillStatus(FillStatus::InternalError);
        return FillResult::internalError();
    }

    // 使用fillBufferMetadataFromFrame统一设置所有元数据
    fillBufferMetadataFromFrame(first_frame, buffer, mat);

    buffer->setAVFrame(first_frame);

    setLastFillStatus(FillStatus::Success);
    return FillResult::success();
}

cv::Mat* OpencvWorker::convertAVFrameToMat(AVFrame* avframe) {
    if (!avframe || !avframe->data[0] ||
        avframe->width <= 0 || avframe->height <= 0) {
        return nullptr;
    }

    int width = avframe->width;
    int height = avframe->height;
    AVPixelFormat fmt = static_cast<AVPixelFormat>(avframe->format);

    switch (fmt){
        case AV_PIX_FMT_RGB24: {
            // 创建堆上的Mat对象并拷贝数据
            cv::Mat temp(height, width, CV_8UC3, avframe->data[0], avframe->linesize[0]);
            return new cv::Mat(temp.clone());
        }
        case AV_PIX_FMT_NV12: {
            // NV12格式：先构建完整的NV12 Mat，然后转换为BGR
            int total_height = height + height / 2;
            cv::Mat nv12_mat(total_height, width, CV_8UC1);

            // 复制Y平面
            int y_size = width * height;
            cv::Mat y_plane(height, width, CV_8UC1, nv12_mat.data, width);
            cv::Mat av_y(height, width, CV_8UC1, avframe->data[0], avframe->linesize[0]);
            av_y.copyTo(y_plane);

            // 复制UV平面（NV12是UV交错）
            cv::Mat uv_plane(height / 2, width, CV_8UC1,
                            nv12_mat.data + y_size, width);
            cv::Mat av_uv(height / 2, width, CV_8UC1,
                        avframe->data[1], avframe->linesize[1]);
            av_uv.copyTo(uv_plane);

            // 转换NV12到BGR
            cv::Mat* result_mat = new cv::Mat();
            cv::cvtColor(nv12_mat, *result_mat, cv::COLOR_YUV2BGR_NV12);

            return result_mat;
        }
        default:
            LOG4CPLUS_ERROR_FMT(logger_, "Unsupported pixel format: %d. Supported: RGB24, NV12", fmt);
            return nullptr;
    }

    return nullptr;
}

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
    LOG4CPLUS_INFO(logger_, "📊 Statistics:");
    
    // 1. 通用信息
    std::string path = packet_source_ ? packet_source_->getPath() : std::string();
    LOG4CPLUS_INFO_FMT(logger_, "   Source: %s", path.empty() ? "(Buffer Mode)" : path.c_str());
    LOG4CPLUS_INFO_FMT(logger_, "   Codec: %s", getCodecName());
    LOG4CPLUS_INFO_FMT(logger_, "   Resolution: %dx%d → %dx%d", getSourceWidth(), getSourceHeight(), output_width_, output_height_);
    LOG4CPLUS_INFO_FMT(logger_, "   Decoded frames: %d", decoded_frames_.load());
    
    // 2. 根据数据源类型显示特定信息
    SourceType type = getDataSourceType();
    if (type == SourceType::FILE_SOURCE) {
        LOG4CPLUS_INFO_FMT(logger_, "   Total frames: %d", packet_source_ ? packet_source_->getTotalFrames() : -1);
        LOG4CPLUS_INFO_FMT(logger_, "   EOF: %s", packet_source_ && packet_source_->isAtEnd() ? "YES" : "NO");
    } else if (type == SourceType::NETWORK_SOURCE) {
        LOG4CPLUS_INFO_FMT(logger_, "   Connected: %s", isConnected() ? "Yes" : "No");
        LOG4CPLUS_INFO_FMT(logger_, "   Dropped frames: %d", dropped_frames_.load());
    } else if (type == SourceType::BUFFER_SOURCE) {
        LOG4CPLUS_INFO_FMT(logger_, "   Dropped frames: %d", dropped_frames_.load());
    }
    
    // 3. BufferPool 信息（通用）
    uint64_t pool_id = getOutputBufferPoolId(BufferPoolType::DECODE_VIDEO_PRIMARY);
    LOG4CPLUS_INFO_FMT(logger_, "   BufferPool ID: %lu", pool_id);
}

// ============ 内部实现 ============

bool OpencvWorker::initializeDecoder(const AVCodecParameters* codec_params) {
    // ⭐ v2.12修改：codec_params 必须提供（从 packet_source_ 获取）
    if (!codec_params) {
        LOG4CPLUS_ERROR(logger_, "Cannot initialize decoder: codec_params is nullptr");
        return false;
    }
    const AVCodecParameters* codecpar = codec_params;
    
    // 1. 查找解码器
    const AVCodec* codec = nullptr;
    
    if (!decoder_name_.empty()) {
        // ⭐ 用户指定了解码器名称（如 "h264_taco"）
        codec = avcodec_find_decoder_by_name(decoder_name_.c_str());
        if (!codec) {
            LOG4CPLUS_WARN_FMT(logger_, "⚠️ Warning: Specified decoder '%s' not found", decoder_name_.c_str());
        } else {
            LOG4CPLUS_DEBUG_FMT(logger_, "Using specified decoder: %s", decoder_name_.c_str());
            
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
            LOG4CPLUS_INFO(logger_, "Searching for pure software decoder...");
            codec = findPureSoftwareDecoder(codecpar->codec_id);
            if (!codec) {
                LOG4CPLUS_ERROR(logger_, "No pure software decoder available for this codec!");
                return false;
            }
            LOG4CPLUS_INFO_FMT(logger_, "✅ Using software decoder: %s", codec->name);
        } else {
            // 硬件解码或自动选择：使用 FFmpeg 默认行为
            codec = avcodec_find_decoder(codecpar->codec_id);
            if (!codec) {
                LOG4CPLUS_ERROR(logger_, "Decoder not found for codec");
                return false;
            }
            
            // 日志：显示选择的解码器类型
            if (isHardwareDecoder(codec)) {
                LOG4CPLUS_INFO_FMT(logger_, "Auto-selected hardware decoder: %s", codec->name);
            } else {
                LOG4CPLUS_INFO_FMT(logger_, "Auto-selected software decoder: %s", codec->name);
            }
        }
    }
    
    // 2. 分配解码器上下文
    codec_ctx_ptr_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_ptr_) {
        LOG4CPLUS_ERROR(logger_, "Failed to allocate codec context");
        return false;
    }
    
    // 3. 复制参数到解码器上下文
    int ret = avcodec_parameters_to_context(codec_ctx_ptr_, codecpar);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to copy codec parameters (FFmpeg: %s)", err_buf);
        avcodec_free_context(&codec_ctx_ptr_);
        codec_ctx_ptr_ = nullptr;
        return false;
    }
    
    // 4. 配置特殊解码器（如 h264_taco）
    if (decoder_name_ == "h264_taco") {
        if (!configureSpecialDecoder()) {
            LOG4CPLUS_ERROR(logger_, "ERROR: Failed to configure special decoder options");
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
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to open codec (FFmpeg: %s)", err_buf);
        avcodec_free_context(&codec_ctx_ptr_);
        codec_ctx_ptr_ = nullptr;
        return false;
    }
    
    LOG4CPLUS_DEBUG(logger_, "Initialized decoder");
    LOG4CPLUS_INFO_FMT(logger_, "   Codec: %s", codec_ctx_ptr_->codec->name);
    LOG4CPLUS_INFO_FMT(logger_, "   Stream resolution: %dx%d", codec_ctx_ptr_->width, codec_ctx_ptr_->height);
    LOG4CPLUS_INFO_FMT(logger_, "   Output resolution: %dx%d", output_width_, output_height_);
    
    return true;
}

bool OpencvWorker::configureSpecialDecoder() {
    // 配置 h264_taco 解码器（从 worker_config_ 读取配置）
    if (!codec_ctx_ptr_->priv_data) {
        LOG4CPLUS_WARN_FMT(logger_, " Warning: codec_ctx->priv_data is NULL, cannot set options");
        return false;
    }
    
    // 🎯 从 worker_config_ 获取 taco 配置（非 const，可能需要修改）
    auto& taco = worker_config_.decoder.taco;
    
    LOG4CPLUS_DEBUG_FMT(logger_, "Configuring h264_taco decoder options from config...");
    
    int ret;
    
    // 禁用重排序（从 config 读取）
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "reorder_disable", 
                         taco.reorder_disable ? 1 : 0, 0);
    LOG4CPLUS_DEBUG_FMT(logger_, "   reorder_disable=%d: %s", taco.reorder_disable ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    // 启用通道（从 config 读取）
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_enable", 
                         taco.ch0_enable ? 1 : 0, 0);
    LOG4CPLUS_DEBUG_FMT(logger_, "   ch0_enable=%d: %s", taco.ch0_enable ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_enable", 
                         taco.ch1_enable ? 1 : 0, 0);
    LOG4CPLUS_DEBUG_FMT(logger_, "   ch1_enable=%d: %s", taco.ch1_enable ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    // ========== 通道0配置 ==========
    
    // 配置通道0裁剪参数（从 config 读取）
    if (taco.ch0_crop_width > 0 && taco.ch0_crop_height > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_x", taco.ch0_crop_x, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_y", taco.ch0_crop_y, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_width", taco.ch0_crop_width, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_crop_height", taco.ch0_crop_height, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "   ch0_crop: (%d, %d, %d, %d)", 
               taco.ch0_crop_x, taco.ch0_crop_y, 
               taco.ch0_crop_width, taco.ch0_crop_height);
    }
    
    // 配置通道0缩放参数（从 config 读取）
    if (taco.ch0_scale_width > 0 && taco.ch0_scale_height > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_scale_width", taco.ch0_scale_width, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch0_scale_height", taco.ch0_scale_height, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "   ch0_scale: (%d, %d)", taco.ch0_scale_width, taco.ch0_scale_height);
    }
    
    // ========== 通道1配置 ==========
    
    // 配置通道1裁剪参数（从 config 读取）
    if (taco.ch1_crop_width > 0 && taco.ch1_crop_height > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_x", taco.ch1_crop_x, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_y", taco.ch1_crop_y, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_width", taco.ch1_crop_width, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_crop_height", taco.ch1_crop_height, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "   ch1_crop: (%d, %d, %d, %d)", 
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
            LOG4CPLUS_DEBUG_FMT(logger_, "   ch1_scale: (%d, %d)", taco.ch1_scale_width, taco.ch1_scale_height);
        }
    }
    
    // 配置通道1 RGB（从 config 读取）
    ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_rgb", 
                         taco.ch1_rgb ? 1 : 0, 0);
    LOG4CPLUS_DEBUG_FMT(logger_, "   ch1_rgb=%d: %s", taco.ch1_rgb ? 1 : 0, 
           ret < 0 ? "FAILED" : "OK");
    
    // ⭐ v2.17: 设置 RGB 格式（使用整型枚举）
    if (taco.ch1_rgb && taco.ch1_rgb_format > 0) {
        ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_rgb_format", 
                             taco.ch1_rgb_format, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "   ch1_rgb_format=%d: %s", taco.ch1_rgb_format, 
               ret < 0 ? "FAILED" : "OK");
    }
    
    // ⭐ v2.17: 设置颜色标准（使用整型枚举）
    if (taco.ch1_rgb && taco.ch1_rgb_std > 0) {
        ret = av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_rgb_std", 
                             taco.ch1_rgb_std, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "   ch1_rgb_std=%d: %s", taco.ch1_rgb_std, 
               ret < 0 ? "FAILED" : "OK");
    }
    
    return true;
}

bool OpencvWorker::extractHardwareAddressFromMetadata(AVFrame* frame, Buffer* buffer) {
    if (!frame || !buffer) {
        LOG4CPLUS_ERROR(logger_, "extractHardwareAddressFromMetadata: Invalid parameters");
        return false;
    }
    
    if (decoder_name_.find("taco") != std::string::npos) {
        // TACO 特定逻辑：从 metadata 中提取 pool_blk_id，转换为物理地址
        uint64_t phys_addr = 0;
        uint32_t blk_id = 0;
        
        if (frame->metadata) {
            AVDictionaryEntry* entry = av_dict_get(frame->metadata, "pool_blk_id", NULL, 0);
            if (entry) {
                blk_id = (uint32_t)std::stoi(entry->value);
                phys_addr = taco_sys_handle2_phys_addr(blk_id);
                
                if (phys_addr != 0) {
                    // ✅ 成功提取物理地址
                    buffer->setPhysicalAddress(phys_addr);
                    return true;
                } else {
                    // ❌ blk_id 有效，但转换失败
                    LOG4CPLUS_ERROR_FMT(logger_, "TACO: Failed to convert blk_id=%u to physical address", blk_id);
                    return false;
                }
            }
        }
        
        // ❌ TACO 解码器但没有 metadata（异常情况）
        LOG4CPLUS_ERROR(logger_, "TACO: AVFrame->metadata is missing or no 'pool_blk_id' entry");
        return false;
    }
    // ⭐ v2.18 改进：软件解码器不需要物理地址
    if (decoder_name_.empty() || !use_hardware_decoder_) {
        // 软件解码器，不需要物理地址
        LOG4CPLUS_DEBUG(logger_, "Software decoder: No hardware address needed");
        return true;  // ✅ 软件解码器返回 true（不是错误）
    }
    
    // 未识别的硬件解码器
    LOG4CPLUS_ERROR_FMT(logger_, "Unknown hardware decoder '%s', cannot extract physical address", 
                 decoder_name_.c_str());
    return false;
}




AVPixelFormat OpencvWorker::getSourcePixelFormat() const {
    return packet_source_ ? packet_source_->getSourcePixelFormat() : AV_PIX_FMT_NONE;
}
