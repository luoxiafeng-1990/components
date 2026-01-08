#ifndef IVIDEO_FILE_NAVIGATOR_HPP
#define IVIDEO_FILE_NAVIGATOR_HPP

#include <cstddef>  // For size_t

/**
 * @brief IVideoFileNavigator - 视频文件导航接口
 * 
 * 架构角色：文件操作接口 - 负责所有文件相关操作
 * 
 * 职责：
 * - 文件打开/关闭操作（open的两个重载版本、close、isOpen）
 * - 文件导航操作（seek、skip等）
 * - 文件状态查询（getTotalFrames、getCurrentFrameIndex等）
 * 
 * 设计模式：接口分离原则（ISP - Interface Segregation Principle）
 * - 将所有文件操作功能从IBufferFillingWorker中分离
 * - Worker可以同时实现IBufferFillingWorker和IVideoFileNavigator
 * - 通过继承IVideoFileNavigator，明确表达Worker的文件操作职责
 * 
 * 优势：
 * - 职责清晰：所有文件操作功能独立为独立接口
 * - 符合ISP：需要文件操作时依赖IVideoFileNavigator，需要填充Buffer时依赖IBufferFillingWorker
 * - 可扩展：未来可以独立扩展文件操作功能
 * - 文档明确：通过接口名称明确表达职责
 */
class IVideoFileNavigator {
public:
    virtual ~IVideoFileNavigator() = default;
    
    // ============ 文件打开/关闭操作 ============
    
    /**
     * 打开视频文件（从 WorkerConfig 读取所有参数）
     * @return 成功返回true
     * 
     * @note v2.13设计：
     *       - Worker 从自己的 worker_config_ 读取所有参数（路径、分辨率等）
     *       - 符合单一数据源原则
     *       - 子类可以选择性地支持 open(path) 作为快捷方式
     */
    virtual bool open() = 0;
    
    /**
     * 打开视频文件（指定路径，可选）
     * @param path 文件路径（可以覆盖 config 中的路径）
     * @return 成功返回true
     * 
     * @note 这是一个便捷方法，用于快速打开指定文件
     *       - 对于 VideoFileWorker，可以覆盖 config 中的路径
     *       - 对于 RtspWorker，需要完整配置，不支持单路径 open
     */
    virtual bool open(const char* path) = 0;
    
    /**
     * 关闭视频文件
     */
    virtual void close() = 0;
    
    /**
     * 检查文件是否已打开
     */
    virtual bool isOpen() const = 0;
    
    // ============ 文件导航操作 ============
    
    /**
     * 跳转到指定帧
     * @param frame_index 帧索引
     * @return 成功返回true
     */
    virtual bool seek(int frame_index) = 0;
    
    /**
     * 回到文件开头
     */
    virtual bool seekToBegin() = 0;
    
    /**
     * 跳转到文件末尾
     */
    virtual bool seekToEnd() = 0;
    
    /**
     * 跳过N帧（可正可负）
     * @param frame_count 跳过的帧数
     * @return 成功返回true
     */
    virtual bool skip(int frame_count) = 0;
    
    // ============ 文件状态查询 ============
    
    /**
     * 获取总帧数
     */
    virtual int getTotalFrames() const = 0;
    
    /**
     * 获取当前帧索引
     */
    virtual int getCurrentFrameIndex() const = 0;
    
    /**
     * 获取单帧大小（字节）
     */
    virtual size_t getFrameSize() const = 0;
    
    /**
     * 获取文件大小（字节）
     */
    virtual long getFileSize() const = 0;
    
    /**
     * 获取视频宽度
     */
    virtual int getWidth() const = 0;
    
    /**
     * 获取视频高度
     */
    virtual int getHeight() const = 0;
    
    /**
     * 获取每像素字节数
     * @return 每像素字节数（浮点数，支持如NV12的1.5字节/像素）
     */
    virtual double getBytesPerPixel() const = 0;
    
    /**
     * 获取文件路径
     */
    virtual const char* getPath() const = 0;
    
    /**
     * 检查是否还有更多帧
     */
    virtual bool hasMoreFrames() const = 0;
    
    /**
     * 检查是否到达文件末尾
     */
    virtual bool isAtEnd() const = 0;
};

#endif // IVIDEO_FILE_NAVIGATOR_HPP

