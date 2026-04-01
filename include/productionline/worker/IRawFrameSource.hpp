#ifndef IRAW_FRAME_SOURCE_HPP
#define IRAW_FRAME_SOURCE_HPP

#include "productionline/worker/IDataSourceNavigator.hpp"

// FFmpeg 前向声明
struct AVFrame;

/**
 * @brief IRawFrameSource - 原始帧数据源抽象接口
 * 
 * 设计模式：策略模式（Strategy Pattern）
 * 
 * 继承关系：
 * - 继承 IDataSourceNavigator，获得所有数据源操作接口（20个方法）
 * - 添加特有方法：readRawFrame
 * 
 * 职责：
 * - 继承：数据源操作（open/close/seek/状态查询/属性查询等）
 * - 特有：提供原始帧数据读取接口（readRawFrame）
 * 
 * 设计理念：
 * - 与 IEncodedPacketSource 形成对称
 * - IEncodedPacketSource: 读取编码后的 AVPacket（解码器输入）
 * - IRawFrameSource: 读取原始的 AVFrame（编码器输入）
 * 
 * 使用场景：
 * - 文件模式：从 YUV/RGB 文件读取原始帧
 * - Buffer 模式：从 BufferPool 获取已解码的帧（Pipeline：解码→编码）
 * 
 * 实现类：
 * - RawFrameSourceFromFile：YUV/RGB 文件实现
 * - RawFrameSourceFromBuffer：BufferPool 实现
 * 
 * 命名说明：
 * - Raw = 原始（未编码）的帧数据（YUV/RGB）
 * - 与 Encoded（编码后的 H.264/H.265 数据）形成对比
 */
class IRawFrameSource : public IDataSourceNavigator {
public:
    virtual ~IRawFrameSource() = default;
    
    // ============ IRawFrameSource 特有方法 ============
    
    /**
     * @brief 读取一帧原始数据
     * @param frame 输出的 AVFrame（必须已分配）
     * @return 0=成功, AVERROR_EOF=结束, <0=错误
     * 
     * 说明：
     * - 文件模式：从 YUV/RGB 文件读取原始帧数据并填充到 AVFrame
     * - Buffer 模式：从 BufferPool 获取已解码的 AVFrame
     * 
     * 命名说明：
     * - 此方法读取的是**原始**帧数据（未编码的 YUV/RGB）
     * - 与 IEncodedPacketSource::readEncodedPacket()（编码后的数据）形成对比
     */
    virtual int readRawFrame(AVFrame* frame) = 0;
    
    /**
     * @brief 获取帧宽度
     * @return 帧宽度（像素），如果不可用返回 0
     */
    virtual int getFrameWidth() const = 0;
    
    /**
     * @brief 获取帧高度
     * @return 帧高度（像素），如果不可用返回 0
     */
    virtual int getFrameHeight() const = 0;
    
    /**
     * @brief 获取帧像素格式
     * @return AVPixelFormat 枚举值
     */
    virtual int getFramePixelFormat() const = 0;
};

#endif // IRAW_FRAME_SOURCE_HPP
