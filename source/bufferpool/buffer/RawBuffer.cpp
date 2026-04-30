#include "bufferpool/buffer/RawBuffer.hpp"

RawBuffer::RawBuffer(uint32_t id, void* virt_addr, uint64_t phys_addr,
                     size_t size, Ownership ownership,
                     IMemoryProvider* memory_provider)
    : Buffer(id, virt_addr, phys_addr, size, ownership, Type::RAW)
    , avpacket_(nullptr)
    , memory_provider_(memory_provider)
{
}

RawBuffer::~RawBuffer() {
    // 释放 AVPacket 结构体
    if (avpacket_) {
        av_packet_free(&avpacket_);
        avpacket_ = nullptr;
    }

    // 通过 MemoryProvider 归还内存
    if (virt_addr_ && memory_provider_) {
        MemoryBlock block;
        block.virt_addr = virt_addr_;
        block.phys_addr = phys_addr_;
        block.size      = size_;
        block.handle    = id_;
        memory_provider_->deallocate(block);
        virt_addr_ = nullptr;
    }
}

void RawBuffer::free() {
    // 清空 AVPacket 引用计数（如果有）
    if (avpacket_) {
        av_packet_unref(avpacket_);
    }

    // 基类清理
    Buffer::free();
}
