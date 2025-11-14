#include "../../include/videoFile/VideoReaderFactory.hpp"
#include "../../include/videoFile/MmapVideoReader.hpp"
#include "../../include/videoFile/IoUringVideoReader.hpp"
#include "../../include/videoFile/RtspVideoReader.hpp"
#include "../../include/videoFile/FfmpegVideoReader.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <liburing.h>

// ============ 公共接口 ============

std::unique_ptr<IVideoReader> VideoReaderFactory::create(ReaderType type) {
    // 1️⃣ 用户显式指定（最高优先级）
    if (type != ReaderType::AUTO) {
        printf("🏭 VideoReaderFactory: User specified type: %s\n", typeToString(type));
        return createByType(type);
    }
    
    // 2️⃣ 环境变量配置
    ReaderType env_type = getTypeFromEnvironment();
    if (env_type != ReaderType::AUTO) {
        printf("🏭 VideoReaderFactory: Type from environment: %s\n", typeToString(env_type));
        return createByType(env_type);
    }
    
    // 3️⃣ 配置文件
    ReaderType config_type = getTypeFromConfig();
    if (config_type != ReaderType::AUTO) {
        printf("🏭 VideoReaderFactory: Type from config: %s\n", typeToString(config_type));
        return createByType(config_type);
    }
    
    // 4️⃣ 自动检测
    printf("🏭 VideoReaderFactory: Auto-detecting best reader type...\n");
    return autoDetect();
}

std::unique_ptr<IVideoReader> VideoReaderFactory::createByName(const char* name) {
    if (strcmp(name, "mmap") == 0) {
        return std::make_unique<MmapVideoReader>();
    } else if (strcmp(name, "iouring") == 0) {
        return std::make_unique<IoUringVideoReader>();
    } else if (strcmp(name, "rtsp") == 0) {
        return std::make_unique<RtspVideoReader>();
    } else if (strcmp(name, "ffmpeg") == 0) {
        return std::make_unique<FfmpegVideoReader>();
    } else if (strcmp(name, "auto") == 0) {
        return create(ReaderType::AUTO);
    }
    
    printf("⚠️  Unknown reader type: %s, using mmap\n", name);
    return std::make_unique<MmapVideoReader>();
}

bool VideoReaderFactory::isIoUringAvailable() {
    struct io_uring ring;
    int ret = io_uring_queue_init(1, &ring, 0);
    if (ret == 0) {
        io_uring_queue_exit(&ring);
        return true;
    }
    return false;
}

bool VideoReaderFactory::isMmapAvailable() {
    // mmap 在所有现代 Linux 系统上都可用
    return true;
}

VideoReaderFactory::ReaderType VideoReaderFactory::getRecommendedType() {
    if (isIoUringAvailable() && isIoUringSuitable()) {
        return ReaderType::IOURING;
    }
    return ReaderType::MMAP;
}

const char* VideoReaderFactory::typeToString(ReaderType type) {
    switch (type) {
        case ReaderType::AUTO:        return "AUTO";
        case ReaderType::MMAP:        return "MMAP";
        case ReaderType::IOURING:     return "IOURING";
        case ReaderType::DIRECT_READ: return "DIRECT_READ";
        case ReaderType::RTSP:        return "RTSP";
        case ReaderType::FFMPEG:      return "FFMPEG";
        default:                      return "UNKNOWN";
    }
}

// ============ 私有辅助方法 ============

std::unique_ptr<IVideoReader> VideoReaderFactory::autoDetect() {
    printf("🔍 Detecting system capabilities:\n");
    
    // 检查 io_uring
    bool iouring_available = isIoUringAvailable();
    printf("   - io_uring: %s\n", iouring_available ? "✓ Available" : "✗ Not available");
    
    // 检查 mmap
    bool mmap_available = isMmapAvailable();
    printf("   - mmap: %s\n", mmap_available ? "✓ Available" : "✗ Not available");
    
    // 决策逻辑
    if (iouring_available && isIoUringSuitable()) {
        printf("✅ Selected: IoUringVideoReader (high-performance async I/O)\n");
        return std::make_unique<IoUringVideoReader>();
    }
    
    if (mmap_available) {
        printf("✅ Selected: MmapVideoReader (memory-mapped I/O)\n");
        return std::make_unique<MmapVideoReader>();
    }
    
    // 默认降级
    printf("⚠️  Warning: No optimal reader available, using MmapVideoReader\n");
    return std::make_unique<MmapVideoReader>();
}

std::unique_ptr<IVideoReader> VideoReaderFactory::createByType(ReaderType type) {
    switch (type) {
        case ReaderType::MMAP:
            return std::make_unique<MmapVideoReader>();
            
        case ReaderType::IOURING:
            if (!isIoUringAvailable()) {
                printf("⚠️  Warning: io_uring not available, falling back to mmap\n");
                return std::make_unique<MmapVideoReader>();
            }
            return std::make_unique<IoUringVideoReader>();
            
        case ReaderType::RTSP:
            return std::make_unique<RtspVideoReader>();
            
        case ReaderType::FFMPEG:
            return std::make_unique<FfmpegVideoReader>();
            
        case ReaderType::DIRECT_READ:
            printf("⚠️  Warning: DIRECT_READ not implemented, using mmap\n");
            return std::make_unique<MmapVideoReader>();
            
        default:
            return autoDetect();
    }
}

VideoReaderFactory::ReaderType VideoReaderFactory::getTypeFromEnvironment() {
    const char* env = getenv("VIDEO_READER_TYPE");
    if (!env) {
        return ReaderType::AUTO;
    }
    
    if (strcmp(env, "mmap") == 0) {
        return ReaderType::MMAP;
    } else if (strcmp(env, "iouring") == 0) {
        return ReaderType::IOURING;
    } else if (strcmp(env, "direct") == 0) {
        return ReaderType::DIRECT_READ;
    } else if (strcmp(env, "rtsp") == 0) {
        return ReaderType::RTSP;
    } else if (strcmp(env, "ffmpeg") == 0) {
        return ReaderType::FFMPEG;
    }
    
    return ReaderType::AUTO;
}

VideoReaderFactory::ReaderType VideoReaderFactory::getTypeFromConfig() {
    // 尝试读取配置文件：/etc/video_reader.conf 或 ~/.config/video_reader.conf
    // 这里简化实现，返回 AUTO
    // 实际项目中可以实现配置文件解析
    return ReaderType::AUTO;
}

bool VideoReaderFactory::isIoUringSuitable() {
    // 简化的适用性检查
    // 实际项目中可以根据以下因素判断：
    // - 系统负载
    // - 可用内存
    // - 并发线程数
    // - 文件大小
    
    // 目前默认认为 io_uring 总是适合（如果可用的话）
    return true;
}




