#ifndef IVIDEO_READER_HPP
#define IVIDEO_READER_HPP

#include "../buffer/Buffer.hpp"
#include <stddef.h>  // For size_t
#include <sys/types.h>  // For ssize_t

/**
 * IVideoReader - 视频读取器抽象接口
 * 
 * 定义统一的视频读取接口，所有具体实现（mmap、io_uring、direct read等）
 * 都必须实现此接口。
 * 
 * 设计模式：策略模式（Strategy Pattern）
 * - 本接口定义"策略"（读取视频的方法）
 * - 不同实现类提供不同的"策略"（mmap、io_uring等）
 * - 上层代码只依赖接口，不依赖具体实现
 * 
 * 优势：
 * - 可扩展：新增实现只需继承此接口
 * - 可替换：不同实现可以互相替换
 * - 解耦合：上层代码与具体实现解耦
 */
class IVideoReader {
public:
    virtual ~IVideoReader() = default;
    
    // ============ 文件操作 ============
    
    /**
     * 打开编码视频文件（自动检测格式）
     * @param path 文件路径
     * @return 成功返回true
     */
    virtual bool open(const char* path) = 0;
    
    /**
     * 打开原始视频文件（需手动指定格式）
     * @param path 文件路径
     * @param width 视频宽度（像素）
     * @param height 视频高度（像素）
     * @param bits_per_pixel 每像素位数
     * @return 成功返回true
     */
    virtual bool openRaw(const char* path, int width, int height, int bits_per_pixel) = 0;
    
    /**
     * 关闭视频文件
     */
    virtual void close() = 0;
    
    /**
     * 检查文件是否已打开
     */
    virtual bool isOpen() const = 0;
    
    // ============ 读取操作 ============
    
    /**
     * 读取一帧到指定Buffer（顺序读取）
     * @param dest_buffer 目标Buffer
     * @return 成功返回true
     */
    virtual bool readFrameTo(Buffer& dest_buffer) = 0;
    
    /**
     * 读取一帧到指定地址（顺序读取）
     * @param dest_buffer 目标地址
     * @param buffer_size 目标缓冲区大小
     * @return 成功返回true
     */
    virtual bool readFrameTo(void* dest_buffer, size_t buffer_size) = 0;
    
    /**
     * 读取指定帧到Buffer（随机访问）
     * @param frame_index 帧索引
     * @param dest_buffer 目标Buffer
     * @return 成功返回true
     */
    virtual bool readFrameAt(int frame_index, Buffer& dest_buffer) = 0;
    
    /**
     * 读取指定帧到地址（随机访问）
     * @param frame_index 帧索引
     * @param dest_buffer 目标地址
     * @param buffer_size 目标缓冲区大小
     * @return 成功返回true
     */
    virtual bool readFrameAt(int frame_index, void* dest_buffer, size_t buffer_size) = 0;
    
    /**
     * 线程安全地读取指定帧（不修改内部状态）
     * @param frame_index 帧索引
     * @param dest_buffer 目标地址
     * @param buffer_size 目标缓冲区大小
     * @return 成功返回true
     */
    virtual bool readFrameAtThreadSafe(int frame_index, void* dest_buffer, size_t buffer_size) const = 0;
    
    // ============ 导航操作 ============
    
    /**
     * 跳转到指定帧
     * @param frame_index 帧索引
     * @return 成功返回true
     */
    virtual bool seek(int frame_index) = 0;
    
    /**
     * 回到文件开头
     */
    virtual bool seekToBegin() = 0;
    
    /**
     * 跳转到文件末尾
     */
    virtual bool seekToEnd() = 0;
    
    /**
     * 跳过N帧（可正可负）
     * @param frame_count 跳过的帧数
     * @return 成功返回true
     */
    virtual bool skip(int frame_count) = 0;
    
    // ============ 信息查询 ============
    
    /**
     * 获取总帧数
     */
    virtual int getTotalFrames() const = 0;
    
    /**
     * 获取当前帧索引
     */
    virtual int getCurrentFrameIndex() const = 0;
    
    /**
     * 获取单帧大小（字节）
     */
    virtual size_t getFrameSize() const = 0;
    
    /**
     * 获取文件大小（字节）
     */
    virtual long getFileSize() const = 0;
    
    /**
     * 获取视频宽度
     */
    virtual int getWidth() const = 0;
    
    /**
     * 获取视频高度
     */
    virtual int getHeight() const = 0;
    
    /**
     * 获取每像素字节数
     */
    virtual int getBytesPerPixel() const = 0;
    
    /**
     * 获取文件路径
     */
    virtual const char* getPath() const = 0;
    
    /**
     * 检查是否还有更多帧
     */
    virtual bool hasMoreFrames() const = 0;
    
    /**
     * 检查是否到达文件末尾
     */
    virtual bool isAtEnd() const = 0;
    
    // ============ 类型信息 ============
    
    /**
     * 获取读取器类型名称（用于调试和日志）
     * @return 类型名称（如 "MmapReader"、"IoUringReader"）
     */
    virtual const char* getReaderType() const = 0;
    
    // ============ 可选依赖注入（用于性能优化）============
    
    /**
     * 设置BufferPool（可选依赖注入）
     * 
     * 用途：
     * - 允许特殊Reader（如RTSP流解码器）直接将数据注入BufferPool
     * - 实现零拷贝优化
     * 
     * 默认实现为空（普通文件Reader不需要）
     * 特殊Reader可以重写此方法以优化性能
     * 
     * @param pool BufferPool指针，nullptr表示取消注入模式
     * 
     * 注意：此接口不改变Reader的外部行为，只是内部优化
     */
    virtual void setBufferPool(void* pool) {
        // 默认实现：什么都不做
        // 普通Reader（Mmap、IoUring）不需要BufferPool
        (void)pool;
    }
};

#endif // IVIDEO_READER_HPP




