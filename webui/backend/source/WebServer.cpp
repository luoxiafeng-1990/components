#include "../include/WebServer.hpp"
#include "../include/ConfigStore.hpp"
#include "../include/DataSourceManager.hpp"
#include "../include/WorkerManager.hpp"
#include "../include/ConsumerManager.hpp"
#include "../include/PreviewService.hpp"

#define CPPHTTPLIB_THREAD_POOL_COUNT 8
#include "../third_party/httplib.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace webui {

// ============================================================
// 构造 / 析构
// ============================================================

WebServer::WebServer(const ServerConfig& cfg)
    : config_(cfg)
{
    server_ = std::make_unique<httplib::Server>();
    config_store_ = std::make_unique<ConfigStore>(cfg.config_path);
    config_store_->load();

    ds_manager_ = std::make_unique<DataSourceManager>(*config_store_);
    worker_manager_ = std::make_unique<WorkerManager>(*ds_manager_, *config_store_);
    consumer_manager_ = std::make_unique<ConsumerManager>();
    preview_service_ = std::make_unique<PreviewService>(*worker_manager_, *consumer_manager_);

    worker_manager_->setConsumerManager(consumer_manager_.get());
    worker_manager_->setPreviewService(preview_service_.get());
}

WebServer::~WebServer() {
    stop();
}

// ============================================================
// 启动 / 停止
// ============================================================

bool WebServer::start() {
    registerRoutes();

    if (!config_.static_dir.empty() && std::filesystem::exists(config_.static_dir)) {
        server_->set_mount_point("/", config_.static_dir);
    }

    server_->set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) -> httplib::Server::HandlerResponse {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        if (req.method == "OPTIONS") {
            res.status = 204;
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    running_ = true;
    std::cout << "[WebUI] Starting server on " << config_.host
              << ":" << config_.port << std::endl;

    if (!server_->listen(config_.host, config_.port)) {
        running_ = false;
        std::cerr << "[WebUI] Failed to start server" << std::endl;
        return false;
    }

    return true;
}

void WebServer::stop() {
    if (running_) {
        running_ = false;
        worker_manager_->stopAll();
        server_->stop();
    }
}

void WebServer::wait() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ============================================================
// 路由注册
// ============================================================

void WebServer::registerRoutes() {
    registerDataSourceRoutes();
    registerWorkerRoutes();
    registerConsumerRoutes();
    registerPreviewRoutes();
    registerFileSystemRoutes();
    registerConfigRoutes();
    registerRecordingRoutes();
}

// ============================================================
// 数据源路由
// ============================================================

void WebServer::registerDataSourceRoutes() {
    server_->Get("/api/datasources", [this](const httplib::Request&, httplib::Response& res) {
        jsonResponse(res, ds_manager_->list());
    });

    server_->Post("/api/datasources", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            jsonResponse(res, ds_manager_->add(body));
        } catch (const json::parse_error&) {
            jsonResponse(res, 400, ApiResponse::error(ErrorCode::PARAM_ERROR, "JSON 解析失败"));
        }
    });

    server_->Put(R"(/api/datasources/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                jsonResponse(res, ds_manager_->update(req.matches[1], body));
            } catch (const json::parse_error&) {
                jsonResponse(res, 400, ApiResponse::error(ErrorCode::PARAM_ERROR, "JSON 解析失败"));
            }
        });

    server_->Delete(R"(/api/datasources/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            jsonResponse(res, ds_manager_->remove(req.matches[1]));
        });

    server_->Get(R"(/api/datasources/([^/]+)/probe)",
        [this](const httplib::Request& req, httplib::Response& res) {
            jsonResponse(res, ds_manager_->probe(req.matches[1]));
        });

    server_->Get(R"(/api/datasources/([^/]+)/preview)",
        [this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            auto ds = ds_manager_->get(id);
            if (!ds.has_value()) {
                jsonResponse(res, 404, ApiResponse::error(ErrorCode::NOT_FOUND, "数据源不存在"));
                return;
            }

            if (ds->type == DataSourceType::RTSP) {
                std::string m3u = "#EXTM3U\n#EXTINF:-1," + ds->name + "\n" + ds->path + "\n";
                res.set_content(m3u, "audio/x-mpegurl");
                res.set_header("Content-Disposition",
                    "attachment; filename=\"" + ds->name + ".m3u\"");
            } else if (ds->type == DataSourceType::FILE) {
                if (!std::filesystem::exists(ds->path)) {
                    jsonResponse(res, 404, ApiResponse::error(ErrorCode::DATASOURCE_UNAVAILABLE, "文件不存在"));
                    return;
                }
                std::ifstream ifs(ds->path, std::ios::binary);

                std::string ext = std::filesystem::path(ds->path).extension().string();
                std::string mime = "application/octet-stream";
                if (ext == ".mp4") mime = "video/mp4";
                else if (ext == ".mkv") mime = "video/x-matroska";
                else if (ext == ".avi") mime = "video/x-msvideo";
                else if (ext == ".ts") mime = "video/mp2t";

                std::string content((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
                res.set_content(content, mime);
            } else {
                jsonResponse(res, 400, ApiResponse::error(ErrorCode::PARAM_ERROR,
                    "BUFFER 类型不支持预览"));
            }
        });
}

// ============================================================
// Worker 路由
// ============================================================

void WebServer::registerWorkerRoutes() {
    server_->Get("/api/workers", [this](const httplib::Request&, httplib::Response& res) {
        jsonResponse(res, worker_manager_->list());
    });

    server_->Post("/api/workers", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            jsonResponse(res, worker_manager_->create(body));
        } catch (const json::parse_error&) {
            jsonResponse(res, 400, ApiResponse::error(ErrorCode::PARAM_ERROR, "JSON 解析失败"));
        }
    });

    server_->Delete(R"(/api/workers/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            jsonResponse(res, worker_manager_->remove(req.matches[1]));
        });

    server_->Post(R"(/api/workers/([^/]+)/start)",
        [this](const httplib::Request& req, httplib::Response& res) {
            jsonResponse(res, worker_manager_->start(req.matches[1]));
        });

    server_->Post(R"(/api/workers/([^/]+)/stop)",
        [this](const httplib::Request& req, httplib::Response& res) {
            jsonResponse(res, worker_manager_->stop(req.matches[1]));
        });

    server_->Get(R"(/api/workers/([^/]+)/status)",
        [this](const httplib::Request& req, httplib::Response& res) {
            jsonResponse(res, worker_manager_->status(req.matches[1]));
        });
}

// ============================================================
// 消费者路由
// ============================================================

void WebServer::registerConsumerRoutes() {
    server_->Get(R"(/api/workers/([^/]+)/consumers)",
        [this](const httplib::Request& req, httplib::Response& res) {
            jsonResponse(res, consumer_manager_->listConsumers(req.matches[1]));
        });

    server_->Post(R"(/api/workers/([^/]+)/consumers)",
        [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                jsonResponse(res, consumer_manager_->addConsumer(req.matches[1], body));
            } catch (const json::parse_error&) {
                jsonResponse(res, 400, ApiResponse::error(ErrorCode::PARAM_ERROR, "JSON 解析失败"));
            }
        });

    server_->Delete(R"(/api/workers/([^/]+)/consumers/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            jsonResponse(res, consumer_manager_->removeConsumer(req.matches[1], req.matches[2]));
        });

    server_->Put(R"(/api/workers/([^/]+)/consumers/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                jsonResponse(res, consumer_manager_->updateConsumer(
                    req.matches[1], req.matches[2], body));
            } catch (const json::parse_error&) {
                jsonResponse(res, 400, ApiResponse::error(ErrorCode::PARAM_ERROR, "JSON 解析失败"));
            }
        });
}

// ============================================================
// 预览路由
// ============================================================

void WebServer::registerPreviewRoutes() {
    server_->Get(R"(/api/preview/stream/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            std::string worker_id = req.matches[1];

            if (!worker_manager_->exists(worker_id)) {
                jsonResponse(res, 404, ApiResponse::error(ErrorCode::NOT_FOUND, "Worker 不存在"));
                return;
            }

            if (!consumer_manager_->hasJpegPreview(worker_id)) {
                jsonResponse(res, 400, ApiResponse::error(ErrorCode::PREVIEW_UNAVAILABLE,
                    "Worker 未添加 JPEG_PREVIEW 消费者"));
                return;
            }

            res.set_header("Cache-Control", "no-cache");
            res.set_content_provider(
                "multipart/x-mixed-replace; boundary=frame",
                [this, worker_id](size_t /*offset*/, httplib::DataSink& sink) {
                    preview_service_->streamMjpeg(worker_id,
                        [&sink](const uint8_t* data, size_t len) -> bool {
                            std::string header = "--frame\r\n"
                                "Content-Type: image/jpeg\r\n"
                                "Content-Length: " + std::to_string(len) + "\r\n\r\n";
                            if (!sink.write(header.data(), header.size())) return false;
                            if (!sink.write(reinterpret_cast<const char*>(data), len)) return false;
                            std::string footer = "\r\n";
                            return sink.write(footer.data(), footer.size());
                        });
                    return true;
                },
                [](bool) {}
            );
        });

    server_->Get(R"(/api/preview/snapshot/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            std::string worker_id = req.matches[1];
            int quality = 80;
            if (req.has_param("quality")) {
                quality = std::stoi(req.get_param_value("quality"));
            }

            auto frame = preview_service_->snapshot(worker_id, quality);
            if (frame.empty()) {
                jsonResponse(res, 404, ApiResponse::error(ErrorCode::PREVIEW_UNAVAILABLE,
                    "无可用帧"));
                return;
            }

            res.set_content(std::string(frame.begin(), frame.end()), "image/jpeg");
        });

    server_->Get("/api/preview/grid", [this](const httplib::Request& req, httplib::Response& res) {
        std::string layout = req.has_param("layout") ? req.get_param_value("layout") : "3x3";
        jsonResponse(res, preview_service_->gridInfo(layout));
    });
}

// ============================================================
// 文件系统路由
// ============================================================

void WebServer::registerFileSystemRoutes() {
    server_->Get("/api/filesystem/browse",
        [](const httplib::Request& req, httplib::Response& res) {
            std::string path = req.has_param("path") ? req.get_param_value("path") : "/";
            std::string filter = req.has_param("filter") ? req.get_param_value("filter") : "all";

            if (!std::filesystem::exists(path) || !std::filesystem::is_directory(path)) {
                auto r = ApiResponse::error(ErrorCode::NOT_FOUND, "目录不存在: " + path);
                res.set_content(r.toJson().dump(), "application/json");
                return;
            }

            static const std::set<std::string> video_exts = {
                ".mp4", ".mkv", ".avi", ".h264", ".h265", ".264", ".265",
                ".ts", ".flv", ".mov", ".wmv", ".webm"
            };

            json entries = json::array();
            for (auto& entry : std::filesystem::directory_iterator(path)) {
                std::string name = entry.path().filename().string();
                if (name.front() == '.') continue; // skip hidden

                if (entry.is_directory()) {
                    auto now = std::chrono::system_clock::now();
                    auto t = std::chrono::system_clock::to_time_t(now);
                    std::ostringstream oss;
                    oss << std::put_time(std::gmtime(&t), "%FT%TZ");

                    entries.push_back({
                        {"name", name},
                        {"path", entry.path().string() + "/"},
                        {"type", "directory"},
                        {"size_bytes", 0},
                        {"modified_at", oss.str()},
                        {"extension", ""}
                    });
                } else if (entry.is_regular_file()) {
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                    if (filter == "video" && video_exts.find(ext) == video_exts.end()) {
                        continue;
                    }

                    auto ftime = std::filesystem::last_write_time(entry);
                    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ftime - std::filesystem::file_time_type::clock::now()
                        + std::chrono::system_clock::now());
                    auto t = std::chrono::system_clock::to_time_t(sctp);
                    std::ostringstream oss;
                    oss << std::put_time(std::gmtime(&t), "%FT%TZ");

                    entries.push_back({
                        {"name", name},
                        {"path", entry.path().string()},
                        {"type", "file"},
                        {"size_bytes", entry.file_size()},
                        {"modified_at", oss.str()},
                        {"extension", ext}
                    });
                }
            }

            std::string parent = std::filesystem::path(path).parent_path().string();
            if (parent.empty()) parent = "/";

            auto r = ApiResponse::ok({
                {"current_path", path},
                {"parent_path", parent},
                {"entries", entries}
            });
            res.set_content(r.toJson().dump(), "application/json");
        });
}

// ============================================================
// 配置路由
// ============================================================

void WebServer::registerConfigRoutes() {
    server_->Get("/api/config/export", [this](const httplib::Request&, httplib::Response& res) {
        jsonResponse(res, config_store_->exportConfig());
    });

    server_->Post("/api/config/import", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "replace";
            jsonResponse(res, config_store_->importConfig(body, mode));
        } catch (const json::parse_error&) {
            jsonResponse(res, 400, ApiResponse::error(ErrorCode::PARAM_ERROR, "JSON 解析失败"));
        }
    });
}

// ============================================================
// 录制路由（占位）
// ============================================================

void WebServer::registerRecordingRoutes() {
    server_->Get("/api/recordings", [](const httplib::Request&, httplib::Response& res) {
        // TODO: 从 RecordingManager 获取列表
        auto r = ApiResponse::ok(json::array());
        res.set_content(r.toJson().dump(), "application/json");
    });

    server_->Delete(R"(/api/recordings/([^/]+))",
        [](const httplib::Request&, httplib::Response& res) {
            auto r = ApiResponse::ok(nullptr, "录制文件已删除");
            res.set_content(r.toJson().dump(), "application/json");
        });

    server_->Get(R"(/api/recordings/([^/]+)/play)",
        [](const httplib::Request&, httplib::Response& res) {
            auto r = ApiResponse::error(ErrorCode::NOT_FOUND, "录制文件不存在");
            res.set_content(r.toJson().dump(), "application/json");
        });
}

// ============================================================
// 辅助方法
// ============================================================

void WebServer::jsonResponse(httplib::Response& res, const ApiResponse& api_res) const {
    int http_status = (api_res.code == 0) ? 200 : 400;
    if (api_res.code == ErrorCode::NOT_FOUND) http_status = 404;
    res.status = http_status;
    res.set_content(api_res.toJson().dump(), "application/json");
}

void WebServer::jsonResponse(httplib::Response& res, int http_status,
                              const ApiResponse& api_res) const {
    res.status = http_status;
    res.set_content(api_res.toJson().dump(), "application/json");
}

} // namespace webui
