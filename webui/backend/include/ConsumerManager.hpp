#ifndef WEBUI_CONSUMER_MANAGER_HPP
#define WEBUI_CONSUMER_MANAGER_HPP

#include "ApiTypes.hpp"
#include <mutex>
#include <unordered_map>
#include <vector>

namespace webui {

class WorkerManager;

class ConsumerManager {
public:
    ConsumerManager();
    ~ConsumerManager() = default;

    ApiResponse listConsumers(const std::string& worker_id) const;
    ApiResponse addConsumer(const std::string& worker_id, const json& body);
    ApiResponse removeConsumer(const std::string& worker_id, const std::string& consumer_id);
    ApiResponse updateConsumer(const std::string& worker_id, const std::string& consumer_id, const json& body);

    std::vector<ConsumerInfo> getConsumersForWorker(const std::string& worker_id) const;
    std::vector<std::string> getConsumerTypeNames(const std::string& worker_id) const;
    bool hasJpegPreview(const std::string& worker_id) const;

private:
    std::string generateId() const;
    bool validateConfig(ConsumerType type, const json& config, std::string& error) const;

    mutable std::mutex mutex_;
    // worker_id -> [consumers]
    std::unordered_map<std::string, std::vector<ConsumerInfo>> consumers_;
};

} // namespace webui

#endif // WEBUI_CONSUMER_MANAGER_HPP
