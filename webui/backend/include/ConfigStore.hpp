#ifndef WEBUI_CONFIG_STORE_HPP
#define WEBUI_CONFIG_STORE_HPP

#include "ApiTypes.hpp"
#include <string>
#include <mutex>

namespace webui {

class ConfigStore {
public:
    explicit ConfigStore(const std::string& config_path = "");
    ~ConfigStore() = default;

    bool load();
    bool save() const;

    json getAll() const;
    void setAll(const json& data);

    // 分段读写
    json getDatasources() const;
    void setDatasources(const json& ds);

    json getWorkers() const;
    void setWorkers(const json& wk);

    json getConsumers() const;
    void setConsumers(const json& cs);

    std::string getConfigPath() const { return config_path_; }

    // 导入导出
    ApiResponse exportConfig() const;
    ApiResponse importConfig(const json& body, const std::string& mode);

private:
    std::string resolveConfigPath() const;

    mutable std::mutex mutex_;
    std::string config_path_;
    json config_data_;
};

} // namespace webui

#endif // WEBUI_CONFIG_STORE_HPP
