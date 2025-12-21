#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <cstring>

// FFmpeg标准格式定义
extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
}

/**
 * @brief Buffer 元数据类
 * 
 * 封装单个 buffer 的完整元数据，包括：
 * - 唯一ID（用于硬件回调识别）
 * - 虚拟地址（CPU访问）
 * - 物理地址（DMA/硬件访问）
 * - 所有权类型（自有/外部）
 * - 状态机（FREE/ACQUIRED/FILLED/IN_USE）
 * - 图像元数据（宽高、格式、stride等）⭐ v2.6新增
 */
class Buffer {
public:
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
     */
    Buffer(uint32_t id, 
           void* virt_addr, 
           uint64_t phys_addr,
           size_t size,
           Ownership ownership);
    
    /**
     * @brief 析构函数
     */
    ~Buffer() = default;
    
    /**
     * @brief 禁止拷贝构造（Buffer 不应该被拷贝）
     */
    Buffer(const Buffer&) = delete;
    
    /**
     * @brief 禁止拷贝赋值（Buffer 不应该被拷贝）
     */
    Buffer& operator=(const Buffer&) = delete;
    
    /**
     * @brief 移动构造函数
     * @param other 源Buffer对象
     * 
     * @note 用于 vector 的 resize/reserve 操作
     * @note 移动后源对象将被清空
     */
    Buffer(Buffer&& other) noexcept;
    
    /**
     * @brief 移动赋值运算符
     * @param other 源Buffer对象
     * @return 当前对象的引用
     * 
     * @note 用于 vector 的内部管理
     * @note 移动后源对象将被清空
     */
    Buffer& operator=(Buffer&& other) noexcept;
    
    // ========== 基础信息接口 ==========
    
    /**
     * @brief 获取Buffer唯一ID
     * @return Buffer的唯一标识符
     */
    uint32_t id() const { return id_; }
    
    /**
     * @brief 获取虚拟地址
     * @return CPU可访问的虚拟地址指针
     */
    void* getVirtualAddress() const { return virt_addr_; }
    
    /**
     * @brief 获取物理地址
     * @return DMA/硬件访问的物理地址（0表示未知）
     */
    uint64_t getPhysicalAddress() const { return phys_addr_; }
    
    /**
     * @brief 获取Buffer大小
     * @return Buffer大小（字节）
     */
    size_t size() const { return size_; }
    
    /**
     * @brief 获取当前状态
     * @return Buffer当前状态（线程安全）
     */
    State state() const { return state_.load(); }
    
    /**
     * @brief 获取数据指针（getVirtualAddress的别名）
     * @return CPU可访问的虚拟地址指针
     * 
     * @note 为兼容旧代码保留
     */
    void* data() const { return virt_addr_; }
    
    // ========== 状态管理接口 ==========
    
    /**
     * @brief 设置Buffer状态
     * @param state 新的状态值
     * 
     * @note 线程安全操作
     */
    void setState(State state) { state_.store(state); }
    
    /**
     * @brief 设置物理地址
     * @param phys_addr 物理地址
     * 
     * @note 用于延迟获取物理地址的场景，如零拷贝解码
     */
    void setPhysicalAddress(uint64_t phys_addr) { phys_addr_ = phys_addr; }
    
    // ========== AVFrame 关联接口 ⭐ v2.7新增 ==========
    
    /**
     * @brief 设置关联的 AVFrame（仅用于 AVFrameAllocator）
     * @param frame AVFrame 指针
     * 
     * @note Buffer 持有引用但不拥有所有权，释放由 Allocator 负责
     */
    void setAVFrame(AVFrame* frame) { avframe_ = frame; }
    
    /**
     * @brief 获取关联的 AVFrame
     * @return AVFrame 指针，如果没有关联则返回 nullptr
     */
    AVFrame* getAVFrame() const { return avframe_; }
    
    /**
     * @brief 更新虚拟地址（解码后更新为 frame->data[0]）
     * @param addr 新的虚拟地址
     * 
     * @note 用于解码后更新数据地址
     */
    void setVirtualAddress(void* addr) { virt_addr_ = addr; }
    
    // ========== 图像元数据接口 ⭐ v2.6新增 ==========
    
    /**
     * @brief 从AVFrame设置图像元数据
     * @param frame AVFrame指针（不能为nullptr）
     * 
     * @note 会自动提取frame的width、height、format、linesize等信息
     * @note 会计算各plane相对于virt_addr_的偏移
     */
    void setImageMetadataFromAVFrame(const AVFrame* frame);
    
    /**
     * @brief 检查是否有图像元数据
     * @return true 如果已设置图像元数据，否则返回 false
     */
    bool hasImageMetadata() const { return has_image_metadata_; }
    
    /**
     * @brief 获取图像宽度
     * @return 图像宽度（像素）
     */
    int getImageWidth() const { return width_; }
    
    /**
     * @brief 获取图像高度
     * @return 图像高度（像素）
     */
    int getImageHeight() const { return height_; }
    
    /**
     * @brief 获取像素格式
     * @return FFmpeg标准的像素格式枚举
     */
    AVPixelFormat getImageFormat() const { return format_; }
    
    /**
     * @brief 获取linesize数组
     * @return 指向包含4个元素的linesize数组的指针（每个plane的stride）
     */
    const int* getImageLinesize() const { return linesize_; }
    
    /**
     * @brief 获取指定plane的数据指针
     * @param plane plane索引（0-3）
     * @return plane数据的起始地址，如果参数无效则返回 nullptr
     * 
     * @note 返回的地址已加上 plane_offset_
     */
    /**
     * @brief 获取指定 plane 的数据指针
     * @param plane plane 索引 [0-3]
     * @return plane 数据指针，失败返回 nullptr
     * 
     * @note v2.7改进：
     *   1. 优先使用 virt_addr_（解码后已更新为 frame->data[0]）
     *   2. 如果 virt_addr_ 为 nullptr，尝试从 avframe_->data[plane] 获取
     *   3. 对于 plane > 0，通过 avframe_->data[plane] 获取（硬件解码零拷贝场景）
     */
    uint8_t* getImagePlaneData(int plane) const {
        if (plane < 0 || plane >= 4) return nullptr;
        
        // ⭐ v2.7修复：对于 plane 0，优先使用 virt_addr_（已是 frame->data[0]）
        if (plane == 0) {
            if (virt_addr_) {
                return (uint8_t*)virt_addr_;
            }
            // 回退到 AVFrame（用于创建时的临时访问）
            if (avframe_) {
                return avframe_->data[0];
            }
            return nullptr;
        }
        
        // ⭐ v2.7修复：对于 plane > 0，必须从 AVFrame 获取（多plane地址不连续）
        if (avframe_) {
            return avframe_->data[plane];
        }
        
        // 最后回退到旧逻辑（兼容非 AVFrame 场景，如纯软件解码）
        if (!virt_addr_) return nullptr;
        return (uint8_t*)virt_addr_ + plane_offset_[plane];
    }
    
    // ========== 校验接口 ==========
    
    /**
     * @brief 基础校验：检查Buffer是否仍然有效
     * @return true 如果 magic number 正确且地址非空
     */
    bool isValid() const { 
        return validation_magic_ == MAGIC_NUMBER && virt_addr_ != nullptr;
    }
    
    // ========== 调试接口 ==========
    
    /**
     * @brief 获取状态字符串
     * @param state Buffer状态
     * @return 状态的中文描述字符串
     * 
     * @note 静态方法，可直接调用
     */
    static const char* stateToString(State state);
    
private:
    // ========== 核心属性 ==========
    uint32_t id_;                    // 唯一标识
    void* virt_addr_;                // 虚拟地址（真实数据地址，如 frame->data[0]）⭐ v2.7语义修正
    uint64_t phys_addr_;             // 物理地址（硬件/DMA）
    size_t size_;                    // Buffer 大小
    Ownership ownership_;            // 所有权类型
    
    // ========== 状态管理 ==========
    std::atomic<State> state_;       // 当前状态（线程安全）
    
    // ========== AVFrame 关联 ⭐ v2.7新增 ==========
    AVFrame* avframe_;               // 关联的 AVFrame 指针（引用，不拥有所有权）
    
    // ========== 图像元数据 ⭐ v2.6新增 ==========
    bool has_image_metadata_;        // 是否包含图像元数据
    int width_;                      // 图像宽度（像素）
    int height_;                     // 图像高度（像素）
    AVPixelFormat format_;           // 像素格式（FFmpeg标准）
    int linesize_[4];                // 各plane的stride（字节）
    size_t plane_offset_[4];         // 各plane相对于virt_addr_的偏移
    int nb_planes_;                  // plane数量（1-4）
    
    // ========== 安全性 ==========
    static constexpr uint32_t MAGIC_NUMBER = 0xBEEFF123;  // 魔数：BEEF + F123
    uint32_t validation_magic_;      // 魔数，用于检测野指针
  
};
