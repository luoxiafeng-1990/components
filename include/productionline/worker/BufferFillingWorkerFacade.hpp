#ifndef BUFFER_FILLING_WORKER_FACADE_HPP
#define BUFFER_FILLING_WORKER_FACADE_HPP

#include "productionline/worker/WorkerBase.hpp"
#include "productionline/worker/BufferFillingWorkerFactory.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "buffer/bufferpool/Buffer.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
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
 * **使用方式（推荐）：**
 * ```cpp
 * // 通过 WorkerConfig 配置 Worker 类型和所有参数
 * WorkerConfig config;
 * config.worker_type = WorkerType::FFMPEG_VIDEO_FILE;
 * config.data_source.path = "video.mp4";
 * 
 * BufferFillingWorkerFacade worker(config);
 * worker.open();  // 所有参数从 config 获取
 * worker.fillBuffer(0, &buffer);
 * ```
 */
class BufferFillingWorkerFacade {
private:
    // ============ 门面模式：持有具体实现 ============
    std::unique_ptr<WorkerBase> worker_base_uptr_;  // 实际的Worker实现（统一基类）
    WorkerConfig config_;  // Worker配置（包含 worker_type 和所有配置参数）

public:
    // ============ 构造/析构 ============
    
    /**
     * 构造函数
     * @param config Worker配置（包含 worker_type 和所有配置参数）
     */
    explicit BufferFillingWorkerFacade(const WorkerConfig& config = WorkerConfig());
    
    /**
     * 析构函数
     */
    ~BufferFillingWorkerFacade();
    
    // 禁止拷贝
    BufferFillingWorkerFacade(const BufferFillingWorkerFacade&) = delete;
    BufferFillingWorkerFacade& operator=(const BufferFillingWorkerFacade&) = delete;
    
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
    uint64_t getOutputBufferPoolId(BufferPoolType type);
    
    /**
     * 获取 Worker 的主要 BufferPool 类型
     * @return BufferPoolType 主要类型
     */
    BufferPoolType getPrimaryBufferPoolType();
    
    /**
     * 获取底层 Worker 指针（用于访问特定Worker的方法）
     * @return Worker 基类指针，如果未创建则返回 nullptr
     */
    WorkerBase* getWorkerBase() const {
        return worker_base_uptr_.get();
    }
    
    /**
     * v2.13新增：设置 BufferPacketSource 的源 BufferPool（用于 Buffer 模式）
     * @param pool_weak Record Worker 的 BufferPool（weak_ptr）
     * @return 成功返回 true，如果底层 Worker 不支持，返回 false
     * 
     * 使用场景：
     * - MultiWorkerProductionLine 创建消费者 Worker 后，需要关联 Record Worker 的 BufferPool
     * - 必须在 open() 之前调用
     */
    bool setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak);
    
    // ============ 文件导航方法（原IVideoFileNavigator的方法）============
    
    /**
     * 打开视频文件（从内部 config_ 获取所有参数）
     * v2.2: 简化接口，所有参数从 config_ 获取
     * @return 成功返回 true
     */
    bool open();
    
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
     * @return 每像素字节数（浮点数，支持如NV12的1.5字节/像素）
     */
    double getBytesPerPixel() const;
    
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
    
    // ============ 编解码器参数获取（v2.14新增）============
    
    /**
     * 获取编解码器参数（用于 BufferWriter 等场景）
     * 
     * v2.14 设计：
     * - 门面类转发调用到底层 Worker
     * - 通过多态机制，自动调用正确的实现
     * - 不需要类型转换，符合开闭原则
     * 
     * @return AVCodecParameters* 编解码器参数指针，如果不可用则返回 nullptr
     * 
     * @note 使用场景：配合 BufferWriter 保存编码流到文件
     * @note 必须在 open() 之后调用
     * 
     * @note 使用示例：
     * @code
     * auto worker_facade = producer.getWorkerFacade();
     * const AVCodecParameters* codec_params = worker_facade->getCodecParameters();
     * AVRational time_base = worker_facade->getTimeBase();
     * 
     * BufferWriter writer;
     * writer.openEncoded(output_file, codec_params, time_base);
     * @endcode
     */
    const struct AVCodecParameters* getCodecParameters() const;
    
    /**
     * 获取时间基（用于 BufferWriter 等场景）
     * 
     * @return AVRational 时间基
     */
    struct AVRational getTimeBase() const;
    
    /**
     * 获取输入数据源的原始视频宽度
     * 
     * @return 视频宽度（像素），如果不可用则返回 0
     * @note 这是输入数据源（文件/流）的原始分辨率，不是解码器输出分辨率
     */
    int getSourceWidth() const;
    
    /**
     * 获取输入数据源的原始视频高度
     * 
     * @return 视频高度（像素），如果不可用则返回 0
     * @note 这是输入数据源（文件/流）的原始分辨率，不是解码器输出分辨率
     */
    int getSourceHeight() const;
    
    /**
     * 获取输入数据源的原始像素格式
     * 
     * @return AVPixelFormat，如果不可用则返回 AV_PIX_FMT_NONE
     * @note 这是输入数据源的编码格式，不是解码器输出格式
     */
    AVPixelFormat getSourcePixelFormat() const;
};

#endif // BUFFER_FILLING_WORKER_FACADE_HPP

