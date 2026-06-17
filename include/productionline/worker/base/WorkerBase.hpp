#ifndef WORKER_BASE_HPP
#define WORKER_BASE_HPP

#include "productionline/worker/datasource/IDataSourceNavigator.hpp"
#include "productionline/error/FillStatus.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"
#include "bufferpool/pool/base/IBufferPoolBuilder.hpp"
#include "bufferpool/pool/builder/BufferPoolBuilderFactory.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"

#include <log4cplus/logger.h>
#include <map>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
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
 * - WorkerBase 继承 IDataSourceNavigator
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
class WorkerBase : public IDataSourceNavigator {
public:
    // ==================== 分辨率限制常量 ====================
    
    /// 最小允许的分辨率（宽或高）
    static constexpr int MIN_RESOLUTION = 16;
    
    /// 最大允许的分辨率（宽或高），支持到 8K
    static constexpr int MAX_RESOLUTION = 8192;
    
    // ==================== 构造/析构 ====================
    
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
     * 1. 父类 WorkerBase 构造（创建 builder_）
     * 2. 子类成员变量初始化
     * 3. 子类构造函数体执行
     * 
     * @param allocator_type Allocator类型（子类传递）
     * @param config Worker配置（v2.2新增）
     */
    explicit WorkerBase(
        BufferPoolBuilderFactory::AllocatorType allocator_type,
        const WorkerConfig& config = WorkerConfig()
    ) : builder_(BufferPoolBuilderFactory::create(allocator_type))
      , topology_id_(0)
      , buffer_pool_type_map_()
      , worker_config_(config)
      , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker")))
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
     * @return FillResult 结果对象
     * 
     * v2.33 变更：返回类型从 bool 改为 FillResult
     */
    virtual FillResult fillBuffer(int frame_index, Buffer* buffer) = 0;
    
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
        // 子类（如 FFmpegDecodeWorker）可以重写此方法
        (void)frame;   // 避免未使用参数警告
        (void)buffer;
        return false;
    }
    
    /**
     * @brief 获取Worker类型名称（用于调试和日志）
     * 
     * 纯虚函数：强制所有子类必须实现
     * 
     * @return 类型名称（如 "FFmpegDecodeWorker"、"MmapRawVideoFileWorker"）
     */
    virtual const char* getWorkerType() const = 0;
    
    /**
     * @brief 获取指定类型的 BufferPool ID（主要接口）
     * 
     * v2.0 设计：
     * - 使用统一的 BufferPoolType 枚举标识不同用途的 BufferPool
     * - Worker 只记录 pool_id，不持有 Pool 指针
     * - 使用者通过枚举获取 pool_id，再从 ComponentTopology 获取 Pool
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
    virtual const AVCodecParameters* getCodecParameters() const override {
        // 默认实现：不支持编解码器参数
        return nullptr;
    }
    
    /**
     * @brief 向后兼容的别名（deprecated，请使用 getCodecParameters()）
     */
    const AVCodecParameters* getSourceCodecParameters() const {
        return getCodecParameters();
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
     * 子类（如 FFmpegDecodeWorker）可以重写此方法
     * 
     * @param pool_weak Record Worker 的 BufferPool（weak_ptr）
     * @return true 如果成功设置，false 如果失败（不支持 Buffer 模式）
     * 
     * @note 子类实现示例：
     * @code
     * bool setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) override {
     *     auto* buffer_source = dynamic_cast<EncodedPacketSourceFromBuffer*>(packet_source_.get());
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
    
    // ==================== Worker 输出属性（处理后的结果）====================
    // 这些是 Worker 处理后的输出属性，不是数据源原始属性
    
    /**
     * @brief 获取 Worker 输出的视频宽度
     * @return 输出宽度（像素），可能与数据源原始宽度不同
     * 
     * @note 这是 Worker 处理后的输出分辨率，不是数据源原始分辨率
     *       - 对于解码Worker：可能经过硬件缩放（如TACO ch1_scale）
     *       - 对于RecorderWorker：等于数据源原始分辨率（不处理）
     *       - 与 getSourceWidth() 的区别：Source是输入，Output是输出
     */
    virtual int getOutputWidth() const = 0;
    
    /**
     * @brief 获取 Worker 输出的视频高度
     * @return 输出高度（像素），可能与数据源原始高度不同
     * 
     * @note 这是 Worker 处理后的输出分辨率，不是数据源原始分辨率
     */
    virtual int getOutputHeight() const = 0;
    
    /**
     * @brief 获取 Worker 输出的每像素字节数
     * 
     * @param channel 通道编号（默认 0）
     *   - channel = 0：主通道（通常是 YUV 格式）
     *   - channel = 1：第二通道（如 TACO 的 RGB 通道）
     * 
     * @return 每像素字节数（浮点数，支持如NV12的1.5字节/像素）
     *   - 返回 0.0 表示该通道不存在或未启用
     * 
     * @note 这是 Worker 解码后输出的像素格式，不是数据源编码格式
     *       - 计算基于 Worker 的解码器输出格式（YUV420、ARGB888等）
     *       - 数据源是压缩的（H.264、H.265），没有"每像素字节数"概念
     *       - 用于计算输出帧大小：getOutputWidth() * getOutputHeight() * getOutputBytesPerPixel()
     * @note 向后兼容：不传参数时等同于 getOutputBytesPerPixel(0)
     */
    virtual double getOutputBytesPerPixel(int channel = 0) const = 0;
    
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
    
    // ==================== 数据源导航功能（继承自IDataSourceNavigator）====================
    // 以下方法继承自 IDataSourceNavigator，子类必须实现
    
    /**
     * @brief 打开数据源（从 worker_config_ 读取所有参数）
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
     * @brief 打开数据源（指定路径）
     * 
     * @param path 数据源路径（可以覆盖 config 中的路径）
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
    virtual std::string getPath() const override = 0;
    virtual bool hasMoreFrames() const override = 0;
    virtual bool isAtEnd() const override = 0;
    virtual SourceType getDataSourceType() const override = 0;
    
protected:
    // ========== 编解码器类型检测工具（v2.18 新增）==========
    
    /**
     * @brief 判断解码器是否是硬件解码器（使用 FFmpeg 官方 API）
     * 
     * v2.18 设计：
     * - 使用 FFmpeg 官方 API 判断（`AV_CODEC_CAP_HARDWARE` 和 `avcodec_get_hw_config`）
     * - 替代不可靠的字符串匹配方式（strstr）
     * - 所有子类通过继承使用，无需重复实现
     * 
     * @param codec AVCodec 指针
     * @return true=硬件解码器，false=软件解码器
     * 
     * @note 判断依据：
     *       1. AVCodec->capabilities 中的 AV_CODEC_CAP_HARDWARE 标志
     *       2. AVCodec 是否有硬件配置（avcodec_get_hw_config）
     * 
     * @note 使用 virtual final 禁止子类覆盖，保证判断逻辑统一
     */
    virtual bool isHardwareDecoder(const AVCodec* codec) const final;
    
    /**
     * @brief 查找指定 codec_id 的纯软件解码器
     * 
     * v2.18 设计：
     * - 遍历所有注册的解码器，使用 isHardwareDecoder() 过滤硬件解码器
     * - 解决 FFmpeg 默认优先返回硬件解码器的问题
     * - 适用于用户明确要求软件解码（use_hardware_decoder_=false）的场景
     * 
     * @param codec_id 编解码器 ID（如 AV_CODEC_ID_H264）
     * @return 软件解码器指针，未找到返回 nullptr
     * 
     * @note 使用 virtual final 禁止子类覆盖，保证查找逻辑统一
     * @note 查找顺序：按 FFmpeg 注册顺序（av_codec_iterate）
     * 
     * @note 使用示例：
     * @code
     * // 在 initializeDecoder() 中，如果用户要求软件解码
     * if (!use_hardware_decoder_) {
     *     codec = findPureSoftwareDecoder(AV_CODEC_ID_H264);
     *     if (!codec) {
     *         return false;  // 无可用软件解码器
     *     }
     * }
     * @endcode
     */
    virtual const AVCodec* findPureSoftwareDecoder(AVCodecID codec_id) const final;
    
    /**
     * @brief BufferPool 构建器（所有Worker子类自动继承）
     */
    std::unique_ptr<IBufferPoolBuilder> builder_;
    
    /**
     * @brief Worker 在 ComponentTopology 中的唯一 ID
     *
     * 由 WorkerFactory::create() 在注册后设置。
     * registerBufferPool() 会自动利用此 ID 向 ComponentTopology 注册 Pool 关联。
     */
    uint64_t topology_id_;
    
public:
    /// 由 Factory 设置 ComponentTopology ID
    void setTopologyId(uint64_t id) { topology_id_ = id; }
    
protected:
    /**
     * @brief BufferPool 类型 → Pool ID 映射表（v2.0 新设计）
     * 
     * v2.0 设计变更：
     * - Worker 只记录 pool_id，不持有 Pool 指针
     * - 使用统一的 BufferPoolType 枚举标识不同用途的 BufferPool
     * - 使用者通过枚举获取 pool_id，再从 ComponentTopology 获取 Pool
     * - 符合中心化资源管理原则
     * 
     * @note 替代了旧的 buffer_pool_id_ 单个变量
     */
    std::map<BufferPoolType, uint64_t> buffer_pool_type_map_;
    
    /**
     * @brief 注册一个 BufferPool（供子类在 open() 中调用）
     * 
     * @param type BufferPool 类型枚举
     * @param pool_id BufferPool ID（由 builder_->allocatePoolWithBuffers() 返回）
     * @return true 注册成功，false 该类型已存在或 pool_id 无效
     * 
     * @note 同一类型只能注册一次，重复注册会返回 false
     * 
     * @note 使用示例：
     * @code
     * uint64_t pool_id = builder_->allocatePoolWithBuffers(...);
     * if (pool_id != 0) {
     *     registerBufferPool(BufferPoolType::DECODE_VIDEO_PRIMARY, pool_id);
     * }
     * @endcode
     */
    bool registerBufferPool(BufferPoolType type, uint64_t pool_id) {
        if (pool_id == 0) {
            return false;
        }
        
        if (buffer_pool_type_map_.find(type) != buffer_pool_type_map_.end()) {
            return false;
        }
        
        buffer_pool_type_map_[type] = pool_id;
        
        // 自动向拓扑注册 Pool → Worker 关联
        if (topology_id_ != 0) {
            ComponentTopology::getInstance().linkPoolToWorker(topology_id_, pool_id);
        }
        return true;
    }
    
    /**
     * @brief 注销一个 BufferPool（供子类在 close() 中调用）
     * 
     * @param type BufferPool 类型
     */
    void unregisterBufferPool(BufferPoolType type) {
        auto it = buffer_pool_type_map_.find(type);
        if (it != buffer_pool_type_map_.end()) {
            ComponentTopology::getInstance().unlinkPool(it->second);
            buffer_pool_type_map_.erase(it);
        }
    }
    
    /**
     * @brief 清空所有 BufferPool 注册（供子类在 close() 中调用）
     */
    void clearAllBufferPools() {
        for (const auto& pair : buffer_pool_type_map_) {
            ComponentTopology::getInstance().unlinkPool(pair.second);
        }
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
    
    // 日志器
    log4cplus::Logger logger_;
    
};

#endif // WORKER_BASE_HPP

