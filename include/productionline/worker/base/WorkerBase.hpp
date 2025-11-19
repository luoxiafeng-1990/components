#ifndef WORKER_BASE_HPP
#define WORKER_BASE_HPP

#include "../interface/IBufferFillingWorker.hpp"
#include "../interface/IVideoFileNavigator.hpp"
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
 * 设计目的：
 * - 统一所有Worker实现类的基类
 * - 同时继承IBufferFillingWorker和IVideoFileNavigator两个接口
 * - 避免在门面类中使用dynamic_cast进行类型转换
 * - 提供统一的类型标识，便于工厂模式和门面模式使用
 * - 提供统一的Allocator和BufferPool管理（所有Worker的共同职责）
 * 
 * 职责：
 * - 作为所有Worker实现类的统一基类
 * - 同时实现两个接口的功能（通过子类实现）
 * - 提供统一的类型系统，便于多态使用
 * - 提供统一的Allocator门面（所有Worker都需要创建BufferPool）
 * - 管理Worker创建的BufferPool（通过Allocator创建）
 * 
 * 继承关系：
 * - WorkerBase 继承 IBufferFillingWorker 和 IVideoFileNavigator
 * - 所有具体Worker实现类继承 WorkerBase
 * 
 * 优势：
 * - 类型安全：不需要dynamic_cast，直接使用基类指针即可访问两个接口
 * - 代码简洁：门面类只需要一个worker_指针，不需要单独的navigator_指针
 * - 架构清晰：明确的继承层次，符合面向对象设计原则
 * - 易于维护：统一的基类便于扩展和维护
 * - 统一管理：所有Worker自动继承allocator_和buffer_pool_，无需每个子类重复定义
 */
class WorkerBase : public IBufferFillingWorker, public IVideoFileNavigator {
public:
    /**
     * @brief 构造函数
     * 
     * @param allocator_type Allocator类型（默认AUTO，自动选择NormalAllocator）
     * 
     * 子类使用方式：
     * ```cpp
     * // 在子类构造函数初始化列表中传递 AllocatorType
     * FfmpegDecodeVideoFileWorker::FfmpegDecodeVideoFileWorker()
     *     : WorkerBase(BufferAllocatorFactory::AllocatorType::NORMAL)  // 指定类型
     *     , format_ctx_(nullptr)
     *     , ...
     * {
     * }
     * ```
     * 
     * Allocator类型选择建议：
     * - NORMAL: Raw视频文件（MmapRawVideoFileWorker, IoUringRawVideoFileWorker）
     * - AVFRAME: FFmpeg解码，需要动态注入（FfmpegDecodeRtspWorker）
     * - FRAMEBUFFER: Framebuffer内存包装
     * - AUTO: 默认使用NormalAllocator
     */
    explicit WorkerBase(
        BufferAllocatorFactory::AllocatorType allocator_type = BufferAllocatorFactory::AllocatorType::AUTO
    ) : allocator_(allocator_type) {
    }
    
    virtual ~WorkerBase() = default;
    
    // ==================== 公开接口（实现 IBufferFillingWorker 和 IVideoFileNavigator）====================
    // 所有方法都是纯虚函数，由子类实现
    
    // IBufferFillingWorker 接口方法
    virtual bool fillBuffer(int frame_index, Buffer* buffer) override = 0;
    virtual const char* getWorkerType() const override = 0;
    /**
     * @brief 获取Worker创建的BufferPool（默认实现）
     * 
     * 子类可以重写此方法，但通常不需要（直接使用基类的buffer_pool_即可）
     * 
     * @return unique_ptr<BufferPool> 成功返回pool，失败返回nullptr
     * 
     * @note Worker必须在open()时创建BufferPool，否则返回nullptr
     */
    virtual std::unique_ptr<BufferPool> getOutputBufferPool() override {
        if (!buffer_pool_) {
            return nullptr;
        }
        // 转移所有权给调用者
        return std::move(buffer_pool_);
    }
    
    // IVideoFileNavigator 接口方法
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
     * 
     * 用途：
     * - Worker在open()时通过allocator_创建BufferPool
     * - 子类直接调用：allocator_.allocatePoolWithBuffers(...)
     * 
     * 注意：
     * - Allocator类型在构造函数中确定（通过基类构造函数参数）
     * - 子类在构造函数初始化列表中指定：WorkerBase(AllocatorType::NORMAL)
     * - 如果子类需要不同的Allocator类型，只需在构造函数中传递不同的类型即可
     * - 通常不需要在运行时更换Allocator类型（类型在构造时确定）
     */
    BufferAllocatorFacade allocator_;
    
    /**
     * @brief Worker创建的BufferPool（所有Worker子类自动继承）
     * 
     * 用途：
     * - Worker在open()时创建并保存BufferPool
     * - Worker通过getOutputBufferPool()返回给ProductionLine（转移所有权）
     * 
     * 生命周期：
     * - 在open()时创建
     * - 在getOutputBufferPool()时转移所有权给ProductionLine
     * - 在close()时如果未转移则自动释放
     */
    std::unique_ptr<BufferPool> buffer_pool_;
};

#endif // WORKER_BASE_HPP

