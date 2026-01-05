#include "productionline/worker/BufferFillingWorkerFactory.hpp"
#include "common/Logger.hpp"
#include "productionline/worker/FfmpegDecodeRtspWorker.hpp"
#include "productionline/worker/FfmpegRecordRtspWorker.hpp"
#include "productionline/worker/FfmpegDecodeVideoFileWorker.hpp"
#include <stdlib.h>
#include <string.h>

// ============ 公共接口 ============

std::unique_ptr<WorkerBase> BufferFillingWorkerFactory::create(const WorkerConfig& config) {
    auto type = config.worker_type;
    // 1️⃣ 用户显式指定（最高优先级）
    if (type != WorkerType::AUTO) {
        LOG_DEBUG_FMT("[WorkerFactory] BufferFillingWorkerFactory: User specified type: %s", typeToString(type));
        return createByType(type, config);
    }
    
    // 2️⃣ 环境变量配置
    WorkerType env_type = getTypeFromEnvironment();
    if (env_type != WorkerType::AUTO) {
        LOG_DEBUG_FMT("[WorkerFactory] BufferFillingWorkerFactory: Type from environment: %s", typeToString(env_type));
        return createByType(env_type, config);
    }
    
    // 3️⃣ 配置文件
    WorkerType config_type = getTypeFromConfig();
    if (config_type != WorkerType::AUTO) {
        LOG_DEBUG_FMT("[WorkerFactory] BufferFillingWorkerFactory: Type from config: %s", typeToString(config_type));
        return createByType(config_type, config);
    }
    
    // 4️⃣ 自动检测
    LOG_DEBUG("[WorkerFactory] BufferFillingWorkerFactory: Auto-detecting best worker type...");
    return autoDetect(config);
}

BufferFillingWorkerFactory::WorkerType BufferFillingWorkerFactory::getRecommendedType() {
    // 推荐使用 FFmpeg Video File Worker
    return WorkerType::FFMPEG_VIDEO_FILE;
}

const char* BufferFillingWorkerFactory::typeToString(WorkerType type) {
    switch (type) {
        case WorkerType::AUTO:                return "AUTO";
        case WorkerType::FFMPEG_RTSP:         return "FFMPEG_RTSP";
        case WorkerType::FFMPEG_RTSP_RECORD:  return "FFMPEG_RTSP_RECORD";
        case WorkerType::FFMPEG_VIDEO_FILE:   return "FFMPEG_VIDEO_FILE";
        default:                              return "UNKNOWN";
    }
}

// ============ 私有辅助方法 ============

std::unique_ptr<WorkerBase> BufferFillingWorkerFactory::autoDetect(const WorkerConfig& config) {
    LOG_INFO("🔍 Auto-detecting Worker type...");
    LOG_INFO("   Using FfmpegDecodeVideoFileWorker as default");
    
    // 默认使用 FFmpeg Video File Worker
    return std::make_unique<FfmpegDecodeVideoFileWorker>(config);
}

std::unique_ptr<WorkerBase> BufferFillingWorkerFactory::createByType(WorkerType type, const WorkerConfig& config) {
    switch (type) {
        case WorkerType::FFMPEG_RTSP:
            return std::make_unique<FfmpegDecodeRtspWorker>(config);
            
        case WorkerType::FFMPEG_RTSP_RECORD:
            return std::make_unique<FfmpegRecordRtspWorker>(config);
            
        case WorkerType::FFMPEG_VIDEO_FILE:
            return std::make_unique<FfmpegDecodeVideoFileWorker>(config);
            
        case WorkerType::AUTO:
        default:
            return autoDetect(config);
    }
}

BufferFillingWorkerFactory::WorkerType BufferFillingWorkerFactory::getTypeFromEnvironment() {
    const char* env = getenv("VIDEO_READER_TYPE");
    if (!env) {
        return WorkerType::AUTO;
    }
    
    if (strcmp(env, "rtsp") == 0 || strcmp(env, "ffmpeg_rtsp") == 0) {
        return WorkerType::FFMPEG_RTSP;
    } else if (strcmp(env, "rtsp_record") == 0 || strcmp(env, "ffmpeg_rtsp_record") == 0) {
        return WorkerType::FFMPEG_RTSP_RECORD;
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




