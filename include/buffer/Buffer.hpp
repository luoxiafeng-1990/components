#ifndef BUFFER_HPP
#define BUFFER_HPP

#include <stddef.h>   // For size_t
#include <string.h>   // For memset, memcpy
#include <stdint.h>   // For uint8_t

/**
 * Buffer - 内存块封装类
 * 
 * 提供对一块内存区域的轻量级封装，包括：
 * - 地址和大小管理
 * - 类型安全的访问接口
 * - 便利的操作方法
 * 
 * 注意：此类不负责内存的分配和释放，仅封装已有的内存块
 */
class Buffer {
private:
    void* data_;      // 内存地址
    size_t size_;     // 内存大小（字节）

public:
    // ============ 构造函数 ============
    
    /**
     * 默认构造：创建无效的Buffer
     */
    Buffer() : data_(nullptr), size_(0) {}
    
    /**
     * 构造：封装指定的内存区域
     * @param data 内存地址
     * @param size 内存大小（字节）
     */
    Buffer(void* data, size_t size) : data_(data), size_(size) {}
    
    // ============ 基本属性访问 ============
    
    /**
     * 获取内存地址
     */
    void* data() { return data_; }
    const void* data() const { return data_; }
    
    /**
     * 获取内存大小（字节）
     */
    size_t size() const { return size_; }
    
    /**
     * 检查Buffer是否有效
     */
    bool isValid() const { 
        return data_ != nullptr && size_ > 0; 
    }
    
    // ============ 类型转换访问 ============
    
    /**
     * 类型安全的指针转换
     * @tparam T 目标类型（默认为uint8_t）
     * @return 转换后的类型指针
     */
    template<typename T = uint8_t>
    T* as() { 
        return static_cast<T*>(data_); 
    }
    
    template<typename T = uint8_t>
    const T* as() const { 
        return static_cast<const T*>(data_); 
    }
    
    // ============ 数组访问 ============
    
    /**
     * 数组下标访问（按字节）
     */
    uint8_t& operator[](size_t index) {
        return as<uint8_t>()[index];
    }
    
    const uint8_t& operator[](size_t index) const {
        return as<uint8_t>()[index];
    }
    
    // ============ 便利操作 ============
    
    /**
     * 用指定值填充整个Buffer
     * @param value 填充值
     */
    void fill(uint8_t value) {
        if (data_) {
            memset(data_, value, size_);
        }
    }
    
    /**
     * 从源地址拷贝数据到Buffer
     * @param src 源地址
     * @param length 拷贝长度
     * @return 成功返回true
     */
    bool copyFrom(const void* src, size_t length) {
        if (!data_ || !src || length > size_) {
            return false;
        }
        memcpy(data_, src, length);
        return true;
    }
    
    /**
     * 清零整个Buffer
     */
    void clear() {
        fill(0);
    }
};

#endif // BUFFER_HPP

