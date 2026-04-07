#ifndef WEBUI_COMPONENTS_BRIDGE_HPP
#define WEBUI_COMPONENTS_BRIDGE_HPP

/**
 * @brief Components 桥接层 —— 通过 qa_cases 子进程调用组件库
 *
 * 核心思路：WebUI 不再直接链接 libcomponents，而是将用户配置
 * 转换为 qa_cases 命令行参数，以子进程方式执行。
 * 
 * 优势：
 *   - qa_cases 已完美覆盖所有场景（解码、显示、保存、NPU 等）
 *   - 无需手动创建 vendor extension、codec_params 等复杂对象
 *   - 与 qa_cases 行为 100% 一致
 *   - webui_server 不再依赖 libcomponents 和硬件库
 */

#include "ApiTypes.hpp"
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace webui {

class PreviewService;

/**
 * @brief 将 WebUI 配置转换为 qa_cases 命令行参数
 */
namespace bridge {

    std::vector<std::string> buildQaCasesArgs(
        const DataSourceInfo& ds,
        const ApiDecoderConfig& decoder,
        const std::vector<ConsumerInfo>& consumers);

    std::string argsToString(const std::vector<std::string>& args);

} // namespace bridge

/**
 * @brief 单个 Worker 运行实例（管理 qa_cases 子进程）
 */
class ComponentsWorkerInstance {
public:
    using OutputCallback = std::function<void(const std::string& line)>;

    ComponentsWorkerInstance();
    ~ComponentsWorkerInstance();

    bool start(const DataSourceInfo& ds,
               const ApiDecoderConfig& decoder,
               const std::vector<ConsumerInfo>& consumers,
               PreviewService* preview_service,
               const std::string& worker_id);

    void stop();
    bool isRunning() const;

    int64_t getDecodedFrames() const;
    int64_t getDroppedFrames() const;
    double getFps() const;
    double getUptimeSeconds() const;
    std::string getLastOutput() const;
    std::string getCommandLine() const;

    void setOutputCallback(OutputCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace webui

#endif // WEBUI_COMPONENTS_BRIDGE_HPP
