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
    , used_size_(0)                  // ⭐ v2.9新增：初始化实际使用大小
    , ownership_(ownership)
    , state_(State::IDLE)
    , avframe_(nullptr)              // ⭐ v2.7新增：初始化 AVFrame 指针
    , avpacket_(nullptr)             // ⭐ v2.8新增：初始化 AVPacket 指针
    , has_image_metadata_(false)
    , width_(0)
    , height_(0)
    , format_(AV_PIX_FMT_NONE)
    , linesize_{0, 0, 0, 0}
    , plane_offset_{0, 0, 0, 0}
    , nb_planes_(0)
    , pts_(AV_NOPTS_VALUE)           // ⭐ v2.26新增：初始化 PTS
    , validation_magic_(MAGIC_NUMBER)
{
}

// ========== 移动构造函数和移动赋值运算符 ==========

Buffer::Buffer(Buffer&& other) noexcept
    : id_(other.id_)
    , virt_addr_(other.virt_addr_)
    , phys_addr_(other.phys_addr_)
    , size_(other.size_)
    , used_size_(other.used_size_)          // ⭐ v2.9新增：移动实际使用大小
    , ownership_(other.ownership_)
    , state_(other.state_.load())           // 从 atomic 读取
    , avframe_(other.avframe_)              // ⭐ v2.7新增：移动 AVFrame 指针
    , avpacket_(other.avpacket_)            // ⭐ v2.8新增：移动 AVPacket 指针
    , has_image_metadata_(other.has_image_metadata_)
    , width_(other.width_)
    , height_(other.height_)
    , format_(other.format_)
    , linesize_{other.linesize_[0], other.linesize_[1], other.linesize_[2], other.linesize_[3]}
    , plane_offset_{other.plane_offset_[0], other.plane_offset_[1], other.plane_offset_[2], other.plane_offset_[3]}
    , nb_planes_(other.nb_planes_)
    , pts_(other.pts_)                     // ⭐ v2.26新增：移动 PTS
    , validation_magic_(other.validation_magic_)
{
    // 清空源对象
    other.virt_addr_ = nullptr;
    other.phys_addr_ = 0;
    other.size_ = 0;
    other.avframe_ = nullptr;              // ⭐ v2.7新增：清空 AVFrame 指针
    other.avpacket_ = nullptr;             // ⭐ v2.8新增：清空 AVPacket 指针
    other.has_image_metadata_ = false;
    other.pts_ = AV_NOPTS_VALUE;           // ⭐ v2.26新增：清空 PTS
    other.validation_magic_ = 0;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        // 复制数据（与移动构造函数一致，避免 avpacket_ 悬空/重复释放导致 double free）
        id_ = other.id_;
        virt_addr_ = other.virt_addr_;
        phys_addr_ = other.phys_addr_;
        size_ = other.size_;
        used_size_ = other.used_size_;
        ownership_ = other.ownership_;
        state_.store(other.state_.load());           // atomic 赋值
        avframe_ = other.avframe_;                   // ⭐ v2.7新增：移动 AVFrame 指针
        avpacket_ = other.avpacket_;                 // ⭐ 与移动构造对齐
        has_image_metadata_ = other.has_image_metadata_;
        width_ = other.width_;
        height_ = other.height_;
        format_ = other.format_;
        memcpy(linesize_, other.linesize_, sizeof(linesize_));
        memcpy(plane_offset_, other.plane_offset_, sizeof(plane_offset_));
        nb_planes_ = other.nb_planes_;
        pts_ = other.pts_;                           // ⭐ v2.26新增：移动 PTS
        validation_magic_ = other.validation_magic_;
        
        // 清空源对象
        other.virt_addr_ = nullptr;
        other.phys_addr_ = 0;
        other.size_ = 0;
        other.used_size_ = 0;
        other.avframe_ = nullptr;                    // ⭐ v2.7新增：清空 AVFrame 指针
        other.avpacket_ = nullptr;                 // ⭐ 与移动构造对齐
        other.has_image_metadata_ = false;
        other.pts_ = AV_NOPTS_VALUE;                 // ⭐ v2.26新增：清空 PTS
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
    
    // ⭐ v2.7简化：不再计算 plane_offset_，getImagePlaneData() 直接从 avframe_->data[i] 读取
    // plane_offset_ 数组保留但不使用，避免破坏二进制兼容性
    
    // 确定plane数量
    nb_planes_ = 0;
    for (int i = 0; i < 4; i++) {
        if (frame->data[i] != nullptr) {
            nb_planes_ = i + 1;
        }
    }
    
    has_image_metadata_ = true;
}

// ========== 硬件平台相关接口实现 ⭐ v2.17新增 ==========

int Buffer::getOutputChannel() const {
    // 1. 检查是否有关联的 AVFrame
    if (!avframe_) {
        return -1;
    }
    
    // 2. 检查 AVFrame 是否有 metadata
    if (!avframe_->metadata) {
        return -1;
    }
    
    // 3. 从 metadata 中查找 "output_channel" 键
    AVDictionaryEntry* channel_entry = av_dict_get(
        avframe_->metadata,    // 元数据字典
        "output_channel",      // 键名
        nullptr,               // 从头开始查找
        0                      // 精确匹配
    );
    
    // 4. 如果找到且有值，转换为整数；否则返回 -1
    if (channel_entry && channel_entry->value) {
        return atoi(channel_entry->value);  // "0" -> 0, "1" -> 1
    }
    
    return -1;  // 未找到或值为空
}

// ========== 生命周期管理接口实现 ⭐ v2.19新增 ==========

void Buffer::freeBuffer() {
    // 1. 清空 AVFrame 的引用计数（清空数据，但不释放结构体）
    // av_frame_unref() 会：
    //   - 清空 frame->data[0] 到 frame->data[7]
    //   - 清空 frame->buf[0] 到 frame->buf[7]（释放引用计数）
    //   - 但保留 AVFrame 结构体本身（不调用 av_frame_free）
    if (avframe_) {
        av_frame_unref(avframe_);
        // virt_addr_ 指向 frame->data[0]，AVFrame 数据清空后地址失效
        virt_addr_ = nullptr;
    }
    
    if (avpacket_) {
        av_packet_unref(avpacket_);
    }
    
    // 4. 清空图像元数据
    has_image_metadata_ = false;
    width_ = 0;
    height_ = 0;
    format_ = AV_PIX_FMT_NONE;
    linesize_[0] = 0;
    linesize_[1] = 0;
    linesize_[2] = 0;
    linesize_[3] = 0;
    nb_planes_ = 0;
    
    // 5. ⭐ v2.26新增：重置 PTS
    pts_ = AV_NOPTS_VALUE;
    
    // 6. 注意：不修改以下内容：
    //    - avframe_ 指针（结构体还在，只是数据被清空）
    //    - avpacket_ 指针（结构体还在，只是数据被清空）
    //    - id_（buffer ID 不变）
    //    - size_（buffer 大小不变）
    //    - used_size_（实际使用大小不变）
    //    - ownership_（所有权类型不变）
    //    - phys_addr_（物理地址可能保留，取决于使用场景）
    //    - state_（由 BufferPool 管理）
    //    - validation_magic_（魔数不变）
}