#include "../include/ConsumerManager.hpp"
#include <atomic>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace webui {

ConsumerManager::ConsumerManager() = default;

ApiResponse ConsumerManager::listConsumers(const std::string& worker_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = consumers_.find(worker_id);
    if (it == consumers_.end()) {
        return ApiResponse::ok(json::array());
    }
    json arr = json::array();
    for (auto& c : it->second) arr.push_back(c);
    return ApiResponse::ok(arr);
}

ApiResponse ConsumerManager::addConsumer(const std::string& worker_id, const json& body) {
    if (!body.contains("type")) {
        return ApiResponse::error(ErrorCode::PARAM_ERROR, "必须提供 type 字段");
    }

    ConsumerInfo ci;
    ci.id = generateId();
    ci.type = body["type"].get<ConsumerType>();
    ci.state = "inactive";
    ci.config = body.value("config", json::object());

    std::string err;
    if (!validateConfig(ci.type, ci.config, err)) {
        return ApiResponse::error(ErrorCode::CONSUMER_CONFIG_INVALID, err);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        consumers_[worker_id].push_back(ci);
    }

    return ApiResponse::ok(json(ci), "消费者添加成功");
}

ApiResponse ConsumerManager::removeConsumer(const std::string& worker_id,
                                            const std::string& consumer_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = consumers_.find(worker_id);
    if (it == consumers_.end()) {
        return ApiResponse::error(ErrorCode::NOT_FOUND, "Worker 无消费者");
    }

    auto& vec = it->second;
    auto cit = std::find_if(vec.begin(), vec.end(),
        [&](const ConsumerInfo& c) { return c.id == consumer_id; });

    if (cit == vec.end()) {
        return ApiResponse::error(ErrorCode::NOT_FOUND,
            "消费者不存在: " + consumer_id);
    }

    vec.erase(cit);
    return ApiResponse::ok(nullptr, "消费者已移除");
}

ApiResponse ConsumerManager::updateConsumer(const std::string& worker_id,
                                            const std::string& consumer_id,
                                            const json& body)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = consumers_.find(worker_id);
    if (it == consumers_.end()) {
        return ApiResponse::error(ErrorCode::NOT_FOUND, "Worker 无消费者");
    }

    auto& vec = it->second;
    auto cit = std::find_if(vec.begin(), vec.end(),
        [&](const ConsumerInfo& c) { return c.id == consumer_id; });

    if (cit == vec.end()) {
        return ApiResponse::error(ErrorCode::NOT_FOUND,
            "消费者不存在: " + consumer_id);
    }

    if (body.contains("config")) {
        cit->config = body["config"];
    }

    return ApiResponse::ok(json(*cit), "消费者配置已更新");
}

std::vector<ConsumerInfo> ConsumerManager::getConsumersForWorker(const std::string& worker_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = consumers_.find(worker_id);
    if (it == consumers_.end()) return {};
    return it->second;
}

std::vector<std::string> ConsumerManager::getConsumerTypeNames(const std::string& worker_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    auto it = consumers_.find(worker_id);
    if (it != consumers_.end()) {
        for (auto& c : it->second) {
            json j = c.type;
            names.push_back(j.get<std::string>());
        }
    }
    return names;
}

bool ConsumerManager::hasJpegPreview(const std::string& worker_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = consumers_.find(worker_id);
    if (it == consumers_.end()) return false;
    for (auto& c : it->second) {
        if (c.type == ConsumerType::JPEG_PREVIEW) return true;
    }
    return false;
}

std::string ConsumerManager::generateId() const {
    static std::atomic<int> counter{1};
    std::ostringstream oss;
    oss << "cs-" << std::setfill('0') << std::setw(3) << counter++;
    return oss.str();
}

bool ConsumerManager::validateConfig(ConsumerType type, const json& config,
                                     std::string& error) const
{
    switch (type) {
    case ConsumerType::DISPLAY:
        break;
    case ConsumerType::SAVE_RAW:
        if (!config.contains("output_paths") || !config["output_paths"].is_array()) {
            error = "SAVE_RAW 需要 output_paths 数组";
            return false;
        }
        break;
    case ConsumerType::SAVE_ENCODED:
        if (!config.contains("output_path")) {
            error = "SAVE_ENCODED 需要 output_path";
            return false;
        }
        break;
    case ConsumerType::NPU_INFERENCE:
        if (!config.contains("model_path")) {
            error = "NPU_INFERENCE 需要 model_path";
            return false;
        }
        break;
    case ConsumerType::JPEG_PREVIEW:
        // encoder_name 可选, 默认 jpeg_taco
        break;
    case ConsumerType::COMPARE:
    case ConsumerType::OPENCV:
    case ConsumerType::COUNT:
        break;
    }
    return true;
}

} // namespace webui
