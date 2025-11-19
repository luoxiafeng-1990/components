#ifndef WORKER_BASE_HPP
#define WORKER_BASE_HPP

#include "../interface/IBufferFillingWorker.hpp"
#include "../interface/IVideoFileNavigator.hpp"

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
 * 
 * 职责：
 * - 作为所有Worker实现类的统一基类
 * - 同时实现两个接口的功能（通过子类实现）
 * - 提供统一的类型系统，便于多态使用
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
 */
class WorkerBase : public IBufferFillingWorker, public IVideoFileNavigator {
public:
    virtual ~WorkerBase() = default;
    
    // 所有方法都是纯虚函数，由子类实现
    // IBufferFillingWorker 接口方法
    virtual bool fillBuffer(int frame_index, Buffer* buffer) = 0;
    virtual const char* getWorkerType() const = 0;
    virtual std::unique_ptr<BufferPool> getOutputBufferPool() override {
        return nullptr;
    }
    
    // IVideoFileNavigator 接口方法
    virtual bool open(const char* path) = 0;
    virtual bool open(const char* path, int width, int height, int bits_per_pixel) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool seek(int frame_index) = 0;
    virtual bool seekToBegin() = 0;
    virtual bool seekToEnd() = 0;
    virtual bool skip(int frame_count) = 0;
    virtual int getTotalFrames() const = 0;
    virtual int getCurrentFrameIndex() const = 0;
    virtual size_t getFrameSize() const = 0;
    virtual long getFileSize() const = 0;
    virtual int getWidth() const = 0;
    virtual int getHeight() const = 0;
    virtual int getBytesPerPixel() const = 0;
    virtual const char* getPath() const = 0;
    virtual bool hasMoreFrames() const = 0;
    virtual bool isAtEnd() const = 0;
};

#endif // WORKER_BASE_HPP

