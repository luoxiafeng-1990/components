#ifndef BUFFER_FILLING_WORKER_FACTORY_HPP
#define BUFFER_FILLING_WORKER_FACTORY_HPP

#include "IBufferFillingWorker.hpp"
#include <memory>

/**
 * @brief BufferFillingWorkerFactory - 填充Buffer的Worker工厂
 * 
 * 架构角色：Worker Factory（工人工厂）
 * 
 * 设计模式：工厂模式（Factory Pattern）
 * 
 * 职责：
 * - 根据环境和配置创建合适的Worker实现
 * - 封装Worker创建逻辑
 * - 支持自动检测和手动指定两种模式
 * 
 * 创建的Worker类型：
 * - FfmpegDecodeVideoFileWorker: FFmpeg解码视频文件Worker
 * - MmapRawVideoFileWorker: Mmap方式打开raw视频文件Worker
 * - FfmpegDecodeRtspWorker: FFmpeg解码RTSP流Worker
 * - IoUringRawVideoFileWorker: IoUring方式打开raw视频文件Worker
 * 
 * 优势：
 * - 用户无需了解具体实现类
 * - 可以根据运行环境自动选择最优实现
 * - 支持通过配置、环境变量控制
 */
class BufferFillingWorkerFactory {
public:
    /**
     * Worker类型枚举
     */
    enum class WorkerType {
        AUTO,          // 自动检测（默认）
        MMAP_RAW,      // 创建 MmapRawVideoFileWorker
        IOURING_RAW,   // 创建 IoUringRawVideoFileWorker
        FFMPEG_RTSP,   // 创建 FfmpegDecodeRtspWorker
        FFMPEG_VIDEO_FILE  // 创建 FfmpegDecodeVideoFileWorker
    };
    
    // 向后兼容：保留ReaderType别名
    using ReaderType = WorkerType;
    
    /**
     * 创建填充Buffer的Worker（工厂方法）
     * 
     * 创建策略（优先级从高到低）：
     * 1. 用户显式指定 (type != AUTO)
     * 2. 环境变量 (VIDEO_READER_TYPE)
     * 3. 配置文件 (/etc/video_reader.conf)
     * 4. 自动检测系统能力
     * 
     * @param type Worker类型（默认AUTO）
     * @return Worker实例（智能指针）
     */
    static std::unique_ptr<IBufferFillingWorker> create(WorkerType type = WorkerType::AUTO);
    
    /**
     * 从名称创建Worker
     * @param name 类型名称（"mmap_raw", "iouring_raw", "ffmpeg_rtsp", "ffmpeg_video_file"）
     * @return Worker实例
     */
    static std::unique_ptr<IBufferFillingWorker> createByName(const char* name);
    
    /**
     * 检查 io_uring 是否可用
     * @return 可用返回true
     */
    static bool isIoUringAvailable();
    
    /**
     * 检查 mmap 是否可用
     * @return 可用返回true
     */
    static bool isMmapAvailable();
    
    /**
     * 获取推荐的Worker类型
     * @return 推荐类型
     */
    static WorkerType getRecommendedType();
    
    /**
     * 将类型转换为字符串
     * @param type 类型
     * @return 类型名称
     */
    static const char* typeToString(WorkerType type);

private:
    /**
     * 自动检测并创建最优Worker
     */
    static std::unique_ptr<IBufferFillingWorker> autoDetect();
    
    /**
     * 根据类型创建Worker
     */
    static std::unique_ptr<IBufferFillingWorker> createByType(WorkerType type);
    
    /**
     * 从环境变量读取类型
     */
    static WorkerType getTypeFromEnvironment();
    
    /**
     * 从配置文件读取类型
     */
    static WorkerType getTypeFromConfig();
    
    /**
     * 检查 io_uring 是否适合当前场景
     */
    static bool isIoUringSuitable();
};

#endif // BUFFER_FILLING_WORKER_FACTORY_HPP




