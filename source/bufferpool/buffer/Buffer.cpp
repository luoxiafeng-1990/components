#include "bufferpool/buffer/Buffer.hpp"
#include <stdio.h>

// ========== 构造函数 ==========

Buffer::Buffer(uint32_t id,
               void* virt_addr,
               uint64_t phys_addr,
               size_t size,
               Ownership ownership,
               Type type)
    : id_(id)
    , virt_addr_(virt_addr)
    , phys_addr_(phys_addr)
    , size_(size)
    , used_size_(0)
    , ownership_(ownership)
    , type_(type)
    , state_(State::IDLE)
    , has_image_metadata_(false)
    , width_(0)
    , height_(0)
    , format_(AV_PIX_FMT_NONE)
    , linesize_{0, 0, 0, 0}
    , plane_offset_{0, 0, 0, 0}
    , nb_planes_(0)
    , pts_(AV_NOPTS_VALUE)
    , validation_magic_(MAGIC_NUMBER)
{
}

// ========== 析构函数 ==========

Buffer::~Buffer() {
}

// ========== 移动构造函数 ==========

Buffer::Buffer(Buffer&& other) noexcept
    : id_(other.id_)
    , virt_addr_(other.virt_addr_)
    , phys_addr_(other.phys_addr_)
    , size_(other.size_)
    , used_size_(other.used_size_)
    , ownership_(other.ownership_)
    , type_(other.type_)
    , state_(other.state_.load())
    , has_image_metadata_(other.has_image_metadata_)
    , width_(other.width_)
    , height_(other.height_)
    , format_(other.format_)
    , linesize_{other.linesize_[0], other.linesize_[1], other.linesize_[2], other.linesize_[3]}
    , plane_offset_{other.plane_offset_[0], other.plane_offset_[1], other.plane_offset_[2], other.plane_offset_[3]}
    , nb_planes_(other.nb_planes_)
    , pts_(other.pts_)
    , validation_magic_(other.validation_magic_)
{
    other.virt_addr_ = nullptr;
    other.phys_addr_ = 0;
    other.size_ = 0;
    other.has_image_metadata_ = false;
    other.pts_ = AV_NOPTS_VALUE;
    other.validation_magic_ = 0;
}

// ========== 移动赋值运算符 ==========

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        id_ = other.id_;
        virt_addr_ = other.virt_addr_;
        phys_addr_ = other.phys_addr_;
        size_ = other.size_;
        used_size_ = other.used_size_;
        ownership_ = other.ownership_;
        type_ = other.type_;
        state_.store(other.state_.load());
        has_image_metadata_ = other.has_image_metadata_;
        width_ = other.width_;
        height_ = other.height_;
        format_ = other.format_;
        memcpy(linesize_, other.linesize_, sizeof(linesize_));
        memcpy(plane_offset_, other.plane_offset_, sizeof(plane_offset_));
        nb_planes_ = other.nb_planes_;
        pts_ = other.pts_;
        validation_magic_ = other.validation_magic_;

        other.virt_addr_ = nullptr;
        other.phys_addr_ = 0;
        other.size_ = 0;
        other.used_size_ = 0;
        other.has_image_metadata_ = false;
        other.pts_ = AV_NOPTS_VALUE;
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

const char* Buffer::typeToString(Type type) {
    switch (type) {
        case Type::RAW:      return "RAW";
        case Type::AVFRAME:  return "AVFRAME";
        case Type::MAT:      return "MAT";
        default:             return "UNKNOWN";
    }
}

// ========== 图像元数据接口实现 ==========

void Buffer::setImageMetadataFromAVFrame(const AVFrame* frame) {
    if (!frame) {
        has_image_metadata_ = false;
        return;
    }
    
    width_ = frame->width;
    height_ = frame->height;
    format_ = (AVPixelFormat)frame->format;
    memcpy(linesize_, frame->linesize, sizeof(linesize_));
    
    // plane_offset_ 数组保留但不使用（AVFrameBuffer 直接从 avframe_->data[i] 读取）
    
    nb_planes_ = 0;
    for (int i = 0; i < 4; i++) {
        if (frame->data[i] != nullptr) {
            nb_planes_ = i + 1;
        }
    }
    
    has_image_metadata_ = true;
}

// ========== getImagePlaneData 基类实现 ==========

uint8_t* Buffer::getImagePlaneData(int plane) const {
    if (plane < 0 || plane >= 4) return nullptr;
    if (!virt_addr_) return nullptr;
    if (plane == 0) return (uint8_t*)virt_addr_;
    return (uint8_t*)virt_addr_ + plane_offset_[plane];
}

// ========== getOutputChannel 基类实现 ==========

int Buffer::getOutputChannel() const {
    return -1;
}

// ========== 生命周期管理 ==========

void Buffer::free() {
    // 基类职责：清空图像元数据 + 重置 PTS
    has_image_metadata_ = false;
    width_ = 0;
    height_ = 0;
    format_ = AV_PIX_FMT_NONE;
    linesize_[0] = 0;
    linesize_[1] = 0;
    linesize_[2] = 0;
    linesize_[3] = 0;
    nb_planes_ = 0;
    pts_ = AV_NOPTS_VALUE;
}