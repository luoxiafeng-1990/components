#include "../include/WebServer.hpp"
#include "../include/ConfigStore.hpp"
#include "../include/DataSourceManager.hpp"
#include "../include/WorkerManager.hpp"
#include "../include/ConsumerManager.hpp"
#include "../include/PreviewService.hpp"

#define CPPHTTPLIB_THREAD_POOL_COUNT 32
#include "../third_party/httplib.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <array>
#include <set>
#include <unordered_map>
#include <cmath>

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

    // 从 ConfigStore 恢复上次保存的 Worker
    worker_manager_->loadFromStore();

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
    running_ = false;
    // 1. 先通知 PreviewService 停止所有流（解除 streamMjpeg 死循环）
    if (preview_service_) {
        preview_service_->requestStop();
    }
    // 2. 停止 HTTP 服务器（关闭连接，让 handler 线程退出）
    if (server_) {
        server_->stop();
    }
    // 3. 停止所有 Worker 线程
    if (worker_manager_) {
        worker_manager_->stopAll();
    }
}

void WebServer::stopHttpOnly() {
    running_ = false;
    if (preview_service_) {
        preview_service_->requestStop();
    }
    if (server_) {
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
    registerSystemRoutes();
    registerDataSourceRoutes();
    registerWorkerRoutes();
    registerConsumerRoutes();
    registerPreviewRoutes();
    registerFileSystemRoutes();
    registerConfigRoutes();
    registerRecordingRoutes();
}

// ============================================================
// 系统信息路由
// ============================================================

namespace {

std::string execCommand(const char* cmd) {
    std::array<char, 4096> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    // trim trailing whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
        result.pop_back();
    return result;
}

json parseCpuUsage() {
    // /proc/stat: cpu user nice system idle iowait irq softirq steal guest guest_nice
    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) return {{"usage_percent", 0}};

    std::string line;
    std::getline(stat, line);
    // cpu  user nice sys idle iowait irq softirq ...
    std::istringstream iss(line);
    std::string cpu_label;
    long long user, nice, sys, idle, iowait, irq, softirq, steal;
    iss >> cpu_label >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal;
    long long total = user + nice + sys + idle + iowait + irq + softirq + steal;
    long long busy = total - idle - iowait;

    // 为了计算瞬时 CPU 使用率，需要两次采样
    static long long prev_total = 0, prev_busy = 0;
    double usage = 0.0;
    if (prev_total > 0) {
        long long dt = total - prev_total;
        long long db = busy - prev_busy;
        if (dt > 0) usage = 100.0 * db / dt;
    }
    prev_total = total;
    prev_busy = busy;

    // 每核使用率
    json per_core = json::array();
    while (std::getline(stat, line)) {
        if (line.substr(0, 3) != "cpu") break;
        std::istringstream iss2(line);
        std::string core_label;
        long long cu, cn, cs, ci, cw, cr, cso, cst;
        iss2 >> core_label >> cu >> cn >> cs >> ci >> cw >> cr >> cso >> cst;
        long long ct = cu + cn + cs + ci + cw + cr + cso + cst;
        long long cb = ct - ci - cw;
        (void)cb; (void)ct;
        per_core.push_back(core_label);
    }

    return {
        {"usage_percent", std::round(usage * 100) / 100},
        {"cores", per_core.size()}
    };
}

json parseMemoryUsage() {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) return {};

    long long total = 0, available = 0, buffers = 0, cached = 0, free = 0;
    std::string line;
    while (std::getline(meminfo, line)) {
        std::istringstream iss(line);
        std::string key;
        long long val;
        iss >> key >> val;
        if (key == "MemTotal:") total = val;
        else if (key == "MemFree:") free = val;
        else if (key == "MemAvailable:") available = val;
        else if (key == "Buffers:") buffers = val;
        else if (key == "Cached:") cached = val;
    }

    long long used = total - available;
    double usage = (total > 0) ? 100.0 * used / total : 0;

    return {
        {"total_mb", total / 1024},
        {"used_mb", used / 1024},
        {"free_mb", free / 1024},
        {"available_mb", available / 1024},
        {"buffers_mb", buffers / 1024},
        {"cached_mb", cached / 1024},
        {"usage_percent", std::round(usage * 100) / 100}
    };
}

json parseNetworkInfo() {
    json interfaces = json::array();
    std::ifstream net("/proc/net/dev");
    if (!net.is_open()) return interfaces;

    static std::unordered_map<std::string, std::pair<long long, long long>> prev_bytes;
    static auto prev_time = std::chrono::steady_clock::now();

    std::string line;
    std::getline(net, line); // header 1
    std::getline(net, line); // header 2

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - prev_time).count();

    while (std::getline(net, line)) {
        std::istringstream iss(line);
        std::string iface;
        iss >> iface;
        if (iface.back() == ':') iface.pop_back();
        if (iface == "lo") continue;

        long long rx_bytes, rx_packets, rx_errs, rx_drop;
        long long tx_bytes, tx_packets, tx_errs, tx_drop;
        long long dummy;
        iss >> rx_bytes >> rx_packets >> rx_errs >> rx_drop
            >> dummy >> dummy >> dummy >> dummy
            >> tx_bytes >> tx_packets >> tx_errs >> tx_drop;

        double rx_rate = 0, tx_rate = 0;
        if (prev_bytes.count(iface) && elapsed > 0.1) {
            auto& [prx, ptx] = prev_bytes[iface];
            rx_rate = (rx_bytes - prx) / elapsed / 1024.0; // KB/s
            tx_rate = (tx_bytes - ptx) / elapsed / 1024.0;
        }
        prev_bytes[iface] = {rx_bytes, tx_bytes};

        // IP address
        std::string ip = execCommand(
            ("ip -4 addr show " + iface + " 2>/dev/null | grep -oP '(?<=inet )\\S+'").c_str());

        interfaces.push_back({
            {"name", iface},
            {"ip", ip},
            {"rx_bytes", rx_bytes},
            {"tx_bytes", tx_bytes},
            {"rx_rate_kbps", std::round(rx_rate * 100) / 100},
            {"tx_rate_kbps", std::round(tx_rate * 100) / 100},
            {"rx_packets", rx_packets},
            {"tx_packets", tx_packets},
            {"rx_errors", rx_errs},
            {"tx_errors", tx_errs},
        });
    }

    // 更新时间戳（所有接口共享同一个 prev_time）
    prev_time = std::chrono::steady_clock::now();

    return interfaces;
}

json parseNpuUsage() {
    // tps-smi 输出包含 NPU 使用率
    std::string raw = execCommand("tps-smi 2>/dev/null");
    json result = {
        {"available", !raw.empty()},
        {"raw_output", raw},
        {"usage_percent", 0.0}
    };

    if (!raw.empty()) {
        // 尝试解析 NPU utilization
        // tps-smi 输出格式可能包含 "NPU Utilization: XX%" 或类似
        auto pos = raw.find("Utilization");
        if (pos == std::string::npos) pos = raw.find("utilization");
        if (pos == std::string::npos) pos = raw.find("Usage");
        if (pos == std::string::npos) pos = raw.find("usage");

        if (pos != std::string::npos) {
            // 找到百分比数字
            for (size_t i = pos; i < raw.size(); ++i) {
                if (std::isdigit(raw[i])) {
                    try {
                        result["usage_percent"] = std::stod(raw.substr(i));
                    } catch (...) {}
                    break;
                }
            }
        }
    }

    return result;
}

json parseCodecPerformance() {
    // 从 tps-smi 或其他方式获取编解码性能信息
    std::string codec_info = execCommand("tps-smi --codec 2>/dev/null");
    json result = {
        {"decode", json::object()},
        {"encode", json::object()},
        {"raw_output", codec_info}
    };

    // 也尝试读取 /sys 下的硬件计数器
    std::string dec_fps = execCommand("cat /sys/class/vpu/*/decode_fps 2>/dev/null");
    std::string enc_fps = execCommand("cat /sys/class/vpu/*/encode_fps 2>/dev/null");

    if (!dec_fps.empty()) {
        try { result["decode"]["fps"] = std::stod(dec_fps); } catch (...) {}
    }
    if (!enc_fps.empty()) {
        try { result["encode"]["fps"] = std::stod(enc_fps); } catch (...) {}
    }

    return result;
}

} // anonymous namespace

void WebServer::registerSystemRoutes() {
    // 静态系统信息（不频繁变化）
    server_->Get("/api/system/info", [](const httplib::Request&, httplib::Response& res) {
        std::string version = execCommand("tps-version 2>/dev/null");
        std::string kernel = execCommand("uname -r 2>/dev/null");
        std::string arch = execCommand("uname -m 2>/dev/null");
        std::string hostname = execCommand("hostname 2>/dev/null");
        std::string uptime_str = execCommand("cat /proc/uptime 2>/dev/null");
        std::string board = execCommand("cat /sys/firmware/devicetree/base/model 2>/dev/null");

        double uptime_sec = 0;
        if (!uptime_str.empty()) {
            try { uptime_sec = std::stod(uptime_str); } catch (...) {}
        }

        // CPU info
        std::string cpu_model = execCommand(
            "grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs");
        if (cpu_model.empty()) {
            cpu_model = execCommand(
                "grep -m1 'isa' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs");
        }
        std::string cpu_count = execCommand("nproc 2>/dev/null");

        json data = {
            {"tps_version", version.empty() ? "未安装 tps-version" : version},
            {"kernel", kernel},
            {"arch", arch},
            {"hostname", hostname},
            {"board_model", board},
            {"uptime_seconds", uptime_sec},
            {"cpu_model", cpu_model},
            {"cpu_cores", cpu_count}
        };

        auto r = ApiResponse::ok(data);
        res.set_content(r.toJson().dump(), "application/json");
    });

    // 实时指标（前端定时轮询）
    server_->Get("/api/system/metrics", [](const httplib::Request&, httplib::Response& res) {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000;

        std::ostringstream ts;
        ts << std::put_time(std::gmtime(&t), "%FT%T") << "." << std::setfill('0') << std::setw(3) << ms << "Z";

        json data = {
            {"timestamp", ts.str()},
            {"cpu", parseCpuUsage()},
            {"memory", parseMemoryUsage()},
            {"npu", parseNpuUsage()},
            {"network", parseNetworkInfo()},
            {"codec", parseCodecPerformance()}
        };

        auto r = ApiResponse::ok(data);
        res.set_content(r.toJson().dump(), "application/json");
    });
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

    server_->Put(R"(/api/workers/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                jsonResponse(res, worker_manager_->update(req.matches[1], body));
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

    // 消费者增删改代理到 WorkerManager（持久化到 WorkerInfo.consumers_config）
    server_->Post(R"(/api/workers/([^/]+)/consumers)",
        [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                jsonResponse(res, worker_manager_->addConsumer(req.matches[1], body));
            } catch (const json::parse_error&) {
                jsonResponse(res, 400, ApiResponse::error(ErrorCode::PARAM_ERROR, "JSON 解析失败"));
            }
        });

    server_->Delete(R"(/api/workers/([^/]+)/consumers/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            jsonResponse(res, worker_manager_->removeConsumer(req.matches[1], req.matches[2]));
        });

    server_->Put(R"(/api/workers/([^/]+)/consumers/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                jsonResponse(res, worker_manager_->updateConsumer(
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
