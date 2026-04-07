#ifndef WEBUI_WEB_SERVER_HPP
#define WEBUI_WEB_SERVER_HPP

#include "ApiTypes.hpp"
#include <string>
#include <memory>
#include <thread>
#include <atomic>

namespace httplib {
class Server;
struct Request;
struct Response;
}

namespace webui {

class DataSourceManager;
class WorkerManager;
class ConsumerManager;
class PreviewService;
class ConfigStore;

struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 8080;
    std::string config_path;             // JSON 配置文件路径
    std::string static_dir;              // 前端静态文件目录
    std::string recordings_dir = "/data/recordings";
};

class WebServer {
public:
    explicit WebServer(const ServerConfig& cfg);
    ~WebServer();

    bool start();
    void stop();
    void wait();

    bool isRunning() const { return running_.load(); }

private:
    void registerRoutes();
    void registerDataSourceRoutes();
    void registerWorkerRoutes();
    void registerConsumerRoutes();
    void registerPreviewRoutes();
    void registerFileSystemRoutes();
    void registerConfigRoutes();
    void registerRecordingRoutes();

    // 工具方法
    void jsonResponse(httplib::Response& res, const ApiResponse& api_res) const;
    void jsonResponse(httplib::Response& res, int http_status, const ApiResponse& api_res) const;

    ServerConfig config_;
    std::atomic<bool> running_{false};
    std::unique_ptr<httplib::Server> server_;
    std::unique_ptr<ConfigStore> config_store_;
    std::unique_ptr<DataSourceManager> ds_manager_;
    std::unique_ptr<WorkerManager> worker_manager_;
    std::unique_ptr<ConsumerManager> consumer_manager_;
    std::unique_ptr<PreviewService> preview_service_;
};

} // namespace webui

#endif // WEBUI_WEB_SERVER_HPP
