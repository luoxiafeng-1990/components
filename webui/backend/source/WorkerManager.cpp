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
#include <algorithm>

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

// ============================================================
// 持久化
// ============================================================

void WorkerManager::saveToStore() {
    json arr = json::array();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [_, rt] : workers_) {
            arr.push_back(rt->info);
        }
    }
    config_store_.setWorkers(arr);
}

void WorkerManager::loadFromStore() {
    json arr = config_store_.getWorkers();
    if (!arr.is_array()) return;

    std::lock_guard<std::mutex> lock(mutex_);

    // 找到已有 ID 中最大的编号，确保后续 generateId() 不冲突
    int max_id = 0;
    for (auto& item : arr) {
        try {
            WorkerInfo info = item.get<WorkerInfo>();

            // 跳过 datasource 不存在的 worker
            auto ds = ds_manager_.get(info.datasource_id);
            if (!ds.has_value()) {
                std::cerr << "[WebUI] loadFromStore: 跳过 Worker '"
                          << info.id << "' (数据源 '" << info.datasource_id
                          << "' 不存在)" << std::endl;
                continue;
            }
            info.datasource_name = ds->name;

            auto rt = std::make_unique<WorkerRuntime>();
            rt->info = std::move(info);
            rt->state = WorkerState::STOPPED;

            // 解析 ID 编号
            if (rt->info.id.size() > 3 && rt->info.id.substr(0, 3) == "wk-") {
                try {
                    int num = std::stoi(rt->info.id.substr(3));
                    if (num > max_id) max_id = num;
                } catch (...) {}
            }

            std::string id = rt->info.id;
            ds_manager_.markInUse(rt->info.datasource_id, true);
            workers_[id] = std::move(rt);

            std::cout << "[WebUI] loadFromStore: 恢复 Worker '"
                      << id << "'" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[WebUI] loadFromStore: 解析失败: "
                      << e.what() << std::endl;
        }
    }

    // 确保 generateId 计数器跳过已有编号
    if (max_id > 0) {
        int expected = id_counter_.load();
        while (expected < max_id + 1) {
            if (id_counter_.compare_exchange_weak(expected, max_id + 1)) break;
        }
    }
}

// ============================================================
// CRUD
// ============================================================

ApiResponse WorkerManager::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json arr = json::array();
    for (auto& [_, rt] : workers_) {
        WorkerInfo info = rt->info;
        info.state = rt->state.load();
        info.refreshConsumerNames();
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

    // 解析 consumers 配置（可选）
    if (body.contains("consumers") && body["consumers"].is_array()) {
        for (auto& item : body["consumers"]) {
            ConsumerInfo ci;
            ci.id = generateConsumerId();
            ci.type = item["type"].get<ConsumerType>();
            ci.config = item.value("config", json::object());
            ci.state = "inactive";
            rt->info.consumers_config.push_back(ci);
        }
    }
    rt->info.refreshConsumerNames();

    rt->state = WorkerState::CREATED;
    std::string id = rt->info.id;

    WorkerInfo info_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        workers_[id] = std::move(rt);
        info_copy = workers_[id]->info;
        info_copy.state = workers_[id]->state.load();
    }

    ds_manager_.markInUse(ds_id, true);
    saveToStore();

    return ApiResponse::ok(json(info_copy), "Worker 创建成功");
}

ApiResponse WorkerManager::update(const std::string& id, const json& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(id);
    if (it == workers_.end()) {
        return ApiResponse::error(ErrorCode::NOT_FOUND, "Worker 不存在: " + id);
    }

    auto state = it->second->state.load();
    if (state == WorkerState::RUNNING || state == WorkerState::STARTING
        || state == WorkerState::STOPPING) {
        return ApiResponse::error(ErrorCode::WORKER_STATE_INVALID,
            "Worker 正在运行中，请先停止再编辑");
    }

    auto& info = it->second->info;

    if (body.contains("name")) {
        info.name = body["name"].get<std::string>();
    }

    if (body.contains("datasource_id")) {
        std::string new_ds_id = body["datasource_id"].get<std::string>();
        auto ds = ds_manager_.get(new_ds_id);
        if (!ds.has_value()) {
            return ApiResponse::error(ErrorCode::WORKER_DS_NOT_FOUND,
                "数据源不存在: " + new_ds_id);
        }
        // 释放旧数据源
        if (info.datasource_id != new_ds_id) {
            bool old_still_in_use = false;
            for (auto& [wid, w] : workers_) {
                if (wid != id && w->info.datasource_id == info.datasource_id) {
                    old_still_in_use = true;
                    break;
                }
            }
            if (!old_still_in_use) {
                ds_manager_.markInUse(info.datasource_id, false);
            }
            ds_manager_.markInUse(new_ds_id, true);
        }
        info.datasource_id = new_ds_id;
        info.datasource_name = ds->name;
    }

    if (body.contains("worker_type")) {
        info.worker_type = body["worker_type"].get<std::string>();
    }

    if (body.contains("decoder")) {
        info.decoder = body["decoder"].get<ApiDecoderConfig>();
    }

    // 完整替换 consumers 配置
    if (body.contains("consumers") && body["consumers"].is_array()) {
        info.consumers_config.clear();
        for (auto& item : body["consumers"]) {
            ConsumerInfo ci;
            ci.id = generateConsumerId();
            ci.type = item["type"].get<ConsumerType>();
            ci.config = item.value("config", json::object());
            ci.state = "inactive";
            info.consumers_config.push_back(ci);
        }
    }
    info.refreshConsumerNames();

    WorkerInfo info_copy = info;
    info_copy.state = it->second->state.load();

    // 在锁内触发持久化（saveToStore 内部有自己的锁，需要先释放）
    // 但 saveToStore 需要 mutex_，所以先收集数据
    json arr = json::array();
    for (auto& [_, rt] : workers_) {
        arr.push_back(rt->info);
    }

    // 不能在持有 mutex_ 时调用 saveToStore（它也锁 mutex_）
    // 改为直接写 ConfigStore
    config_store_.setWorkers(arr);

    return ApiResponse::ok(json(info_copy), "Worker 配置已更新");
}

ApiResponse WorkerManager::remove(const std::string& id) {
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

    if (instance_to_stop) {
        instance_to_stop->stop();
    }

    saveToStore();
    return ApiResponse::ok(nullptr, "Worker 已删除");
}

// ============================================================
// 启动 / 停止
// ============================================================

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

    // 从 WorkerInfo 自身获取消费者配置
    std::vector<ConsumerInfo> consumers = rt->info.consumers_config;

    // 同步到 ConsumerManager（供 status 等 API 查询）
    if (consumer_manager_) {
        syncConsumersToManager(id, consumers);
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

    if (instance_to_stop) {
        instance_to_stop->stop();
    }

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

    ws.consumers = rt->info.consumers_config;

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

// ============================================================
// Consumer 管理（操作 WorkerInfo.consumers_config）
// ============================================================

ApiResponse WorkerManager::addConsumer(const std::string& worker_id, const json& body) {
    if (!body.contains("type")) {
        return ApiResponse::error(ErrorCode::PARAM_ERROR, "必须提供 type 字段");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) {
        return ApiResponse::error(ErrorCode::NOT_FOUND, "Worker 不存在: " + worker_id);
    }

    ConsumerInfo ci;
    ci.id = generateConsumerId();
    ci.type = body["type"].get<ConsumerType>();
    ci.config = body.value("config", json::object());
    ci.state = "inactive";

    it->second->info.consumers_config.push_back(ci);
    it->second->info.refreshConsumerNames();

    // 持久化
    json arr = json::array();
    for (auto& [_, rt] : workers_) arr.push_back(rt->info);
    config_store_.setWorkers(arr);

    return ApiResponse::ok(json(ci), "消费者添加成功");
}

ApiResponse WorkerManager::removeConsumer(const std::string& worker_id,
                                          const std::string& consumer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) {
        return ApiResponse::error(ErrorCode::NOT_FOUND, "Worker 不存在: " + worker_id);
    }

    auto& vec = it->second->info.consumers_config;
    auto cit = std::find_if(vec.begin(), vec.end(),
        [&](const ConsumerInfo& c) { return c.id == consumer_id; });

    if (cit == vec.end()) {
        return ApiResponse::error(ErrorCode::NOT_FOUND,
            "消费者不存在: " + consumer_id);
    }

    vec.erase(cit);
    it->second->info.refreshConsumerNames();

    json arr = json::array();
    for (auto& [_, rt] : workers_) arr.push_back(rt->info);
    config_store_.setWorkers(arr);

    return ApiResponse::ok(nullptr, "消费者已移除");
}

ApiResponse WorkerManager::updateConsumer(const std::string& worker_id,
                                          const std::string& consumer_id,
                                          const json& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) {
        return ApiResponse::error(ErrorCode::NOT_FOUND, "Worker 不存在: " + worker_id);
    }

    auto& vec = it->second->info.consumers_config;
    auto cit = std::find_if(vec.begin(), vec.end(),
        [&](const ConsumerInfo& c) { return c.id == consumer_id; });

    if (cit == vec.end()) {
        return ApiResponse::error(ErrorCode::NOT_FOUND,
            "消费者不存在: " + consumer_id);
    }

    if (body.contains("config")) {
        cit->config = body["config"];
    }

    json arr = json::array();
    for (auto& [_, rt] : workers_) arr.push_back(rt->info);
    config_store_.setWorkers(arr);

    return ApiResponse::ok(json(*cit), "消费者配置已更新");
}

// ============================================================
// 辅助方法
// ============================================================

void WorkerManager::syncConsumersToManager(const std::string& worker_id,
                                           const std::vector<ConsumerInfo>& consumers) {
    if (!consumer_manager_) return;
    // 清除 ConsumerManager 中该 worker 的旧记录，重新添加
    // 由于 ConsumerManager 没有 clear 方法，我们逐个移除再添加
    auto existing = consumer_manager_->getConsumersForWorker(worker_id);
    for (auto& c : existing) {
        consumer_manager_->removeConsumer(worker_id, c.id);
    }
    for (auto& c : consumers) {
        json body = {{"type", c.type}, {"config", c.config}};
        consumer_manager_->addConsumer(worker_id, body);
    }
}

std::string WorkerManager::generateId() {
    std::ostringstream oss;
    oss << "wk-" << std::setfill('0') << std::setw(3) << id_counter_++;
    return oss.str();
}

std::string WorkerManager::generateConsumerId() {
    std::ostringstream oss;
    oss << "cs-" << std::setfill('0') << std::setw(3) << consumer_id_counter_++;
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
