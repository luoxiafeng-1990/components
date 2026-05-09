#include "bufferpool/buffer/AVFrameBuffer.hpp"

AVFrameBuffer::AVFrameBuffer(uint32_t id, void* virt_addr, uint64_t phys_addr,
                             size_t size, Ownership ownership)
    : Buffer(id, virt_addr, phys_addr, size, ownership, Type::AVFRAME)
    , avframe_(nullptr)
    , avpacket_(nullptr)
{
}

AVFrameBuffer::~AVFrameBuffer() {
    // 释放 AVFrame 结构体本身（av_frame_free 会 unref + free）
    if (avframe_) {
        av_frame_free(&avframe_);
        avframe_ = nullptr;
    }
    // 释放 AVPacket 结构体本身
    if (avpacket_) {
        av_packet_free(&avpacket_);
        avpacket_ = nullptr;
    }
}




void AVFrameBuffer::free() {
    // 1. 清空 AVFrame 的引用计数（保留结构体）
    if (avframe_) {
        av_frame_unref(avframe_);
        // virt_addr_ 指向 frame->data[0]，数据清空后地址失效
        virt_addr_ = nullptr;
    }

    // 2. 清空 AVPacket 的引用计数（保留结构体）
    if (avpacket_) {
        av_packet_unref(avpacket_);
    }

    // 3. 调用基类清理图像元数据 + PTS
    Buffer::free();
}

int AVFrameBuffer::getOutputChannel() const {
    if (!avframe_) return -1;
    if (!avframe_->metadata) return -1;
    
    AVDictionaryEntry* channel_entry = av_dict_get(
        avframe_->metadata, "output_channel", nullptr, 0);
    
    if (channel_entry && channel_entry->value) {
        return atoi(channel_entry->value);
    }
    return -1;
}

