#include "productionline/worker/FfmpegDecodeRtspWorker.hpp"
#include "productionline/worker/RtspPacketSource.hpp"
#include "productionline/worker/BufferPacketSource.hpp"
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
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Rtsp")))
    , packet_source_(nullptr)  // ⚠️ 数据源将在下面根据配置创建
    , codec_ctx_ptr_(nullptr)
    , output_width_(0)
    , output_height_(0)
    , use_hardware_decoder_(config.decoder.enable_hardware)  // 🎯 从配置读取
    , decoder_name_(config.decoder.name.value_or(""))  // 🎯 从配置读取（使用 optional 的 value_or）
    , codec_options_ptr_(nullptr)
    , decoded_frames_(0)
    , dropped_frames_(0)
{
    LOG4CPLUS_DEBUG(logger_, "[Worker] FfmpegDecodeRtspWorker created with config");
    
    // ⭐ v2.18 重构：根据配置创建数据源（统一在构造函数中，学习 VideoFileWorker）
    // ⭐ v2.19 修复：支持共享数据源模式（与 FfmpegDecodeVideoFileWorker 保持一致）
    if (config.decoder.datasource_buffer_mode) {
        // Buffer 数据源模式：从 BufferPacketSource 获取 packet
        
        // ⭐ v2.19 新增：检查是否使用共享实例（MultiWorker 共享模式）
        if (config.decoder.shared_packet_source) {
            // ✅ 共享模式：使用 config 中的共享实例（ONE_TO_MANY 零拷贝）
            packet_source_ = config.decoder.shared_packet_source;
            LOG4CPLUS_INFO(logger_, "⭐ v2.19 使用共享 PacketSource（MultiWorker 共享模式）");
        } else {
            // ✅ 普通模式：创建独立的 BufferPacketSource 实例（ONE_TO_ONE）
            if (config.decoder.codec_params) {
                packet_source_ = std::make_shared<BufferPacketSource>(config.decoder.codec_params);
                LOG4CPLUS_DEBUG(logger_, "Created BufferPacketSource (v2.20: 需要调用 setSourceBufferPool 关联源 Pool)");
            } else {
                LOG4CPLUS_WARN(logger_, "datasource_buffer_mode=true but codec_params is nullptr");
            }
        }
    } else {
        // ⭐ v2.18 新增：RTSP 模式也在构造函数中创建数据源（统一设计）
        // 从配置读取 RTSP URL
        const std::string& rtsp_url = config.data_source.path;
        if (!rtsp_url.empty()) {
            packet_source_ = std::make_shared<RtspPacketSource>(rtsp_url);
            LOG4CPLUS_DEBUG_FMT(logger_, "Created RtspPacketSource for '%s'", rtsp_url.c_str());
        } else {
            LOG4CPLUS_WARN(logger_, "RTSP URL not configured in worker_config_.data_source.path");
        }
    }
}

FfmpegDecodeRtspWorker::~FfmpegDecodeRtspWorker() {
    LOG4CPLUS_DEBUG(logger_, "🧹 Destroying FfmpegDecodeRtspWorker...");
    
    // 清理缓存的帧（避免内存泄漏）
    for (AVFrame* frame : cached_frames_) {
        if (frame) {
            av_frame_free(&frame);
        }
    }
    cached_frames_.clear();
    
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
    if (packet_source_ && packet_source_->isOpen() && packet_source_->getDataSourceType() != IPacketSource::SourceType::BUFFER_SOURCE) {
        LOG4CPLUS_WARN(logger_, "[Worker] ⚠️  Stream already open, closing previous stream");
        close();
    }
    
    // ⭐ v2.18 重构：统一处理，不区分 Buffer/RTSP 模式（学习 VideoFileWorker）
    // 数据源应该在构造函数中已经创建
    if (!packet_source_) {
        setError("Cannot open: packet source is nullptr. Worker must be created with WorkerConfig");
        return false;
    }
    
    // 从配置读取输出参数
    int width = worker_config_.display.width;
    int height = worker_config_.display.height;
    int bits_per_pixel = worker_config_.display.bits_per_pixel;
    bool is_buffer_mode = worker_config_.decoder.datasource_buffer_mode;
    
    // 验证参数（RTSP 模式必须提供分辨率）
    if (!is_buffer_mode) {
        // RTSP 模式：必须配置分辨率
        if (width == 0 || height == 0 || bits_per_pixel == 0) {
            setError("Display resolution and bits_per_pixel must be configured for RTSP stream");
            LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ❌ Current config: %dx%d@%dbpp", width, height, bits_per_pixel);
            LOG4CPLUS_ERROR(logger_, "[Worker]    Please set worker_config_.display.width/height/bits_per_pixel");
            return false;
        }
        
        LOG4CPLUS_INFO(logger_, "");
        LOG4CPLUS_INFO_FMT(logger_, "📡 Opening RTSP stream: %s", worker_config_.data_source.path.c_str());
        LOG4CPLUS_INFO_FMT(logger_, "   Output resolution: %dx%d@%dbpp", width, height, bits_per_pixel);
    } else {
        LOG4CPLUS_INFO(logger_, "[Worker] 📦 Opening BufferPacketSource (Buffer mode)");
    }
    
    // 1. 打开数据源
    if (!packet_source_->open()) {
        setError("Failed to open packet source");
        return false;
    }
    
    // 2. 从数据源获取编解码器参数
    const AVCodecParameters* codecpar = packet_source_->getCodecParameters();
    if (!codecpar) {
        setError("Failed to get codec parameters from packet source");
        packet_source_->close();
        return false;
    }
    
    // 3. 检查编解码器类型是否匹配
    checkCodecMismatch(codecpar->codec_id, decoder_name_);
    
    // 4. 初始化解码器
    if (!initializeDecoder(codecpar)) {
        packet_source_->close();
        return false;
    }
    
    // 5. 设置输出分辨率（智能判断，学习 VideoFileWorker）
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
    
    if (bits_per_pixel == 0) {
        bits_per_pixel = 32;  // 默认值
        LOG4CPLUS_DEBUG_FMT(logger_, "[Worker] bits_per_pixel not set, using default: %d", bits_per_pixel);
    }
    
    // 生成 BufferPool 名称
    std::string pool_name;
    if (is_buffer_mode) {
        pool_name = "FfmpegDecodeRtspWorker_BufferMode";
    } else {
        pool_name = std::string("FfmpegDecodeRtspWorker_") + worker_config_.data_source.path;
    }
    
    uint64_t pool_id = allocator_facade_.allocatePoolWithBuffers(
        worker_config_.data_source.buffer_count,
        0,
        pool_name,
        is_buffer_mode ? "RTSP_BUFFER" : "RTSP"
    );
    
    if (pool_id == 0) {
        setError("Failed to create BufferPool via Allocator");
        packet_source_->close();
        return false;
    }
    
    // 7. ✅ v2.18 修复：统一注册 BufferPool（Buffer 和 RTSP 模式都需要）
    if (!registerBufferPool(BufferPoolType::DECODE_VIDEO_PRIMARY, pool_id)) {
        setError("Failed to register BufferPool");
        packet_source_->close();
        return false;
    }
    
    // 8. 从 Registry 获取 Pool 名称
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    std::string actual_pool_name = pool ? pool->getName() : "Unknown";
    
    decoded_frames_ = 0;
    dropped_frames_ = 0;
    
    // 9. 详细日志输出（学习 VideoFileWorker）
    const char* mode_str = is_buffer_mode ? "Buffer mode" : "RTSP stream";
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker] FfmpegDecodeRtspWorker (%s): Opened", mode_str);
    if (!is_buffer_mode) {
        LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    RTSP URL: %s", worker_config_.data_source.path.c_str());
    }
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    Output resolution: %dx%d@%dbpp", output_width_, output_height_, bits_per_pixel);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    Codec: %s", codec_ctx_ptr_->codec->name);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    BufferPool: '%s' (ID: %lu, %d buffers)", 
                  actual_pool_name.c_str(), pool_id, 
                  worker_config_.data_source.buffer_count);
    
    return true;
}

// ============ v2.13 BufferPacketSource 配置 ============

bool FfmpegDecodeRtspWorker::setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) {
    // 检查是否是 BufferPacketSource
    auto* buffer_source = dynamic_cast<BufferPacketSource*>(packet_source_.get());
    if (!buffer_source) {
        LOG4CPLUS_WARN(logger_, "setSourceBufferPool 失败：不是 Buffer 模式");
        return false;
    }
    
    // 设置源 BufferPool
    buffer_source->setSourceBufferPool(pool_weak);
    LOG4CPLUS_DEBUG(logger_, "✅ 已设置源 BufferPool（v2.13 Pool 模式）");
    
    return true;
}

void FfmpegDecodeRtspWorker::close() {
    // ⚠️ 注意：打开状态由数据源管理
    if (!packet_source_ || !packet_source_->isOpen()) {
        return;  // 已经关闭过了
    }
    
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        LOG4CPLUS_INFO(logger_, "");
        LOG4CPLUS_INFO(logger_, "🛑 Closing RTSP stream...");
        
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
    
    LOG4CPLUS_DEBUG(logger_, "[Worker] RTSP stream closed");
    LOG4CPLUS_INFO_FMT(logger_, "   Decoded frames: %d", decoded_frames_.load());
    LOG4CPLUS_INFO_FMT(logger_, "   Dropped frames: %d", dropped_frames_.load());
}

bool FfmpegDecodeRtspWorker::isOpen() const {
    // ⚠️ 注意：打开状态从数据源获取
    if (!packet_source_) {
        return false;
    }
    return packet_source_->isOpen();
}


bool FfmpegDecodeRtspWorker::seek(int frame_index) {
    LOG4CPLUS_WARN(logger_, "[Worker]  Warning: RTSP stream does not support seeking");
    return false;
}

bool FfmpegDecodeRtspWorker::seekToBegin() {
    LOG4CPLUS_WARN(logger_, "[Worker]  Warning: RTSP stream does not support seeking");
    return false;
}

bool FfmpegDecodeRtspWorker::seekToEnd() {
    LOG4CPLUS_WARN(logger_, "[Worker]  Warning: RTSP stream does not support seeking");
    return false;
}

bool FfmpegDecodeRtspWorker::skip(int frame_count) {
    LOG4CPLUS_WARN(logger_, "[Worker]  Warning: RTSP stream does not support frame skipping");
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
        LOG4CPLUS_WARN(logger_, "[Worker] getBytesPerPixel() fallback: assuming YUV420 (1.5 bytes/pixel)");
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
    return !packet_source_->isAtEnd();
}

bool FfmpegDecodeRtspWorker::isAtEnd() const {
    // ⭐ v2.12修改：从数据源获取 EOF 状态
    if (!packet_source_) {
        return true;
    }
    return packet_source_->isAtEnd();
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

/**
 * @brief 从 AVFrame 填充 Buffer 的元数据
 * @param frame_ptr AVFrame 指针（必须已填充数据）
 * @param buffer Buffer 指针（用于存储元数据）
 * @return true 成功设置元数据，false 失败
 */
bool FfmpegDecodeRtspWorker::fillBufferMetadataFromFrame(AVFrame* frame_ptr, Buffer* buffer) {
    // ⭐ 硬件解码器：提取物理内存地址
    if (!decoder_name_.empty() && use_hardware_decoder_) {
        if (!extractHardwareAddressFromMetadata(frame_ptr, buffer)) {
            LOG4CPLUS_ERROR_FMT(logger_, "[Worker] Hardware decoder '%s': Failed to extract physical address",
                         decoder_name_.c_str());
            return false;
        }
    }
    
    // ⭐ 设置虚拟地址
    buffer->setVirtualAddress(frame_ptr->data[0]);
    
    // ⭐ 设置图像元数据
    buffer->setImageMetadataFromAVFrame(frame_ptr);
    
    // ⭐ 更新统计计数器
    decoded_frames_++;
    
    return true;
}

/**
 * @brief 从数据源读取 packet 并发送到解码器
 * @param packet_ptr AVPacket 指针（必须已分配）
 * @return true 成功发送 packet 到解码器，false 失败或 EOF
 */
bool FfmpegDecodeRtspWorker::readAndSendPacket(AVPacket* packet_ptr) {
    // 步骤1: 从数据源读取 packet
    int read_ret = packet_source_->readPacket(packet_ptr);
    
    if (read_ret < 0) {
        if (read_ret == AVERROR_EOF) {
            // ⚠️ 不调用 av_packet_unref()，因为 packet_ptr 只是指向 Buffer 的视图
            return false;
        } else {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(read_ret, err_buf, sizeof(err_buf));
            LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ERROR: readPacket failed: %d (%s)", read_ret, err_buf);
            // ⚠️ 不调用 av_packet_unref()
            return false;
        }
    }
    
    // 步骤2: 发送 packet 到解码器
    // avcodec_send_packet() 会在内部拷贝数据，函数返回后 packet_ptr 就不再需要了
    int ret = avcodec_send_packet(codec_ctx_ptr_, packet_ptr);
    
    // ✅ 不释放 packet 引用，因为：
    //    1. packet_ptr 只是指向 Buffer 中 AVPacket 的"视图"
    //    2. packet_ptr->buf == nullptr，没有引用计数
    //    3. Buffer 的生命周期由 BufferPacketSource 的 Fetch 任务管理
    //    4. 所有订阅者完成后，Fetch 任务会释放 Buffer
    // av_packet_unref(packet_ptr);  // ❌ 不再调用
    
    if (ret < 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ERROR: avcodec_send_packet failed: %d", ret);
        return false;
    }
    
    return true;
}

bool FfmpegDecodeRtspWorker::fillBuffer(int frame_index, Buffer* buffer) {
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
    
    // ========== 步骤2: 缓存为空，读取新 packet 并发送到解码器 ==========
    if (!readAndSendPacket(packet_ptr)) {
        return false;
    }
    
    // ========== 步骤3: 循环读取所有解码的帧到缓存 ==========
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
        
        // 成功解码一帧，放入缓存
        cached_frames_.push_back(temp_frame);
    }
    
    // ========== 步骤4: 从缓存取第一帧填充 buffer ==========
    if (cached_frames_.empty()) {
        return false;
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
    LOG4CPLUS_INFO(logger_, "");
    LOG4CPLUS_INFO(logger_, "📊 FfmpegDecodeRtspWorker Statistics:");
    // ⭐ v2.12修改：从数据源获取 RTSP URL
    std::string rtsp_url = packet_source_ ? packet_source_->getFilePath() : std::string();
    LOG4CPLUS_INFO_FMT(logger_, "   RTSP URL: %s", rtsp_url.empty() ? "(Not Set)" : rtsp_url.c_str());
    LOG4CPLUS_INFO_FMT(logger_, "   Connected: %s", isConnected() ? "Yes" : "No");
    LOG4CPLUS_INFO_FMT(logger_, "   Decoded frames: %d", decoded_frames_.load());
    LOG4CPLUS_INFO_FMT(logger_, "   Dropped frames: %d", dropped_frames_.load());
    
    uint64_t pool_id = getOutputBufferPoolId(BufferPoolType::DECODE_VIDEO_PRIMARY);
    LOG4CPLUS_INFO_FMT(logger_, "   BufferPool ID: %lu", pool_id);
}

// ============ 内部实现 ============

bool FfmpegDecodeRtspWorker::initializeDecoder(const AVCodecParameters* codec_params) {
    // ⭐ v2.12修改：codec_params 必须提供（从 packet_source_ 获取）
    if (!codec_params) {
        setError("Cannot initialize decoder: codec_params is nullptr");
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
                setError("No pure software decoder available for this codec!");
                return false;
            }
            LOG4CPLUS_INFO_FMT(logger_, "[Worker] ✅ Using software decoder: %s", codec->name);
        } else {
            // 硬件解码或自动选择：使用 FFmpeg 默认行为
            codec = avcodec_find_decoder(codecpar->codec_id);
            if (!codec) {
                setError("Decoder not found for codec");
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
            LOG4CPLUS_ERROR(logger_, "[Worker] ERROR: Failed to configure special decoder options");
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
    
    LOG4CPLUS_DEBUG(logger_, "[Worker] Initialized decoder");
    LOG4CPLUS_INFO_FMT(logger_, "   Codec: %s", codec_ctx_ptr_->codec->name);
    LOG4CPLUS_INFO_FMT(logger_, "   Stream resolution: %dx%d", codec_ctx_ptr_->width, codec_ctx_ptr_->height);
    LOG4CPLUS_INFO_FMT(logger_, "   Output resolution: %dx%d", output_width_, output_height_);
    
    return true;
}

bool FfmpegDecodeRtspWorker::configureSpecialDecoder() {
    // 配置 h264_taco 解码器（从 worker_config_ 读取配置）
    if (!codec_ctx_ptr_->priv_data) {
        LOG4CPLUS_WARN(logger_, "[Worker]  Warning: codec_ctx->priv_data is NULL, cannot set options");
        return false;
    }
    
    // 🎯 从 worker_config_ 获取 taco 配置
    const auto& taco = worker_config_.decoder.taco;
    
    LOG4CPLUS_DEBUG(logger_, "[Worker] Configuring h264_taco decoder options from config...");
    
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
    
    // 配置通道1缩放参数（从 config 读取）
    if (taco.ch1_scale_width > 0 && taco.ch1_scale_height > 0) {
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_scale_width", taco.ch1_scale_width, 0);
        av_opt_set_int(codec_ctx_ptr_->priv_data, "ch1_scale_height", taco.ch1_scale_height, 0);
        LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    ch1_scale: (%d, %d)", taco.ch1_scale_width, taco.ch1_scale_height);
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

void FfmpegDecodeRtspWorker::setError(const std::string& error, int ffmpeg_error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    last_error_ = error;
    
    if (ffmpeg_error != 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ffmpeg_error, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] FfmpegDecodeRtspWorker Error: %s (FFmpeg: %s)", error.c_str(), err_buf);
    } else {
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] FfmpegDecodeRtspWorker Error: %s", error.c_str());
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
        LOG4CPLUS_ERROR(logger_, "[Worker] extractHardwareAddressFromMetadata: Invalid parameters");
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
    //     // QSV 特定逻辑：从 AVFrame 的 data[3] 获取 mfxFrameSurface1 指针
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
    
    // 未知硬件解码器
    LOG4CPLUS_WARN_FMT(logger_, "[Worker] Hardware decoder '%s': No hardware address extraction implemented", 
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
