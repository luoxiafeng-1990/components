#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <cstring>

// FFmpeg标准格式定义
extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
#include <libavcodec/packet.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
}

#include "opencv2/opencv.hpp"
#include "opencv2/core/tacv.hpp"

/**
 * @brief Buffer 基类 — 纯粹的内存容器 + 传输时间戳
 * 
 * 封装单个 buffer 的完整元数据，包括：
 * - 唯一ID（用于硬件回调识别）
 * - 虚拟地址（CPU访问）
 * - 物理地址（DMA/硬件访问）
 * - 所有权类型（自有/外部）
 * - 状态机（IDLE/LOCKED_BY_PRODUCER/READY_FOR_CONSUME/LOCKED_BY_CONSUMER）
 * - PTS 时间戳（帧同步/比较）
 * 
 * 图像描述信息（宽高、格式、stride 等）不再由 Buffer 持有，
 * 消费者使用 ImageMeta::fromBuffer() 按需从载荷中提取。
 * 
 * 子类负责管理各自的载荷：
 * - AVFrameBuffer:  持有 AVFrame* + AVPacket*
 * - MatBuffer:      持有 cv::Mat* + AVPacket* + 可选 AVFrame*
 * - RawBuffer:      纯原始内存 + 可选 AVPacket*
 */
class Buffer {
public:
    // Buffer 类型
    enum class Type {
        RAW,        // 原始内存 (RawBuffer)
        AVFRAME,    // FFmpeg AVFrame (AVFrameBuffer)
        MAT         // OpenCV Mat (MatBuffer)
    };

    // 所有权类型
    enum class Ownership {
        OWNED,      // BufferPool 拥有并管理生命周期
        EXTERNAL    // 外部拥有，BufferPool 只负责调度
    };
    
    // Buffer 状态（用于调试和校验）
    enum class State {
        IDLE,                    // 空闲，等待生产者获取（在 free_queue）
        LOCKED_BY_PRODUCER,      // 被生产者锁定，正在填充数据
        READY_FOR_CONSUME,       // 数据就绪，等待消费者获取（在 filled_queue）
        LOCKED_BY_CONSUMER       // 被消费者锁定，正在使用数据
    };
    
    /**
     * @brief 构造函数
     * @param id 唯一标识符
     * @param virt_addr 虚拟地址（用户空间）
     * @param phys_addr 物理地址（硬件/DMA，0表示未知）
     * @param size Buffer 大小（字节）
     * @param ownership 所有权类型
     * @param type Buffer 类型
     */
    Buffer(uint32_t id, 
           void* virt_addr, 
           uint64_t phys_addr,
           size_t size,
           Ownership ownership,
           Type type);
    
    /**
     * @brief 虚析构函数（子类覆写以释放各自的载荷资源）
     */
    virtual ~Buffer();
    
    // 禁止拷贝
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    
    // 移动构造函数和移动赋值运算符
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    
    // ========== 基础信息接口（非虚，所有子类共用）==========
    
    uint32_t id() const { return id_; }
    void* getVirtualAddress() const { return virt_addr_; }
    uint64_t getPhysicalAddress() const { return phys_addr_; }
    size_t size() const { return size_; }
    void setSize(size_t size) { size_ = size; }
    void setUsedSize(size_t used_size) { used_size_ = used_size; }
    size_t getUsedSize() const { return used_size_ > 0 ? used_size_ : size_; }
    State state() const { return state_.load(); }
    Type type() const { return type_; }
    
    // ========== 状态管理接口 ==========
    
    void setState(State state) { state_.store(state); }
    void setPhysicalAddress(uint64_t phys_addr) { phys_addr_ = phys_addr; }
    void setVirtualAddress(void* addr) { virt_addr_ = addr; }
    
    // ========== 载荷接口（虚方法，基类返回 nullptr / no-op）==========
    
    virtual AVFrame* getAVFrame() const { return nullptr; }
    virtual void setAVFrame(AVFrame* frame) { (void)frame; }
    
    virtual AVPacket* getAVPacket() const { return nullptr; }
    virtual void setAVPacket(AVPacket* packet) { (void)packet; }

    virtual cv::Mat* getMat() const { return nullptr; }
    virtual void setMat(cv::Mat* mat) { (void)mat; }
    
    
    // ========== 帧同步接口 ==========
    
    void setPts(int64_t pts) { pts_ = pts; }
    int64_t getPts() const { return pts_; }
    
    // ========== 生命周期管理接口 ==========
    
    /**
     * @brief 清理 Buffer 中的引用计数和元数据（用于归还到 free 队列前）
     * 
     * 基类职责：重置 PTS
     * 子类覆写：先清理各自载荷（unref），再调用 Buffer::free()
     * 
     * 注意：不释放结构体本身（由析构函数负责）
     */
    virtual void free();
    
    // ========== 校验接口 ==========
    
    bool isValid() const { 
        return validation_magic_ == MAGIC_NUMBER && virt_addr_ != nullptr;
    }
    
    // ========== 调试接口 ==========
    
    static const char* stateToString(State state);
    static const char* typeToString(Type type);
    
protected:
    // ========== 核心属性 ==========
    uint32_t id_;
    void* virt_addr_;
    uint64_t phys_addr_;
    size_t size_;
    size_t used_size_;
    Ownership ownership_;
    Type type_;
    
    // ========== 状态管理 ==========
    std::atomic<State> state_;
    
    
    // ========== 帧同步信息 ==========
    int64_t pts_;
    
    // ========== 安全性 ==========
    static constexpr uint32_t MAGIC_NUMBER = 0xBEEFF123;
    uint32_t validation_magic_;
};
