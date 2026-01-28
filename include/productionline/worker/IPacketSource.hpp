#ifndef IPACKET_SOURCE_HPP
#define IPACKET_SOURCE_HPP

#include "productionline/worker/IDataSourceNavigator.hpp"

// FFmpeg 前向声明
struct AVPacket;

/**
 * @brief IPacketSource - 数据源抽象接口
 * 
 * 设计模式：策略模式（Strategy Pattern）
 * 
 * 继承关系：
 * - 继承 IDataSourceNavigator，获得所有数据源操作接口（20个方法）
 * - 添加底层特有方法：readPacket、getVideoStreamIndex
 * 
 * 职责：
 * - 继承：数据源操作（open/close/seek/状态查询/属性查询等）
 * - 特有：提供 packet 读取接口（readPacket）
 * - 特有：提供视频流索引查询（getVideoStreamIndex）
 * 
 * 设计理念：
 * - 符合 SOLID 原则（依赖倒置、开闭原则、里氏替换）
 * - 支持多种数据源（文件、Buffer、网络流等）
 * - 易于扩展和测试
 * 
 * 使用场景：
 * - 文件模式：从本地文件读取 packet
 * - Buffer 模式：从 BufferPool 获取 packet（MultiWorkerProductionLine）
 * - 网络流模式：从网络流读取 packet（RTSP等）
 * 
 * 实现类：
 * - FilePacketSource：本地文件实现
 * - RtspPacketSource：RTSP流实现
 * - BufferPacketSource：BufferPool实现
 */
class IPacketSource : public IDataSourceNavigator {
public:
    virtual ~IPacketSource() = default;
    
    // ============ IPacketSource 特有方法（2个）============
    
    /**
     * @brief 读取一个 packet
     * @param packet 输出的 packet（必须已分配）
     * @return 0=成功, AVERROR_EOF=文件结束, <0=错误
     * 
     * 注意：
     * - 文件模式：使用 FFmpeg 的 av_read_frame() 读取
     * - Buffer 模式：从 BufferPool 获取 filled buffer
     * - 网络流模式：从 RTSP 流读取
     */
    virtual int readPacket(AVPacket* packet) = 0;
    
    /**
     * @brief 获取视频流索引
     * @return 视频流索引，如果不可用则返回 -1
     * 
     * @note 用于 FFmpeg 解码时判断 packet 属于哪个流
     */
    virtual int getVideoStreamIndex() const = 0;
};

#endif // IPACKET_SOURCE_HPP

