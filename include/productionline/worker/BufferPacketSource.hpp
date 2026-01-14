#ifndef BUFFER_PACKET_SOURCE_HPP
#define BUFFER_PACKET_SOURCE_HPP

#include "productionline/worker/IPacketSource.hpp"
#include "buffer/bufferpool/Buffer.hpp"
#include <memory>
#include <string>
#include <atomic>

// 前向声明
struct AVCodecParameters;
struct AVPacket;
class BufferPool;

/**
 * @brief BufferPacketSource - Buffer 数据源实现
 * 
 * 功能：直接从 BufferPool 获取 filled Buffer（已由 Record Worker 填充）
 * 
 * 使用场景：
 * - MultiWorkerProductionLine 场景
 * - 从 Record Worker 的 BufferPool 直接获取 packet
 * 
 * 工作流程（v2.13 重构后）：
 * 1. Record Worker 读取 RTSP 流，填充 AVPacket 到 BufferPool
 * 2. BufferPacketSource 关联 Record Worker 的 BufferPool
 * 3. readPacket() 时：acquireFilled() → 复制 AVPacket → releaseFilled()
 * 4. 传递给解码器进行解码
 * 
 * 优势：
 * - 数据源自己负责从哪里获取数据（符合抽象语义）
 * - 无需 MultiWorkerPL 做中间复制
 * - 代码更简洁，职责更清晰
 */
class BufferPacketSource : public IPacketSource {
public:
    /**
     * @brief 构造函数
     * @param codec_params 编解码器参数（从 Record Worker 获取）
     */
    explicit BufferPacketSource(const AVCodecParameters* codec_params);
    
    /**
     * @brief 析构函数
     */
    ~BufferPacketSource() override;
    
    // 禁止拷贝
    BufferPacketSource(const BufferPacketSource&) = delete;
    BufferPacketSource& operator=(const BufferPacketSource&) = delete;
    
    // IPacketSource 接口实现
    bool open() override;
    void close() override;
    bool isOpen() const override;
    int readPacket(AVPacket* packet) override;
    const AVCodecParameters* getCodecParameters() const override;
    int getVideoStreamIndex() const override;
    int getTotalFrames() const override;
    long getFileSize() const override;
    std::string getFilePath() const override;
    
    /**
     * @brief 定位到指定帧索引（Buffer 模式不支持）
     * @param frame_index 帧索引
     * @return 总是返回 false（Buffer 模式不支持 seek）
     */
    bool seek(int frame_index) override;
    bool isEof() const override;
    int getSourceWidth() const override;
    int getSourceHeight() const override;
    AVPixelFormat getSourcePixelFormat() const override;
    
    /**
     * @brief 设置数据源 BufferPool（v2.13 新增）
     * @param pool_weak Record Worker 的 BufferPool（weak_ptr）
     * 
     * 说明：BufferPacketSource 会直接从这个 BufferPool 的 filled queue 获取数据
     */
    void setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak);
    
private:
    const AVCodecParameters* codec_params_;     // 编解码器参数（从 Record Worker 获取）
    std::weak_ptr<BufferPool> source_pool_;     // ⭐ v2.13：关联的 BufferPool（从 Record Worker）
    std::atomic<bool> is_open_;                 // 🎯 原子变量，保证线程安全的状态检查
    
    /**
     * @brief 从当前 Buffer 复制 packet 数据
     * @param dst_packet 目标 packet
     * @param src_packet 源 packet（从 Buffer 获取）
     * @return 0=成功, <0=错误
     */
    int copyPacket(AVPacket* dst_packet, const AVPacket* src_packet);
};

#endif // BUFFER_PACKET_SOURCE_HPP

