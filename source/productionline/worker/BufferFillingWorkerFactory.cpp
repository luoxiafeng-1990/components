#include "productionline/worker/BufferFillingWorkerFactory.hpp"
#include "common/Logger.hpp"
#include "productionline/worker/MmapRawVideoFileWorker.hpp"
#include "productionline/worker/IoUringRawVideoFileWorker.hpp"
#include "productionline/worker/FfmpegDecodeRtspWorker.hpp"
#include "productionline/worker/FfmpegDecodeVideoFileWorker.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <liburing.h>

// ============ 公共接口 ============

std::unique_ptr<WorkerBase> BufferFillingWorkerFactory::create(const WorkerConfig& config) {
    auto type = config.worker_type;
    // 1️⃣ 用户显式指定（最高优先级）
    if (type != WorkerType::AUTO) {
        LOG_DEBUG_FMT("[WorkerFactory] BufferFillingWorkerFactory: User specified type: %s\n", typeToString(type));
        return createByType(type, config);
    }
    
    // 2️⃣ 环境变量配置
    WorkerType env_type = getTypeFromEnvironment();
    if (env_type != WorkerType::AUTO) {
        LOG_DEBUG_FMT("[WorkerFactory] BufferFillingWorkerFactory: Type from environment: %s\n", typeToString(env_type));
        return createByType(env_type, config);
    }
    
    // 3️⃣ 配置文件
    WorkerType config_type = getTypeFromConfig();
    if (config_type != WorkerType::AUTO) {
        LOG_DEBUG_FMT("[WorkerFactory] BufferFillingWorkerFactory: Type from config: %s\n", typeToString(config_type));
        return createByType(config_type, config);
    }
    
    // 4️⃣ 自动检测
    LOG_DEBUG("[WorkerFactory] BufferFillingWorkerFactory: Auto-detecting best worker type...\n");
    return autoDetect(config);
}

/* std::unique_ptr<WorkerBase> BufferFillingWorkerFactory::createByName(const char* name) {
    if (strcmp(name, "mmap") == 0 || strcmp(name, "mmap_raw") == 0) {
        return std::make_unique<MmapRawVideoFileWorker>();
    } else if (strcmp(name, "iouring") == 0 || strcmp(name, "iouring_raw") == 0) {
        return std::make_unique<IoUringRawVideoFileWorker>();
    } else if (strcmp(name, "rtsp") == 0 || strcmp(name, "ffmpeg_rtsp") == 0) {
        return std::make_unique<FfmpegDecodeRtspWorker>();
    } else if (strcmp(name, "ffmpeg") == 0 || strcmp(name, "ffmpeg_video_file") == 0) {
        return std::make_unique<FfmpegDecodeVideoFileWorker>();
    } else if (strcmp(name, "auto") == 0) {
        return create(WorkerType::AUTO);
    }
    
    LOG_WARN_FMT("[Worker]  Unknown worker type: %s, using mmap\n", name);
    return std::make_unique<MmapRawVideoFileWorker>();
} */

bool BufferFillingWorkerFactory::isIoUringAvailable() {
    struct io_uring ring;
    int ret = io_uring_queue_init(1, &ring, 0);
    if (ret == 0) {
        io_uring_queue_exit(&ring);
        return true;
    }
    return false;
}

bool BufferFillingWorkerFactory::isMmapAvailable() {
    // mmap 在所有现代 Linux 系统上都可用
    return true;
}

BufferFillingWorkerFactory::WorkerType BufferFillingWorkerFactory::getRecommendedType() {
    if (isIoUringAvailable() && isIoUringSuitable()) {
        return WorkerType::IOURING_RAW;
    }
    return WorkerType::MMAP_RAW;
}

const char* BufferFillingWorkerFactory::typeToString(WorkerType type) {
    switch (type) {
        case WorkerType::AUTO:            return "AUTO";
        case WorkerType::MMAP_RAW:        return "MMAP_RAW";
        case WorkerType::IOURING_RAW:     return "IOURING_RAW";
        case WorkerType::FFMPEG_RTSP:     return "FFMPEG_RTSP";
        case WorkerType::FFMPEG_VIDEO_FILE: return "FFMPEG_VIDEO_FILE";
        default:                          return "UNKNOWN";
    }
}

// ============ 私有辅助方法 ============

std::unique_ptr<WorkerBase> BufferFillingWorkerFactory::autoDetect(const WorkerConfig& config) {
    printf("🔍 Detecting system capabilities:\n");
    
    // 检查 io_uring
    bool iouring_available = isIoUringAvailable();
    printf("   - io_uring: %s\n", iouring_available ? "✓ Available" : "✗ Not available");
    
    // 检查 mmap
    bool mmap_available = isMmapAvailable();
    printf("   - mmap: %s\n", mmap_available ? "✓ Available" : "✗ Not available");
    
    // 决策逻辑
    if (iouring_available && isIoUringSuitable()) {
        LOG_DEBUG("[Worker] Selected: IoUringRawVideoFileWorker (high-performance async I/O)\n");
        return std::make_unique<IoUringRawVideoFileWorker>();
    }
    
    if (mmap_available) {
        LOG_DEBUG("[Worker] Selected: MmapRawVideoFileWorker (memory-mapped I/O)\n");
        return std::make_unique<MmapRawVideoFileWorker>();
    }
    
    // 默认降级
    LOG_WARN_FMT("[Worker]  Warning: No optimal worker available, using MmapRawVideoFileWorker\n");
    return std::make_unique<MmapRawVideoFileWorker>();
}

std::unique_ptr<WorkerBase> BufferFillingWorkerFactory::createByType(WorkerType type, const WorkerConfig& config) {
    switch (type) {
        case WorkerType::MMAP_RAW:
            return std::make_unique<MmapRawVideoFileWorker>(config);  // ✅ 传递 config
            
        case WorkerType::IOURING_RAW:
            if (!isIoUringAvailable()) {
                LOG_WARN_FMT("[Worker]  Warning: io_uring not available, falling back to mmap\n");
                return std::make_unique<MmapRawVideoFileWorker>(config);  // ✅ 传递 config
            }
            return std::make_unique<IoUringRawVideoFileWorker>(config);  // ✅ 传递 config
            
        case WorkerType::FFMPEG_RTSP:
            return std::make_unique<FfmpegDecodeRtspWorker>(config);  // ✅ 传递 config
            
        case WorkerType::FFMPEG_VIDEO_FILE:
            return std::make_unique<FfmpegDecodeVideoFileWorker>(config);  // ✅ 已经传递 config
            
        default:
            return autoDetect(config);
    }
}

BufferFillingWorkerFactory::WorkerType BufferFillingWorkerFactory::getTypeFromEnvironment() {
    const char* env = getenv("VIDEO_READER_TYPE");
    if (!env) {
        return WorkerType::AUTO;
    }
    
    if (strcmp(env, "mmap") == 0 || strcmp(env, "mmap_raw") == 0) {
        return WorkerType::MMAP_RAW;
    } else if (strcmp(env, "iouring") == 0 || strcmp(env, "iouring_raw") == 0) {
        return WorkerType::IOURING_RAW;
    } else if (strcmp(env, "rtsp") == 0 || strcmp(env, "ffmpeg_rtsp") == 0) {
        return WorkerType::FFMPEG_RTSP;
    } else if (strcmp(env, "ffmpeg") == 0 || strcmp(env, "ffmpeg_video_file") == 0) {
        return WorkerType::FFMPEG_VIDEO_FILE;
    }
    
    return WorkerType::AUTO;
}

BufferFillingWorkerFactory::WorkerType BufferFillingWorkerFactory::getTypeFromConfig() {
    // 尝试读取配置文件：/etc/video_reader.conf 或 ~/.config/video_reader.conf
    // 这里简化实现，返回 AUTO
    // 实际项目中可以实现配置文件解析
    return WorkerType::AUTO;
}

bool BufferFillingWorkerFactory::isIoUringSuitable() {
    // 简化的适用性检查
    // 实际项目中可以根据以下因素判断：
    // - 系统负载
    // - 可用内存
    // - 并发线程数
    // - 文件大小
    
    // 目前默认认为 io_uring 总是适合（如果可用的话）
    return true;
}




