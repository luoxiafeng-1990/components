#ifndef BUFFER_FILLING_WORKER_HPP
#define BUFFER_FILLING_WORKER_HPP

#include "IBufferFillingWorker.hpp"
#include "IVideoFileNavigator.hpp"
#include "BufferFillingWorkerFactory.hpp"
#include "../../buffer/Buffer.hpp"
#include "../../buffer/BufferPool.hpp"
#include <memory>
#include <stddef.h>
#include <sys/types.h>

/**
 * @brief BufferFillingWorker - Buffer填充Worker门面类
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
 * - API 保持不变，向后兼容
 * - 底层实现可以透明切换
 * - 支持自动和手动选择Worker类型
 * 
 * 使用方式（统一智能接口）：
 * 
 * **编码视频（MP4, AVI, RTSP等）：**
 * ```cpp
 * BufferFillingWorker worker;
 * worker.setWorkerType(BufferFillingWorkerFactory::WorkerType::FFMPEG_VIDEO_FILE);
 * worker.open("video.mp4");  // 自动检测格式，无需指定宽高
 * worker.fillBuffer(0, &buffer);
 * ```
 * 
 * **Raw视频：**
 * ```cpp
 * BufferFillingWorker worker;
 * worker.setWorkerType(BufferFillingWorkerFactory::WorkerType::MMAP_RAW);
 * worker.openRaw("video.raw", 1920, 1080, 32);  // 指定格式参数
 * worker.fillBuffer(0, &buffer);
 * ```
 * 
 * **通用方式（推荐）：**
 * ```cpp
 * // 调用者无需关心视频类型，只需传入所有可能需要的参数
 * // 门面类会根据Worker类型自动判断是否使用这些参数
 * worker.open(path, width, height, bpp);
 * ```
 */
class BufferFillingWorker {
private:
    // ============ 门面模式：持有具体实现 ============
    std::unique_ptr<IBufferFillingWorker> worker_;  // 实际的Worker实现（Buffer填充接口）
    IVideoFileNavigator* navigator_;  // 文件导航接口（指向同一对象）
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
     * BufferFillingWorker worker;
     * worker.setWorkerType(BufferFillingWorkerFactory::WorkerType::MMAP_RAW);  // 明确指定
     * worker.openRaw(path, width, height, bpp);
     * @endcode
     */
    BufferFillingWorker(BufferFillingWorkerFactory::WorkerType type = BufferFillingWorkerFactory::WorkerType::AUTO);
    
    /**
     * 析构函数
     */
    ~BufferFillingWorker();
    
    // 禁止拷贝
    BufferFillingWorker(const BufferFillingWorker&) = delete;
    BufferFillingWorker& operator=(const BufferFillingWorker&) = delete;
    
    // ============ Worker类型控制 ============
    
    /**
     * 设置Worker类型（在 open 之前调用）
     * @param type Worker类型
     */
    void setWorkerType(BufferFillingWorkerFactory::WorkerType type);
    
    /**
     * 设置读取器类型（向后兼容）
     * @deprecated 推荐使用 setWorkerType()
     */
    void setReaderType(BufferFillingWorkerFactory::WorkerType type) {
        setWorkerType(type);
    }
    
    /**
     * 获取当前Worker类型名称
     * @return 类型名称（如 "MmapRawVideoFileWorker"）
     */
    const char* getWorkerType() const;
    
    /**
     * 获取读取器类型名称（向后兼容）
     * @deprecated 推荐使用 getWorkerType()
     */
    const char* getReaderType() const {
        return getWorkerType();
    }
    
    // ============ 文件操作（转发到 worker_） ============
    
    /**
     * 打开视频文件（统一智能接口）
     * @param path 文件路径
     * @param width 视频宽度（可选，用于raw视频）
     * @param height 视频高度（可选，用于raw视频）
     * @param bits_per_pixel 每像素位数（可选，用于raw视频）
     * @return 成功返回true
     * 
     * @note 门面类会根据Worker类型自动判断：
     *       - 编码视频Worker（FFMPEG_VIDEO_FILE, FFMPEG_RTSP）：忽略 width/height/bpp，自动检测格式
     *       - Raw视频Worker（MMAP_RAW, IOURING_RAW）：使用传入的 width/height/bpp 参数
     * 
     * @note 调用者无需关心内部实现细节，只需传入所有可能需要的参数即可
     * 
     * 使用示例：
     * @code
     * // 编码视频（自动检测格式）
     * worker.setWorkerType(BufferFillingWorkerFactory::WorkerType::FFMPEG_VIDEO_FILE);
     * worker.open("video.mp4");  // width/height/bpp 被忽略
     * 
     * // Raw视频（需要格式参数）
     * worker.setWorkerType(BufferFillingWorkerFactory::WorkerType::MMAP_RAW);
     * worker.open("video.raw", 1920, 1080, 32);  // 使用格式参数
     * @endcode
     */
    bool open(const char* path, int width = 0, int height = 0, int bits_per_pixel = 0);
    
    /**
     * 打开raw视频文件（向后兼容接口）
     * @deprecated 推荐直接使用 open(path, width, height, bpp)
     */
    bool openRaw(const char* path, int width, int height, int bits_per_pixel);
    
    void close();
    bool isOpen() const;
    
    // ============ 读取操作（转发） ============
    
    /**
     * @brief 填充Buffer（核心功能）
     * @param frame_index 帧索引
     * @param buffer 输出 Buffer（从 BufferPool 获取）
     * @return 成功返回 true
     */
    bool fillBuffer(int frame_index, Buffer* buffer);
    
    
    /**
     * 读取并填充一帧（向后兼容接口）
     * @deprecated 推荐使用fillBuffer()
     * @note 此方法转发到fillBuffer()，保留用于向后兼容
     */
    bool readFrame(int frame_index, Buffer* buffer) {
        return fillBuffer(frame_index, buffer);
    }
    
    // ============ 导航操作（转发） ============
    
    bool seek(int frame_index);
    bool seekToBegin();
    bool seekToEnd();
    bool skip(int frame_count);
    
    // ============ 信息查询（转发） ============
    
    int getTotalFrames() const;
    int getCurrentFrameIndex() const;
    size_t getFrameSize() const;
    long getFileSize() const;
    int getWidth() const;
    int getHeight() const;
    int getBytesPerPixel() const;
    const char* getPath() const;
    bool hasMoreFrames() const;
    bool isAtEnd() const;
    
    /**
     * 查询 Worker 是否需要外部提供 buffer
     * @return true - 需要外部 buffer（预分配模式）
     *         false - 不需要外部 buffer（动态注入模式）
     */
    bool requiresExternalBuffer() const;
    
    // ============ 提供原材料（BufferPool）============
    
    /**
     * 获取输出 BufferPool（如果有）
     * @return BufferPool的智能指针，如果Worker创建了内部BufferPool，返回unique_ptr；否则返回nullptr
     * 
     * @note Worker在open()时自动调用allocator创建BufferPool（如果需要）
     *       如果Worker创建了BufferPool，通过此方法返回，转移所有权给调用者
     */
    std::unique_ptr<BufferPool> getOutputBufferPool();
    
    /**
     * 获取输出BufferPool原始指针（向后兼容）
     * @deprecated 推荐使用getOutputBufferPool()返回智能指针
     */
    void* getOutputBufferPoolRaw() const;
};

#endif // BUFFER_FILLING_WORKER_HPP

