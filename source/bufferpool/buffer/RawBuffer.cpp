#include "bufferpool/buffer/RawBuffer.hpp"

RawBuffer::RawBuffer(uint32_t id, void* virt_addr, uint64_t phys_addr,
                     size_t size, Ownership ownership,
                     IMemoryProvider* memory_provider)
    : Buffer(id, virt_addr, phys_addr, size, ownership, Type::RAW)
    , memory_provider_(memory_provider)
{
}

RawBuffer::~RawBuffer() {
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
    // 基类清理
    Buffer::free();
}
