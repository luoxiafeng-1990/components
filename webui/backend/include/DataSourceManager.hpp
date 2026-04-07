#ifndef WEBUI_DATASOURCE_MANAGER_HPP
#define WEBUI_DATASOURCE_MANAGER_HPP

#include "ApiTypes.hpp"
#include <mutex>
#include <unordered_map>
#include <functional>

namespace webui {

class ConfigStore;

class DataSourceManager {
public:
    explicit DataSourceManager(ConfigStore& store);
    ~DataSourceManager() = default;

    ApiResponse list() const;
    ApiResponse add(const json& body);
    ApiResponse update(const std::string& id, const json& body);
    ApiResponse remove(const std::string& id);
    ApiResponse probe(const std::string& id) const;

    // 供 WorkerManager 查询
    std::optional<DataSourceInfo> get(const std::string& id) const;
    bool markInUse(const std::string& id, bool in_use);
    bool isInUse(const std::string& id) const;

private:
    std::string generateId() const;
    std::string nowISO8601() const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, DataSourceInfo> datasources_;
    ConfigStore& config_store_;
};

} // namespace webui

#endif // WEBUI_DATASOURCE_MANAGER_HPP
