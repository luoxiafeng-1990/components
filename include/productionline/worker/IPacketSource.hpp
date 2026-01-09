#ifndef IPACKET_SOURCE_HPP
#define IPACKET_SOURCE_HPP

#include <memory>
#include <string>

// FFmpeg 头文件
extern "C" {
#include <libavutil/pixfmt.h>  // ⭐ AVPixelFormat 需要完整定义，不能前向声明
}

// FFmpeg 前向声明
struct AVPacket;
struct AVCodecParameters;

/**
 * @brief IPacketSource - 数据源抽象接口
 * 
 * 设计模式：策略模式（Strategy Pattern）
 * 
 * 职责：
 * - 抽象数据源操作（文件、Buffer、网络流等）
 * - 提供统一的 packet 读取接口
 * - 隐藏具体数据源的实现细节
 * 
 * 设计理念：
 * - 符合 SOLID 原则（依赖倒置、开闭原则）
 * - 支持多种数据源（文件、Buffer、网络流等）
 * - 易于扩展和测试
 * 
 * 使用场景：
 * - 文件模式：从本地文件读取 packet
 * - Buffer 模式：从 BufferPool 获取 packet（MultiWorkerProductionLine）
 * - 网络流模式：从网络流读取 packet（未来扩展）
 */
class IPacketSource {
public:
    virtual ~IPacketSource() = default;
    
    /**
     * @brief 打开数据源
     * @return true 如果成功
     */
    virtual bool open() = 0;
    
    /**
     * @brief 关闭数据源
     */
    virtual void close() = 0;
    
    /**
     * @brief 检查数据源是否已打开
     * @return true 如果已打开
     */
    virtual bool isOpen() const = 0;
    
    /**
     * @brief 读取一个 packet
     * @param packet 输出的 packet（必须已分配）
     * @return 0=成功, AVERROR_EOF=文件结束, <0=错误
     */
    virtual int readPacket(AVPacket* packet) = 0;
    
    /**
     * @brief 获取编解码器参数
     * @return AVCodecParameters* 指针，如果不可用则返回 nullptr
     */
    virtual const AVCodecParameters* getCodecParameters() const = 0;
    
    /**
     * @brief 获取视频流索引
     * @return 视频流索引，如果不可用则返回 -1
     */
    virtual int getVideoStreamIndex() const = 0;
    
    /**
     * @brief 获取总帧数（如果可用）
     * @return 总帧数，如果不可用则返回 -1
     */
    virtual int getTotalFrames() const = 0;
    
    /**
     * @brief 获取文件大小（仅文件模式可用）
     * @return 文件大小（字节），如果不可用则返回 -1
     */
    virtual long getFileSize() const = 0;
    
    /**
     * @brief 获取文件路径（仅文件模式可用）
     * @return 文件路径，如果不可用则返回空字符串
     */
    virtual std::string getFilePath() const = 0;
    
    /**
     * @brief 定位到指定帧索引（仅文件模式支持）
     * @param frame_index 帧索引
     * @return true 如果成功，false 如果不支持或失败
     * 
     * 注意：
     * - 文件模式：使用 FFmpeg 的 av_seek_frame() 实现真正的定位
     * - Buffer 模式：流式数据，不支持 seek，返回 false
     * - 网络流模式：可能不支持 seek，取决于具体实现
     */
    virtual bool seek(int frame_index) = 0;
    
    /**
     * @brief 检查是否到达文件末尾
     * @return true 如果已到达 EOF
     * 
     * 注意：
     * - 文件模式：基于最后一次 readPacket() 的返回值，在 seek() 时重置
     * - Buffer 模式：基于 BufferPool 的可用性，无法准确判断（让 readPacket() 超时返回）
     * - 网络流模式：基于连接状态
     */
    virtual bool isEof() const = 0;
    
    /**
     * @brief 获取输入数据源的原始视频宽度
     * @return 视频宽度（像素），如果不可用则返回 0
     * 
     * @note 这是输入数据源（文件/流）的原始分辨率，不是解码器输出分辨率
     * @example 输入视频是 1920x1080，即使 TACO 配置了缩放，此方法仍返回 1920
     */
    virtual int getSourceWidth() const = 0;
    
    /**
     * @brief 获取输入数据源的原始视频高度
     * @return 视频高度（像素），如果不可用则返回 0
     * 
     * @note 这是输入数据源（文件/流）的原始分辨率，不是解码器输出分辨率
     * @example 输入视频是 1920x1080，即使 TACO 配置了缩放，此方法仍返回 1080
     */
    virtual int getSourceHeight() const = 0;
    
    /**
     * @brief 获取输入数据源的原始像素格式
     * @return AVPixelFormat，如果不可用则返回 AV_PIX_FMT_NONE
     * 
     * @note 这是输入数据源的编码格式，不是解码器输出格式
     * @example H.264 编码的视频通常是 AV_PIX_FMT_YUV420P（编码前的格式）
     */
    virtual AVPixelFormat getSourcePixelFormat() const = 0;
};

#endif // IPACKET_SOURCE_HPP

