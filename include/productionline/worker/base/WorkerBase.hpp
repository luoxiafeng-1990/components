  #ifndef WORKER_BASE_HPP
#define WORKER_BASE_HPP

#include "../interface/IVideoFileNavigator.hpp"
#include "../../../buffer/Buffer.hpp"
#include "../../../buffer/allocator/facade/BufferAllocatorFacade.hpp"
#include "../../../buffer/allocator/factory/BufferAllocatorFactory.hpp"
#include "../../../buffer/BufferPool.hpp"
#include <memory>
#include <utility>  // for std::move

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
     */
    explicit WorkerBase(
        BufferAllocatorFactory::AllocatorType allocator_type
    ) : allocator_facade_(allocator_type)  // 🎯 父类直接创建Allocator门面
      , buffer_pool_id_(0)  // v2.0: 记录 pool_id 而不是指针
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
     * @brief 获取Worker类型名称（用于调试和日志）
     * 
     * 纯虚函数：强制所有子类必须实现
     * 
     * @return 类型名称（如 "FfmpegDecodeVideoFileWorker"、"MmapRawVideoFileWorker"）
     */
    virtual const char* getWorkerType() const = 0;
    
    /**
     * @brief 获取输出 BufferPool ID（如果有）
     * 
     * 默认实现：返回 buffer_pool_id_
     * 子类可以重写此方法
     * 
     * @return uint64_t pool_id（成功），0（失败或未创建）
     * 
     * @note Worker必须在open()时创建BufferPool，否则返回 0
     * @note 调用者从 Registry 获取临时访问（getPool(pool_id)）
     */
    virtual uint64_t getOutputBufferPoolId() {
        return buffer_pool_id_;
    }
    
    // ==================== 可选配置接口（特定 Worker 可重写）====================
    
    /**
     * @brief 设置解码器名称（可选配置，仅部分 Worker 支持）
     * 
     * 默认实现：空操作（非编码视频 Worker 忽略此配置）
     * 子类可选择性重写此方法以提供实际功能
     * 
     * 适用 Worker：
     * - FfmpegDecodeVideoFileWorker: 设置 FFmpeg 解码器（如 "h264_taco"）
     * - FfmpegDecodeRtspWorker: 设置 RTSP 流解码器
     * 
     * @param decoder_name 解码器名称（nullptr 表示自动选择）
     * 
     * @note 此方法应在 open() 之前调用
     * @note 非编码视频 Worker（如 MmapRawVideoFileWorker）可忽略此配置
     */
    virtual void setDecoderName(const char* decoder_name) {
        // 默认空实现：不支持解码器配置的 Worker 忽略此调用
        (void)decoder_name;  // 避免未使用参数警告
    }
    
    // ==================== 文件导航功能（继承自IVideoFileNavigator）====================
    // 以下方法继承自 IVideoFileNavigator，子类必须实现
    virtual bool open(const char* path) override = 0;
    virtual bool open(const char* path, int width, int height, int bits_per_pixel) override = 0;
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
    virtual int getBytesPerPixel() const override = 0;
    virtual const char* getPath() const override = 0;
    virtual bool hasMoreFrames() const override = 0;
    virtual bool isAtEnd() const override = 0;
    
protected:
    /**
     * @brief Allocator门面（所有Worker子类自动继承）
     */
    BufferAllocatorFacade allocator_facade_;
    
    /**
     * @brief Worker创建的BufferPool ID（v2.0 所有Worker子类自动继承）
     * 
     * v2.0 设计变更：
     * - 使用 pool_id 而不是指针
     * - Registry 独占持有 BufferPool（shared_ptr，引用计数=1）
     * - Worker 只记录 pool_id，从 Registry 临时访问
     * - 符合中心化资源管理原则
     */
    uint64_t buffer_pool_id_;
};

#endif // WORKER_BASE_HPP

