#ifndef BUFFER_PACKET_SOURCE_HPP
#define BUFFER_PACKET_SOURCE_HPP

#include "productionline/worker/IPacketSource.hpp"
#include "buffer/bufferpool/Buffer.hpp"
#include <memory>
#include <string>
#include <atomic>

// FFmpeg 前向声明
struct AVCodecParameters;
struct AVPacket;

/**
 * @brief BufferPacketSource - Buffer 数据源实现
 * 
 * 功能：从 Buffer 中获取 AVPacket（已由 Record Worker 填充）
 * 
 * 使用场景：
 * - MultiWorkerProductionLine 场景
 * - 从 Record Worker 的 BufferPool 获取 packet
 * 
 * 工作流程：
 * 1. Record Worker 读取 RTSP 流，填充 AVPacket 到 BufferPool
 * 2. MultiWorkerProductionLine 从 Record BufferPool 获取 Buffer
 * 3. BufferPacketSource 从 Buffer 中读取 AVPacket
 * 4. 传递给解码器进行解码
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
    
    /**
     * @brief 设置当前要读取的 Buffer
     * @param buffer 包含 AVPacket 的 Buffer（由 Record Worker 填充）
     */
    void setCurrentBuffer(Buffer* buffer);
    
    /**
     * @brief 清除当前 Buffer
     */
    void clearCurrentBuffer();
    
private:
    const AVCodecParameters* codec_params_;  // 编解码器参数（从 Record Worker 获取）
    Buffer* current_buffer_;                 // 当前要读取的 Buffer
    std::atomic<bool> is_open_;              // 🎯 原子变量，保证线程安全的状态检查
    
    /**
     * @brief 从当前 Buffer 复制 packet 数据
     * @param dst_packet 目标 packet
     * @param src_packet 源 packet（从 Buffer 获取）
     * @return 0=成功, <0=错误
     */
    int copyPacket(AVPacket* dst_packet, const AVPacket* src_packet);
};

#endif // BUFFER_PACKET_SOURCE_HPP

