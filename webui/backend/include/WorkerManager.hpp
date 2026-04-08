#ifndef WEBUI_WORKER_MANAGER_HPP
#define WEBUI_WORKER_MANAGER_HPP

#include "ApiTypes.hpp"
#include "ComponentsBridge.hpp"
#include <mutex>
#include <unordered_map>
#include <memory>
#include <atomic>

namespace webui {

class DataSourceManager;
class ConsumerManager;
class ConfigStore;
class PreviewService;

struct WorkerRuntime {
    WorkerInfo info;
    std::atomic<WorkerState> state{WorkerState::CREATED};

    std::unique_ptr<ComponentsWorkerInstance> instance;

    ~WorkerRuntime() {
        if (instance) instance->stop();
    }
};

class WorkerManager {
public:
    WorkerManager(DataSourceManager& ds_mgr, ConfigStore& store);
    ~WorkerManager();

    void setConsumerManager(ConsumerManager* cm);
    void setPreviewService(PreviewService* ps);

    // Worker CRUD
    ApiResponse list() const;
    ApiResponse create(const json& body);
    ApiResponse update(const std::string& id, const json& body);
    ApiResponse remove(const std::string& id);
    ApiResponse start(const std::string& id);
    ApiResponse stop(const std::string& id);
    ApiResponse status(const std::string& id) const;

    // Consumer 管理（操作 WorkerInfo.consumers_config）
    ApiResponse addConsumer(const std::string& worker_id, const json& body);
    ApiResponse removeConsumer(const std::string& worker_id, const std::string& consumer_id);
    ApiResponse updateConsumer(const std::string& worker_id, const std::string& consumer_id, const json& body);

    bool exists(const std::string& id) const;
    WorkerState getState(const std::string& id) const;

    void stopAll();

    // 持久化
    void saveToStore();
    void loadFromStore();

private:
    std::string generateId();
    std::string generateConsumerId();
    std::string nowISO8601() const;
    void syncConsumersToManager(const std::string& worker_id,
                                const std::vector<ConsumerInfo>& consumers);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<WorkerRuntime>> workers_;
    DataSourceManager& ds_manager_;
    ConsumerManager* consumer_manager_ = nullptr;
    PreviewService* preview_service_ = nullptr;
    ConfigStore& config_store_;
    std::atomic<int> id_counter_{1};
    std::atomic<int> consumer_id_counter_{1};
};

} // namespace webui

#endif // WEBUI_WORKER_MANAGER_HPP
