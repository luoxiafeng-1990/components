#pragma once

#include "bufferpool/buffer/Buffer.hpp"

/**
 * @brief MatBuffer - 持有 OpenCV cv::Mat 的 Buffer 子类
 *
 * 用于图像处理场景：
 * - mat_ 保存 OpenCV 图像数据
 * - avpacket_ 用于编码互操作
 * - avframe_ 可选，Mat 与 AVFrame 共享 GPU 内存时使用
 */
class MatBuffer : public Buffer {
public:
    MatBuffer(uint32_t id, void* virt_addr, uint64_t phys_addr,
              size_t size, Ownership ownership);
    ~MatBuffer() override;

    // === Mat 载荷 ===
    cv::Mat* getMat() const override { return mat_; }
    void setMat(cv::Mat* mat) override { mat_ = mat; }

    // === AVPacket 载荷（编码互操作） ===
    AVPacket* getAVPacket() const override { return avpacket_; }
    void setAVPacket(AVPacket* packet) override { avpacket_ = packet; }

    // === AVFrame 载荷（可选，Mat 与 AVFrame 共享内存时） ===
    AVFrame* getAVFrame() const override { return avframe_; }
    void setAVFrame(AVFrame* frame) override { avframe_ = frame; }

    // === 生命周期 ===
    void free() override;

    // === 图像 plane 数据 ===
    uint8_t* getImagePlaneData(int plane) const override;

private:
    cv::Mat* mat_ = nullptr;
    AVPacket* avpacket_ = nullptr;
    AVFrame* avframe_ = nullptr;
};
