#include "../include/DataSourceManager.hpp"
#include "../include/ConfigStore.hpp"
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <random>
#include <filesystem>

namespace webui {

DataSourceManager::DataSourceManager(ConfigStore& store)
    : config_store_(store)
{
    auto saved = config_store_.getDatasources();
    for (auto& item : saved) {
        DataSourceInfo ds = item.get<DataSourceInfo>();
        datasources_[ds.id] = ds;
    }
}

ApiResponse DataSourceManager::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json arr = json::array();
    for (auto& [_, ds] : datasources_) {
        arr.push_back(ds);
    }
    return ApiResponse::ok(arr);
}

ApiResponse DataSourceManager::add(const json& body) {
    if (!body.contains("name") || !body.contains("type") || !body.contains("path")) {
        return ApiResponse::error(ErrorCode::PARAM_ERROR,
            "必须提供 name, type, path 字段");
    }

    DataSourceInfo ds;
    ds.id = generateId();
    ds.name = body["name"].get<std::string>();
    ds.type = body["type"].get<DataSourceType>();
    ds.path = body["path"].get<std::string>();
    if (body.contains("buffer_count")) ds.buffer_count = body["buffer_count"].get<int>();
    if (body.contains("max_frames"))   ds.max_frames = body["max_frames"].get<int>();
    if (body.contains("loop"))         ds.loop = body["loop"].get<bool>();
    ds.created_at = nowISO8601();
    ds.status = "idle";

    if (ds.type == DataSourceType::FILE && !std::filesystem::exists(ds.path)) {
        return ApiResponse::error(ErrorCode::DATASOURCE_UNAVAILABLE,
            "文件不存在: " + ds.path);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        datasources_[ds.id] = ds;
    }

    // 持久化
    json arr = json::array();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [_, d] : datasources_) arr.push_back(d);
    }
    config_store_.setDatasources(arr);

    return ApiResponse::ok(json(ds), "数据源添加成功");
}

ApiResponse DataSourceManager::update(const std::string& id, const json& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = datasources_.find(id);
    if (it == datasources_.end()) {
        return ApiResponse::error(ErrorCode::NOT_FOUND, "数据源不存在: " + id);
    }

    if (it->second.status == "in_use") {
        return ApiResponse::error(ErrorCode::DATASOURCE_IN_USE,
            "数据源正在使用中，不可修改");
    }

    auto& ds = it->second;
    if (body.contains("name"))         ds.name = body["name"].get<std::string>();
    if (body.contains("type"))         ds.type = body["type"].get<DataSourceType>();
    if (body.contains("path"))         ds.path = body["path"].get<std::string>();
    if (body.contains("buffer_count")) ds.buffer_count = body["buffer_count"].get<int>();
    if (body.contains("max_frames"))   ds.max_frames = body["max_frames"].get<int>();
    if (body.contains("loop"))         ds.loop = body["loop"].get<bool>();

    json arr = json::array();
    for (auto& [_, d] : datasources_) arr.push_back(d);
    config_store_.setDatasources(arr);

    return ApiResponse::ok(json(ds), "数据源更新成功");
}

ApiResponse DataSourceManager::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = datasources_.find(id);
    if (it == datasources_.end()) {
        return ApiResponse::error(ErrorCode::NOT_FOUND, "数据源不存在: " + id);
    }
    if (it->second.status == "in_use") {
        return ApiResponse::error(ErrorCode::DATASOURCE_IN_USE,
            "数据源正在被 Worker 使用，请先删除关联的 Worker");
    }

    datasources_.erase(it);

    json arr = json::array();
    for (auto& [_, d] : datasources_) arr.push_back(d);
    config_store_.setDatasources(arr);

    return ApiResponse::ok(nullptr, "数据源已删除");
}

ApiResponse DataSourceManager::probe(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = datasources_.find(id);
    if (it == datasources_.end()) {
        return ApiResponse::error(ErrorCode::NOT_FOUND, "数据源不存在: " + id);
    }

    // TODO: 调用 FFmpeg avformat_open_input / avformat_find_stream_info 探测
    ProbeResult result;
    result.format = "unknown";
    result.codec = "unknown";
    result.width = 0;
    result.height = 0;
    result.fps = 0.0;
    result.duration_seconds = -1.0;

    return ApiResponse::ok(json(result));
}

std::optional<DataSourceInfo> DataSourceManager::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = datasources_.find(id);
    if (it != datasources_.end()) return it->second;
    return std::nullopt;
}

bool DataSourceManager::markInUse(const std::string& id, bool in_use) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = datasources_.find(id);
    if (it == datasources_.end()) return false;
    it->second.status = in_use ? "in_use" : "idle";
    return true;
}

bool DataSourceManager::isInUse(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = datasources_.find(id);
    if (it == datasources_.end()) return false;
    return it->second.status == "in_use";
}

std::string DataSourceManager::generateId() const {
    static std::atomic<int> counter{1};
    std::ostringstream oss;
    oss << "ds-" << std::setfill('0') << std::setw(3) << counter++;
    return oss.str();
}

std::string DataSourceManager::nowISO8601() const {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&t), "%FT%TZ");
    return oss.str();
}

} // namespace webui
