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
 * - 采用构造函数参数传递模式，父类统一管理Allocator创建逻辑
 * 
 * 职责：
 * - 作为所有Worker实现类的统一基类
 * - 同时实现两个接口的功能（通过子类实现）
 * - 提供统一的类型系统，便于多态使用
 * - 提供统一的Allocator门面（所有Worker都需要创建BufferPool）
 * - 管理Worker创建的BufferPool（通过Allocator创建）
 * - 统一决策：根据子类传递的AllocatorType，创建合适的Allocator
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
 * - 责任明确：子类只需传递类型参数，父类统一管理Allocator创建逻辑
 * - 符合单一职责原则：子类关注业务逻辑，父类关注Allocator管理
 * 
 * 构造函数参数传递模式：
 * - 子类通过初始化列表向父类传递 AllocatorType
 * - 父类在构造函数中统一创建 Allocator
 * - 所有Allocator配置细节封装在Factory中
 * - 子类无需关心Allocator内部实现
 */
class WorkerBase : public IBufferFillingWorker, public IVideoFileNavigator {
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
      , buffer_pool_sptr_(nullptr) 
    {
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
     * @note 从 shared_ptr 转换为 unique_ptr（通过 release，但 Allocator 和 Registry 仍持有 shared_ptr）
     */
    virtual std::unique_ptr<BufferPool> getOutputBufferPool() override {
        if (!buffer_pool_sptr_) {
            return nullptr;
        }
        // 从 shared_ptr 转换为 unique_ptr
        // 注意：Allocator 和 Registry 仍持有 shared_ptr，所以不会销毁
        BufferPool* raw_ptr = buffer_pool_sptr_.get();
        buffer_pool_sptr_.reset();  // Worker 不再持有
        return std::unique_ptr<BufferPool>(raw_ptr);  // ProductionLine 持有 unique_ptr
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
     */
    BufferAllocatorFacade allocator_facade_;
    
    /**
     * @brief Worker创建的BufferPool（所有Worker子类自动继承）
     */
    std::shared_ptr<BufferPool> buffer_pool_sptr_;
};

#endif // WORKER_BASE_HPP

