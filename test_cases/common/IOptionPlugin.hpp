/**
 * @file IOptionPlugin.hpp
 * @brief 命令行选项插件接口（CLI11 版本）
 *
 * 所有功能模块（vdec、pp、record、writer、display、npu 等）
 * 都实现此接口，通过 CLI11 注册选项、解析参数、构建 WorkerConfig。
 *
 * 架构说明：
 * - main() 创建 CLI::App 及子命令
 * - 每个插件通过 registerOptions() 将选项注册到对应子命令
 * - CLI11 统一解析后，选项值自动填充到插件成员变量
 * - 插件通过 applyTo() 注入配置，buildPipelineConfigs() 构建执行管线
 *
 * @version 7.0 - 从 getopt_long 迁移到 CLI11
 */

#ifndef IOPTION_PLUGIN_HPP
#define IOPTION_PLUGIN_HPP

#include <string>
#include <vector>
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"

namespace CLI { class App; }

namespace test {

/**
 * @brief 插件分类
 *
 * PIPELINE  — 数据消费型插件（vdec / pp / record 等），走消费策略执行
 * UTILITY   — 独立工具型插件（memleak / logconfig 等），由 main 直接调用 run()
 */
enum class PluginCategory {
    PIPELINE,
    UTILITY
};

class IOptionPlugin {
public:
    virtual ~IOptionPlugin() = default;

    virtual std::string getName() const = 0;
    virtual std::string getDescription() const { return ""; }

    /**
     * @brief 返回插件分类，默认为 PIPELINE
     */
    virtual PluginCategory getCategory() const { return PluginCategory::PIPELINE; }

    /**
     * @brief UTILITY 插件的执行入口
     * @return 退出码（0 成功）
     */
    virtual int run() { return 0; }

    /**
     * @brief 向 CLI::App（子命令）注册本插件的命令行选项
     *
     * CLI11 解析完成后，注册时绑定的成员变量自动填充。
     * 不同子命令有独立的选项命名空间，不会冲突。
     */
    virtual void registerOptions(CLI::App& app) = 0;

    /**
     * @brief 将解析到的参数注入共享 WorkerConfig
     */
    virtual void applyTo(WorkerConfig& config) const = 0;

    virtual void listTests() const {}

    /**
     * @brief 构建管线配置
     *
     * 根据插件解析状态和共享 config，构建用于执行的 WorkerConfig 列表。
     * - 返回空表示本插件没有可执行的工作
     * - 返回 1 个 → SINGLE
     * - 返回 2 个 → COMPARE (hw vs sw)
     * - 返回 N 个 → PARALLEL / BATCH
     */
    virtual std::vector<WorkerConfig> buildPipelineConfigs(const WorkerConfig& shared_config) {
        (void)shared_config; return {};
    }

    virtual std::string getTestName() const { return ""; }

    /**
     * @brief 解析后的预处理动作（list、校验等）
     * @return >=0 表示应退出（返回值为退出码），-1 表示继续执行
     */
    virtual int handlePreActions() { return -1; }
};

} // namespace test

#endif // IOPTION_PLUGIN_HPP
