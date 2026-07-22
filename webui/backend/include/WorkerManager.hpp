#ifndef WEBUI_WORKER_MANAGER_HPP
#define WEBUI_WORKER_MANAGER_HPP

#include "ApiTypes.hpp"
#include <mutex>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>

namespace consumer { class BufferConsumerService; }

namespace webui {

class DataSourceManager;
class ConsumerManager;
class ConfigStore;
class PreviewService;
class PreviewSessionManager;

struct WorkerRuntime {
    WorkerInfo info;
    std::atomic<WorkerState> state{WorkerState::CREATED};
};

class WorkerManager {
public:
    WorkerManager(DataSourceManager& ds_mgr, ConfigStore& store);
    ~WorkerManager();

    void setConsumerManager(ConsumerManager* cm);
    void setPreviewService(PreviewService* ps);
    void setPreviewSessionManager(PreviewSessionManager* mgr);

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

    /**
     * @brief 重建 PARALLEL 服务
     *
     * 收集所有 state==RUNNING 的 worker 的 config，
     * 停止旧服务，用新 config 集合启动 BufferConsumerService(PARALLEL)。
     * 如果没有 RUNNING 的 worker，则只停止旧服务。
     */
    void restartParallelService();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<WorkerRuntime>> workers_;
    DataSourceManager& ds_manager_;
    ConsumerManager* consumer_manager_ = nullptr;
    PreviewService* preview_service_ = nullptr;
    PreviewSessionManager* preview_session_manager_ = nullptr;
    ConfigStore& config_store_;
    std::atomic<int> id_counter_{1};
    std::atomic<int> consumer_id_counter_{1};

    // PARALLEL 服务（全局唯一）
    std::unique_ptr<consumer::BufferConsumerService> parallel_service_;
    std::thread service_thread_;
    std::chrono::steady_clock::time_point service_start_time_;
    std::atomic<bool> intentional_restart_{false};

    // 延迟重启机制（debounce：批量 start 时只触发一次 restart）
    std::atomic<int> restart_seq_{0};
    void scheduleRestart();

    // 防止 restartParallelService 被多个 detached 线程并发调用
    std::mutex restart_mutex_;

    // 服务失败自动重试计数（最多重试一次，避免无限循环）
    std::atomic<int> retry_count_{0};

    // Ctrl+C / stop 时置 true，阻止 detached 线程再拉起服务
    std::atomic<bool> shutting_down_{false};
};

} // namespace webui

#endif // WEBUI_WORKER_MANAGER_HPP
