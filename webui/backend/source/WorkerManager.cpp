#include "../include/WorkerManager.hpp"
#include "../include/DataSourceManager.hpp"
#include "../include/ConsumerManager.hpp"
#include "../include/PreviewService.hpp"
#include "../include/ConfigStore.hpp"
#include <atomic>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <iostream>

namespace webui {

WorkerManager::WorkerManager(DataSourceManager& ds_mgr, ConfigStore& store)
    : ds_manager_(ds_mgr), config_store_(store)
{
}

WorkerManager::~WorkerManager() {
    stopAll();
}

void WorkerManager::setConsumerManager(ConsumerManager* cm) {
    consumer_manager_ = cm;
}

void WorkerManager::setPreviewService(PreviewService* ps) {
    preview_service_ = ps;
}

ApiResponse WorkerManager::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json arr = json::array();
    for (auto& [_, rt] : workers_) {
        WorkerInfo info = rt->info;
        info.state = rt->state.load();
        if (consumer_manager_) {
            info.consumers = consumer_manager_->getConsumerTypeNames(info.id);
        }
        arr.push_back(info);
    }
    return ApiResponse::ok(arr);
}

ApiResponse WorkerManager::create(const json& body) {
    if (!body.contains("name") || !body.contains("datasource_id")) {
        return ApiResponse::error(ErrorCode::PARAM_ERROR,
            "必须提供 name 和 datasource_id");
    }

    std::string ds_id = body["datasource_id"].get<std::string>();
    auto ds = ds_manager_.get(ds_id);
    if (!ds.has_value()) {
        return ApiResponse::error(ErrorCode::WORKER_DS_NOT_FOUND,
            "数据源不存在: " + ds_id);
    }

    auto rt = std::make_unique<WorkerRuntime>();
    rt->info.id = generateId();
    rt->info.name = body["name"].get<std::string>();
    rt->info.datasource_id = ds_id;
    rt->info.datasource_name = ds->name;
    rt->info.worker_type = body.value("worker_type", "FFMPEG_DECODE");
    rt->info.created_at = nowISO8601();

    if (body.contains("decoder")) {
        rt->info.decoder = body["decoder"].get<ApiDecoderConfig>();
    }

    rt->state = WorkerState::CREATED;
    std::string id = rt->info.id;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        workers_[id] = std::move(rt);
    }

    ds_manager_.markInUse(ds_id, true);

    auto& w = workers_[id];
    WorkerInfo info_copy = w->info;
    info_copy.state = w->state.load();
    return ApiResponse::ok(json(info_copy), "Worker 创建成功");
}

ApiResponse WorkerManager::remove(const std::string& id) {
    // 先取出 instance（需要在锁外停止）
    std::unique_ptr<ComponentsWorkerInstance> instance_to_stop;
    std::string ds_id;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = workers_.find(id);
        if (it == workers_.end()) {
            return ApiResponse::error(ErrorCode::NOT_FOUND, "Worker 不存在: " + id);
        }

        auto state = it->second->state.load();
        if (state == WorkerState::RUNNING || state == WorkerState::STARTING) {
            instance_to_stop = std::move(it->second->instance);
        }

        ds_id = it->second->info.datasource_id;
        workers_.erase(it);

        bool still_in_use = false;
        for (auto& [_, w] : workers_) {
            if (w->info.datasource_id == ds_id) {
                still_in_use = true;
                break;
            }
        }
        if (!still_in_use) {
            ds_manager_.markInUse(ds_id, false);
        }
    }

    // 在锁外停止 instance（可能阻塞等待 join）
    if (instance_to_stop) {
        instance_to_stop->stop();
    }

    return ApiResponse::ok(nullptr, "Worker 已删除");
}

ApiResponse WorkerManager::start(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(id);
    if (it == workers_.end()) {
        return ApiResponse::error(ErrorCode::NOT_FOUND, "Worker 不存在: " + id);
    }

    auto state = it->second->state.load();
    if (state == WorkerState::RUNNING) {
        return ApiResponse::error(ErrorCode::WORKER_STATE_INVALID, "Worker 已在运行");
    }

    auto& rt = it->second;

    auto ds = ds_manager_.get(rt->info.datasource_id);
    if (!ds.has_value()) {
        rt->state = WorkerState::ERROR;
        return ApiResponse::error(ErrorCode::WORKER_DS_NOT_FOUND,
            "数据源不存在: " + rt->info.datasource_id);
    }

    std::vector<ConsumerInfo> consumers;
    if (consumer_manager_) {
        consumers = consumer_manager_->getConsumersForWorker(id);
    }

    rt->state = WorkerState::STARTING;

    rt->instance = std::make_unique<ComponentsWorkerInstance>();
    bool ok = rt->instance->start(
        ds.value(), rt->info.decoder, consumers, preview_service_, id);

    if (!ok) {
        rt->state = WorkerState::ERROR;
        rt->instance.reset();
        return ApiResponse::error(ErrorCode::WORKER_START_FAILED,
            "Worker 启动失败，请检查数据源和解码器配置");
    }

    rt->state = WorkerState::RUNNING;
    std::cout << "[WebUI] Worker " << id << " 已启动" << std::endl;

    return ApiResponse::ok(json{{"id", id}, {"state", "RUNNING"}}, "Worker 已启动");
}

ApiResponse WorkerManager::stop(const std::string& id) {
    // 在锁内：找到 worker，设置状态，取走 instance 的所有权
    std::unique_ptr<ComponentsWorkerInstance> instance_to_stop;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = workers_.find(id);
        if (it == workers_.end()) {
            return ApiResponse::error(ErrorCode::NOT_FOUND, "Worker 不存在: " + id);
        }

        auto state = it->second->state.load();
        if (state != WorkerState::RUNNING && state != WorkerState::STARTING) {
            return ApiResponse::error(ErrorCode::WORKER_STATE_INVALID,
                "Worker 未在运行");
        }

        it->second->state = WorkerState::STOPPING;
        instance_to_stop = std::move(it->second->instance);
    }

    // 在锁外停止（可能阻塞等 join，但不影响其他 API 请求）
    if (instance_to_stop) {
        instance_to_stop->stop();
    }

    // 重新获取锁设置最终状态
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = workers_.find(id);
        if (it != workers_.end()) {
            it->second->state = WorkerState::STOPPED;
        }
    }

    std::cout << "[WebUI] Worker " << id << " 已停止" << std::endl;
    return ApiResponse::ok(json{{"id", id}, {"state", "STOPPED"}}, "Worker 已停止");
}

ApiResponse WorkerManager::status(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(id);
    if (it == workers_.end()) {
        return ApiResponse::error(ErrorCode::NOT_FOUND, "Worker 不存在: " + id);
    }

    auto& rt = it->second;
    WorkerStatus ws;
    ws.id = id;
    ws.state = rt->state.load();

    if (rt->instance) {
        ws.fps = rt->instance->getFps();
        ws.decoded_frames = rt->instance->getDecodedFrames();
        ws.dropped_frames = rt->instance->getDroppedFrames();
        ws.uptime_seconds = rt->instance->getUptimeSeconds();
        ws.command_line = rt->instance->getCommandLine();
        ws.output = rt->instance->getLastOutput();
    }

    if (consumer_manager_) {
        ws.consumers.clear();
        auto clist = consumer_manager_->getConsumersForWorker(id);
        for (auto& c : clist) {
            ws.consumers.push_back(c);
        }
    }

    return ApiResponse::ok(json(ws));
}

bool WorkerManager::exists(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return workers_.count(id) > 0;
}

WorkerState WorkerManager::getState(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(id);
    if (it == workers_.end()) return WorkerState::ERROR;
    return it->second->state.load();
}

void WorkerManager::stopAll() {
    // 取出所有 instance 的所有权
    std::vector<std::pair<std::string, std::unique_ptr<ComponentsWorkerInstance>>> to_stop;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, rt] : workers_) {
            if (rt->instance) {
                to_stop.emplace_back(id, std::move(rt->instance));
                rt->state = WorkerState::STOPPING;
            }
        }
    }

    // 在锁外逐个停止
    for (auto& [id, inst] : to_stop) {
        std::cout << "[WebUI] 正在停止 Worker " << id << std::endl;
        inst->stop();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, rt] : workers_) {
            rt->state = WorkerState::STOPPED;
        }
    }
}

std::string WorkerManager::generateId() const {
    static std::atomic<int> counter{1};
    std::ostringstream oss;
    oss << "wk-" << std::setfill('0') << std::setw(3) << counter++;
    return oss.str();
}

std::string WorkerManager::nowISO8601() const {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&t), "%FT%TZ");
    return oss.str();
}

} // namespace webui
