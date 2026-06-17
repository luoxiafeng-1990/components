#ifndef WEBUI_COMPONENTS_BRIDGE_HPP
#define WEBUI_COMPONENTS_BRIDGE_HPP

/**
 * @brief Components 桥接层 —— API 参数 → WorkerConfig 构建
 *
 * 纯配置构建器：将 WebUI API 参数转换为 qa_cases 等价的 WorkerConfig，
 * 由 WorkerManager 收集后统一交给 BufferConsumerService(PARALLEL) 执行。
 */

#include "ApiTypes.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"
#include <string>
#include <vector>

namespace webui {

class PreviewService;

struct BuildResult {
    bool success = false;
    std::string error;
    std::string description;
    WorkerConfig config;
    uint32_t flags = 0;
};

/**
 * @brief 从 WebUI API 参数构建 WorkerConfig（与 qa_cases 完全一致的插件解析流程）
 */
BuildResult buildWorkerConfig(
    const DataSourceInfo& ds,
    const ApiDecoderConfig& decoder,
    const std::vector<ConsumerInfo>& consumers,
    PreviewService* preview_service,
    const std::string& worker_id);

} // namespace webui

#endif // WEBUI_COMPONENTS_BRIDGE_HPP
