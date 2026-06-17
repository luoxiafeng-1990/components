#pragma once

#include "bufferpool/buffer/Buffer.hpp"

/**
 * @brief AVFrameBuffer - 持有 FFmpeg AVFrame + AVPacket 的 Buffer 子类
 *
 * 用于视频解码场景：
 * - avframe_ 保存解码后的帧数据
 * - avpacket_ 保存编码后的包数据
 */
class AVFrameBuffer : public Buffer {
public:
    AVFrameBuffer(uint32_t id, void* virt_addr, uint64_t phys_addr,
                  size_t size, Ownership ownership);
    ~AVFrameBuffer() override;

    // === AVFrame 载荷 ===
    AVFrame* getAVFrame() const override { return avframe_; }
    void setAVFrame(AVFrame* frame) override { avframe_ = frame; }

    // === AVPacket 载荷 ===
    AVPacket* getAVPacket() const override { return avpacket_; }
    void setAVPacket(AVPacket* packet) override { avpacket_ = packet; }

    // === 生命周期 ===
    void free() override;

    // === 硬件输出通道（仅 AVFrameBuffer 支持）===
    int getOutputChannel() const;




private:
    AVFrame* avframe_ = nullptr;
    AVPacket* avpacket_ = nullptr;
};
