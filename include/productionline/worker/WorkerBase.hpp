  #ifndef WORKER_BASE_HPP
#define WORKER_BASE_HPP

#include "productionline/worker/IVideoFileNavigator.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "buffer/bufferpool/Buffer.hpp"
#include "buffer/BufferAllocatorFacade.hpp"
#include "buffer/BufferAllocatorFactory.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include <memory>
#include <utility>  // for std::move
#include <map>
#include <vector>
#include <optional>
#include <string>

// FFmpeg 头文件（用于编解码器类型检测）
extern "C" {
#include <libavcodec/avcodec.h>
}

/**
 * @brief BufferPool 类型枚举（统一规范）
 * 
 * 定义 Worker 可以创建的所有 BufferPool 类型
 * 所有 Worker 使用此枚举标识其输出的 BufferPool
 * 
 * v2.0 设计原则：
 * - 统一规范：所有 Worker 共用此枚举
 * - 类型安全：编译期检查，避免字符串拼写错误
 * - 易于扩展：添加新类型只需在此枚举中增加
 */
enum class BufferPoolType {
    // ========== 视频相关 ==========
    DECODE_VIDEO_PRIMARY,      // 主视频解码输出（默认通道）
    DECODE_VIDEO_SECONDARY,    // 副视频解码输出（如 TACO CH1）
    DECODE_VIDEO_THUMBNAIL,    // 视频缩略图输出
    DECODE_VIDEO_PREVIEW,      // 视频预览输出（低分辨率）
    
    // ========== 音频相关 ==========
    DECODE_AUDIO_PRIMARY,      // 主音频解码输出
    DECODE_AUDIO_SECONDARY,    // 副音频解码输出（多声道）
    
    // ========== 数据包相关 ==========
    PACKET_VIDEO,              // 视频 AVPacket 缓冲池
    PACKET_AUDIO,              // 音频 AVPacket 缓冲池
    PACKET_SUBTITLE,           // 字幕 AVPacket 缓冲池
    
    // ========== 编码相关 ==========
    ENCODE_VIDEO_INPUT,        // 编码器输入 BufferPool
    ENCODE_VIDEO_OUTPUT,       // 编码器输出 BufferPool
    ENCODE_AUDIO_INPUT,        // 音频编码器输入
    ENCODE_AUDIO_OUTPUT,       // 音频编码器输出
    
    // ========== 特殊用途 ==========
    RAW_FILE_READ,             // 原始文件读取缓冲池
    FRAMEBUFFER_OUTPUT,        // Framebuffer 输出缓冲池
    NETWORK_STREAM,            // 网络流缓冲池
    
    // ========== 扩展保留 ==========
    CUSTOM_1,                  // 自定义类型 1
    CUSTOM_2,                  // 自定义类型 2
    CUSTOM_3,                  // 自定义类型 3
};

/**
 * @brief BufferPoolType 转字符串（调试用）
 */
inline const char* bufferPoolTypeToString(BufferPoolType type) {
    switch (type) {
        case BufferPoolType::DECODE_VIDEO_PRIMARY:    return "DECODE_VIDEO_PRIMARY";
        case BufferPoolType::DECODE_VIDEO_SECONDARY:  return "DECODE_VIDEO_SECONDARY";
        case BufferPoolType::DECODE_VIDEO_THUMBNAIL:  return "DECODE_VIDEO_THUMBNAIL";
        case BufferPoolType::DECODE_VIDEO_PREVIEW:    return "DECODE_VIDEO_PREVIEW";
        case BufferPoolType::DECODE_AUDIO_PRIMARY:    return "DECODE_AUDIO_PRIMARY";
        case BufferPoolType::DECODE_AUDIO_SECONDARY:  return "DECODE_AUDIO_SECONDARY";
        case BufferPoolType::PACKET_VIDEO:            return "PACKET_VIDEO";
        case BufferPoolType::PACKET_AUDIO:            return "PACKET_AUDIO";
        case BufferPoolType::PACKET_SUBTITLE:         return "PACKET_SUBTITLE";
        case BufferPoolType::ENCODE_VIDEO_INPUT:      return "ENCODE_VIDEO_INPUT";
        case BufferPoolType::ENCODE_VIDEO_OUTPUT:     return "ENCODE_VIDEO_OUTPUT";
        case BufferPoolType::ENCODE_AUDIO_INPUT:      return "ENCODE_AUDIO_INPUT";
        case BufferPoolType::ENCODE_AUDIO_OUTPUT:     return "ENCODE_AUDIO_OUTPUT";
        case BufferPoolType::RAW_FILE_READ:           return "RAW_FILE_READ";
        case BufferPoolType::FRAMEBUFFER_OUTPUT:      return "FRAMEBUFFER_OUTPUT";
        case BufferPoolType::NETWORK_STREAM:          return "NETWORK_STREAM";
        case BufferPoolType::CUSTOM_1:                return "CUSTOM_1";
        case BufferPoolType::CUSTOM_2:                return "CUSTOM_2";
        case BufferPoolType::CUSTOM_3:                return "CUSTOM_3";
        default:                                      return "UNKNOWN";
    }
}

/**
 * @brief WorkerBase - Worker基类
 * 
 * 架构角色：抽象基类（Abstract Base Class）
 * 
 * 设计变更（v2.0）：
 * - 去除 IBufferFillingWorker 接口类
 * - 直接在 WorkerBase 中定义 Buffer 填充相关的纯虚函数
 * - 简化架构，减少不必要的抽象层
 * 
 * 设计目的：
 * - 统一所有Worker实现类的基类
 * - 定义 Buffer 填充功能（原 IBufferFillingWorker 的方法）
 * - 继承文件导航功能（IVideoFileNavigator 接口）
 * - 提供统一的Allocator和BufferPool管理（所有Worker的共同职责）
 * - 采用构造函数参数传递模式，父类统一管理Allocator创建逻辑
 * 
 * 职责：
 * - 作为所有Worker实现类的统一基类
 * - 定义 Buffer 填充功能（纯虚函数，强制子类实现）
 * - 继承文件导航功能（IVideoFileNavigator 接口）
 * - 提供统一的Allocator门面（所有Worker都需要创建BufferPool）
 * - 管理Worker创建的BufferPool（通过Allocator创建）
 * 
 * 继承关系：
 * - WorkerBase 继承 IVideoFileNavigator
 * - 所有具体Worker实现类继承 WorkerBase
 * 
 * 优势：
 * - 架构简化：减少一层接口抽象
 * - 强制实现：通过基类纯虚函数强制子类实现
 * - 易于维护：统一的基类便于扩展和维护
 * - 统一管理：所有Worker自动继承allocator_和buffer_pool_，无需每个子类重复定义
 * - 符合单一职责原则：子类关注业务逻辑，父类关注Allocator管理
 * 
 * 构造函数参数传递模式：
 * - 子类通过初始化列表向父类传递 AllocatorType
 * - 父类在构造函数中统一创建 Allocator
 * - 所有Allocator配置细节封装在Factory中
 * - 子类无需关心Allocator内部实现
 */
class WorkerBase : public IVideoFileNavigator {
public:
    /**
     * @brief 构造函数
     * 
     * Allocator类型选择建议：
     * - NORMAL: Raw视频文件Worker（需要内部分配内存）
     * - AVFRAME: FFmpeg解码Worker（需要动态注入AVFrame）
     * - FRAMEBUFFER: Framebuffer设备Worker（需要包装外部内存）
     * - AUTO: 默认使用NORMAL（不推荐，子类应明确指定）
     * 
     * 构造顺序：
     * 1. 父类 WorkerBase 构造（创建 allocator_facade_）
     * 2. 子类成员变量初始化
     * 3. 子类构造函数体执行
     * 
     * @param allocator_type Allocator类型（子类传递）
     * @param config Worker配置（v2.2新增）
     */
    explicit WorkerBase(
        BufferAllocatorFactory::AllocatorType allocator_type,
        const WorkerConfig& config = WorkerConfig()
    ) : allocator_facade_(allocator_type)  // 🎯 父类直接创建Allocator门面
      , buffer_pool_type_map_()  // v2.0: 初始化 BufferPool 类型映射表
      , worker_config_(config)  // 🎯 v2.2: 存储配置
    {
    }
    
    virtual ~WorkerBase() = default;
    
    // ==================== Buffer填充功能（原IBufferFillingWorker的方法）====================
    
    /**
     * @brief 填充Buffer（核心功能）
     * 
     * 纯虚函数：强制所有子类必须实现
     * 
     * @param frame_index 帧索引
     * @param buffer 输出 Buffer（从 BufferPool 获取）
     * @return 成功返回 true
     */
    virtual bool fillBuffer(int frame_index, Buffer* buffer) = 0;
    
    /**
     * @brief 从AVFrame元数据中提取硬件解码器的物理内存地址
     * 
     * ⭐ 设计原则：
     * - 由 Worker 负责提取，因为 Worker 知道解码器类型和上下文
     * - 默认实现返回 false（不支持硬件地址提取）
     * - 子类重写实现特定硬件解码器的提取逻辑
     * 
     * ⚠️ 调用时机：
     * - 只在使用硬件解码器时调用（decoder_name 非空且 enable_hardware=true）
     * - 软件解码不应调用此函数
     * 
     * @param frame AVFrame 指针（需要包含 libavcodec/avcodec.h）
     * @param buffer Buffer 指针（用于存储提取的物理地址）
     * @return true 成功提取物理地址，false 提取失败或不支持
     * 
     * @note 扩展点：不同硬件解码器子类可以重写此方法
     *       - h264_taco: 从 metadata 提取 pool_blk_id
     *       - h264_cuvid: 从 CUDA 设备内存获取
     *       - h264_qsv: 从 QSV 表面获取
     */
    virtual bool extractHardwareAddressFromMetadata(struct AVFrame* frame, Buffer* buffer) {
        // 默认实现：不支持硬件地址提取
        // 子类（如 FfmpegDecodeVideoFileWorker）可以重写此方法
        (void)frame;   // 避免未使用参数警告
        (void)buffer;
        return false;
    }
    
    /**
     * @brief 获取Worker类型名称（用于调试和日志）
     * 
     * 纯虚函数：强制所有子类必须实现
     * 
     * @return 类型名称（如 "FfmpegDecodeVideoFileWorker"、"MmapRawVideoFileWorker"）
     */
    virtual const char* getWorkerType() const = 0;
    
    /**
     * @brief 获取指定类型的 BufferPool ID（主要接口）
     * 
     * v2.0 设计：
     * - 使用统一的 BufferPoolType 枚举标识不同用途的 BufferPool
     * - Worker 只记录 pool_id，不持有 Pool 指针
     * - 使用者通过枚举获取 pool_id，再从 Registry 获取 Pool
     * 
     * @param type BufferPool 类型枚举
     * @return uint64_t Pool ID，如果不存在返回 0
     * 
     * @note 使用示例：
     * @code
     * uint64_t video_pool_id = worker->getOutputBufferPoolId(BufferPoolType::DECODE_VIDEO_PRIMARY);
     * uint64_t packet_pool_id = worker->getOutputBufferPoolId(BufferPoolType::PACKET_VIDEO);
     * @endcode
     */
    virtual uint64_t getOutputBufferPoolId(BufferPoolType type) const {
        auto it = buffer_pool_type_map_.find(type);
        return (it != buffer_pool_type_map_.end()) ? it->second : 0;
    }
    
    /**
     * @brief 检查是否存在指定类型的 BufferPool
     * 
     * @param type BufferPool 类型
     * @return true 存在，false 不存在
     */
    virtual bool hasBufferPoolType(BufferPoolType type) const {
        return buffer_pool_type_map_.find(type) != buffer_pool_type_map_.end();
    }
    
    /**
     * @brief 获取 Worker 的主要 BufferPool 类型
     * 
     * 这是一个查询方法，告诉调用者这个 Worker 的主要输出是什么类型。
     * 子类可以重写此方法以返回正确的主要类型。
     * 
     * @return BufferPoolType 主要 BufferPool 类型
     * 
     * @note 默认返回 DECODE_VIDEO_PRIMARY（适用于视频解码 Worker）
     * @note FfmpegRecordRtspWorker 应该重写为 PACKET_VIDEO
     * 
     * @note 使用示例：
     * @code
     * // 调用者不需要硬编码类型
     * BufferPoolType type = worker->getPrimaryBufferPoolType();
     * uint64_t pool_id = worker->getOutputBufferPoolId(type);
     * @endcode
     */
    virtual BufferPoolType getPrimaryBufferPoolType() const {
        return BufferPoolType::DECODE_VIDEO_PRIMARY;  // 默认值
    }
    
    // ==================== 编解码器参数获取功能（v2.14新增）====================
    
    /**
     * @brief 获取编解码器参数（用于 BufferWriter 等场景）
     * 
     * v2.14 设计：
     * - 虚函数：子类根据实际情况重写
     * - 默认实现：返回 nullptr（不支持编解码器参数的 Worker）
     * - 适用场景：Packet录制、编码器等需要提供编解码器信息的 Worker
     * 
     * @return AVCodecParameters* 编解码器参数指针，如果不可用则返回 nullptr
     * 
     * @note 子类实现示例：
     * @code
     * // FfmpegPacketRecorderWorker 实现
     * const AVCodecParameters* getCodecParameters() const override {
     *     return packet_source_ ? packet_source_->getCodecParameters() : nullptr;
     * }
     * @endcode
     */
    virtual const struct AVCodecParameters* getCodecParameters() const {
        // 默认实现：不支持编解码器参数
        return nullptr;
    }
    
    /**
     * @brief 设置源 BufferPool（用于 Buffer 模式）
     * 
     * 功能：在 Buffer 模式下，关联 Record Worker 的 BufferPool
     * 
     * 使用场景：
     * - MultiWorkerProductionLine 场景
     * - 消费者 Worker 从生产者 Worker 的 BufferPool 获取数据
     * 
     * 默认实现：返回 false（不支持 Buffer 模式）
     * 子类（如 FfmpegDecodeVideoFileWorker、FfmpegDecodeRtspWorker）可以重写此方法
     * 
     * @param pool_weak Record Worker 的 BufferPool（weak_ptr）
     * @return true 如果成功设置，false 如果失败（不支持 Buffer 模式）
     * 
     * @note 子类实现示例：
     * @code
     * bool setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) override {
     *     auto* buffer_source = dynamic_cast<BufferPacketSource*>(packet_source_.get());
     *     if (!buffer_source) {
     *         return false;
     *     }
     *     buffer_source->setSourceBufferPool(pool_weak);
     *     return true;
     * }
     * @endcode
     */
    virtual bool setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) {
        // 默认实现：不支持 Buffer 模式
        (void)pool_weak;  // 避免未使用参数警告
        return false;
    }
    
    /**
     * @brief 获取输入数据源的原始视频宽度
     * @return 视频宽度（像素），如果不可用则返回 0
     * 
     * @note 这是输入数据源（文件/流）的原始分辨率，不是解码器输出分辨率
     */
    virtual int getSourceWidth() const {
        return 0;
    }
    
    /**
     * @brief 获取输入数据源的原始视频高度
     * @return 视频高度（像素），如果不可用则返回 0
     * 
     * @note 这是输入数据源（文件/流）的原始分辨率，不是解码器输出分辨率
     */
    virtual int getSourceHeight() const {
        return 0;
    }
    
    /**
     * @brief 获取输入数据源的原始像素格式
     * @return AVPixelFormat，如果不可用则返回 AV_PIX_FMT_NONE
     * 
     * @note 这是输入数据源的编码格式，不是解码器输出格式
     */
    virtual AVPixelFormat getSourcePixelFormat() const {
        return AV_PIX_FMT_NONE;
    }
    
    /**
     * @brief 获取时间基（用于 BufferWriter 等场景）
     * 
     * v2.14 设计：
     * - 虚函数：子类根据实际情况重写
     * - 默认实现：返回 {1, 25}（25fps）
     * - 适用场景：Packet录制、编码器等需要提供时间基的 Worker
     * 
     * @return AVRational 时间基
     * 
     * @note 子类实现示例：
     * @code
     * // FfmpegPacketRecorderWorker 实现
     * AVRational getTimeBase() const override {
     *     return packet_source_ ? packet_source_->getTimeBase() : AVRational{1, 25};
     * }
     * @endcode
     */
    virtual struct AVRational getTimeBase() const {
        // 默认实现：返回 25fps
        return {1, 25};
    }
    
    // ==================== 解码器配置功能（v2.2新增）====================
    
    /**
     * @brief 设置解码器名称（用于FFmpeg解码Worker）
     * 
     * 默认实现：空操作（不支持解码器配置的Worker忽略此调用）
     * 子类可以重写此方法
     * 
     * @param decoder_name 解码器名称（如 "h264_taco", "h264_cuvid"）
     * 
     * @note 必须在 open() 之前调用
     * @note 只有FFmpeg类型的Worker需要重写此方法
     */
    virtual void setDecoderName(const char* decoder_name) {
        // 默认空实现：不支持解码器配置的 Worker 忽略此调用
        (void)decoder_name;
    }
    
    /**
     * @brief 启用/禁用硬件解码（用于FFmpeg解码Worker）
     * 
     * 默认实现：空操作（不支持硬件解码配置的Worker忽略此调用）
     * 子类可以重写此方法
     * 
     * @param enable true启用硬件解码，false禁用
     * 
     * @note 必须在 open() 之前调用
     * @note 只有FFmpeg类型的Worker需要重写此方法
     */
    virtual void setHardwareDecoder(bool enable) {
        // 默认空实现：不支持硬件解码配置的 Worker 忽略此调用
        (void)enable;
    }
    
    // ==================== 文件导航功能（继承自IVideoFileNavigator）====================
    // 以下方法继承自 IVideoFileNavigator，子类必须实现
    
    /**
     * @brief 打开视频文件（从 worker_config_ 读取所有参数）
     * 
     * v2.13设计：
     * - Worker 从自己的 worker_config_ 读取所有参数
     * - 默认实现：调用 open(path)，从 config 中提取路径
     * - 子类可以重写此方法以实现更复杂的初始化逻辑
     */
    virtual bool open() override {
        // 默认实现：调用 open(path)，从 config 中提取路径
        return open(worker_config_.data_source.path.c_str());
    }
    
    /**
     * @brief 打开视频文件（指定路径）
     * 
     * @param path 文件路径（可以覆盖 config 中的路径）
     * @return 成功返回true
     * 
     * @note 子类必须实现此方法
     */
    virtual bool open(const char* path) override = 0;
    virtual void close() override = 0;
    virtual bool isOpen() const override = 0;
    virtual bool seek(int frame_index) override = 0;
    virtual bool seekToBegin() override = 0;
    virtual bool seekToEnd() override = 0;
    virtual bool skip(int frame_count) override = 0;
    virtual int getTotalFrames() const override = 0;
    virtual int getCurrentFrameIndex() const override = 0;
    virtual size_t getFrameSize() const override = 0;
    virtual long getFileSize() const override = 0;
    virtual int getWidth() const override = 0;
    virtual int getHeight() const override = 0;
    virtual double getBytesPerPixel() const override = 0;
    virtual const char* getPath() const override = 0;
    virtual bool hasMoreFrames() const override = 0;
    virtual bool isAtEnd() const override = 0;
    
protected:
    /**
     * @brief Allocator门面（所有Worker子类自动继承）
     */
    BufferAllocatorFacade allocator_facade_;
    
    /**
     * @brief BufferPool 类型 → Pool ID 映射表（v2.0 新设计）
     * 
     * v2.0 设计变更：
     * - Worker 只记录 pool_id，不持有 Pool 指针
     * - 使用统一的 BufferPoolType 枚举标识不同用途的 BufferPool
     * - 使用者通过枚举获取 pool_id，再从 Registry 获取 Pool
     * - 符合中心化资源管理原则
     * 
     * @note 替代了旧的 buffer_pool_id_ 单个变量
     */
    std::map<BufferPoolType, uint64_t> buffer_pool_type_map_;
    
    /**
     * @brief 注册一个 BufferPool（供子类在 open() 中调用）
     * 
     * @param type BufferPool 类型枚举
     * @param pool_id BufferPool ID（由 allocator_facade_.allocatePoolWithBuffers() 返回）
     * @return true 注册成功，false 该类型已存在或 pool_id 无效
     * 
     * @note 同一类型只能注册一次，重复注册会返回 false
     * 
     * @note 使用示例：
     * @code
     * uint64_t pool_id = allocator_facade_.allocatePoolWithBuffers(...);
     * if (pool_id != 0) {
     *     registerBufferPool(BufferPoolType::DECODE_VIDEO_PRIMARY, pool_id);
     * }
     * @endcode
     */
    bool registerBufferPool(BufferPoolType type, uint64_t pool_id) {
        if (pool_id == 0) {
            return false;  // 无效的 pool_id
        }
        
        // 检查是否已存在
        if (buffer_pool_type_map_.find(type) != buffer_pool_type_map_.end()) {
            // 注意：不使用 LOG 宏，因为 Logger.hpp 可能未包含（避免循环依赖）
            return false;
        }
        
        buffer_pool_type_map_[type] = pool_id;
        return true;
    }
    
    /**
     * @brief 注销一个 BufferPool（供子类在 close() 中调用）
     * 
     * @param type BufferPool 类型
     */
    void unregisterBufferPool(BufferPoolType type) {
        buffer_pool_type_map_.erase(type);
    }
    
    /**
     * @brief 清空所有 BufferPool 注册（供子类在 close() 中调用）
     */
    void clearAllBufferPools() {
        buffer_pool_type_map_.clear();
    }
    
    // ========== 编解码器类型检测工具（v2.11 新增）==========
    
    /**
     * @brief 检查配置的解码器与实际编解码器是否匹配
     * @param actual_codec_id 实际的编解码器ID（从AVCodecParameters->codec_id获取）
     * @param decoder_name 配置的解码器名称（从config.decoder.name获取）
     * 
     * @note 只打印警告，不影响程序执行
     * @note 如果decoder_name为空或"auto"，则跳过检查
     * @note 如果匹配成功，不打印任何信息
     * 
     * @note 子类使用示例：
     * @code
     * // 在 open() 方法中，openMediaSource() 成功后调用
     * AVCodecParameters* codecpar = format_ctx_->streams[video_idx]->codecpar;
     * checkCodecMismatch(codecpar->codec_id, decoder_name_);
     * @endcode
     */
    void checkCodecMismatch(AVCodecID actual_codec_id, const std::string& decoder_name) const;
    
    /**
     * @brief 从解码器名称推断期望的编解码器ID
     * @param decoder_name 解码器名称（如 "h264_taco", "hevc", "vp9"）
     * @return 期望的AVCodecID，如果无法确定则返回AV_CODEC_ID_NONE
     * 
     * @note 支持常见的解码器名称映射：
     *       - "h264", "h264_taco", "h264_cuvid" → AV_CODEC_ID_H264
     *       - "h265", "hevc", "hevc_taco" → AV_CODEC_ID_HEVC
     *       - "vp8", "vp9", "av1" → 对应的 ID
     *       - "mpeg2", "mpeg4" → 对应的 ID
     * @note 名称匹配不区分大小写，使用 std::string::find()
     */
    static AVCodecID getExpectedCodecIdFromDecoderName(const std::string& decoder_name);
    
    /**
     * @brief 获取 AVCodecID 的友好名称
     * @param codec_id 编解码器ID
     * @return 友好的名称字符串（如 "H.264/AVC", "H.265/HEVC"）
     * 
     * @note 对于常见编解码器返回易读名称，其他返回 FFmpeg 原始名称
     */
    static std::string getCodecFriendlyName(AVCodecID codec_id);
    
    /**
     * @brief Worker配置（v2.2 所有Worker子类自动继承）
     * 
     * v2.2 设计变更：
     * - Worker 在构造时接收配置
     * - Worker 从配置中读取需要的参数
     * - 符合依赖注入原则
     */
    WorkerConfig worker_config_;
};

#endif // WORKER_BASE_HPP

