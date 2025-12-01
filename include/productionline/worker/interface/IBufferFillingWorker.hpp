#ifndef IBUFFER_FILLING_WORKER_HPP
#define IBUFFER_FILLING_WORKER_HPP

#include "../../../buffer/Buffer.hpp"
#include "../../../buffer/BufferPool.hpp"
#include <stddef.h>  // For size_t
#include <sys/types.h>  // For ssize_t
#include <memory>  // For std::unique_ptr

/**
 * @brief IBufferFillingWorker - 填充Buffer的Worker接口
 * 
 * 架构角色：Worker（工人）- 负责填充Buffer
 * 
 * 核心目的：从不同数据源获取数据，填充到Buffer中
 * 
 * 职责：
 * - 从不同视频源（RTSP/RAW/MP4等）获取数据
 * - 解码/处理数据
 * - 填充Buffer（核心功能）
 * - 自动调用allocator创建BufferPool
 * - 提供BufferPool（原材料）给ProductionLine
 * 
 * Worker实现类：
 * - FfmpegDecodeVideoFileWorker: FFmpeg解码视频文件Worker
 * - MmapRawVideoFileWorker: Mmap方式打开raw视频文件Worker
 * - FfmpegDecodeRtspWorker: FFmpeg解码RTSP流Worker
 * - IoUringRawVideoFileWorker: IoUring方式打开raw视频文件Worker
 * 
 * 设计模式：
 * - 策略模式（Strategy Pattern）：定义"Worker策略"（填充Buffer的方法）
 * - 接口分离原则（ISP）：与IVideoFileNavigator并列，Worker可选择性实现
 * 
 * 注意：
 * - Worker实现类需要同时继承IBufferFillingWorker和IVideoFileNavigator（如果需要文件导航功能）
 * - 如果Worker不需要文件导航功能，可以只实现IBufferFillingWorker
 * 
 * 优势：
 * - 可扩展：新增实现只需继承此接口
 * - 可替换：不同实现可以互相替换
 * - 解耦合：上层代码与具体实现解耦
 * - 职责清晰：文件操作功能独立为IVideoFileNavigator接口（并列关系）
 */
class IBufferFillingWorker {
public:
    virtual ~IBufferFillingWorker() = default;
    
    // ============ 核心功能：填充Buffer ============
    
    /**
     * 填充Buffer（核心功能）
     * @param frame_index 帧索引
     * @param buffer 输出 Buffer（从 BufferPool 获取）
     * @return 成功返回 true
     * 
     * 核心目的：填充Buffer，得到填充后的buffer
     * 
     * 实现要求：
     * - MmapRawVideoFileWorker: 读取数据到 buffer->data()
     * - 硬件解码器: 解码并设置 DMA 到 buffer->id()
     * - FfmpegDecodeRtspWorker: 解码并填充 buffer 元数据
     * 
     * 注意：
     * - buffer 由调用者（VideoProductionLine）从 BufferPool 获取
     * - 实现者只负责填充数据或设置元数据
     * - 成功返回 true，失败返回 false（buffer 由调用者归还）
     * - 子类必须实现此方法，无默认实现
     */
    virtual bool fillBuffer(int frame_index, Buffer* buffer) = 0;
    
    // ============ 类型信息 ============
    
    /**
     * 获取Worker类型名称（用于调试和日志）
     * @return 类型名称（如 "FfmpegDecodeVideoFileWorker"、"MmapRawVideoFileWorker"）
     */
    virtual const char* getWorkerType() const = 0;
    
    // ============ 提供原材料（BufferPool）============
    
    /**
     * @brief v2.0: 获取输出 BufferPool ID（如果有）
     * 
     * @return uint64_t pool_id（成功），0（失败或未创建）
     * 
     * 职责：
     * - Worker在open()时自动调用allocator创建BufferPool（如果需要）
     * - 如果Worker创建了BufferPool，通过此方法返回pool_id
     * - 如果Worker没有创建BufferPool，返回0，调用者使用外部BufferPool
     * 
     * 使用场景：
     * - 硬件解码器Worker：返回内部创建的overlay BufferPool ID
     * - MmapRawVideoFileWorker：返回0（使用外部BufferPool）
     * - VideoProductionLine：通过此方法获取Worker的BufferPool ID（如果有）
     * 
     * v2.0 设计：
     * - 返回pool_id而不是指针
     * - Registry独占持有BufferPool（shared_ptr，引用计数=1）
     * - 调用者从Registry获取临时访问（getPool(pool_id)）
     * 
     * 默认实现：返回0（普通Worker没有内部BufferPool）
     */
    virtual uint64_t getOutputBufferPoolId() {
        return 0;
    }
};

#endif // IBUFFER_FILLING_WORKER_HPP

