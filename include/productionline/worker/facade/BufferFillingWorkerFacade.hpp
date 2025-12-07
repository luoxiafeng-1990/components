#ifndef BUFFER_FILLING_WORKER_FACADE_HPP
#define BUFFER_FILLING_WORKER_FACADE_HPP

#include "../base/WorkerBase.hpp"
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
 * 设计变更（v2.1）：
 * - 去除对 IBufferFillingWorker 接口的依赖
 * - 不继承任何接口或基类，直接定义所有方法
 * - 所有方法转发给内部的 worker_base_uptr_
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
 * - 使用组合模式（持有 WorkerBase 指针），而不是继承
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
 */
class BufferFillingWorkerFacade {
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
    
    /**
     * 设置解码器名称（在 open 之前调用，仅对 FFmpeg Worker 有效）
     * @param decoder_name 解码器名称（如 "h264_taco"，nullptr=自动选择）
     */
    void setDecoderName(const char* decoder_name);
    
    // ============ Buffer填充方法（原IBufferFillingWorker的方法）============
    
    /**
     * 获取Worker类型名称
     * @return 类型名称
     */
    const char* getWorkerType() const;
    
    /**
     * 填充Buffer（核心功能）
     * @param frame_index 帧索引
     * @param buffer 输出 Buffer
     * @return 成功返回 true
     */
    bool fillBuffer(int frame_index, Buffer* buffer);
    
    /**
     * 获取输出 BufferPool ID
     * @return pool_id（成功），0（失败或未创建）
     */
    uint64_t getOutputBufferPoolId();
    
    // ============ 文件导航方法（原IVideoFileNavigator的方法）============
    
    /**
     * 打开编码视频文件（自动检测格式）
     * @param path 文件路径
     * @return 成功返回 true
     */
    bool open(const char* path);
    
    /**
     * 打开视频文件（统一智能接口）
     * @param path 文件路径
     * @param width 视频宽度（可选，用于raw视频）
     * @param height 视频高度（可选，用于raw视频）
     * @param bits_per_pixel 每像素位数（可选，用于raw视频）
     * @return 成功返回 true
     */
    bool open(const char* path, int width, int height, int bits_per_pixel);
    
    /**
     * 关闭视频文件
     */
    void close();
    
    /**
     * 检查文件是否已打开
     */
    bool isOpen() const;
    
    /**
     * 跳转到指定帧
     * @param frame_index 帧索引
     * @return 成功返回 true
     */
    bool seek(int frame_index);
    
    /**
     * 回到文件开头
     */
    bool seekToBegin();
    
    /**
     * 跳转到文件末尾
     */
    bool seekToEnd();
    
    /**
     * 跳过N帧（可正可负）
     * @param frame_count 跳过的帧数
     * @return 成功返回 true
     */
    bool skip(int frame_count);
    
    /**
     * 获取总帧数
     */
    int getTotalFrames() const;
    
    /**
     * 获取当前帧索引
     */
    int getCurrentFrameIndex() const;
    
    /**
     * 获取单帧大小（字节）
     */
    size_t getFrameSize() const;
    
    /**
     * 获取文件大小（字节）
     */
    long getFileSize() const;
    
    /**
     * 获取视频宽度
     */
    int getWidth() const;
    
    /**
     * 获取视频高度
     */
    int getHeight() const;
    
    /**
     * 获取每像素字节数
     */
    int getBytesPerPixel() const;
    
    /**
     * 获取文件路径
     */
    const char* getPath() const;
    
    /**
     * 检查是否还有更多帧
     */
    bool hasMoreFrames() const;
    
    /**
     * 检查是否到达文件末尾
     */
    bool isAtEnd() const;
};

#endif // BUFFER_FILLING_WORKER_FACADE_HPP

