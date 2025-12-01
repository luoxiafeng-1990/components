#ifndef BUFFER_FILLING_WORKER_FACADE_HPP
#define BUFFER_FILLING_WORKER_FACADE_HPP

#include "../base/WorkerBase.hpp"
#include "../interface/IBufferFillingWorker.hpp"
#include "../interface/IVideoFileNavigator.hpp"
#include "../factory/BufferFillingWorkerFactory.hpp"
#include "../../../buffer/Buffer.hpp"
#include "../../../buffer/BufferPool.hpp"
#include <memory>
#include <stddef.h>
#include <sys/types.h>

/**
 * @brief BufferFillingWorkerFacade - Buffer填充Worker门面类
 * 
 * 架构角色：门面（Facade）- 统一Worker接口
 * 
 * 设计模式：门面模式（Facade Pattern）
 * 
 * 职责：
 * - 为用户提供统一、简单的Buffer填充操作接口
 * - 隐藏底层多种实现（mmap、io_uring、FFmpeg等）的复杂性
 * - 自动选择最优的Worker实现
 * 
 * 特点：
 * - 统一的API接口，简化使用
 * - 底层实现可以透明切换
 * - 支持自动和手动选择Worker类型
 * - 使用WorkerBase基类，无需dynamic_cast
 * 
 * 使用方式（统一智能接口）：
 * 
 * **编码视频（MP4, AVI, RTSP等）：**
 * ```cpp
 * BufferFillingWorkerFacade worker;
 * worker.setWorkerType(BufferFillingWorkerFactory::WorkerType::FFMPEG_VIDEO_FILE);
 * worker.open("video.mp4");  // 自动检测格式，无需指定宽高
 * worker.fillBuffer(0, &buffer);
 * ```
 * 
 * **Raw视频：**
 * ```cpp
 * BufferFillingWorkerFacade worker;
 * worker.setWorkerType(BufferFillingWorkerFactory::WorkerType::MMAP_RAW);
 * worker.open("video.raw", 1920, 1080, 32);  // 指定格式参数
 * worker.fillBuffer(0, &buffer);
 * ```
 * 
 * **通用方式（推荐）：**
 * ```cpp
 * // 调用者无需关心视频类型，只需传入所有可能需要的参数
 * // 门面类会根据Worker类型自动判断是否使用这些参数
 * worker.open(path, width, height, bpp);
 * ```
 * 
 * **接口实现：**
 * - 实现 IBufferFillingWorker 和 IVideoFileNavigator 两个接口
 * - 所有方法签名与接口保持一致，确保类型安全
 */
class BufferFillingWorkerFacade : public IBufferFillingWorker, public IVideoFileNavigator {
private:
    // ============ 门面模式：持有具体实现 ============
    std::unique_ptr<WorkerBase> worker_base_uptr_;  // 实际的Worker实现（统一基类）
    BufferFillingWorkerFactory::WorkerType preferred_type_;  // 用户偏好的类型

public:
    // ============ 构造/析构 ============
    
    /**
     * 构造函数
     * @param type Worker类型（默认AUTO，自动选择最优实现）
     * 
     * @note 推荐做法：不依赖默认值，显式调用 setWorkerType() 来明确Worker类型
     * 
     * 使用示例：
     * @code
     * BufferFillingWorkerFacade worker;
     * worker.setWorkerType(BufferFillingWorkerFactory::WorkerType::MMAP_RAW);  // 明确指定
     * worker.open(path, width, height, bpp);
     * @endcode
     */
    BufferFillingWorkerFacade(BufferFillingWorkerFactory::WorkerType type = BufferFillingWorkerFactory::WorkerType::AUTO);
    
    /**
     * 析构函数
     */
    ~BufferFillingWorkerFacade();
    
    // 禁止拷贝
    BufferFillingWorkerFacade(const BufferFillingWorkerFacade&) = delete;
    BufferFillingWorkerFacade& operator=(const BufferFillingWorkerFacade&) = delete;
    
    // ============ Worker类型控制 ============
    
    /**
     * 设置Worker类型（在 open 之前调用）
     * @param type Worker类型
     */
    void setWorkerType(BufferFillingWorkerFactory::WorkerType type);
    
    // ============ IBufferFillingWorker 接口实现 ============
    const char* getWorkerType() const override;
    
    // ============ IVideoFileNavigator 接口实现 ============
    bool open(const char* path) override;
    bool open(const char* path, int width, int height, int bits_per_pixel) override;
    void close() override;
    bool isOpen() const override;
    bool fillBuffer(int frame_index, Buffer* buffer) override;
    bool seek(int frame_index) override;
    bool seekToBegin() override;
    bool seekToEnd() override;
    bool skip(int frame_count) override;
    int getTotalFrames() const override;
    int getCurrentFrameIndex() const override;
    size_t getFrameSize() const override;
    long getFileSize() const override;
    int getWidth() const override;
    int getHeight() const override;
    int getBytesPerPixel() const override;
    const char* getPath() const override;
    bool hasMoreFrames() const override;
    bool isAtEnd() const override;
    uint64_t getOutputBufferPoolId() override;
};

#endif // BUFFER_FILLING_WORKER_FACADE_HPP

