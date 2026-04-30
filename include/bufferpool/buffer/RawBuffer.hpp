#pragma once

#include "bufferpool/buffer/Buffer.hpp"
#include "vendor/contracts/IMemoryProvider.hpp"

/**
 * @brief RawBuffer - 纯原始内存的 Buffer 子类
 *
 * 用于物理连续内存场景（Framebuffer 显示、DMA 传输等）：
 * - memory_provider_ 非拥有指针，用于析构时归还内存
 * - avpacket_ 可选
 */
class RawBuffer : public Buffer {
public:
    /**
     * @param memory_provider 非拥有指针，Builder 拥有 provider 的生命周期
     *                        可为 nullptr（表示不需要 provider 释放内存）
     */
    RawBuffer(uint32_t id, void* virt_addr, uint64_t phys_addr,
              size_t size, Ownership ownership,
              IMemoryProvider* memory_provider = nullptr);
    ~RawBuffer() override;

    // === AVPacket 载荷（可选） ===
    AVPacket* getAVPacket() const override { return avpacket_; }
    void setAVPacket(AVPacket* packet) override { avpacket_ = packet; }

    // === 生命周期 ===
    void free() override;

private:
    AVPacket* avpacket_ = nullptr;
    IMemoryProvider* memory_provider_ = nullptr;
};
