#include "productionline/worker/FfmpegDecodeVideoFileWorker.hpp"
#include "productionline/worker/FilePacketSource.hpp"
#include "productionline/worker/BufferPacketSource.hpp"
#include "common/Logger.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include <cstring>
#include <cstdio>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>  // 用于 av_strerror
#include <libavutil/pixdesc.h>  // 用于 av_pix_fmt_desc_get, av_get_bits_per_pixel
#include <libswscale/swscale.h>
#include "taco_sys_api.h"
}

// ============================================================================
// 构造/析构
// ============================================================================

// 构造函数（v2.2新增，v2.9支持数据源抽象）
FfmpegDecodeVideoFileWorker::FfmpegDecodeVideoFileWorker(const WorkerConfig& config)
    : WorkerBase(BufferAllocatorFactory::AllocatorType::AVFRAME, config)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.VideoFile")))
    , packet_source_(nullptr)  // 将在下面根据配置创建
    , codec_ctx_ptr_(nullptr)
    // ⚠️ 注意：video_stream_index_ 已移除，视频流索引从数据源获取
    , output_width_(config.display.width)      // 🎯 从配置读取输出宽度（初始值）
    , output_height_(config.display.height)   // 🎯 从配置读取输出高度（初始值）
    // ⚠️ 注意：total_frames_ 已移除，总帧数从数据源获取
    , current_frame_index_(0)
    // ⚠️ 注意：is_open_ 已移除，打开状态从数据源获取
    , use_hardware_decoder_(config.decoder.enable_hardware)  // 🎯 从配置读取
    , decoder_name_(config.decoder.name.value_or(""))  // 🎯 从配置读取（使用 optional 的 value_or）
    , codec_options_ptr_(nullptr)
    , decoded_frames_(0)
    , dropped_frames_(0)  // 初始化丢帧计数
{
    // ⚠️ 注意：file_path_ 已移除，文件路径由数据源类管理
    
    // ⭐ v2.9新增：根据配置创建数据源
    // ⭐ v2.19修复：支持共享数据源模式（与 FfmpegDecodeRtspWorker 保持一致）
    if (config.decoder.datasource_buffer_mode) {
        // Buffer 数据源模式：从 BufferPacketSource 获取 packet
        
        // ⭐ v2.19 新增：检查是否使用共享实例（MultiWorker 共享模式）
        if (config.decoder.shared_packet_source) {
            // ✅ 共享模式：使用 config 中的共享实例
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
        // 文件模式：从文件读取 packet
        packet_source_ = std::make_shared<FilePacketSource>(config.data_source.path);
        LOG4CPLUS_DEBUG_FMT(logger_, "Created FilePacketSource for '%s'", config.data_source.path.c_str());
    }
}

// ============ v2.13 BufferPacketSource 配置 ============

bool FfmpegDecodeVideoFileWorker::setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) {
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

FfmpegDecodeVideoFileWorker::~FfmpegDecodeVideoFileWorker() {
    LOG4CPLUS_DEBUG(logger_, "析构函数开始");
    
    // ⭐ 关键修改：正确的清理顺序
    //
    // 问题根源：
    // - 成员变量的析构永远在析构函数体执行完毕后
    // - 如果在函数体内先调用 close()，再让成员变量析构
    // - 顺序就变成：关闭解码器 → 释放 AVFrame
    // - 但此时 AVFrame 可能还引用了解码器的资源，导致 free(): invalid pointer
    //
    // 正确顺序：
    // 1. 手动调用 allocator_facade_.destroyPool() 先释放所有 AVFrame
    // 2. 再调用 close() 关闭解码器和数据源
    // 3. 成员变量自动析构（但 Pool 已清理，destroyPool() 幂等性保证不会重复释放）
    
    // 步骤1：清理缓存的帧（避免内存泄漏）
    for (AVFrame* frame : cached_frames_) {
        if (frame) {
            av_frame_free(&frame);
        }
    }
    cached_frames_.clear();
    LOG4CPLUS_DEBUG_FMT(logger_, "清理了 %zu 个缓存帧", cached_frames_.size());
    
    // 步骤2：先清理 BufferPool 和 AVFrame
    if (!buffer_pool_type_map_.empty()) {
        LOG4CPLUS_DEBUG(logger_, "手动清理 BufferPool 和 AVFrame...");
        allocator_facade_.destroyPool();  // 释放所有 Pool 中的 Buffer 和 AVFrame
        clearAllBufferPools();
    }
    
    // 步骤3：再关闭解码器和数据源（此时 AVFrame 已全部释放）
    if (packet_source_ && packet_source_->isOpen()) {
        LOG4CPLUS_DEBUG(logger_, "关闭解码器和数据源...");
        close();  // 关闭解码器和数据源，不再清理 Pool（已在上面清理）
    }
    
    LOG4CPLUS_DEBUG(logger_, "析构函数体结束");
    
    // ⭐ 函数体执行完毕后，成员变量自动析构
    // ⭐ 但由于 Pool 已经手动清理，allocator_facade_.destroyPool() 会因为幂等性直接返回
}

// ============================================================================
// 打开/关闭
// ============================================================================

bool FfmpegDecodeVideoFileWorker::open(const char* path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 如果已经打开，先关闭
    if (packet_source_ && packet_source_->isOpen()) {
        close();
    }
    
    // ⭐ v2.9修改：使用数据源抽象
    // 数据源应该在构造函数中已经创建（必须通过配置构造函数创建）
    if (!packet_source_) {
        setError("Cannot open: packet source is nullptr. Worker must be created with WorkerConfig");
        return false;
    }
    // 数据源已创建，直接使用
    // Buffer 模式：忽略 path 参数
    // 文件模式：使用构造函数中创建的数据源
    
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
    
    // 3. 获取视频流信息
    // ⚠️ 注意：video_stream_index_ 已移除，视频流索引从数据源获取（不缓存）
    // ⚠️ 注意：total_frames_ 已移除，总帧数从数据源获取（不缓存）
    
    // 4. 宽高信息从数据源获取（不再缓存，保持数据源一致性）
    // ⚠️ 注意：width_ 和 height_ 已移除，使用 getOriginalWidth()/getOriginalHeight() 获取
    
    // 5. 检查编解码器类型是否匹配（仅文件模式需要）
    if (auto* file_source = dynamic_cast<FilePacketSource*>(packet_source_.get())) {
        (void)file_source;  // 仅用于类型检查
        checkCodecMismatch(codecpar->codec_id, decoder_name_);
    }
    
    // 6. 初始化解码器（使用从数据源获取的 codec_params）
    if (!initializeDecoder(codecpar)) {
        packet_source_->close();
        return false;
    }
    
    // 6. 设置输出分辨率（如果配置中未设置，使用原始分辨率）
    // ⚠️ 注意：output_width_ 和 output_height_ 已在构造函数中从 config.display 读取
    // 只有在配置中未设置（为0）的情况下，才使用原始分辨率
    if (output_width_ == 0 || output_height_ == 0) {
        output_width_ = getOriginalWidth();
        output_height_ = getOriginalHeight();
        LOG4CPLUS_DEBUG_FMT(logger_, "[Worker] Output resolution not set in config, using original resolution: %dx%d", 
                      output_width_, output_height_);
    } else {
        LOG4CPLUS_DEBUG_FMT(logger_, "[Worker] Output resolution from config: %dx%d", output_width_, output_height_);
    }
    
    // 🎯 Worker职责：在open()时自动创建BufferPool（通过调用Allocator）
    // 计算帧大小（使用配置值）
    size_t frame_size = output_width_ * output_height_ * (worker_config_.display.bits_per_pixel / 8);
    if (frame_size == 0) {
        setError("Invalid frame size, cannot create BufferPool");
        packet_source_->close();
        return false;
    }
    // v2.0: allocatePoolWithBuffers 返回 pool_id
    std::string pool_name;
    if (path) {
        pool_name = std::string("FfmpegDecodeVideoFileWorker_") + std::string(path);
    } else {
        // Buffer 模式：使用默认名称
        pool_name = "FfmpegDecodeVideoFileWorker_BufferMode";
    }
    
    uint64_t pool_id = allocator_facade_.allocatePoolWithBuffers(
        worker_config_.data_source.buffer_count,
        frame_size,
        pool_name,
        "Video"
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
    std::string actual_pool_name = pool ? pool->getName() : "Unknown";
    
    // ⚠️ 注意：is_open_ 已移除，打开状态由数据源管理
    // 此时 packet_source_->isOpen() 应该已经返回 true（在 packet_source_->open() 成功后）
    current_frame_index_ = 0;
    decoded_frames_ = 0;
    
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker] FfmpegDecodeVideoFileWorker: Opened '%s'", path);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    Resolution: %dx%d → %dx%d", getOriginalWidth(), getOriginalHeight(), output_width_, output_height_);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    Codec: %s", codec_ctx_ptr_->codec->name);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    Total frames (estimated): %d", packet_source_ ? packet_source_->getTotalFrames() : -1);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker]    BufferPool: '%s' (ID: %lu, %d buffers, %zu bytes each)", 
           actual_pool_name.c_str(), pool_id, worker_config_.data_source.buffer_count, frame_size);
    
    return true;
}

void FfmpegDecodeVideoFileWorker::close() {
    // ⚠️ 注意：is_open_ 已移除，打开状态由数据源管理
    // 检查数据源是否已打开，如果未打开则直接返回
    if (!packet_source_ || !packet_source_->isOpen()) {
        return;  // 已经关闭过了
    }
    
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        // ⭐ v2.9新增：关闭数据源（数据源的 close() 内部会处理线程安全）
        if (packet_source_) {
            packet_source_->close();
        }
        
        // ⭐ 关键修改：Worker 只负责业务逻辑（关闭解码器）
        //    BufferPool 和 AVFrame 的清理由 allocator_facade_ 析构时自动处理
        //
        // 资源释放顺序（析构时）：
        // 1. 关闭数据源和解码器（业务资源）
        // 2. ~allocator_facade_() - 释放 BufferPool 和 AVFrame（底层内存资源）
        //
        // 设计原则：
        // - Worker::close() 只负责业务逻辑清理
        // - Allocator::~Allocator() 负责内存资源清理
        // - 符合单一职责原则和 RAII 原则
        
        // ⚠️ 注意：sws_ctx_ptr_ 已移除（当前未使用格式转换功能）
        
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
        
        // ⚠️ 注意：video_stream_index_ 已移除，视频流索引由数据源管理
        
        // ⭐ 清除所有 BufferPool 注册（标记不再使用）
        clearAllBufferPools();
    }
    
    // ⚠️ 注意：is_open_ 已移除，打开状态由数据源管理
    // 此时 packet_source_->isOpen() 应该已经返回 false（在 packet_source_->close() 后）
}

bool FfmpegDecodeVideoFileWorker::isOpen() const {
    // ⚠️ 注意：is_open_ 已移除，直接查询数据源状态
    if (!packet_source_) {
        return false;
    }
    return packet_source_->isOpen();  // 🎯 数据源的 isOpen() 是线程安全的（使用原子变量）
}

// ============================================================================
// 内部方法：初始化解码器
// ============================================================================
// ⚠️ 注意：openMediaSource()、closeMediaSource()、findVideoStream() 已移除
// 这些功能已由数据源抽象（IPacketSource）接管

bool FfmpegDecodeVideoFileWorker::initializeDecoder(const AVCodecParameters* codec_params) {
    // ⭐ v2.9修改：codec_params 必须提供（从 packet_source_ 获取）
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
        codec_ctx_ptr_ = nullptr;  // 🔧 置空防止 double free
        return false;
    }
    
    // 4. 配置特殊解码器（如 h264_taco）
    if (decoder_name_ == "h264_taco") {
        if (!configureSpecialDecoder()) {
            // 🔧 修复：配置失败是致命错误，必须返回
            LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ERROR: Failed to configure special decoder options");
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
        int orig_width = getOriginalWidth();
        int orig_height = getOriginalHeight();
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


// ⚠️ 注意：estimateTotalFrames() 已移除
// 总帧数现在从 packet_source_->getTotalFrames() 获取

// ============================================================================
// 导航操作
// ============================================================================

bool FfmpegDecodeVideoFileWorker::seek(int frame_index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (!packet_source_) {
        setError("Cannot seek: packet source is nullptr");
        return false;
    }
    
    if (!packet_source_->isOpen()) {
        setError("Cannot seek: worker is not open");
        return false;
    }
    
    // ⭐ v2.9重构：委托给数据源实现真正的 seek
    if (!packet_source_->seek(frame_index)) {
        setError("Seek failed or not supported by packet source");
        return false;
    }
    
    // seek 成功后，需要清理解码器状态
    // 1. flush 解码器（清空内部缓冲区）
    if (codec_ctx_ptr_) {
        avcodec_flush_buffers(codec_ctx_ptr_);
    }
    
    // 2. 重置 Worker 状态
    current_frame_index_ = frame_index;
    // ⚠️ 注意：EOF 状态由数据源的 seek() 自动重置，不需要手动重置
    
    LOG4CPLUS_DEBUG_FMT(logger_, "[Worker] Successfully seeked to frame %d", frame_index);
    return true;
}

bool FfmpegDecodeVideoFileWorker::seekToBegin() {
    return seek(0);
}

bool FfmpegDecodeVideoFileWorker::seekToEnd() {
    // ⭐ v2.9修改：从数据源获取总帧数
    if (packet_source_) {
        int total = packet_source_->getTotalFrames();
        if (total > 0) {
            return seek(total - 1);
        }
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
    // ⭐ v2.9修改：从数据源获取总帧数（适配器模式）
    if (packet_source_) {
        return packet_source_->getTotalFrames();
    }
    return -1;
}

int FfmpegDecodeVideoFileWorker::getCurrentFrameIndex() const {
    return current_frame_index_;
}

size_t FfmpegDecodeVideoFileWorker::getFrameSize() const {
    // ✅ 使用实际解码输出格式计算（getBytesPerPixel从实际格式获取）
    return (size_t)(output_width_ * output_height_ * getBytesPerPixel());
}

long FfmpegDecodeVideoFileWorker::getFileSize() const {
    // ⭐ v2.9修改：从数据源获取文件大小
    if (packet_source_) {
        return packet_source_->getFileSize();
    }
    return -1;
}

int FfmpegDecodeVideoFileWorker::getWidth() const {
    return output_width_;
}

int FfmpegDecodeVideoFileWorker::getHeight() const {
    return output_height_;
}

double FfmpegDecodeVideoFileWorker::getBytesPerPixel() const {
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

const char* FfmpegDecodeVideoFileWorker::getPath() const {
    // ⭐ v2.9修改：从数据源获取文件路径
    if (!packet_source_) {
        return nullptr;
    }
    
    // Buffer 模式：返回 nullptr（无文件路径）
    if (dynamic_cast<BufferPacketSource*>(packet_source_.get())) {
        return nullptr;
    }
    
    // 文件模式：从数据源获取路径
    // 注意：返回的指针需要保证生命周期，这里使用静态变量存储
    static thread_local std::string cached_path;
    cached_path = packet_source_->getFilePath();
    return cached_path.empty() ? nullptr : cached_path.c_str();
}

bool FfmpegDecodeVideoFileWorker::hasMoreFrames() const {
    // ⭐ v2.9修改：从数据源获取 EOF 状态
    if (!packet_source_) {
        return false;
    }
    return !packet_source_->isEof();
}

bool FfmpegDecodeVideoFileWorker::isAtEnd() const {
    // ⭐ v2.9修改：从数据源获取 EOF 状态
    if (!packet_source_) {
        return true;
    }
    return packet_source_->isEof();
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
bool FfmpegDecodeVideoFileWorker::fillBufferMetadataFromFrame(AVFrame* frame_ptr, Buffer* buffer) {
    // ⭐ 硬件解码器：提取物理内存地址
    if (!decoder_name_.empty() && use_hardware_decoder_) {
        if (!extractHardwareAddressFromMetadata(frame_ptr, buffer)) {
            LOG4CPLUS_ERROR_FMT(logger_, "[Worker] Hardware decoder '%s': Failed to extract physical address",
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
        LOG_TRACE_FMT("[Worker] Updated buffer size to actual frame size: %d bytes", actual_frame_size);
    } else {
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] Failed to get frame buffer size: %d", actual_frame_size);
    }
    
    // ⭐ 设置图像元数据（格式、宽高、linesize 等）
    buffer->setImageMetadataFromAVFrame(frame_ptr);
    
    // ⭐ 更新统计计数器
    decoded_frames_++;
    current_frame_index_++;
    
    return true;
}

/**
 * @brief 从数据源读取 packet 并发送到解码器
 * @param packet_ptr AVPacket 指针（必须已分配）
 * @return true 成功发送 packet 到解码器，false 失败或 EOF
 */
bool FfmpegDecodeVideoFileWorker::readAndSendPacket(AVPacket* packet_ptr) {
    // 步骤1: 从数据源读取 packet
    const int AVERROR_INVALIDDATA_VALUE = -1094995529;  // AVERROR(0x41444e49)
    const int MAX_CORRUPTED_RETRIES = 10;  // 最大重试次数，避免无限循环
    
    int corrupted_retries = 0;
    int read_ret;
    
    while (true) {
        // 使用数据源抽象读取 packet
        read_ret = packet_source_->readPacket(packet_ptr);
        
        if (read_ret < 0) {
            if (read_ret == AVERROR_EOF) {
                LOG4CPLUS_DEBUG(logger_, "🔄 EOF reached");
                // 🔧 修复：Worker 不应该决定是否循环，只返回 false
                // EOF 状态由数据源管理（通过 isEof() 查询）
                // 循环逻辑由 ProductionLine 根据 loop_ 变量控制
                av_packet_unref(packet_ptr);
                return false;
            } else if (read_ret == AVERROR_INVALIDDATA_VALUE) {
                // 🔧 修复：遇到损坏帧时，在内部循环跳过，继续读取下一个 packet
                corrupted_retries++;
                if (corrupted_retries <= MAX_CORRUPTED_RETRIES) {
                    LOG4CPLUS_WARN_FMT(logger_, "[Worker] WARNING: Corrupted packet detected (attempt %d/%d), skipping...\n", 
                           corrupted_retries, MAX_CORRUPTED_RETRIES);
                    av_packet_unref(packet_ptr);
                    // 继续循环，尝试读取下一个 packet
                    continue;
                } else {
                    // 连续多次都是损坏帧，可能文件确实损坏严重，返回失败
                    LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ERROR: Too many corrupted packets (%d), giving up\n", corrupted_retries);
                    av_packet_unref(packet_ptr);
                    return false;
                }
            } else {
                // 其他错误（非 EOF，非损坏帧）：记录错误并返回
                char err_buf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(read_ret, err_buf, sizeof(err_buf));
                LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ERROR: readPacket failed: %d (%s)\n", read_ret, err_buf);
                av_packet_unref(packet_ptr);
                return false;
            }
        } else {
            // 成功读取到 packet，退出循环
            break;
        }
    }
    
    // 步骤2: 检查是否是视频流（仅文件模式需要，Buffer 模式已经过滤）
    if (auto* file_source = dynamic_cast<FilePacketSource*>(packet_source_.get())) {
        (void)file_source;  // 仅用于类型检查
        // ⚠️ 注意：video_stream_index_ 已移除，直接从数据源获取
        if (packet_ptr->stream_index != packet_source_->getVideoStreamIndex()) {
            // 🔧 修复：不是视频流的packet需要释放，然后继续读取下一个
            av_packet_unref(packet_ptr);
            return false;  // 让调用者再次调用以读取下一个packet
        }
    }
    // Buffer 模式：packet 已经是视频流，不需要检查
    
    // 步骤3: 发送 packet 到解码器
    int ret = avcodec_send_packet(codec_ctx_ptr_, packet_ptr);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ERROR: avcodec_send_packet failed: %d (%s)", ret, err_buf);
        return false;
    }
    
    return true;
}

bool FfmpegDecodeVideoFileWorker::fillBuffer(int frame_index, Buffer* buffer) {
    // ========== 参数校验 ==========
    if (!buffer) {
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ERROR: buffer is nullptr");
        return false;
    }
    
    if (!packet_source_->isOpen()) {
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ERROR: Worker is not open");
        return false;
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 从 Buffer 获取关联的 AVFrame* 和 AVPacket*
    AVFrame* frame_ptr = buffer->getAVFrame();
    if (!frame_ptr) {
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ERROR: buffer->getAVFrame() is nullptr");
        return false;
    }
    
    AVPacket* packet_ptr = buffer->getAVPacket();
    if (!packet_ptr) {
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ERROR: buffer->getAVPacket() is nullptr");
        return false;
    }
    
    if (!packet_source_) {
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] ERROR: packet_source_ is nullptr");
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
// 辅助方法
// ============================================================================

void FfmpegDecodeVideoFileWorker::setError(const std::string& error, int ffmpeg_error) {
    last_error_ = error;
    
    if (ffmpeg_error != 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ffmpeg_error, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] FfmpegDecodeVideoFileWorker Error: %s (FFmpeg: %s)\n", error.c_str(), err_buf);
    } else {
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker] FfmpegDecodeVideoFileWorker Error: %s", error.c_str());
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

const AVCodecParameters* FfmpegDecodeVideoFileWorker::getCodecParameters() const {
    if (!packet_source_) {
        return nullptr;
    }
    return packet_source_->getCodecParameters();
}

AVRational FfmpegDecodeVideoFileWorker::getTimeBase() const {
    if (!packet_source_) {
        return {1, 25};  // 默认值
    }
    
    // 从数据源获取编解码器参数
    const AVCodecParameters* codecpar = packet_source_->getCodecParameters();
    if (!codecpar) {
        return {1, 25};  // 默认值
    }
    
    // 对于视频流，通常使用帧率的倒数作为时间基
    // 这里返回一个通用的时间基（可以根据实际需求调整）
    // 注意：如果需要更精确的时间基，应该从 AVStream 获取
    return {1, 25};  // 默认25fps
}

int FfmpegDecodeVideoFileWorker::getOriginalWidth() const {
    if (packet_source_) {
        const AVCodecParameters* codecpar = packet_source_->getCodecParameters();
        if (codecpar) {
            return codecpar->width;
        }
    }
    return 0;
}

int FfmpegDecodeVideoFileWorker::getOriginalHeight() const {
    if (packet_source_) {
        const AVCodecParameters* codecpar = packet_source_->getCodecParameters();
        if (codecpar) {
            return codecpar->height;
        }
    }
    return 0;
}

void FfmpegDecodeVideoFileWorker::printStats() const {
    LOG4CPLUS_INFO(logger_, "\n[Worker] 📊 FfmpegDecodeVideoFileWorker Statistics:");
    // ⭐ v2.9修改：从数据源获取文件路径
    std::string file_path = packet_source_ ? packet_source_->getFilePath() : std::string();
    LOG4CPLUS_INFO_FMT(logger_, "[Worker]    File: %s", file_path.empty() ? "(Buffer Mode)" : file_path.c_str());
    LOG4CPLUS_INFO_FMT(logger_, "[Worker]    Codec: %s", getCodecName());
    LOG4CPLUS_INFO_FMT(logger_, "[Worker]    Resolution: %dx%d → %dx%d", getOriginalWidth(), getOriginalHeight(), output_width_, output_height_);
    LOG4CPLUS_INFO_FMT(logger_, "[Worker]    Total frames: %d", packet_source_ ? packet_source_->getTotalFrames() : -1);
    LOG4CPLUS_INFO_FMT(logger_, "[Worker]    Current frame: %d", current_frame_index_);
    LOG4CPLUS_INFO_FMT(logger_, "[Worker]    Decoded frames: %d", decoded_frames_.load());
    LOG4CPLUS_INFO_FMT(logger_, "[Worker]    EOF: %s", packet_source_ && packet_source_->isEof() ? "YES" : "NO");
}

// ============================================================================
// 硬件解码器元数据提取（重写基类虚函数）
// ============================================================================

bool FfmpegDecodeVideoFileWorker::extractHardwareAddressFromMetadata(AVFrame* frame, Buffer* buffer) {
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


int FfmpegDecodeVideoFileWorker::getSourceWidth() const {
    return packet_source_ ? packet_source_->getSourceWidth() : 0;
}

int FfmpegDecodeVideoFileWorker::getSourceHeight() const {
    return packet_source_ ? packet_source_->getSourceHeight() : 0;
}

AVPixelFormat FfmpegDecodeVideoFileWorker::getSourcePixelFormat() const {
    return packet_source_ ? packet_source_->getSourcePixelFormat() : AV_PIX_FMT_NONE;
}
