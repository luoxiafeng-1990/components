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
    , pts_(other.pts_)
    , validation_magic_(other.validation_magic_)
{
    other.virt_addr_ = nullptr;
    other.phys_addr_ = 0;
    other.size_ = 0;
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
        pts_ = other.pts_;
        validation_magic_ = other.validation_magic_;

        other.virt_addr_ = nullptr;
        other.phys_addr_ = 0;
        other.size_ = 0;
        other.used_size_ = 0;
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

// ========== 生命周期管理 ==========

void Buffer::free() {
    // 基类职责：重置 PTS
    pts_ = AV_NOPTS_VALUE;
}