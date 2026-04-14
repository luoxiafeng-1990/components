#include "../include/DataSourceManager.hpp"
#include "../include/ConfigStore.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <sys/wait.h>

namespace webui {

namespace {

std::string shellSingleQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += '\'';
    return out;
}

/// 约定：在 WebUI 所在主机上对 RTSP 视频轨连续解码若干秒（人文标准「能正常播一小段」）；TCP，出错即失败
constexpr int kRtspDecodeProbeSeconds = 10;
/// 单路探测墙钟上限（秒）：防止 RTSP 握手/读流挂起时 ffmpeg 永不退出，拖死 HTTP 请求
constexpr int kRtspProbeWallSeconds = 95;

bool ffmpegRtspDecodeProbe(const std::string& url) {
    std::ostringstream oss;
    // timeout：GNU coreutils，超时返回 124，避免 RTSP 握手/读流挂起时 ffmpeg 永不退出、拖死 HTTP
    oss << "timeout -k 10 " << kRtspProbeWallSeconds
        << " ffmpeg -nostdin -hide_banner -loglevel error "
        << "-rtsp_transport tcp -stimeout 8000000 "
        << "-i " << shellSingleQuote(url) << " "
        << "-t " << kRtspDecodeProbeSeconds << " "
        << "-map 0:v:0 -an -sn -xerror "
        << "-f null - "
        << ">/dev/null 2>&1";
    int st = std::system(oss.str().c_str());
    if (st == -1) {
        return false;
    }
    if (!WIFEXITED(st)) {
        return false;
    }
    return WEXITSTATUS(st) == 0;
}

} // namespace

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
    if (body.contains("rtsp_urls") && body["rtsp_urls"].is_array()) {
        ds.rtsp_urls.clear();
        for (const auto& item : body["rtsp_urls"]) {
            if (item.is_string()) {
                ds.rtsp_urls.push_back(item.get<std::string>());
            }
        }
    }
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
    if (body.contains("rtsp_urls") && body["rtsp_urls"].is_array()) {
        ds.rtsp_urls.clear();
        for (const auto& item : body["rtsp_urls"]) {
            if (item.is_string()) {
                ds.rtsp_urls.push_back(item.get<std::string>());
            }
        }
    }

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

ApiResponse DataSourceManager::probeRtspUrls(const json& body) const {
    if (!body.contains("urls") || !body["urls"].is_array()) {
        return ApiResponse::error(ErrorCode::PARAM_ERROR, "必须提供 urls 数组");
    }
    json results = json::array();
    for (const auto& u : body["urls"]) {
        if (!u.is_string()) {
            continue;
        }
        std::string url = u.get<std::string>();
        // 顺序探测，避免多路同时连接占用设备连接数
        bool playable = ffmpegRtspDecodeProbe(url);
        results.push_back({{"url", url}, {"playable", playable}});
    }
    return ApiResponse::ok(results);
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
