#include "../../include/buffer/Buffer.hpp"
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
    , ref_count_(0)
    , dma_fd_(-1)
    , validation_magic_(MAGIC_NUMBER)
    , validation_callback_(nullptr)
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
    , ref_count_(other.ref_count_.load())   // 从 atomic 读取
    , dma_fd_(other.dma_fd_)
    , validation_magic_(other.validation_magic_)
    , validation_callback_(std::move(other.validation_callback_))
{
    // 清空源对象
    other.virt_addr_ = nullptr;
    other.phys_addr_ = 0;
    other.size_ = 0;
    other.dma_fd_ = -1;
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
        ref_count_.store(other.ref_count_.load());   // atomic 赋值
        dma_fd_ = other.dma_fd_;
        validation_magic_ = other.validation_magic_;
        validation_callback_ = std::move(other.validation_callback_);
        
        // 清空源对象
        other.virt_addr_ = nullptr;
        other.phys_addr_ = 0;
        other.size_ = 0;
        other.dma_fd_ = -1;
        other.validation_magic_ = 0;
    }
    return *this;
}

// ========== 校验接口实现 ==========

bool Buffer::validate() const {
    // 基础校验
    if (!isValid()) {
        return false;
    }
    
    // 用户自定义校验
    if (validation_callback_) {
        try {
            return validation_callback_(this);
        } catch (...) {
            // 校验回调抛出异常，视为校验失败
            return false;
        }
    }
    
    return true;
}

// ========== 调试接口实现 ==========

void Buffer::printInfo() const {
    printf("📦 Buffer #%u:\n", id_);
    printf("   Virtual Address:  %p\n", virt_addr_);
    printf("   Physical Address: 0x%016lx\n", phys_addr_);
    printf("   Size:             %zu bytes (%.2f MB)\n", 
           size_, size_ / (1024.0 * 1024.0));
    printf("   Ownership:        %s\n", ownershipToString(ownership_));
    printf("   State:            %s\n", stateToString(state_.load()));
    printf("   Ref Count:        %d\n", ref_count_.load());
    printf("   DMA-BUF FD:       %d\n", dma_fd_);
    printf("   Valid:            %s\n", isValid() ? "✅ Yes" : "❌ No");
}

const char* Buffer::stateToString(State state) {
    switch (state) {
        case State::IDLE:                return "IDLE (空闲)";
        case State::LOCKED_BY_PRODUCER:  return "LOCKED_BY_PRODUCER (生产者持有)";
        case State::READY_FOR_CONSUME:   return "READY_FOR_CONSUME (就绪)";
        case State::LOCKED_BY_CONSUMER:  return "LOCKED_BY_CONSUMER (消费者持有)";
        default:                         return "UNKNOWN";
    }
}

const char* Buffer::ownershipToString(Ownership ownership) {
    switch (ownership) {
        case Ownership::OWNED:    return "OWNED (自有内存)";
        case Ownership::EXTERNAL: return "EXTERNAL (外部托管)";
        default:                  return "UNKNOWN";
    }
}

