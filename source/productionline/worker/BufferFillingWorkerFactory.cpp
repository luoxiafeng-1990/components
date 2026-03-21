#include "productionline/worker/BufferFillingWorkerFactory.hpp"
#include "common/Logger.hpp"
#include "productionline/worker/FFmpegDecodeWorker.hpp"
#include "productionline/worker/FfmpegPacketRecorderWorker.hpp"
#include "productionline/worker/FFmpegEncodeWorker.hpp"  // ⭐ v2.29 新增
#include <stdlib.h>
#include <string.h>

log4cplus::Logger BufferFillingWorkerFactory::logger_ =
    log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.BufferFillingWorkerFactory"));

// ============ 公共接口 ============

std::unique_ptr<WorkerBase> BufferFillingWorkerFactory::create(const WorkerConfig& config) {
    auto type = config.global.worker_type;
    // 1️⃣ 用户显式指定（最高优先级）
    if (type != WorkerType::AUTO) {
        LOG4CPLUS_DEBUG_FMT(logger_, "User specified type: %s", typeToString(type));
        return createByType(type, config);
    }
    
    // 2️⃣ 环境变量配置
    WorkerType env_type = getTypeFromEnvironment();
    if (env_type != WorkerType::AUTO) {
        LOG4CPLUS_DEBUG_FMT(logger_, "Type from environment: %s", typeToString(env_type));
        return createByType(env_type, config);
    }
    
    // 3️⃣ 配置文件
    WorkerType config_type = getTypeFromConfig();
    if (config_type != WorkerType::AUTO) {
        LOG4CPLUS_DEBUG_FMT(logger_, "Type from config: %s", typeToString(config_type));
        return createByType(config_type, config);
    }
    
    // 4️⃣ 自动检测
    LOG4CPLUS_DEBUG(logger_, "Auto-detecting best worker type...");
    return autoDetect(config);
}

BufferFillingWorkerFactory::WorkerType BufferFillingWorkerFactory::getRecommendedType() {
    // 推荐使用 FFmpeg Decode Worker（统一处理文件和 RTSP）
    return WorkerType::FFMPEG_DECODE;
}

const char* BufferFillingWorkerFactory::typeToString(WorkerType type) {
    switch (type) {
        case WorkerType::AUTO:                  return "AUTO";
        case WorkerType::FFMPEG_DECODE:         return "FFMPEG_DECODE";
        case WorkerType::FFMPEG_PACKET_RECORDER: return "FFMPEG_PACKET_RECORDER";
        case WorkerType::FFMPEG_ENCODE:         return "FFMPEG_ENCODE";  // ⭐ v2.29 新增
        default:                                return "UNKNOWN";
    }
}

// ============ 私有辅助方法 ============

std::unique_ptr<WorkerBase> BufferFillingWorkerFactory::autoDetect(const WorkerConfig& config) {
    LOG4CPLUS_INFO(logger_, "Auto-detecting Worker type...");
    LOG4CPLUS_INFO(logger_, "Using FFmpegDecodeWorker as default");
    
    // 默认使用 FFmpeg Decode Worker（统一处理文件和 RTSP）
    return std::make_unique<FFmpegDecodeWorker>(config);
}

std::unique_ptr<WorkerBase> BufferFillingWorkerFactory::createByType(WorkerType type, const WorkerConfig& config) {
    switch (type) {
        case WorkerType::FFMPEG_DECODE:
            return std::make_unique<FFmpegDecodeWorker>(config);
            
        case WorkerType::FFMPEG_PACKET_RECORDER:
            return std::make_unique<FfmpegPacketRecorderWorker>(config);
            
        case WorkerType::FFMPEG_ENCODE:  // ⭐ v2.29 新增
            return std::make_unique<FFmpegEncodeWorker>(config);
            
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
    
    if (strcmp(env, "ffmpeg") == 0 || strcmp(env, "ffmpeg_decode") == 0 ||
        strcmp(env, "rtsp") == 0 || strcmp(env, "ffmpeg_rtsp") == 0 ||
        strcmp(env, "ffmpeg_video_file") == 0) {
        return WorkerType::FFMPEG_DECODE;
    } else if (strcmp(env, "packet_recorder") == 0 || strcmp(env, "ffmpeg_packet_recorder") == 0) {
        return WorkerType::FFMPEG_PACKET_RECORDER;
    } else if (strcmp(env, "encode") == 0 || strcmp(env, "ffmpeg_encode") == 0 ||
               strcmp(env, "encoder") == 0) {  // ⭐ v2.29 新增
        return WorkerType::FFMPEG_ENCODE;
    }
    
    return WorkerType::AUTO;
}

BufferFillingWorkerFactory::WorkerType BufferFillingWorkerFactory::getTypeFromConfig() {
    // 尝试读取配置文件：/etc/video_reader.conf 或 ~/.config/video_reader.conf
    // 这里简化实现，返回 AUTO
    // 实际项目中可以实现配置文件解析
    return WorkerType::AUTO;
}



