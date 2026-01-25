#ifndef IDATA_SOURCE_NAVIGATOR_HPP
#define IDATA_SOURCE_NAVIGATOR_HPP

#include <cstddef>  // For size_t
#include <string>

// FFmpeg 头文件
extern "C" {
#include <libavutil/pixfmt.h>  // AVPixelFormat 需要完整定义
}

// FFmpeg 前向声明
struct AVCodecParameters;

/**
 * @brief IDataSourceNavigator - 数据源导航接口
 * 
 * 架构角色：数据源操作接口 - 规范所有数据源相关操作
 * 
 * 职责：
 * - 数据源打开/关闭操作（open的两个重载版本、close、isOpen）
 * - 数据源导航操作（seek、skip等）
 * - 数据源状态查询（getTotalFrames、getCurrentFrameIndex等）
 * - 数据源属性查询（getSourceWidth、getSourceHeight等）
 * 
 * 设计模式：接口分离原则（ISP - Interface Segregation Principle）
 * - 将所有数据源操作功能从Worker中分离
 * - Worker继承此接口，实现时转发给内部的PacketSource（策略模式）
 * - 通过继承IDataSourceNavigator，明确表达Worker的数据源操作职责
 * 
 * 继承关系：
 * - IPacketSource 继承此接口，添加底层特有方法（readPacket、getVideoStreamIndex）
 * - Worker 继承此接口，转发给内部的 IPacketSource
 * 
 * 适用范围：
 * - 视频数据源（本地文件、RTSP流、共享Buffer等）
 * - 音频数据源（未来扩展）
 * - 其他流媒体数据源
 * 
 * 优势：
 * - 职责清晰：所有数据源操作功能独立为独立接口
 * - 符合ISP：需要数据源操作时依赖IDataSourceNavigator
 * - 可扩展：未来可以独立扩展数据源功能
 * - 文档明确：通过接口名称明确表达职责
 * 
 * 注意：
 * - Worker的输出属性（getOutputWidth/Height/BytesPerPixel等）不在此接口中
 * - 这些是Worker处理后的结果，定义在WorkerBase类中
 */
class IDataSourceNavigator {
public:
    virtual ~IDataSourceNavigator() = default;
    
    // ============ 数据源类型枚举 ============
    
    /**
     * @brief 数据源类型
     */
    enum class SourceType {
        FILE_SOURCE,      // 本地文件
        BUFFER_SOURCE,    // BufferPool（共享内存）
        NETWORK_SOURCE    // 网络流（RTSP等）
    };
    
    // ============ 数据源打开/关闭操作 ============
    
    /**
     * 打开数据源（从 WorkerConfig 读取所有参数）
     * @return 成功返回true
     * 
     * @note v2.13设计：
     *       - Worker 从自己的 worker_config_ 读取所有参数（路径、分辨率等）
     *       - 符合单一数据源原则
     *       - 子类可以选择性地支持 open(path) 作为快捷方式
     */
    virtual bool open() = 0;
    
    /**
     * 打开数据源（指定路径，可选）
     * @param path 数据源路径（文件路径/URL等，可以覆盖 config 中的路径）
     * @return 成功返回true
     * 
     * @note 这是一个便捷方法，用于快速打开指定数据源
     *       - 对于文件数据源，可以覆盖 config 中的路径
     *       - 对于某些数据源（如RTSP），需要完整配置，不支持单路径 open
     */
    virtual bool open(const char* path) = 0;
    
    /**
     * 关闭数据源
     */
    virtual void close() = 0;
    
    /**
     * 检查数据源是否已打开
     */
    virtual bool isOpen() const = 0;
    
    // ============ 数据源导航操作 ============
    
    /**
     * 跳转到指定帧
     * @param frame_index 帧索引
     * @return 成功返回true
     */
    virtual bool seek(int frame_index) = 0;
    
    /**
     * 回到数据源开头
     */
    virtual bool seekToBegin() = 0;
    
    /**
     * 跳转到数据源末尾
     */
    virtual bool seekToEnd() = 0;
    
    /**
     * 跳过N帧（可正可负）
     * @param frame_count 跳过的帧数
     * @return 成功返回true
     */
    virtual bool skip(int frame_count) = 0;
    
    // ============ 数据源状态查询 ============
    
    /**
     * 获取总帧数
     */
    virtual int getTotalFrames() const = 0;
    
    /**
     * 获取当前帧索引（数据源当前进度）
     */
    virtual int getCurrentFrameIndex() const = 0;
    
    /**
     * 获取单帧大小（字节）
     */
    virtual size_t getFrameSize() const = 0;
    
    /**
     * 获取文件/数据大小（字节）
     */
    virtual long getFileSize() const = 0;
    
    /**
     * 获取数据源路径
     * @return 数据源路径（文件路径/URL等）
     */
    virtual std::string getPath() const = 0;
    
    /**
     * 检查是否还有更多帧
     */
    virtual bool hasMoreFrames() const = 0;
    
    /**
     * 检查是否到达数据源末尾
     */
    virtual bool isAtEnd() const = 0;
    
    // ============ 数据源属性（原始的、未处理的）============
    
    /**
     * 获取数据源的原始视频宽度
     * @return 原始宽度（像素），如果不可用返回 0
     * 
     * @note 这是数据源的原始分辨率，不是Worker输出分辨率
     *       - 对于视频文件：从编解码器参数获取
     *       - 对于RTSP流：从流信息获取
     */
    virtual int getSourceWidth() const = 0;
    
    /**
     * 获取数据源的原始视频高度
     * @return 原始高度（像素），如果不可用返回 0
     * 
     * @note 这是数据源的原始分辨率，不是Worker输出分辨率
     */
    virtual int getSourceHeight() const = 0;
    
    /**
     * 获取数据源的编解码器参数
     * @return AVCodecParameters 指针，如果不可用返回 nullptr
     * 
     * @note 用于：
     *       - Worker初始化解码器
     *       - BufferWriter写入文件时需要编解码器信息
     *       - MultiWorkerProductionLine创建消费者Worker时传递编解码器参数
     */
    virtual const AVCodecParameters* getCodecParameters() const = 0;
    
    /**
     * 获取数据源的原始像素格式
     * @return AVPixelFormat，如果不可用返回 AV_PIX_FMT_NONE
     * 
     * @note 这是输入数据源的编码格式，不是解码器输出格式
     * @example H.264 编码的视频通常是 AV_PIX_FMT_YUV420P（编码前的格式）
     */
    virtual AVPixelFormat getSourcePixelFormat() const = 0;
    
    /**
     * 获取数据源类型
     * @return 数据源类型枚举（FILE_SOURCE、BUFFER_SOURCE、NETWORK_SOURCE）
     */
    virtual SourceType getDataSourceType() const = 0;
};

#endif // IDATA_SOURCE_NAVIGATOR_HPP
