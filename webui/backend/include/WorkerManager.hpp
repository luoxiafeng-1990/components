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

    ApiResponse list() const;
    ApiResponse create(const json& body);
    ApiResponse remove(const std::string& id);
    ApiResponse start(const std::string& id);
    ApiResponse stop(const std::string& id);
    ApiResponse status(const std::string& id) const;

    bool exists(const std::string& id) const;
    WorkerState getState(const std::string& id) const;

    void stopAll();

private:
    std::string generateId() const;
    std::string nowISO8601() const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<WorkerRuntime>> workers_;
    DataSourceManager& ds_manager_;
    ConsumerManager* consumer_manager_ = nullptr;
    PreviewService* preview_service_ = nullptr;
    ConfigStore& config_store_;
};

} // namespace webui

#endif // WEBUI_WORKER_MANAGER_HPP
