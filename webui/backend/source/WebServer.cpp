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

    server_->Get("/api/preview/fps", [this](const httplib::Request&, httplib::Response& res) {
        jsonResponse(res, ApiResponse::ok(json{{"fps", preview_service_->getTargetFps()}}));
    });

    server_->Post("/api/preview/fps", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            int fps = body.value("fps", 15);
            preview_service_->setTargetFps(fps);
            jsonResponse(res, ApiResponse::ok(json{{"fps", fps}}, "帧率已设置"));
        } catch (...) {
            jsonResponse(res, 400, ApiResponse::error(ErrorCode::PARAM_ERROR, "参数错误"));
        }
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
