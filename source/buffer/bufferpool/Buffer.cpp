#include "buffer/bufferpool/Buffer.hpp"
#include <stdio.h>

// ========== 构造函数 ==========

Buffer::Buffer(uint32_t id, 
               void* virt_addr, 
               uint64_t phys_addr,
               size_t size,
               Ownership ownership)
    : id_(id)
    , virt_addr_(virt_addr)
    , phys_addr_(phys_addr)
    , size_(size)
    , ownership_(ownership)
    , state_(State::IDLE)
    , has_image_metadata_(false)
    , width_(0)
    , height_(0)
    , format_(AV_PIX_FMT_NONE)
    , linesize_{0, 0, 0, 0}
    , plane_offset_{0, 0, 0, 0}
    , nb_planes_(0)
    , validation_magic_(MAGIC_NUMBER)
{
}

// ========== 移动构造函数和移动赋值运算符 ==========

Buffer::Buffer(Buffer&& other) noexcept
    : id_(other.id_)
    , virt_addr_(other.virt_addr_)
    , phys_addr_(other.phys_addr_)
    , size_(other.size_)
    , ownership_(other.ownership_)
    , state_(other.state_.load())           // 从 atomic 读取
    , has_image_metadata_(other.has_image_metadata_)
    , width_(other.width_)
    , height_(other.height_)
    , format_(other.format_)
    , linesize_{other.linesize_[0], other.linesize_[1], other.linesize_[2], other.linesize_[3]}
    , plane_offset_{other.plane_offset_[0], other.plane_offset_[1], other.plane_offset_[2], other.plane_offset_[3]}
    , nb_planes_(other.nb_planes_)
    , validation_magic_(other.validation_magic_)
{
    // 清空源对象
    other.virt_addr_ = nullptr;
    other.phys_addr_ = 0;
    other.size_ = 0;
    other.has_image_metadata_ = false;
    other.validation_magic_ = 0;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        // 复制数据
        id_ = other.id_;
        virt_addr_ = other.virt_addr_;
        phys_addr_ = other.phys_addr_;
        size_ = other.size_;
        ownership_ = other.ownership_;
        state_.store(other.state_.load());           // atomic 赋值
        has_image_metadata_ = other.has_image_metadata_;
        width_ = other.width_;
        height_ = other.height_;
        format_ = other.format_;
        memcpy(linesize_, other.linesize_, sizeof(linesize_));
        memcpy(plane_offset_, other.plane_offset_, sizeof(plane_offset_));
        nb_planes_ = other.nb_planes_;
        validation_magic_ = other.validation_magic_;
        
        // 清空源对象
        other.virt_addr_ = nullptr;
        other.phys_addr_ = 0;
        other.size_ = 0;
        other.has_image_metadata_ = false;
        other.validation_magic_ = 0;
    }
    return *this;
}

// ========== 调试接口实现 ==========

const char* Buffer::stateToString(State state) {
    switch (state) {
        case State::IDLE:                return "IDLE (空闲)";
        case State::LOCKED_BY_PRODUCER:  return "LOCKED_BY_PRODUCER (生产者持有)";
        case State::READY_FOR_CONSUME:   return "READY_FOR_CONSUME (就绪)";
        case State::LOCKED_BY_CONSUMER:  return "LOCKED_BY_CONSUMER (消费者持有)";
        default:                         return "UNKNOWN";
    }
}

// ========== 图像元数据接口实现 ⭐ v2.6新增 ==========

void Buffer::setImageMetadataFromAVFrame(const AVFrame* frame) {
    if (!frame) {
        has_image_metadata_ = false;
        return;
    }
    
    // 设置基本信息
    width_ = frame->width;
    height_ = frame->height;
    format_ = (AVPixelFormat)frame->format;
    
    // 复制linesize
    memcpy(linesize_, frame->linesize, sizeof(linesize_));
    
    // 计算各plane的偏移
    // 假设virt_addr_指向第一个plane的起始位置
    plane_offset_[0] = 0;
    
    // 计算后续plane的偏移
    for (int i = 1; i < 4; i++) {
        if (frame->data[i] && frame->data[i-1]) {
            // 计算相对偏移
            ptrdiff_t offset = frame->data[i] - frame->data[0];
            plane_offset_[i] = (offset >= 0) ? offset : 0;
        } else {
            plane_offset_[i] = 0;
        }
    }
    
    // 确定plane数量
    nb_planes_ = 0;
    for (int i = 0; i < 4; i++) {
        if (frame->data[i] != nullptr) {
            nb_planes_ = i + 1;
        }
    }
    
    has_image_metadata_ = true;
}

