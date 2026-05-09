#include "bufferpool/buffer/MatBuffer.hpp"

MatBuffer::MatBuffer(uint32_t id, void* virt_addr, uint64_t phys_addr,
                     size_t size, Ownership ownership)
    : Buffer(id, virt_addr, phys_addr, size, ownership, Type::MAT)
    , mat_(nullptr)
    , avpacket_(nullptr)
    , avframe_(nullptr)
{
}

MatBuffer::~MatBuffer() {
    // 先释放 Mat（必须在 AVFrame unref 之前，因为可能共享 GPU 内存）
    if (mat_) {
        delete mat_;
        mat_ = nullptr;
    }
    if (avframe_) {
        av_frame_free(&avframe_);
        avframe_ = nullptr;
    }
    // MatPoolBuilder 用 delete 释放 AVPacket（不是 av_packet_free），保持一致
    if (avpacket_) {
        delete avpacket_;
        avpacket_ = nullptr;
    }
}

void MatBuffer::free() {
    // 1. 先释放 Mat（必须在 AVFrame unref 之前）
    if (mat_) {
        delete mat_;
        mat_ = nullptr;
    }

    // 2. 清空 AVFrame 引用计数（如果有）
    if (avframe_) {
        av_frame_unref(avframe_);
        virt_addr_ = nullptr;
    }

    // 3. 清空 AVPacket 引用计数（如果有）
    if (avpacket_) {
        av_packet_unref(avpacket_);
    }

    // 4. 基类清理
    Buffer::free();
}

