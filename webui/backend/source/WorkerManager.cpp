#include "../include/WorkerManager.hpp"
#include "../include/DataSourceManager.hpp"
#include "../include/ConsumerManager.hpp"
#include "../include/PreviewService.hpp"
#include "../include/ConfigStore.hpp"
#include "../include/ComponentsBridge.hpp"

#include "consumptionline/BufferConsumerService.hpp"

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
    restart_seq_ = -1;
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

    int max_id = 0;
    for (auto& item : arr) {
        try {
            WorkerInfo info = item.get<WorkerInfo>();

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
    rt->info.loop = body.value("loop", true);
    rt->info.created_at = nowISO8601();

    if (body.contains("decoder")) {
        rt->info.decoder = body["decoder"].get<ApiDecoderConfig>();
    }

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
    if (body.contains("loop")) {
        info.loop = body["loop"].get<bool>();
    }

    if (body.contains("decoder")) {
        info.decoder = body["decoder"].get<ApiDecoderConfig>();
    }

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

    json arr = json::array();
    for (auto& [_, rt] : workers_) {
        arr.push_back(rt->info);
    }
    config_store_.setWorkers(arr);

    return ApiResponse::ok(json(info_copy), "Worker 配置已更新");
}

ApiResponse WorkerManager::remove(const std::string& id) {
    std::string ds_id;
    bool was_running = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = workers_.find(id);
        if (it == workers_.end()) {
            return ApiResponse::error(ErrorCode::NOT_FOUND, "Worker 不存在: " + id);
        }

        auto state = it->second->state.load();
        was_running = (state == WorkerState::RUNNING || state == WorkerState::STARTING);

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

    if (was_running) {
        scheduleRestart();
    }

    saveToStore();
    return ApiResponse::ok(nullptr, "Worker 已删除");
}

// ============================================================
// 启动 / 停止
// ============================================================

ApiResponse WorkerManager::start(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = workers_.find(id);
        if (it == workers_.end()) {
            return ApiResponse::error(ErrorCode::NOT_FOUND, "Worker 不存在: " + id);
        }

        auto state = it->second->state.load();
        if (state == WorkerState::RUNNING) {
            return ApiResponse::error(ErrorCode::WORKER_STATE_INVALID, "Worker 已在运行");
        }

        auto ds = ds_manager_.get(it->second->info.datasource_id);
        if (!ds.has_value()) {
            it->second->state = WorkerState::ERROR;
            return ApiResponse::error(ErrorCode::WORKER_DS_NOT_FOUND,
                "数据源不存在: " + it->second->info.datasource_id);
        }

        if (consumer_manager_) {
            syncConsumersToManager(id, it->second->info.consumers_config);
        }

        it->second->state = WorkerState::RUNNING;
    }

    scheduleRestart();

    std::cout << "[WebUI] Worker " << id << " 已标记 RUNNING" << std::endl;
    return ApiResponse::ok(json{{"id", id}, {"state", "RUNNING"}}, "Worker 已启动");
}

ApiResponse WorkerManager::stop(const std::string& id) {
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

        it->second->state = WorkerState::STOPPED;
    }

    // 检查是否还有 RUNNING 的 worker，如果没有则直接停服务
    bool any_running = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [_, rt] : workers_) {
            if (rt->state == WorkerState::RUNNING) { any_running = true; break; }
        }
    }

    if (!any_running) {
        // 没有剩余 RUNNING worker，直接停止服务（不走 debounce）
        if (parallel_service_) {
            parallel_service_->requestStop();
        }
        if (service_thread_.joinable()) {
            service_thread_.join();
        }
        parallel_service_.reset();
        std::cout << "[WebUI] 所有 Worker 已停止，PARALLEL 服务已关闭" << std::endl;
    } else {
        scheduleRestart();
    }

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

    if (parallel_service_ && ws.state == WorkerState::RUNNING) {
        auto elapsed = std::chrono::steady_clock::now() - service_start_time_;
        ws.uptime_seconds = std::chrono::duration<double>(elapsed).count();
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [_, rt] : workers_) {
            if (rt->state == WorkerState::RUNNING || rt->state == WorkerState::STARTING) {
                rt->state = WorkerState::STOPPED;
            }
        }
    }

    // 停止 PARALLEL 服务
    if (parallel_service_) {
        std::cout << "[WebUI] 正在停止 PARALLEL 服务..." << std::endl;
        parallel_service_->requestStop();
    }
    if (service_thread_.joinable()) {
        service_thread_.join();
    }
    parallel_service_.reset();
    std::cout << "[WebUI] PARALLEL 服务已停止" << std::endl;
}

// ============================================================
// PARALLEL 服务管理（核心）
// ============================================================

void WorkerManager::restartParallelService() {
    // 1. 停止旧服务（标记为主动重启，不改 worker 状态）
    intentional_restart_ = true;
    if (parallel_service_) {
        parallel_service_->requestStop();
    }
    if (service_thread_.joinable()) {
        service_thread_.join();
    }
    parallel_service_.reset();
    intentional_restart_ = false;

    // 2. 收集所有 RUNNING 的 worker 的 config
    std::vector<WorkerConfig> all_configs;
    uint32_t flags = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, rt] : workers_) {
            if (rt->state != WorkerState::RUNNING) continue;

            auto ds = ds_manager_.get(rt->info.datasource_id);
            if (!ds.has_value()) {
                rt->state = WorkerState::ERROR;
                continue;
            }

            auto ds_copy = ds.value();
            ds_copy.loop = rt->info.loop;

            auto br = buildWorkerConfig(
                ds_copy, rt->info.decoder, rt->info.consumers_config,
                preview_service_, id);

            if (!br.success) {
                std::cerr << "[WebUI] Worker " << id
                          << " 配置构建失败: " << br.error << std::endl;
                rt->state = WorkerState::ERROR;
                continue;
            }

            flags |= br.flags;
            all_configs.push_back(std::move(br.config));

            std::cout << "[WebUI] Worker " << id
                      << " 加入 PARALLEL: " << br.description << std::endl;
        }
    }

    if (all_configs.empty()) {
        std::cout << "[WebUI] 无 RUNNING 的 Worker，PARALLEL 服务不启动" << std::endl;
        return;
    }

    // 3. 启动 PARALLEL 服务
    std::cout << "[WebUI] 启动 PARALLEL 服务: " << all_configs.size()
              << " 个 Worker, flags=0x" << std::hex << flags << std::dec << std::endl;

    parallel_service_ = std::make_unique<consumer::BufferConsumerService>();
    service_start_time_ = std::chrono::steady_clock::now();

    service_thread_ = std::thread(
        [this, configs = std::move(all_configs), flags]() {
            auto result = parallel_service_->start(
                configs, consumer::ExecuteMode::PARALLEL, flags);

            std::cout << "[WebUI] PARALLEL 服务完成: "
                      << (result.success ? "OK" : "FAILED")
                      << " frames=" << result.frames_consumed
                      << " fps=" << result.average_fps;
            if (!result.error_message.empty())
                std::cout << " error=" << result.error_message;
            std::cout << std::endl;

            // 只有自然结束（非主动重启）才标记 worker 为 STOPPED
            if (!intentional_restart_.load()) {
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto& [_, rt] : workers_) {
                    if (rt->state == WorkerState::RUNNING) {
                        rt->state = WorkerState::STOPPED;
                    }
                }
            }
        });
}

void WorkerManager::scheduleRestart() {
    int seq = ++restart_seq_;

    std::thread([this, seq]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        if (restart_seq_.load() != seq) return;
        std::cout << "[WebUI] Debounce 到期，执行 restartParallelService" << std::endl;
        restartParallelService();
    }).detach();
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
