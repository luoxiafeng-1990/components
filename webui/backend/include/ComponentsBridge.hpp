#ifndef WEBUI_COMPONENTS_BRIDGE_HPP
#define WEBUI_COMPONENTS_BRIDGE_HPP

/**
 * @brief Components 桥接层 —— 同进程调用 libcomponents
 *
 * 核心思路：webui_server 直接链接 libcomponents，在同一进程内
 * 构造 WorkerConfig 并调用 BufferConsumerService。
 * JPEG 预览帧通过内存回调直传 PreviewService，无需 IPC。
 *
 * 等价于 qa_cases 的 test_module_main.cpp，但以后台线程方式运行，
 * 由 HTTP API 控制生命周期。
 */

#include "ApiTypes.hpp"
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace webui {

class PreviewService;

/**
 * @brief 单个 Worker 运行实例（同进程内调用 libcomponents）
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
