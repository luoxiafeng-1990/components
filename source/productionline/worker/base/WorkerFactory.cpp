#include "productionline/worker/base/WorkerFactory.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include "common/Logger.hpp"
#include "productionline/worker/core/FFmpegDecodeWorker.hpp"
#include "productionline/worker/core/FfmpegPacketRecorderWorker.hpp"
#include "productionline/worker/core/FFmpegEncodeWorker.hpp"
#include <stdlib.h>
#include <string.h>

log4cplus::Logger WorkerFactory::logger_ =
    log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.WorkerFactory"));

// ============ 公共接口 ============

std::shared_ptr<WorkerBase> WorkerFactory::create(
    const WorkerConfig& config,
    TopologyOwnerType owner_type,
    uint64_t owner_id)
{
    auto type = config.global.worker_type;
    std::unique_ptr<WorkerBase> worker;

    if (type != WorkerType::AUTO) {
        LOG4CPLUS_DEBUG_FMT(logger_, "User specified type: %s", typeToString(type));
        worker = createByType(type, config);
    } else {
        WorkerType env_type = getTypeFromEnvironment();
        if (env_type != WorkerType::AUTO) {
            LOG4CPLUS_DEBUG_FMT(logger_, "Type from environment: %s", typeToString(env_type));
            worker = createByType(env_type, config);
        } else {
            WorkerType config_type = getTypeFromConfig();
            if (config_type != WorkerType::AUTO) {
                LOG4CPLUS_DEBUG_FMT(logger_, "Type from config: %s", typeToString(config_type));
                worker = createByType(config_type, config);
            } else {
                LOG4CPLUS_DEBUG(logger_, "Auto-detecting best worker type...");
                worker = autoDetect(config);
            }
        }
    }

    if (!worker) {
        LOG4CPLUS_ERROR(logger_, "Failed to create Worker");
        return nullptr;
    }

    // unique_ptr → shared_ptr，注册到 ComponentTopology
    std::shared_ptr<WorkerBase> shared_worker(std::move(worker));
    uint64_t worker_id = ComponentTopology::getInstance().registerWorker(shared_worker);

    // 设置 Topology ID，供后续 registerBufferPool() 自动建立 Pool→Worker 关联
    shared_worker->setTopologyId(worker_id);

    // 建立 Worker → Owner 拓扑关联
    if (owner_type == TopologyOwnerType::LINE && owner_id != 0) {
        ComponentTopology::getInstance().linkWorkerToLine(owner_id, worker_id);
    } else if (owner_type == TopologyOwnerType::GROUP && owner_id != 0) {
        ComponentTopology::getInstance().linkWorkerToGroup(owner_id, worker_id);
    }

    LOG4CPLUS_INFO_FMT(logger_, "Worker created and registered: type='%s', registry_id=%lu",
           shared_worker->getWorkerType(), worker_id);

    return shared_worker;
}

const char* WorkerFactory::typeToString(WorkerType type) {
    switch (type) {
        case WorkerType::AUTO:                  return "AUTO";
        case WorkerType::FFMPEG_DECODE:         return "FFMPEG_DECODE";
        case WorkerType::FFMPEG_PACKET_RECORDER: return "FFMPEG_PACKET_RECORDER";
        case WorkerType::FFMPEG_ENCODE:         return "FFMPEG_ENCODE";
        default:                                return "UNKNOWN";
    }
}

// ============ 私有辅助方法 ============

std::unique_ptr<WorkerBase> WorkerFactory::autoDetect(const WorkerConfig& config) {
    LOG4CPLUS_INFO(logger_, "Auto-detecting Worker type...");
    LOG4CPLUS_INFO(logger_, "Using FFmpegDecodeWorker as default");
    
    return std::make_unique<FFmpegDecodeWorker>(config);
}

std::unique_ptr<WorkerBase> WorkerFactory::createByType(WorkerType type, const WorkerConfig& config) {
    switch (type) {
        case WorkerType::FFMPEG_DECODE:
            return std::make_unique<FFmpegDecodeWorker>(config);

        case WorkerType::FFMPEG_PACKET_RECORDER:
            return std::make_unique<FfmpegPacketRecorderWorker>(config);

        case WorkerType::FFMPEG_ENCODE:
            return std::make_unique<FFmpegEncodeWorker>(config);

        case WorkerType::AUTO:
        default:
            return autoDetect(config);
    }
}

WorkerFactory::WorkerType WorkerFactory::getTypeFromEnvironment() {
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
               strcmp(env, "encoder") == 0) {
        return WorkerType::FFMPEG_ENCODE;
    }
    
    return WorkerType::AUTO;
}

WorkerFactory::WorkerType WorkerFactory::getTypeFromConfig() {
    return WorkerType::AUTO;
}
