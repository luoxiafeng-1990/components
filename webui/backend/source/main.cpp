#include "../include/WebServer.hpp"
#include "common/Logger.hpp"

#include <iostream>
#include <csignal>
#include <string>
#include <memory>

static std::unique_ptr<webui::WebServer> g_server;

static void signalHandler(int sig) {
    std::cout << "\n[WebUI] Received signal " << sig << ", shutting down..." << std::endl;
    if (g_server) {
        // 只停 HTTP 服务器，让 listen() 返回；
        // Worker 清理在 listen() 返回后由析构函数完成，
        // 不在信号处理函数中调用含 mutex 的操作（避免死锁）
        g_server->stopHttpOnly();
    }
}

static void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --port <port>         HTTP port (default: 8080)\n"
              << "  --host <host>         Bind address (default: 0.0.0.0)\n"
              << "  --config <path>       Config file path\n"
              << "  --static <dir>        Frontend static files directory\n"
              << "  --recordings <dir>    Recordings output directory\n"
              << "  -h, --help            Show this help\n";
}

int main(int argc, char* argv[]) {
    LoggerGuard logger_guard;

    webui::ServerConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--port") && i + 1 < argc) {
            cfg.port = std::stoi(argv[++i]);
        } else if ((arg == "--host") && i + 1 < argc) {
            cfg.host = argv[++i];
        } else if ((arg == "--config") && i + 1 < argc) {
            cfg.config_path = argv[++i];
        } else if ((arg == "--static") && i + 1 < argc) {
            cfg.static_dir = argv[++i];
        } else if ((arg == "--recordings") && i + 1 < argc) {
            cfg.recordings_dir = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    g_server = std::make_unique<webui::WebServer>(cfg);

    std::cout << "[WebUI] Components WebUI Server" << std::endl;
    std::cout << "[WebUI] Listening on http://" << cfg.host << ":" << cfg.port << std::endl;

    if (!g_server->start()) {
        std::cerr << "[WebUI] Server failed to start" << std::endl;
        return 1;
    }

    // listen() 返回后（被 signal handler 的 stopHttpOnly() 中断），
    // 在主线程安全地停止所有 Worker 和清理资源
    std::cout << "[WebUI] Shutting down..." << std::endl;
    g_server->stop();    // 停 preview 流 + HTTP + workers
    std::cout << "[WebUI] All workers stopped." << std::endl;
    g_server.reset();

    std::cout << "[WebUI] Server stopped." << std::endl;
    return 0;
}
