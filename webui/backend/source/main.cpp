#include "../include/WebServer.hpp"
#include "common/Logger.hpp"

#include <iostream>
#include <csignal>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <unistd.h>

static std::unique_ptr<webui::WebServer> g_server;
static std::atomic<int> signal_count{0};

static void signalHandler(int sig) {
    int count = ++signal_count;
    if (count >= 2) {
        std::cerr << "\n[WebUI] 再次收到信号，强制退出" << std::endl;
        _exit(1);
    }
    std::cerr << "\n[WebUI] Received signal " << sig << ", shutting down..." << std::endl;

    // 10 秒后若进程仍未退出则强制终止
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::cerr << "[WebUI] 优雅退出超时(10s)，强制退出" << std::endl;
        _exit(1);
    }).detach();

    if (g_server) {
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
