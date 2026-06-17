#include "../include/ConfigStore.hpp"
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace webui {

ConfigStore::ConfigStore(const std::string& config_path)
    : config_path_(config_path.empty() ? resolveConfigPath() : config_path)
{
    config_data_ = {
        {"version", "1.0"},
        {"datasources", json::array()},
        {"workers", json::array()},
        {"consumers", json::object()}
    };
}

std::string ConfigStore::resolveConfigPath() const {
    const char* home = std::getenv("HOME");
    std::string dir = home ? std::string(home) + "/.components" : "/tmp/.components";
    std::filesystem::create_directories(dir);
    return dir + "/webui_config.json";
}

bool ConfigStore::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream ifs(config_path_);
    if (!ifs.is_open()) return false;

    try {
        ifs >> config_data_;
        return true;
    } catch (const json::parse_error&) {
        return false;
    }
}

bool ConfigStore::save() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::filesystem::create_directories(
        std::filesystem::path(config_path_).parent_path());

    std::ofstream ofs(config_path_);
    if (!ofs.is_open()) return false;

    ofs << config_data_.dump(2);
    return ofs.good();
}

json ConfigStore::getAll() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_data_;
}

void ConfigStore::setAll(const json& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_data_ = data;
}

json ConfigStore::getDatasources() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_data_.value("datasources", json::array());
}

void ConfigStore::setDatasources(const json& ds) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_data_["datasources"] = ds;
    }
    save();
}

json ConfigStore::getWorkers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_data_.value("workers", json::array());
}

void ConfigStore::setWorkers(const json& wk) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_data_["workers"] = wk;
    }
    save();
}

json ConfigStore::getConsumers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_data_.value("consumers", json::object());
}

void ConfigStore::setConsumers(const json& cs) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_data_["consumers"] = cs;
    }
    save();
}

ApiResponse ConfigStore::exportConfig() const {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&t), "%FT%TZ");

    json data = getAll();
    data["exported_at"] = oss.str();
    return ApiResponse::ok(data);
}

ApiResponse ConfigStore::importConfig(const json& body, const std::string& mode) {
    if (mode == "replace") {
        setAll(body);
    } else if (mode == "merge") {
        std::lock_guard<std::mutex> lock(mutex_);
        if (body.contains("datasources")) {
            auto& existing = config_data_["datasources"];
            for (auto& ds : body["datasources"]) {
                existing.push_back(ds);
            }
        }
        if (body.contains("workers")) {
            auto& existing = config_data_["workers"];
            for (auto& wk : body["workers"]) {
                existing.push_back(wk);
            }
        }
    } else {
        return ApiResponse::error(ErrorCode::PARAM_ERROR, "mode must be 'replace' or 'merge'");
    }
    save();
    return ApiResponse::ok(nullptr, "配置导入成功");
}

} // namespace webui
