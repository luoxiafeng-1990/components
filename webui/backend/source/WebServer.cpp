#include "../include/WebServer.hpp"
#include "../include/ConfigStore.hpp"
#include "../include/DataSourceManager.hpp"
#include "../include/WorkerManager.hpp"
#include "../include/ConsumerManager.hpp"
#include "../include/PreviewService.hpp"
#include "../include/PreviewSessionManager.hpp"

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

namespace {

/// M3U #EXTINF 标题中避免逗号/换行破坏解析
std::string sanitizeM3uTitle(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == '\n' || c == '\r' || c == ',') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

std::string safeAttachmentFileStem(const std::string& name) {
    std::string s = sanitizeM3uTitle(name);
    for (auto& c : s) {
        if (c == '/' || c == '\\' || c == '"' || c == '<' || c == '>' || c == ':' || c == '*' || c == '?') {
            c = '_';
        }
    }
    while (!s.empty() && s.front() == ' ') {
        s.erase(s.begin());
    }
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    return s.empty() ? std::string("stream") : s;
}

/// 与 RTSP 探测一致优先 RTP-over-RTSP(TCP)；预览 m3u 给 VLC 常用缓冲
std::string buildRtspVlcPlaylist(const std::string& name, const std::string& url) {
    const std::string title = sanitizeM3uTitle(name);
    std::ostringstream oss;
    oss << "#EXTM3U\n"
        << "#EXTINF:-1," << title << "\n"
        << "#EXTVLCOPT:network-caching=1200\n"
        // 与 ffprobe -rtsp_transport tcp 对齐；: 前缀为 VLC 模块选项写法
        << "#EXTVLCOPT: :rtsp-tcp\n"
        << url << "\n";
    return oss.str();
}

}  // namespace

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
    preview_session_manager_ = std::make_unique<PreviewSessionManager>();

    // Real jpeg_taco factory (null → ENCODER_RESOURCE_EXHAUSTED on START).
    preview_session_manager_->setEncoderFactory(
        [](const std::string& worker_id, const PreviewSessionConfig& cfg) {
            return PreviewSessionManager::makeJpegTacoEncoder(worker_id, cfg);
        },
        [](std::shared_ptr<PreviewSessionManager::EncoderHandle>& h) {
            h.reset();
        });

    preview_service_->setSessionManager(preview_session_manager_.get());
    worker_manager_->setConsumerManager(consumer_manager_.get());
    worker_manager_->setPreviewService(preview_service_.get());
    worker_manager_->setPreviewSessionManager(preview_session_manager_.get());
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
        // index.html 禁止缓存，确保浏览器加载最新前端
        if (req.path == "/" || req.path == "/index.html" ||
            req.path.find(".html") != std::string::npos) {
            res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
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
    // 1b. Tear down on-demand preview encoders (join outside map lock).
    if (preview_session_manager_) {
        preview_session_manager_->shutdown();
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
    // Signal-handler safe: only unblock listen(). Full session shutdown runs
    // later on the main thread via stop() (joins encoder threads).
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
    auto deleter = [](FILE* f) { if (f) pclose(f); };
    std::unique_ptr<FILE, decltype(deleter)> pipe(popen(cmd, "r"), deleter);
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    // trim trailing whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
        result.pop_back();
    return result;
}

std::string execCommandWithTimeout(const char* cmd, int timeout_sec) {
    std::ostringstream wrapped;
    wrapped << "timeout " << timeout_sec << " bash -lc '" << cmd << "' 2>/dev/null";
    return execCommand(wrapped.str().c_str());
}

json parseCpuUsage() {
    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) return {{"usage_percent", 0}, {"cores", 0}, {"per_core", json::array()}};

    std::string line;
    std::getline(stat, line);
    std::istringstream iss(line);
    std::string cpu_label;
    long long user, nice, sys, idle, iowait, irq, softirq, steal;
    iss >> cpu_label >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal;
    long long total = user + nice + sys + idle + iowait + irq + softirq + steal;
    long long busy = total - idle - iowait;

    static long long prev_total = 0, prev_busy = 0;
    double usage = 0.0;
    if (prev_total > 0) {
        long long dt = total - prev_total;
        long long db = busy - prev_busy;
        if (dt > 0) usage = 100.0 * db / dt;
    }
    prev_total = total;
    prev_busy = busy;

    static std::vector<std::pair<long long, long long>> prev_cores;
    json per_core = json::array();
    int core_idx = 0;
    while (std::getline(stat, line)) {
        if (line.substr(0, 3) != "cpu") break;
        std::istringstream iss2(line);
        std::string core_label;
        long long cu, cn, cs, ci, cw, cr, cso, cst;
        iss2 >> core_label >> cu >> cn >> cs >> ci >> cw >> cr >> cso >> cst;
        long long ct = cu + cn + cs + ci + cw + cr + cso + cst;
        long long cb = ct - ci - cw;

        double core_usage = 0.0;
        if (core_idx < static_cast<int>(prev_cores.size())) {
            long long dct = ct - prev_cores[core_idx].first;
            long long dcb = cb - prev_cores[core_idx].second;
            if (dct > 0) core_usage = 100.0 * dcb / dct;
            prev_cores[core_idx] = {ct, cb};
        } else {
            prev_cores.push_back({ct, cb});
        }

        per_core.push_back({
            {"core", core_idx},
            {"usage_percent", std::round(core_usage * 100) / 100}
        });
        core_idx++;
    }

    return {
        {"usage_percent", std::round(usage * 100) / 100},
        {"cores", per_core.size()},
        {"per_core", per_core}
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
    // tps-smi 在部分板子上会进入交互界面，必须限制超时
    std::string raw = execCommandWithTimeout("tps-smi", 1);
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
    // 优先走 /sys，tps-smi --codec 只做补充且需要超时保护
    std::string codec_info = execCommandWithTimeout("tps-smi --codec", 1);
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

std::string readFileAll(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

std::string trimStr(const std::string& s) {
    std::string r = s;
    while (!r.empty() && (r.back() == '\n' || r.back() == '\r' || r.back() == ' ' || r.back() == '\0'))
        r.pop_back();
    while (!r.empty() && (r.front() == ' ' || r.front() == '\t'))
        r.erase(r.begin());
    return r;
}

json parseCpuInfoDetailed() {
    json processors = json::array();
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (!cpuinfo.is_open()) return {{"processors", processors}, {"count", 0}};

    json proc = json::object();
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.empty()) {
            if (!proc.empty()) {
                processors.push_back(proc);
                proc = json::object();
            }
            continue;
        }
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trimStr(line.substr(0, colon));
        std::string val = trimStr(colon + 1 < line.size() ? line.substr(colon + 1) : "");
        proc[key] = val;
    }
    if (!proc.empty()) processors.push_back(proc);

    std::string freq = execCommand("cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq 2>/dev/null");
    std::string max_freq = execCommand("cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq 2>/dev/null");
    std::string min_freq = execCommand("cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq 2>/dev/null");

    std::string dt = execCommand(
        "if [ -d /sys/firmware/devicetree/base/cpus ]; then "
        "for d in /sys/firmware/devicetree/base/cpus/cpu@*/; do "
        "echo \"--- $(basename $d) ---\"; "
        "[ -f $d/compatible ] && echo \"compatible: $(cat $d/compatible 2>/dev/null)\"; "
        "[ -f $d/status ] && echo \"status: $(cat $d/status 2>/dev/null)\"; "
        "[ -f \"$d/riscv,isa\" ] && echo \"isa: $(cat \"$d/riscv,isa\" 2>/dev/null)\"; "
        "done; fi 2>/dev/null");
    if (dt.empty()) {
        std::string dtb = execCommand("ls /boot/firmware/*.dtb 2>/dev/null | head -1");
        if (!dtb.empty())
            dt = execCommand(("dtc -I dtb -O dts " + dtb + " 2>/dev/null | grep -A10 'cpus {' | head -30").c_str());
    }

    return {
        {"processors", processors},
        {"count", processors.size()},
        {"cur_freq_khz", freq},
        {"max_freq_khz", max_freq},
        {"min_freq_khz", min_freq},
        {"device_tree", dt}
    };
}

json parseMemInfoDetailed() {
    json fields = json::array();
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) return {{"fields", fields}};

    std::string line;
    while (std::getline(meminfo, line)) {
        std::istringstream iss(line);
        std::string key;
        long long val = 0;
        std::string unit;
        iss >> key >> val >> unit;
        if (!key.empty() && key.back() == ':') key.pop_back();
        fields.push_back({{"name", key}, {"value", val}, {"unit", unit.empty() ? "" : unit}});
    }

    std::string swap = execCommand("swapon --show 2>/dev/null");
    std::string dt_mem = execCommand(
        "cat /sys/firmware/devicetree/base/memory*/device_type 2>/dev/null; "
        "echo; cat /sys/firmware/devicetree/base/memory*/reg 2>/dev/null | xxd 2>/dev/null | head -5");

    return {{"fields", fields}, {"swap", swap}, {"device_tree", dt_mem}};
}

json parseHwModule(const std::string& module) {
    json result = json::object();

    if (module == "npu") {
        result["tps_smi"] = execCommand("tps-smi 2>/dev/null");
        result["devices"] = execCommand("ls -la /dev/npu* /dev/accel* 2>/dev/null");
        result["driver"] = execCommand(
            "ls -la /sys/class/misc/npu* 2>/dev/null; "
            "cat /sys/class/misc/npu*/uevent 2>/dev/null; "
            "cat /sys/class/accel/*/device/uevent 2>/dev/null");
        result["device_tree"] = execCommand(
            "for f in /sys/firmware/devicetree/base/soc/npu*/compatible "
            "/sys/firmware/devicetree/base/soc/*npu*/compatible "
            "/sys/firmware/devicetree/base/npu*/compatible; do "
            "[ -f \"$f\" ] && echo \"$(dirname $f): $(cat $f 2>/dev/null)\"; done 2>/dev/null");
        if (result["device_tree"].get<std::string>().empty())
            result["device_tree"] = execCommand(
                "grep -rl 'npu' /sys/firmware/devicetree/base/soc/*/compatible 2>/dev/null | "
                "while read f; do echo \"$(dirname $f): $(cat $f)\"; done 2>/dev/null");
    }
    else if (module == "encoder") {
        result["vpu_info"] = execCommand(
            "ls -la /sys/class/vpu/ 2>/dev/null; "
            "for f in /sys/class/vpu/*/encode_fps; do [ -f \"$f\" ] && echo \"$f: $(cat $f)\"; done 2>/dev/null");
        result["devices"] = execCommand("ls -la /dev/vpu* /dev/video* /dev/hantro* 2>/dev/null");
        result["v4l2"] = execCommand("v4l2-ctl --list-devices 2>/dev/null");
        result["driver"] = execCommand(
            "for f in /sys/class/vpu/*/uevent; do [ -f \"$f\" ] && echo \"--- $f ---\" && cat $f; done 2>/dev/null; "
            "for f in /sys/class/video4linux/*/name; do [ -f \"$f\" ] && echo \"$(dirname $f): $(cat $f)\"; done 2>/dev/null");
        result["device_tree"] = execCommand(
            "for f in /sys/firmware/devicetree/base/soc/*enc*/compatible "
            "/sys/firmware/devicetree/base/soc/*vpu*/compatible "
            "/sys/firmware/devicetree/base/soc/*hantro*/compatible; do "
            "[ -f \"$f\" ] && echo \"$(dirname $f): $(cat $f 2>/dev/null)\"; done 2>/dev/null");
    }
    else if (module == "decoder") {
        result["vpu_info"] = execCommand(
            "ls -la /sys/class/vpu/ 2>/dev/null; "
            "for f in /sys/class/vpu/*/decode_fps; do [ -f \"$f\" ] && echo \"$f: $(cat $f)\"; done 2>/dev/null");
        result["devices"] = execCommand("ls -la /dev/vpu* /dev/video* /dev/hantro* 2>/dev/null");
        result["v4l2"] = execCommand("v4l2-ctl --list-devices 2>/dev/null");
        result["driver"] = execCommand(
            "for f in /sys/class/vpu/*/uevent; do [ -f \"$f\" ] && echo \"--- $f ---\" && cat $f; done 2>/dev/null; "
            "for f in /sys/class/video4linux/*/name; do [ -f \"$f\" ] && echo \"$(dirname $f): $(cat $f)\"; done 2>/dev/null");
        result["device_tree"] = execCommand(
            "for f in /sys/firmware/devicetree/base/soc/*dec*/compatible "
            "/sys/firmware/devicetree/base/soc/*vpu*/compatible; do "
            "[ -f \"$f\" ] && echo \"$(dirname $f): $(cat $f 2>/dev/null)\"; done 2>/dev/null");
    }
    else if (module == "pp") {
        result["devices"] = execCommand(
            "ls -la /dev/*pp* /sys/class/vpu/*pp* 2>/dev/null; "
            "ls -la /dev/hantro* 2>/dev/null");
        result["driver"] = execCommand(
            "for f in /sys/class/vpu/*pp*/uevent; do [ -f \"$f\" ] && echo \"--- $f ---\" && cat $f; done 2>/dev/null");
        result["device_tree"] = execCommand(
            "for f in /sys/firmware/devicetree/base/soc/*pp*/compatible "
            "/sys/firmware/devicetree/base/soc/*post*/compatible; do "
            "[ -f \"$f\" ] && echo \"$(dirname $f): $(cat $f 2>/dev/null)\"; done 2>/dev/null");
    }

    if (result.contains("device_tree") && result["device_tree"].is_string()
        && result["device_tree"].get<std::string>().empty()) {
        std::string dtb = execCommand("ls /boot/firmware/*.dtb 2>/dev/null | head -1");
        if (!dtb.empty())
            result["device_tree_from_dtb"] = execCommand(
                ("dtc -I dtb -O dts " + dtb + " 2>/dev/null | grep -A8 '" + module + "' | head -40").c_str());
    }

    return result;
}

json parseClockInfoDetailed() {
    json result = json::object();

    std::string clk_summary = execCommand("cat /sys/kernel/debug/clk/clk_summary 2>/dev/null");
    result["clk_summary"] = clk_summary;

    json clocks = json::array();
    std::string clk_list = execCommand(
        "if [ -d /sys/kernel/debug/clk ]; then "
        "for d in /sys/kernel/debug/clk/*/; do "
        "name=$(basename \"$d\"); "
        "rate=$(cat \"$d/clk_rate\" 2>/dev/null); "
        "enable=$(cat \"$d/clk_enable_count\" 2>/dev/null); "
        "[ -n \"$rate\" ] && echo \"$name|$rate|$enable\"; "
        "done; fi 2>/dev/null");

    if (!clk_list.empty()) {
        std::istringstream iss(clk_list);
        std::string line;
        while (std::getline(iss, line)) {
            auto p1 = line.find('|');
            if (p1 == std::string::npos) continue;
            auto p2 = line.find('|', p1 + 1);
            json clk = json::object();
            clk["name"] = line.substr(0, p1);
            clk["rate"] = (p2 != std::string::npos) ? line.substr(p1 + 1, p2 - p1 - 1) : line.substr(p1 + 1);
            clk["enable_count"] = (p2 != std::string::npos) ? line.substr(p2 + 1) : "";
            clocks.push_back(clk);
        }
    }
    result["clocks"] = clocks;

    return result;
}

json parseInterruptInfoDetailed() {
    json result = json::object();
    std::string raw = readFileAll("/proc/interrupts");
    result["raw"] = raw;

    json headers = json::array();
    json interrupts = json::array();
    std::istringstream iss(raw);
    std::string line;

    if (std::getline(iss, line)) {
        std::istringstream hss(line);
        std::string h;
        while (hss >> h) headers.push_back(h);
    }
    result["cpu_headers"] = headers;

    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        json entry = json::object();
        std::istringstream lss(line);
        std::string irq_name;
        lss >> irq_name;
        if (!irq_name.empty() && irq_name.back() == ':') irq_name.pop_back();
        entry["irq"] = irq_name;

        json counts = json::array();
        for (size_t i = 0; i < headers.size(); ++i) {
            long long count = 0;
            if (lss >> count) counts.push_back(count);
            else break;
        }
        entry["counts"] = counts;

        std::string rest;
        std::getline(lss, rest);
        entry["description"] = trimStr(rest);
        interrupts.push_back(entry);
    }
    result["interrupts"] = interrupts;
    return result;
}

json parseGpioInfoDetailed() {
    json result = json::object();
    result["debug_output"] = execCommand("cat /sys/kernel/debug/gpio 2>/dev/null");

    json chips = json::array();
    std::string chip_list = execCommand("ls -d /sys/class/gpio/gpiochip* 2>/dev/null");
    if (!chip_list.empty()) {
        std::istringstream iss(chip_list);
        std::string chip_path;
        while (std::getline(iss, chip_path)) {
            chip_path = trimStr(chip_path);
            if (chip_path.empty()) continue;
            json chip = json::object();
            chip["path"] = chip_path;
            chip["label"] = trimStr(readFileAll(chip_path + "/label"));
            chip["base"] = trimStr(readFileAll(chip_path + "/base"));
            chip["ngpio"] = trimStr(readFileAll(chip_path + "/ngpio"));
            chips.push_back(chip);
        }
    }
    result["chips"] = chips;
    result["gpioinfo"] = execCommand("gpioinfo 2>/dev/null | head -300");

    result["device_tree"] = execCommand(
        "for f in /sys/firmware/devicetree/base/soc/*gpio*/compatible "
        "/sys/firmware/devicetree/base/soc/*pinctrl*/compatible; do "
        "[ -f \"$f\" ] && echo \"$(dirname $f): $(cat $f 2>/dev/null)\"; done 2>/dev/null");
    if (result["device_tree"].get<std::string>().empty()) {
        std::string dtb = execCommand("ls /boot/firmware/*.dtb 2>/dev/null | head -1");
        if (!dtb.empty())
            result["device_tree"] = execCommand(
                ("dtc -I dtb -O dts " + dtb + " 2>/dev/null | grep -B2 -A5 'gpio' | head -60").c_str());
    }

    return result;
}

json parseDebPackages() {
    json result = json::object();

    result["sources"] = execCommand(
        "cat /etc/apt/sources.list 2>/dev/null; "
        "echo '---'; "
        "cat /etc/apt/sources.list.d/*.list 2>/dev/null");

    std::string installed_raw = execCommand(
        "dpkg-query -W -f '${Package}\\t${Version}\\t${Architecture}\\t${db:Status-Status}\\t${binary:Summary}\\n' 2>/dev/null");

    json packages = json::object();
    std::istringstream iss(installed_raw);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        std::string pkg, ver, arch, status, desc;
        std::istringstream lss(line);
        std::getline(lss, pkg, '\t');
        std::getline(lss, ver, '\t');
        std::getline(lss, arch, '\t');
        std::getline(lss, status, '\t');
        std::getline(lss, desc);

        if (!packages.contains(pkg)) {
            packages[pkg] = json::object();
            packages[pkg]["versions"] = json::array();
            packages[pkg]["description"] = desc;
            packages[pkg]["arch"] = arch;
        }
        packages[pkg]["versions"].push_back({
            {"version", ver},
            {"status", status}
        });
    }

    std::string apt_avail = execCommand(
        "apt list --all-versions 2>/dev/null | tail -n +2");
    if (!apt_avail.empty()) {
        std::istringstream aiss(apt_avail);
        while (std::getline(aiss, line)) {
            if (line.empty()) continue;
            auto slash = line.find('/');
            if (slash == std::string::npos) continue;
            std::string pkg = line.substr(0, slash);

            auto space = line.find(' ', slash);
            if (space == std::string::npos) continue;
            std::string rest = line.substr(space + 1);
            auto sp2 = rest.find(' ');
            std::string ver = rest.substr(0, sp2);
            bool is_installed = (line.find("[installed") != std::string::npos);

            if (!packages.contains(pkg)) {
                packages[pkg] = json::object();
                packages[pkg]["versions"] = json::array();
                packages[pkg]["description"] = "";
                packages[pkg]["arch"] = "";
            }

            bool exists = false;
            for (auto& v : packages[pkg]["versions"]) {
                if (v["version"] == ver) { exists = true; break; }
            }
            if (!exists) {
                packages[pkg]["versions"].push_back({
                    {"version", ver},
                    {"status", is_installed ? "installed" : "available"}
                });
            }
        }
    }

    result["packages"] = packages;
    return result;
}

json parseFilesystemInfo() {
    json result = json::object();

    // structured df output for charts
    json partitions = json::array();
    std::string df_raw = execCommand("df -BM --output=source,size,used,avail,pcent,target 2>/dev/null");
    {
        std::istringstream ss(df_raw);
        std::string line;
        std::getline(ss, line); // skip header
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            std::istringstream ls(line);
            std::string dev, sizeS, usedS, availS, pctS, mount;
            ls >> dev >> sizeS >> usedS >> availS >> pctS;
            std::getline(ls, mount);
            while (!mount.empty() && mount[0] == ' ') mount.erase(mount.begin());
            if (dev.find("/dev/") != 0 && dev != "tmpfs" && dev != "overlay") continue;
            auto toMB = [](const std::string& s) -> long long {
                std::string n;
                for (char c : s) { if (c >= '0' && c <= '9') n += c; }
                return n.empty() ? 0 : std::stoll(n);
            };
            int pct = 0;
            for (char c : pctS) { if (c >= '0' && c <= '9') pct = pct * 10 + (c - '0'); }
            partitions.push_back({
                {"device", dev}, {"mount", mount},
                {"size_mb", toMB(sizeS)}, {"used_mb", toMB(usedS)},
                {"avail_mb", toMB(availS)}, {"percent", pct}
            });
        }
    }
    result["partitions"] = partitions;
    result["lsblk"] = execCommand("lsblk -o NAME,SIZE,TYPE,MOUNTPOINT,FSTYPE 2>/dev/null");
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

    // ---- 详细 CPU 信息 ----
    server_->Get("/api/system/cpu", [](const httplib::Request&, httplib::Response& res) {
        auto r = ApiResponse::ok(parseCpuInfoDetailed());
        res.set_content(r.toJson().dump(), "application/json");
    });

    // ---- 详细内存信息 ----
    server_->Get("/api/system/memory", [](const httplib::Request&, httplib::Response& res) {
        auto r = ApiResponse::ok(parseMemInfoDetailed());
        res.set_content(r.toJson().dump(), "application/json");
    });

    // ---- 硬件模块信息 (NPU / encoder / decoder / PP) ----
    server_->Get(R"(/api/system/hw/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string module = req.matches[1];
        if (module != "npu" && module != "encoder" && module != "decoder" && module != "pp") {
            auto r = ApiResponse::error(ErrorCode::PARAM_ERROR, "未知模块: " + module);
            res.status = 400;
            res.set_content(r.toJson().dump(), "application/json");
            return;
        }
        auto r = ApiResponse::ok(parseHwModule(module));
        res.set_content(r.toJson().dump(), "application/json");
    });

    // ---- 时钟信息 ----
    server_->Get("/api/system/clocks", [](const httplib::Request&, httplib::Response& res) {
        auto r = ApiResponse::ok(parseClockInfoDetailed());
        res.set_content(r.toJson().dump(), "application/json");
    });

    // ---- 中断信息 ----
    server_->Get("/api/system/interrupts", [](const httplib::Request&, httplib::Response& res) {
        auto r = ApiResponse::ok(parseInterruptInfoDetailed());
        res.set_content(r.toJson().dump(), "application/json");
    });

    // ---- GPIO 信息 ----
    server_->Get("/api/system/gpio", [](const httplib::Request&, httplib::Response& res) {
        auto r = ApiResponse::ok(parseGpioInfoDetailed());
        res.set_content(r.toJson().dump(), "application/json");
    });

    // ---- DEB 包列表 ----
    server_->Get("/api/system/debs", [](const httplib::Request&, httplib::Response& res) {
        auto r = ApiResponse::ok(parseDebPackages());
        res.set_content(r.toJson().dump(), "application/json");
    });

    // ---- 安装 DEB 包 ----
    server_->Post("/api/system/deb/install", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string pkg = body.value("package", "");
            std::string ver = body.value("version", "");
            if (pkg.empty()) {
                res.status = 400;
                auto r = ApiResponse::error(ErrorCode::PARAM_ERROR, "package 参数必填");
                res.set_content(r.toJson().dump(), "application/json");
                return;
            }

            std::string cmd = "apt-get install -y " + pkg;
            if (!ver.empty()) cmd += "=" + ver;
            cmd += " 2>&1";
            std::string output = execCommand(cmd.c_str());

            std::string check = execCommand(
                ("dpkg -s " + pkg + " 2>/dev/null | grep '^Status'").c_str());
            bool success = check.find("install ok installed") != std::string::npos;

            if (success) {
                auto r = ApiResponse::ok({{"output", output}}, "安装成功");
                res.set_content(r.toJson().dump(), "application/json");
            } else {
                auto r = ApiResponse::error(ErrorCode::INTERNAL_ERROR, "安装失败: " + output);
                res.set_content(r.toJson().dump(), "application/json");
            }
        } catch (const json::parse_error&) {
            res.status = 400;
            auto r = ApiResponse::error(ErrorCode::PARAM_ERROR, "JSON 解析失败");
            res.set_content(r.toJson().dump(), "application/json");
        }
    });

    // ---- 文件系统信息 ----
    server_->Get("/api/system/filesystem", [](const httplib::Request&, httplib::Response& res) {
        auto r = ApiResponse::ok(parseFilesystemInfo());
        res.set_content(r.toJson().dump(), "application/json");
    });

    // ---- 厂商 APT 数据源 ----
    server_->Get("/api/system/apt-source", [](const httplib::Request&, httplib::Response& res) {
        std::string content = execCommand("cat /etc/apt/sources.list 2>/dev/null");
        if (content.empty()) {
            content = "deb http://172.16.1.193:6520/ubuntu noble main\n"
                      "deb https://mirrors.aliyun.com/ubuntu-ports noble main universe";
        }
        auto r = ApiResponse::ok({{"source", content}});
        res.set_content(r.toJson().dump(), "application/json");
    });

    server_->Post("/api/system/apt-source", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string source = body.value("source", "");
            if (source.empty()) {
                res.status = 400;
                auto r = ApiResponse::error(ErrorCode::PARAM_ERROR, "source 参数必填");
                res.set_content(r.toJson().dump(), "application/json");
                return;
            }

            std::ofstream f("/etc/apt/sources.list");
            if (!f.is_open()) {
                auto r2 = ApiResponse::error(ErrorCode::PARAM_ERROR, "无法写入 /etc/apt/sources.list，请检查权限");
                res.set_content(r2.toJson().dump(), "application/json");
                return;
            }
            f << source << std::endl;
            f.close();

            std::string output = execCommand("apt-get update 2>&1");

            json pkgs = parseDebPackages();

            auto r = ApiResponse::ok({{"output", output}, {"packages", pkgs["packages"]}}, "数据源已更新，已获取可用包列表");
            res.set_content(r.toJson().dump(), "application/json");
        } catch (const json::parse_error&) {
            res.status = 400;
            auto r = ApiResponse::error(ErrorCode::PARAM_ERROR, "JSON 解析失败");
            res.set_content(r.toJson().dump(), "application/json");
        }
    });

    // ---- DMA / CMA 内存池 ----
    server_->Get("/api/system/dma-memory", [](const httplib::Request&, httplib::Response& res) {
        json data = json::object();

        data["cma_total"] = execCommand("grep CmaTotal /proc/meminfo 2>/dev/null | awk '{print $2}'");
        data["cma_free"] = execCommand("grep CmaFree /proc/meminfo 2>/dev/null | awk '{print $2}'");
        data["dma_buf"] = execCommand("cat /sys/kernel/debug/dma_buf/bufinfo 2>/dev/null | head -60");

        data["umap_list"] = execCommand("ls /proc/umap/ 2>/dev/null");
        data["umap_media_mem"] = execCommand("cat /proc/umap/media-mem 2>/dev/null | head -40");

        std::string smi = execCommand("tps-smi 2>/dev/null");
        std::string mem_lines;
        std::istringstream iss(smi);
        std::string line;
        while (std::getline(iss, line)) {
            std::string lower = line;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.find("mem") != std::string::npos ||
                lower.find("dma") != std::string::npos ||
                lower.find("pool") != std::string::npos) {
                mem_lines += line + "\n";
            }
        }
        data["tps_smi_memory"] = mem_lines;

        auto r = ApiResponse::ok(data);
        res.set_content(r.toJson().dump(), "application/json");
    });

    // ---- /boot/firmware 板级配置 ----
    server_->Get("/api/system/board-config", [](const httplib::Request&, httplib::Response& res) {
        json data = json::object();
        namespace fs = std::filesystem;

        const std::string boot_dir = "/boot/firmware";

        // Board model from device tree
        data["board_model"] = execCommand("cat /sys/firmware/devicetree/base/model 2>/dev/null");
        data["board_compatible"] = execCommand("cat /sys/firmware/devicetree/base/compatible 2>/dev/null");

        // Boot firmware directory listing
        json boot_files = json::array();
        std::error_code ec;
        if (fs::exists(boot_dir, ec)) {
            for (auto& entry : fs::directory_iterator(boot_dir, ec)) {
                auto fname = entry.path().filename().string();
                json finfo = {{"name", fname}};
                if (entry.is_regular_file(ec)) {
                    finfo["size"] = entry.file_size(ec);
                    finfo["type"] = "file";
                } else if (entry.is_directory(ec)) {
                    finfo["type"] = "directory";
                }
                boot_files.push_back(finfo);
            }
            std::sort(boot_files.begin(), boot_files.end(),
                [](const json& a, const json& b) {
                    return a.value("name", "") < b.value("name", "");
                });
        }
        data["boot_files"] = boot_files;
        data["boot_dir_exists"] = fs::exists(boot_dir, ec);

        // Parse config.txt
        const std::string config_path = boot_dir + "/config.txt";
        json config_sections = json::array();
        json active_params = json::object();
        json commented_params = json::object();
        std::string config_raw;

        std::ifstream cfg(config_path);
        if (cfg.is_open()) {
            std::string current_section;
            std::string line;
            std::ostringstream raw_oss;

            while (std::getline(cfg, line)) {
                raw_oss << line << "\n";

                // Trim
                std::string trimmed = line;
                while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
                    trimmed.erase(trimmed.begin());
                while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' ||
                       trimmed.back() == '\r' || trimmed.back() == '\n'))
                    trimmed.pop_back();

                if (trimmed.empty()) continue;

                // Section header: [default]
                if (trimmed.front() == '[' && trimmed.back() == ']') {
                    current_section = trimmed.substr(1, trimmed.size() - 2);
                    continue;
                }

                // Section comment: ##########...##########
                if (trimmed.size() > 10 && trimmed.substr(0, 10) == "##########") {
                    // Extract section name from between ##########
                    auto content = trimmed;
                    while (!content.empty() && content.front() == '#') content.erase(content.begin());
                    while (!content.empty() && content.back() == '#') content.pop_back();
                    while (!content.empty() && content.front() == ' ') content.erase(content.begin());
                    while (!content.empty() && content.back() == ' ') content.pop_back();
                    if (!content.empty()) {
                        current_section = content;
                        bool found = false;
                        for (auto& s : config_sections) {
                            if (s.get<std::string>() == current_section) { found = true; break; }
                        }
                        if (!found) config_sections.push_back(current_section);
                    }
                    continue;
                }

                // Commented parameter: # key=value
                if (trimmed.front() == '#') {
                    auto param = trimmed.substr(1);
                    while (!param.empty() && param.front() == ' ') param.erase(param.begin());
                    auto eq_pos = param.find('=');
                    if (eq_pos != std::string::npos && eq_pos > 0) {
                        auto key = param.substr(0, eq_pos);
                        auto val = param.substr(eq_pos + 1);
                        commented_params[key] = val;
                    }
                    continue;
                }

                // Active parameter: key=value
                auto eq_pos = trimmed.find('=');
                if (eq_pos != std::string::npos && eq_pos > 0) {
                    auto key = trimmed.substr(0, eq_pos);
                    auto val = trimmed.substr(eq_pos + 1);
                    active_params[key] = val;
                }
            }
            config_raw = raw_oss.str();
        }

        data["config_exists"] = cfg.is_open() || fs::exists(config_path, ec);
        data["config_sections"] = config_sections;
        data["active_params"] = active_params;
        data["commented_params"] = commented_params;
        data["config_raw"] = config_raw;

        // DTB/DTBO files (root + overlays/ subdirectory)
        json dtb_files = json::array();
        if (fs::exists(boot_dir, ec)) {
            auto scan_dtb = [&](const std::string& dir, const std::string& prefix) {
                std::error_code ec2;
                if (!fs::exists(dir, ec2)) return;
                for (auto& entry : fs::directory_iterator(dir, ec2)) {
                    auto ext = entry.path().extension().string();
                    if (ext == ".dtb" || ext == ".dtbo") {
                        dtb_files.push_back(prefix + entry.path().filename().string());
                    }
                }
            };
            scan_dtb(boot_dir, "");
            scan_dtb(boot_dir + "/overlays", "overlays/");
            std::sort(dtb_files.begin(), dtb_files.end());
        }
        data["dtb_files"] = dtb_files;

        // boot_fitconfig from active_params (board type identifier)
        if (active_params.contains("boot_fitconfig")) {
            data["board_type"] = active_params["boot_fitconfig"];
        } else {
            data["board_type"] = data["board_model"];
        }

        auto r = ApiResponse::ok(data);
        res.set_content(r.toJson().dump(), "application/json");
    });

    // ---- Device Tree runtime info (overrides / symbols / top nodes) ----
    server_->Get("/api/system/device-tree", [](const httplib::Request&, httplib::Response& res) {
        json data = json::object();
        namespace fs = std::filesystem;
        std::error_code ec;

        const std::string dt_base = "/sys/firmware/devicetree/base";

        // Helper: read binary file contents
        auto readBinaryFile = [](const std::string& path) -> std::vector<uint8_t> {
            std::ifstream f(path, std::ios::binary);
            if (!f.is_open()) return {};
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
        };

        // Helper: read DT string property (may contain embedded NULs for multi-string)
        auto readStringProp = [&](const std::string& path) -> std::string {
            auto bytes = readBinaryFile(path);
            if (bytes.empty()) return "";
            // DT multi-strings are NUL-separated; replace embedded NULs with ", "
            std::string result;
            result.reserve(bytes.size());
            for (size_t i = 0; i < bytes.size(); ++i) {
                uint8_t b = bytes[i];
                if (b == 0) {
                    if (i + 1 < bytes.size() && bytes[i + 1] != 0)
                        result += ", ";
                } else if (b >= 0x20 && b < 0x7f) {
                    result += static_cast<char>(b);
                } else {
                    // Non-printable: skip or replace
                }
            }
            while (!result.empty() && (result.back() == ' ' || result.back() == ','))
                result.pop_back();
            return result;
        };

        // Helper: bytes to hex string
        auto toHex = [](const std::vector<uint8_t>& v) -> std::string {
            std::ostringstream oss;
            for (auto b : v) oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
            return oss.str();
        };

        // 1. Read __overrides__
        json overrides = json::array();
        std::string ovr_dir = dt_base + "/__overrides__";
        if (fs::exists(ovr_dir, ec) && fs::is_directory(ovr_dir, ec)) {
            for (auto& entry : fs::directory_iterator(ovr_dir, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                auto name = entry.path().filename().string();
                if (name == "name") continue;

                auto bytes = readBinaryFile(entry.path().string());
                json item = {{"name", name}, {"raw_hex", toHex(bytes)}};

                // Parse override entries: each is 4-byte phandle (BE) + NUL-terminated prop string
                // Multiple targets may be concatenated
                json targets = json::array();
                size_t pos = 0;
                while (pos + 4 < bytes.size()) {
                    uint32_t ph = ((uint32_t)bytes[pos] << 24) | ((uint32_t)bytes[pos+1] << 16) |
                                  ((uint32_t)bytes[pos+2] << 8) | (uint32_t)bytes[pos+3];
                    pos += 4;
                    // Read NUL-terminated string
                    std::string prop_spec;
                    while (pos < bytes.size() && bytes[pos] != 0) {
                        char c = static_cast<char>(bytes[pos]);
                        if (c >= 0x20 && c < 0x7f)
                            prop_spec += c;
                        ++pos;
                    }
                    if (pos < bytes.size()) ++pos; // skip NUL

                    json target = {{"phandle", ph}};
                    auto colon = prop_spec.find(':');
                    if (colon != std::string::npos) {
                        target["target_prop"] = prop_spec.substr(0, colon);
                        target["offset"] = prop_spec.substr(colon + 1);
                    } else {
                        target["target_prop"] = prop_spec;
                        target["offset"] = "";
                    }
                    targets.push_back(target);
                }

                // Use first target for primary fields
                if (!targets.empty()) {
                    item["phandle"] = targets[0].value("phandle", 0u);
                    item["target_prop"] = targets[0].value("target_prop", "");
                    item["offset"] = targets[0].value("offset", "");
                    item["prop_spec"] = item["target_prop"].get<std::string>() +
                        (item["offset"].get<std::string>().empty() ? "" : ":" + item["offset"].get<std::string>());
                }
                item["targets"] = targets;
                item["target_count"] = (int)targets.size();
                overrides.push_back(item);
            }
            std::sort(overrides.begin(), overrides.end(),
                [](const json& a, const json& b) { return a.value("name","") < b.value("name",""); });
        }
        data["overrides"] = overrides;
        data["overrides_count"] = overrides.size();

        // 2. Read __symbols__
        json symbols = json::object();
        std::string sym_dir = dt_base + "/__symbols__";
        if (fs::exists(sym_dir, ec) && fs::is_directory(sym_dir, ec)) {
            for (auto& entry : fs::directory_iterator(sym_dir, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                auto name = entry.path().filename().string();
                if (name == "name") continue;
                symbols[name] = readStringProp(entry.path().string());
            }
        }
        data["symbols"] = symbols;

        // Build phandle → path lookup from symbols
        // We need to scan the tree to map phandle values to node paths
        // Use __symbols__ for label→path, then read phandle from each node
        std::unordered_map<uint32_t, std::string> phandle_map;
        for (auto& [label, path_val] : symbols.items()) {
            std::string node_path = dt_base + path_val.get<std::string>();
            std::string ph_file = node_path + "/phandle";
            if (fs::exists(ph_file, ec)) {
                auto ph_bytes = readBinaryFile(ph_file);
                if (ph_bytes.size() >= 4) {
                    uint32_t ph = ((uint32_t)ph_bytes[0] << 24) | ((uint32_t)ph_bytes[1] << 16) |
                                   ((uint32_t)ph_bytes[2] << 8)  | (uint32_t)ph_bytes[3];
                    phandle_map[ph] = path_val.get<std::string>();
                }
            }
        }

        // Resolve phandle → path in overrides (all targets)
        for (auto& item : overrides) {
            if (item.contains("targets")) {
                for (auto& t : item["targets"]) {
                    uint32_t ph = t.value("phandle", 0u);
                    auto it = phandle_map.find(ph);
                    if (it != phandle_map.end()) {
                        t["target_path"] = it->second;
                    } else {
                        std::ostringstream o; o << "(phandle 0x" << std::hex << ph << ")";
                        t["target_path"] = o.str();
                    }
                }
            }
            // Primary target_path from first target
            if (item.contains("targets") && !item["targets"].empty()) {
                item["target_path"] = item["targets"][0].value("target_path", "");
            }
        }
        data["overrides"] = overrides;

        // 3. Read top-level nodes (depth 1) with key properties
        json top_nodes = json::array();
        if (fs::exists(dt_base, ec) && fs::is_directory(dt_base, ec)) {
            for (auto& entry : fs::directory_iterator(dt_base, ec)) {
                if (!entry.is_directory(ec)) continue;
                auto name = entry.path().filename().string();
                if (name.front() == '_') continue; // skip __overrides__, __symbols__, etc.

                json node = {{"name", name}, {"path", "/" + name}};

                auto compat_path = entry.path().string() + "/compatible";
                if (fs::exists(compat_path, ec)) {
                    node["compatible"] = readStringProp(compat_path);
                }
                auto status_path = entry.path().string() + "/status";
                if (fs::exists(status_path, ec)) {
                    node["status"] = readStringProp(status_path);
                }

                // Children (depth 2)
                json children = json::array();
                for (auto& child : fs::directory_iterator(entry.path(), ec)) {
                    if (!child.is_directory(ec)) continue;
                    auto cname = child.path().filename().string();
                    if (cname.front() == '_') continue;

                    json cnode = {{"name", cname}};
                    auto cc = child.path().string() + "/compatible";
                    if (fs::exists(cc, ec)) cnode["compatible"] = readStringProp(cc);
                    auto cs = child.path().string() + "/status";
                    if (fs::exists(cs, ec)) cnode["status"] = readStringProp(cs);
                    children.push_back(cnode);
                }
                if (!children.empty()) {
                    std::sort(children.begin(), children.end(),
                        [](const json& a, const json& b) { return a.value("name","") < b.value("name",""); });
                    node["children"] = children;
                }

                top_nodes.push_back(node);
            }
            std::sort(top_nodes.begin(), top_nodes.end(),
                [](const json& a, const json& b) { return a.value("name","") < b.value("name",""); });
        }
        data["top_nodes"] = top_nodes;

        auto r = ApiResponse::ok(data);
        res.set_content(r.toJson().dump(), "application/json");
    });

    // ---- DTBO file detail (decompile with dtc) ----
    server_->Get("/api/system/dtbo-detail", [](const httplib::Request& req, httplib::Response& res) {
        auto file_param = req.get_param_value("file");
        if (file_param.empty()) {
            auto r = ApiResponse::error(400, "Missing 'file' parameter");
            res.set_content(r.toJson().dump(), "application/json");
            return;
        }

        // Security: only allow .dtbo files under /boot/firmware/
        if (file_param.find("..") != std::string::npos ||
            file_param.find('/') == 0 ||
            (file_param.substr(file_param.size() - 5) != ".dtbo" &&
             file_param.substr(file_param.size() - 4) != ".dtb")) {
            auto r = ApiResponse::error(400, "Invalid file path");
            res.set_content(r.toJson().dump(), "application/json");
            return;
        }

        std::string full_path = "/boot/firmware/" + file_param;
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(full_path, ec)) {
            auto r = ApiResponse::error(404, "File not found: " + file_param);
            res.set_content(r.toJson().dump(), "application/json");
            return;
        }

        json data = json::object();
        data["file"] = file_param;
        data["size"] = (int64_t)fs::file_size(full_path, ec);

        // Try decompile with dtc
        std::string cmd = "dtc -I dtb -O dts " + full_path + " 2>&1";
        std::string dts_content = execCommand(cmd.c_str());

        if (dts_content.empty() || dts_content.find("Error") != std::string::npos) {
            data["dts_available"] = false;
            data["dts_error"] = dts_content.empty() ? "dtc command not found or failed" : dts_content;
            data["dts_content"] = "";
        } else {
            data["dts_available"] = true;
            data["dts_content"] = dts_content;
        }

        // Extract fragments summary from dts_content
        json fragments = json::array();
        if (!dts_content.empty()) {
            std::istringstream iss(dts_content);
            std::string line;
            std::string current_fragment;
            while (std::getline(iss, line)) {
                auto trimmed = line;
                while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
                    trimmed.erase(trimmed.begin());

                // Detect fragment node
                if (trimmed.find("fragment@") != std::string::npos ||
                    trimmed.find("fragment-") != std::string::npos) {
                    auto brace = trimmed.find('{');
                    if (brace != std::string::npos) {
                        current_fragment = trimmed.substr(0, brace);
                        while (!current_fragment.empty() && current_fragment.back() == ' ')
                            current_fragment.pop_back();
                    } else {
                        current_fragment = trimmed;
                    }
                }

                // Detect target
                if (!current_fragment.empty()) {
                    if (trimmed.find("target-path") != std::string::npos ||
                        trimmed.find("target =") != std::string::npos ||
                        trimmed.find("target=") != std::string::npos) {
                        json frag = {{"fragment", current_fragment}, {"target_line", trimmed}};
                        fragments.push_back(frag);
                    }
                }
            }
        }
        data["fragments"] = fragments;

        auto r = ApiResponse::ok(data);
        res.set_content(r.toJson().dump(), "application/json");
    });

    // ---- Device Tree modular view (nodes grouped by functional module) ----
    server_->Get("/api/system/device-tree-modules", [](const httplib::Request&, httplib::Response& res) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::string dt_base = "/sys/firmware/devicetree/base";

        // Safe binary file read
        auto readBin = [](const std::string& p) -> std::vector<uint8_t> {
            std::ifstream f(p, std::ios::binary);
            if (!f.is_open()) return {};
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
        };
        // Safe string property read (handles multi-string NUL separators)
        auto readStr = [&](const std::string& p) -> std::string {
            auto bytes = readBin(p);
            if (bytes.empty()) return "";
            std::string r;
            for (size_t i = 0; i < bytes.size(); ++i) {
                uint8_t b = bytes[i];
                if (b == 0) { if (i + 1 < bytes.size() && bytes[i+1] != 0) r += ", "; }
                else if (b >= 0x20 && b < 0x7f) r += static_cast<char>(b);
            }
            while (!r.empty() && (r.back() == ' ' || r.back() == ',')) r.pop_back();
            return r;
        };

        // Module classification rules
        struct ModuleRule {
            std::string name;
            std::string color;
            std::vector<std::string> compat_keywords;
            std::vector<std::string> path_keywords;
            std::vector<std::string> override_params;
            std::vector<std::string> dtbo_keywords;
        };
        std::vector<ModuleRule> rules = {
            {"CPU / 频率", "blue", {"cpufreq"}, {"cpus", "cpu@", "tps-cpufreq"},
             {"cpu_freq", "cpu_max_freq", "cpu_min_freq", "over_voltage", "temp_limit"}, {}},
            {"内存预留", "purple", {}, {"reserved-memory"},
             {"tacosys_mem_addr", "tacosys_mem_size", "npu_mem_addr", "npu_mem_size",
              "total_mem", "npumem", "tacosysmem"}, {"reserved_mem"}},
            {"SD / MMC", "green", {"sdhci", "mmc"}, {},
             {"sd_bus_width", "sd_clk_freq", "emmc_bus_width", "emmc_clk_freq"}, {"sd_sdr104", "emmc_hs200"}},
            {"网络 / 以太网", "cyan", {"ethernet", "gmac", "dwmac"}, {},
             {"eth1_tx_delay", "eth1_rx_delay", "eth1_mac", "eth1_max_speed", "eth1_reset",
              "eth2_mac", "eth2_max_speed", "eth2_rx_delay", "eth2_tx_delay"}, {}},
            {"NPU", "red", {"npu", "vip", "galcore"}, {},
             {"npu_freq", "npu_core0_r_ratio", "npu_core0_w_ratio", "npu_core1_r_ratio",
              "npu_core1_w_ratio", "npu_dual_bus", "npu_volt0", "npu_volt1"}, {}},
            {"显示 / IDS", "pink", {"ids", "display", "hdmi", "dss"}, {},
             {}, {"hdmi_60fps", "lcd_1024x600"}},
            {"蓝牙 / WiFi", "orange", {"bluetooth", "wireless", "wifi", "wlan"}, {},
             {"bt", "bt_baudrate", "wifi"}, {}},
            {"UART / 串口", "gray", {"serial", "uart", "ns16550"}, {},
             {"uart0", "uart2", "uart5", "uart9", "uart11", "console_size"}, {"uart11_overlay"}},
            {"SPI / Flash", "yellow", {"spi", "qspi"}, {},
             {"qspi", "spi2"}, {"flash_overlay"}},
            {"PWM", "teal", {"pwm"}, {"pwm@"},
             {"pwm2", "pwm3", "pwm4", "pwm5", "pwm6", "pwm7"}, {"pwm4_overlay"}},
            {"I2C", "indigo", {"i2c"}, {},
             {"i2c1", "i2c2", "i2c3", "i2c5", "i2c7"}, {}},
            {"看门狗", "brown", {"wdt", "watchdog"}, {},
             {"watchdog0", "watchdog1"}, {}},
            {"蜂鸣器 / LED / 风扇", "lime", {"buzzer", "beeper", "leds", "fan", "gpio-fan"}, {"fan", "leds"},
             {"boot_disable_fan", "fan_maxpwm", "fan_minpwm", "fan_temp1", "fan_temp1_hyst",
              "fan_temp2", "fan_temp2_hyst", "fan_temp3", "fan_temp3_hyst",
              "fan_temp4", "fan_temp4_hyst", "fan_temp5", "fan_temp5_hyst",
              "buzzer_panic_beep_time", "buzzer_panic_interval_time", "buzzer_panic_period",
              "buzzer_power_off_beep_time", "buzzer_power_off_period",
              "buzzer_start_beep_period", "buzzer_start_beep_time",
              "act_led_trigger"}, {}},
            {"Ramoops / 调试", "gray", {"ramoops"}, {"ramoops"},
             {"base_addr", "record_size", "total_size", "ramoops"}, {}},
            {"音频 / I2S", "violet", {"i2s", "audio", "sound"}, {},
             {}, {"i2s_mode_overlay"}},
            {"SATA", "steel", {"sata", "ahci"}, {},
             {"sata"}, {}},
            {"EEPROM", "amber", {"eeprom", "at24"}, {},
             {"eeprom_write_protect"}, {}},
            {"OneWire", "lime", {"onewire", "w1"}, {},
             {}, {"onewire_overlay"}},
        };

        // Format a binary property value for display
        auto formatPropValue = [&](const std::string& prop_name, const std::vector<uint8_t>& bytes) -> std::string {
            if (bytes.empty()) return "";
            // Try to interpret as NUL-terminated string(s)
            bool is_string = true;
            bool has_printable = false;
            for (size_t i = 0; i < bytes.size(); ++i) {
                uint8_t b = bytes[i];
                if (b == 0) continue;
                if (b >= 0x20 && b < 0x7f) { has_printable = true; }
                else { is_string = false; break; }
            }
            if (is_string && has_printable) {
                return readStr(prop_name); // reuse readStr which handles multi-string
            }
            // Numeric: 4-byte big-endian integers
            if (bytes.size() == 4) {
                uint32_t v = ((uint32_t)bytes[0]<<24)|((uint32_t)bytes[1]<<16)|
                             ((uint32_t)bytes[2]<<8)|(uint32_t)bytes[3];
                return "0x" + ([](uint32_t n) {
                    char buf[16]; snprintf(buf, sizeof(buf), "%08x", n); return std::string(buf);
                })(v) + " (" + std::to_string(v) + ")";
            }
            if (bytes.size() == 8) {
                uint64_t v = 0;
                for (int i = 0; i < 8; ++i) v = (v << 8) | bytes[i];
                char buf[32]; snprintf(buf, sizeof(buf), "0x%016lx", (unsigned long)v);
                return std::string(buf);
            }
            // Fallback: hex dump (up to 32 bytes)
            std::string hex;
            for (size_t i = 0; i < std::min(bytes.size(), (size_t)32); ++i) {
                char buf[4]; snprintf(buf, sizeof(buf), "%02x ", bytes[i]);
                hex += buf;
            }
            if (bytes.size() > 32) hex += "...";
            return hex;
        };

        // Read all properties of a device tree node directory
        auto readNodeProps = [&](const std::string& dir_path) -> json {
            json props = json::array();
            if (!fs::exists(dir_path, ec)) return props;
            for (auto& entry : fs::directory_iterator(dir_path, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                auto fname = entry.path().filename().string();
                if (fname == "name") continue; // redundant
                auto bytes = readBin(entry.path().string());
                std::string value = formatPropValue(entry.path().string(), bytes);
                props.push_back({{"key", fname}, {"value", value}, {"size", (int)bytes.size()}});
            }
            // Sort by key name
            std::sort(props.begin(), props.end(), [](const json& a, const json& b) {
                return a["key"].get<std::string>() < b["key"].get<std::string>();
            });
            return props;
        };

        // Collect all DTB nodes (depth 2) with full properties
        struct DtNode {
            std::string path;
            std::string name;
            std::string fs_path; // filesystem path for lazy property reading
            std::string compatible;
            std::string status;
            json properties;
            int assigned_module = -1;
        };
        std::vector<DtNode> all_nodes;

        auto scanDir = [&](const std::string& base_path, const std::string& prefix) {
            if (!fs::exists(base_path, ec) || !fs::is_directory(base_path, ec)) return;
            for (auto& entry : fs::directory_iterator(base_path, ec)) {
                if (!entry.is_directory(ec)) continue;
                auto name = entry.path().filename().string();
                if (name.front() == '_' || name.front() == '#') continue;

                DtNode node;
                node.name = name;
                node.path = prefix + name;
                node.fs_path = entry.path().string();

                auto cp = entry.path().string() + "/compatible";
                if (fs::exists(cp, ec)) node.compatible = readStr(cp);
                auto sp = entry.path().string() + "/status";
                if (fs::exists(sp, ec)) node.status = readStr(sp);
                node.properties = readNodeProps(entry.path().string());

                all_nodes.push_back(node);

                // Scan children (depth 2 under soc, cpus, reserved-memory, etc.)
                for (auto& child : fs::directory_iterator(entry.path(), ec)) {
                    if (!child.is_directory(ec)) continue;
                    auto cname = child.path().filename().string();
                    if (cname.front() == '_' || cname.front() == '#') continue;

                    DtNode cnode;
                    cnode.name = cname;
                    cnode.path = prefix + name + "/" + cname;
                    cnode.fs_path = child.path().string();
                    auto ccp = child.path().string() + "/compatible";
                    if (fs::exists(ccp, ec)) cnode.compatible = readStr(ccp);
                    auto csp = child.path().string() + "/status";
                    if (fs::exists(csp, ec)) cnode.status = readStr(csp);
                    cnode.properties = readNodeProps(child.path().string());
                    all_nodes.push_back(cnode);
                }
            }
        };
        scanDir(dt_base, "/");

        // Classify nodes into modules
        auto matchesAny = [](const std::string& haystack, const std::vector<std::string>& needles) {
            for (auto& n : needles)
                if (haystack.find(n) != std::string::npos) return true;
            return false;
        };

        for (auto& node : all_nodes) {
            std::string lpath = node.path;
            std::transform(lpath.begin(), lpath.end(), lpath.begin(), ::tolower);
            std::string lcompat = node.compatible;
            std::transform(lcompat.begin(), lcompat.end(), lcompat.begin(), ::tolower);

            for (size_t i = 0; i < rules.size(); ++i) {
                if (matchesAny(lcompat, rules[i].compat_keywords) ||
                    matchesAny(lpath, rules[i].path_keywords)) {
                    node.assigned_module = (int)i;
                    break;
                }
            }
        }

        // Read __overrides__
        struct OverrideInfo {
            std::string name;
            std::string target_path;
            std::string target_prop;
        };
        std::vector<OverrideInfo> all_overrides;

        // Build phandle map from __symbols__
        std::unordered_map<uint32_t, std::string> ph_map;
        std::string sym_dir = dt_base + "/__symbols__";
        if (fs::exists(sym_dir, ec)) {
            for (auto& entry : fs::directory_iterator(sym_dir, ec)) {
                if (!entry.is_regular_file(ec) || entry.path().filename() == "name") continue;
                std::string sympath = readStr(entry.path().string());
                std::string ph_file = dt_base + sympath + "/phandle";
                if (fs::exists(ph_file, ec)) {
                    auto phb = readBin(ph_file);
                    if (phb.size() >= 4) {
                        uint32_t ph = ((uint32_t)phb[0]<<24)|((uint32_t)phb[1]<<16)|
                                      ((uint32_t)phb[2]<<8)|(uint32_t)phb[3];
                        ph_map[ph] = sympath;
                    }
                }
            }
        }

        std::string ovr_dir = dt_base + "/__overrides__";
        if (fs::exists(ovr_dir, ec)) {
            for (auto& entry : fs::directory_iterator(ovr_dir, ec)) {
                if (!entry.is_regular_file(ec) || entry.path().filename() == "name") continue;
                auto bytes = readBin(entry.path().string());
                size_t pos = 0;
                while (pos + 4 < bytes.size()) {
                    uint32_t ph = ((uint32_t)bytes[pos]<<24)|((uint32_t)bytes[pos+1]<<16)|
                                  ((uint32_t)bytes[pos+2]<<8)|(uint32_t)bytes[pos+3];
                    pos += 4;
                    std::string prop;
                    while (pos < bytes.size() && bytes[pos] != 0) {
                        char c = static_cast<char>(bytes[pos]);
                        if (c >= 0x20 && c < 0x7f) prop += c;
                        ++pos;
                    }
                    if (pos < bytes.size()) ++pos;

                    OverrideInfo oi;
                    oi.name = entry.path().filename().string();
                    auto it = ph_map.find(ph);
                    oi.target_path = (it != ph_map.end()) ? it->second : "";
                    auto colon = prop.find(':');
                    oi.target_prop = (colon != std::string::npos) ? prop.substr(0, colon) : prop;
                    all_overrides.push_back(oi);
                    break; // only first target for classification
                }
            }
        }

        // Read active config.txt params
        std::unordered_map<std::string, std::string> active_params;
        std::string config_path = "/boot/firmware/config.txt";
        {
            std::ifstream cf(config_path);
            if (cf.is_open()) {
                std::string line;
                bool in_section = false;
                while (std::getline(cf, line)) {
                    auto trimmed = line;
                    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
                        trimmed.erase(trimmed.begin());
                    if (trimmed.empty() || trimmed.front() == '#') continue;
                    if (trimmed.front() == '[') { in_section = true; continue; }
                    auto eq = trimmed.find('=');
                    if (eq != std::string::npos) {
                        auto key = trimmed.substr(0, eq);
                        auto val = trimmed.substr(eq + 1);
                        while (!key.empty() && key.back() == ' ') key.pop_back();
                        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
                        active_params[key] = val;
                    }
                }
            }
        }

        // Collect available DTBO files
        std::vector<std::string> dtbo_files;
        std::string overlay_dir = "/boot/firmware/overlays";
        if (fs::exists(overlay_dir, ec)) {
            for (auto& entry : fs::directory_iterator(overlay_dir, ec)) {
                if (entry.is_regular_file(ec)) {
                    auto fname = entry.path().filename().string();
                    if (fname.size() > 5 && fname.substr(fname.size()-5) == ".dtbo")
                        dtbo_files.push_back(fname);
                }
            }
            std::sort(dtbo_files.begin(), dtbo_files.end());
        }

        // Active dtoverlay from config.txt
        std::string active_dtoverlay = active_params.count("dtoverlay") ? active_params["dtoverlay"] : "";

        // Build module JSON
        json modules = json::array();
        std::set<int> used_node_indices;

        for (size_t mi = 0; mi < rules.size(); ++mi) {
            auto& rule = rules[mi];
            json mod = json::object();
            mod["name"] = rule.name;
            mod["color"] = rule.color;

            // Nodes in this module
            json mnodes = json::array();
            for (size_t ni = 0; ni < all_nodes.size(); ++ni) {
                if (all_nodes[ni].assigned_module == (int)mi) {
                    used_node_indices.insert((int)ni);
                    json n = {{"path", all_nodes[ni].path},
                              {"compatible", all_nodes[ni].compatible},
                              {"status", all_nodes[ni].status},
                              {"properties", all_nodes[ni].properties}};
                    mnodes.push_back(n);
                }
            }
            mod["nodes"] = mnodes;

            // Overrides for this module
            json movrs = json::array();
            for (auto& param : rule.override_params) {
                json ov = {{"param", param}, {"active", active_params.count(param) > 0}};
                if (active_params.count(param)) ov["value"] = active_params[param];
                else ov["value"] = nullptr;

                // Find target info from __overrides__
                for (auto& oi : all_overrides) {
                    if (oi.name == param) {
                        ov["target_path"] = oi.target_path;
                        ov["target_prop"] = oi.target_prop;
                        break;
                    }
                }
                movrs.push_back(ov);
            }
            mod["overrides"] = movrs;

            // DTBOs for this module
            json mdtbos = json::array();
            for (auto& dtbo : dtbo_files) {
                std::string ldtbo = dtbo;
                std::transform(ldtbo.begin(), ldtbo.end(), ldtbo.begin(), ::tolower);
                if (matchesAny(ldtbo, rule.dtbo_keywords)) {
                    bool is_active = (active_dtoverlay == dtbo);
                    mdtbos.push_back({{"file", dtbo}, {"active", is_active}});
                }
            }
            mod["dtbos"] = mdtbos;

            // Summary stats
            int active_count = 0;
            for (auto& ov : movrs) if (ov.value("active", false)) active_count++;
            mod["active_overrides"] = active_count;
            mod["active_dtbos"] = (int)std::count_if(mdtbos.begin(), mdtbos.end(),
                [](const json& d) { return d.value("active", false); });

            // Only include module if it has nodes, overrides, or dtbos
            if (!mnodes.empty() || !movrs.empty() || !mdtbos.empty()) {
                modules.push_back(mod);
            }
        }

        // Uncategorized nodes
        json uncat_nodes = json::array();
        for (size_t ni = 0; ni < all_nodes.size(); ++ni) {
            if (used_node_indices.find((int)ni) == used_node_indices.end()) {
                json n = {{"path", all_nodes[ni].path},
                          {"compatible", all_nodes[ni].compatible},
                          {"status", all_nodes[ni].status},
                          {"properties", all_nodes[ni].properties}};
                uncat_nodes.push_back(n);
            }
        }
        if (!uncat_nodes.empty()) {
            modules.push_back({
                {"name", "其他"},
                {"color", "gray"},
                {"nodes", uncat_nodes},
                {"overrides", json::array()},
                {"dtbos", json::array()},
                {"active_overrides", 0},
                {"active_dtbos", 0}
            });
        }

        json data = json::object();
        data["modules"] = modules;
        data["total_nodes"] = (int)all_nodes.size();
        data["total_overrides"] = (int)all_overrides.size();
        data["active_dtoverlay"] = active_dtoverlay;
        data["dtbo_files"] = dtbo_files;

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

    server_->Post("/api/datasources/rtsp-probe", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            jsonResponse(res, ds_manager_->probeRtspUrls(body));
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
                std::string m3u = buildRtspVlcPlaylist(ds->name, ds->path);
                res.set_content(m3u, "audio/x-mpegurl");
                res.set_header("Content-Disposition",
                    "attachment; filename=\"" + safeAttachmentFileStem(ds->name) + ".m3u\"");
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

    server_->Post("/api/workers/stop-all",
        [this](const httplib::Request&, httplib::Response& res) {
            worker_manager_->stopAll();
            jsonResponse(res, ApiResponse::ok(nullptr, "所有 Worker 已停止"));
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
    // === On-demand preview sessions (spec §6) ===
    server_->Post("/api/preview/sessions",
        [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                const std::string worker_id = body.value("worker_id", "");
                if (worker_id.empty()) {
                    jsonResponse(res, 400, ApiResponse::error(
                        ErrorCode::PARAM_ERROR, "worker_id 必填"));
                    return;
                }
                if (!worker_manager_->exists(worker_id)) {
                    jsonResponse(res, 404, ApiResponse::error(
                        ErrorCode::NOT_FOUND, "Worker 不存在"));
                    return;
                }
                if (worker_manager_->getState(worker_id) != WorkerState::RUNNING) {
                    jsonResponse(res, 400, ApiResponse::error(
                        ErrorCode::WORKER_STATE_INVALID,
                        "Worker 必须处于 RUNNING 才能创建预览会话"));
                    return;
                }

                PreviewSessionConfig cfg;
                cfg.fps = body.value("fps", 15);
                cfg.quality = body.value("quality", 80);
                cfg.encoder = body.value("encoder", std::string("jpeg_taco"));

                auto result = preview_session_manager_->startSession(worker_id, cfg);
                if (!result.ok) {
                    int api_code = ErrorCode::INTERNAL_ERROR;
                    if (result.error_code == "ENCODER_RESOURCE_EXHAUSTED") {
                        api_code = ErrorCode::ENCODER_RESOURCE_EXHAUSTED;
                    } else if (result.error_code == "CONFIG_CONFLICT") {
                        api_code = ErrorCode::CONFIG_CONFLICT;
                    } else if (result.http_status == 400) {
                        api_code = ErrorCode::PARAM_ERROR;
                    }
                    // Prefer machine-readable code in message for HW exhaustion.
                    const std::string& msg =
                        result.error_code == "ENCODER_RESOURCE_EXHAUSTED"
                            ? result.error_code
                            : (result.error_message.empty()
                                   ? result.error_code
                                   : result.error_message);
                    jsonResponse(res, result.http_status,
                                 ApiResponse::error(api_code, msg));
                    return;
                }

                auto data = preview_session_manager_->getSession(result.session_id);
                if (data.is_null()) {
                    // Fallback to result fields (should not happen after ok START).
                    data = json{
                        {"session_id", result.session_id},
                        {"worker_id", result.worker_id},
                        {"state", "RUNNING"},
                        {"stream_url", result.stream_url},
                        {"fps", cfg.fps},
                        {"quality", cfg.quality},
                        {"encoder", cfg.encoder},
                    };
                }
                jsonResponse(res, ApiResponse::ok(data));
            } catch (const json::parse_error&) {
                jsonResponse(res, 400, ApiResponse::error(
                    ErrorCode::PARAM_ERROR, "JSON 解析失败"));
            } catch (const std::exception& e) {
                jsonResponse(res, 400, ApiResponse::error(
                    ErrorCode::PARAM_ERROR, e.what()));
            }
        });

    server_->Delete(R"(/api/preview/sessions/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            const std::string session_id = req.matches[1];
            // Idempotent success per §6.2.
            preview_session_manager_->stopSession(session_id);
            jsonResponse(res, ApiResponse::ok(json{
                {"session_id", session_id},
                {"state", "STOPPED"},
            }));
        });

    server_->Get("/api/preview/sessions",
        [this](const httplib::Request&, httplib::Response& res) {
            jsonResponse(res, ApiResponse::ok(
                preview_session_manager_->listSessions()));
        });

    server_->Get(R"(/api/preview/sessions/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            auto data = preview_session_manager_->getSession(req.matches[1]);
            if (data.is_null()) {
                jsonResponse(res, 404, ApiResponse::error(
                    ErrorCode::NOT_FOUND, "Session 不存在"));
                return;
            }
            jsonResponse(res, ApiResponse::ok(data));
        });

    server_->Get(R"(/api/preview/stream/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            std::string worker_id = req.matches[1];

            if (!worker_manager_->exists(worker_id)) {
                jsonResponse(res, 404, ApiResponse::error(ErrorCode::NOT_FOUND, "Worker 不存在"));
                return;
            }

            // Prefer on-demand session encoder; legacy static JPEG_PREVIEW still allowed.
            const bool session_active = preview_session_manager_ &&
                preview_session_manager_->hasActiveSession(worker_id);
            const bool legacy_jpeg = consumer_manager_->hasJpegPreview(worker_id);
            if (!session_active && !legacy_jpeg) {
                jsonResponse(res, 400, ApiResponse::error(ErrorCode::PREVIEW_UNAVAILABLE,
                    "无可用预览（请先 POST /api/preview/sessions 或启用 JPEG_PREVIEW）"));
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

    // Real stitcher layout (§13.1) — slots from view_slots_, not workers[] order
    server_->Get("/api/preview/layout",
        [this](const httplib::Request&, httplib::Response& res) {
            jsonResponse(res, preview_service_->getLayout());
        });

    server_->Get("/api/preview/fps", [this](const httplib::Request&, httplib::Response& res) {
        jsonResponse(res, ApiResponse::ok(json{{"fps", preview_service_->getTargetFps()}}));
    });

    server_->Post("/api/preview/fps", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            int fps = body.value("fps", 25);
            preview_service_->setTargetFps(fps);
            jsonResponse(res, ApiResponse::ok(json{{"fps", fps}}, "帧率已设置"));
        } catch (...) {
            jsonResponse(res, 400, ApiResponse::error(ErrorCode::PARAM_ERROR, "参数错误"));
        }
    });

    // === Channel FPS stats ===
    server_->Get("/api/preview/channel-fps",
        [this](const httplib::Request&, httplib::Response& res) {
            jsonResponse(res, ApiResponse::ok(preview_service_->getChannelFps()));
        });

    // === Composite config (browser JPEG target_fps/quality; not IDS FPS) ===
    server_->Get("/api/preview/composite/config",
        [this](const httplib::Request&, httplib::Response& res) {
            jsonResponse(res, ApiResponse::ok(json{
                {"target_fps", preview_service_->getTargetFps()},
                {"quality", 60},
            }));
        });

    server_->Put("/api/preview/composite/config",
        [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                if (body.contains("target_fps")) {
                    preview_service_->setTargetFps(body["target_fps"].get<int>());
                }
                // quality reserved for Phase 3 encoder path; accepted but not applied yet.
                jsonResponse(res, ApiResponse::ok(json{
                    {"target_fps", preview_service_->getTargetFps()},
                    {"quality", body.value("quality", 60)},
                }, "Composite 配置已更新"));
            } catch (...) {
                jsonResponse(res, 400, ApiResponse::error(ErrorCode::PARAM_ERROR, "参数错误"));
            }
        });

    // === Composite preview availability check (lightweight JSON, no frame needed) ===
    server_->Get("/api/preview/composite/available",
        [this](const httplib::Request&, httplib::Response& res) {
            bool avail = preview_service_->hasCompositePreview();
            jsonResponse(res, ApiResponse::ok(json{{"available", avail}}));
        });

    // === Composite preview (stitched multi-channel) ===
    server_->Get("/api/preview/composite/stream",
        [this](const httplib::Request&, httplib::Response& res) {
            if (!preview_service_->hasCompositePreview()) {
                jsonResponse(res, 503, ApiResponse::error(ErrorCode::PREVIEW_UNAVAILABLE,
                    "Composite preview not available (no stitcher connected)"));
                return;
            }

            res.set_header("Cache-Control", "no-cache");
            res.set_content_provider(
                "multipart/x-mixed-replace; boundary=frame",
                [this](size_t /*offset*/, httplib::DataSink& sink) {
                    preview_service_->streamCompositeMjpeg(
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

    server_->Get("/api/preview/composite/snapshot",
        [this](const httplib::Request&, httplib::Response& res) {
            if (!preview_service_->hasCompositePreview()) {
                jsonResponse(res, 503, ApiResponse::error(ErrorCode::PREVIEW_UNAVAILABLE,
                    "Composite preview not available"));
                return;
            }
            auto frame = preview_service_->compositeSnapshot();
            if (frame.empty()) {
                jsonResponse(res, 404, ApiResponse::error(ErrorCode::PREVIEW_UNAVAILABLE,
                    "No composite frame available yet"));
                return;
            }
            res.set_content(std::string(frame.begin(), frame.end()), "image/jpeg");
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
