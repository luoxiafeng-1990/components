/**
 * @file test_module_main.cpp
 * @brief 插件化测试入口（CLI11 版本）
 *
 * 架构：所有功能（vdec、pp、record、writer、display、npu）均为 IOptionPlugin，
 * 地位平等，统一注册为子命令。用户可在一条命令中组合多个子命令：
 *
 *   ./qa_cases vdec --file video.mp4 display --display-mode vo npu --model m.nb
 *
 * 流程：
 *   1. 为每个插件创建独立子命令并注册选项
 *   2. CLI11 允许多子命令，统一解析
 *   3. 遍历所有被解析到的插件：handlePreActions → applyTo → buildPipelineConfigs
 *   4. 从 config 推断执行模式（SINGLE / COMPARE / PARALLEL）
 *   5. 调用 ExecuteMode 执行
 *
 * @version 8.0 - 插件一视同仁，支持多子命令组合
 */

#include "common/IOptionPlugin.hpp"
#include "common/ExecuteMode.hpp"
#include "common/third_party/CLI11.hpp"
#include "display/DisplayPlugin.hpp"
#include "npu/NpuPlugin.hpp"
#include "vdec/VdecPlugin.hpp"
#include "pp/PPPlugin.hpp"
#include "record/RecordPlugin.hpp"
#include "writer/WriterPlugin.hpp"
#include "common/Logger.hpp"

#include <iostream>
#include <memory>
#include <vector>
#include <string>

int main(int argc, char* argv[]) {
    LoggerGuard logger_guard;

    CLI::App app{"qa_cases - 测试套件"};
    app.require_subcommand(1, 0);

    // ── 1. 创建所有插件 ──
    auto vdec_plugin    = std::make_unique<test::vdec::VdecPlugin>();
    auto pp_plugin      = std::make_unique<test::pp::PPPlugin>();
    auto record_plugin  = std::make_unique<test::record::RecordPlugin>();
    auto writer_plugin  = std::make_unique<test::writer::WriterPlugin>();
    auto display_plugin = std::make_unique<test::display::DisplayPlugin>();
    auto npu_plugin     = std::make_unique<test::npu::NpuPlugin>();

    // ── 2. 统一注册：每个插件 = 一个子命令 ──
    struct PluginEntry {
        test::IOptionPlugin* plugin;
        CLI::App* cmd;
    };
    std::vector<PluginEntry> all_plugin_entries;

    auto register_plugin = [&](test::IOptionPlugin* p) {
        auto* cmd = app.add_subcommand(p->getName(), p->getDescription());
        p->registerOptions(*cmd);
        all_plugin_entries.push_back({p, cmd});
    };

    register_plugin(vdec_plugin.get());
    register_plugin(pp_plugin.get());
    register_plugin(record_plugin.get());
    register_plugin(writer_plugin.get());
    register_plugin(display_plugin.get());
    register_plugin(npu_plugin.get());

    // ── 3. 解析命令行（支持多子命令） ──
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    // ── 4. 收集所有被解析到的插件 ──
    std::vector<test::IOptionPlugin*> actived_plugins;
    for (auto& entry : all_plugin_entries) {
        if (entry.cmd->parsed())
            actived_plugins.push_back(entry.plugin);
    }

    if (actived_plugins.empty()) {
        std::cout << app.help() << std::endl;
        return 0;
    }

    // ── 5. 预处理（list、校验等） ──
    for (auto* p : actived_plugins) {
        int rc = p->handlePreActions();
        if (rc >= 0) return rc;
    }

    // ── 6. 所有被解析的插件依次 applyTo → 构建共享 WorkerConfig ──
    WorkerConfig config;
    for (auto* p : actived_plugins) {
        p->applyTo(config);
    }

    // ── 7. 获取管线配置（第一个返回非空 pipeline 的插件为驱动插件） ──
    std::vector<WorkerConfig> pipeline_configs;
    std::string test_name;
    for (auto* p : actived_plugins) {
        auto configs = p->buildPipelineConfigs(config);
        if (!configs.empty()) {
            pipeline_configs = std::move(configs);
            test_name = p->getTestName();
            break;
        }
    }

    if (pipeline_configs.empty()) {
        std::cerr << "Error: No recognized module or insufficient parameters.\n" << std::endl;
        return 1;
    }

    // ── 8. 从 config 推断执行模式，统一执行 ──
    uint32_t flags = test::ExecuteMode::buildConsumeFlags(config);

    // CHANNEL COMPARE
    if (config.consumer_type.compare.enable_channel_compare) {
        auto result = test::ExecuteMode::channelCompare(pipeline_configs[0], test_name + " (CHANNEL_COMPARE)");
        return result.success ? 0 : 1;
    }

    // COMPARE (PSNR/SSIM, 2 configs = hw vs sw)
    if ((config.consumer_type.compare.enable_psnr || config.consumer_type.compare.enable_ssim)
        && pipeline_configs.size() == 2) {
        auto result = test::ExecuteMode::compare(pipeline_configs, flags, test_name + " (COMPARE)");
        consumer::BufferConsumerService::printResult(test_name, result);
        return result.success ? 0 : 1;
    }

    // PARALLEL / BATCH (configs.size() > 1)
    if (pipeline_configs.size() > 1) {
        bool is_batch = (config.consumer_type.save_raw.enable || config.consumer_type.save_encoded.enable)
            && !config.consumer_type.display.enable
            && !config.consumer_type.compare.enable_psnr && !config.consumer_type.compare.enable_ssim;
        if (is_batch) {
            bool all_ok = true;
            int total_frames = 0;
            std::cout << "\n═══════════════════════════════════════════════════════\n"
                      << "  " << test_name << " (BATCH x" << pipeline_configs.size() << ")\n"
                      << "═══════════════════════════════════════════════════════\n\n";
            for (size_t i = 0; i < pipeline_configs.size(); i++) {
                auto r = test::ExecuteMode::single(pipeline_configs[i],
                    test::ExecuteMode::buildConsumeFlags(pipeline_configs[i]),
                    test_name + " [" + std::to_string(i + 1) + "/" + std::to_string(pipeline_configs.size()) + "]");
                if (!r.success) all_ok = false;
                total_frames += r.frames_consumed;
                std::cout << "  [" << (i + 1) << "/" << pipeline_configs.size() << "] "
                          << (r.success ? "PASSED" : "FAILED") << " (" << r.frames_consumed << " frames)\n";
            }
            std::cout << "\n  Summary: " << (all_ok ? "ALL PASSED" : "SOME FAILED")
                      << " (" << total_frames << " total frames)\n";
            return all_ok ? 0 : 1;
        }

        auto result = test::ExecuteMode::parallel(pipeline_configs, flags,
            test_name + " (PARALLEL x" + std::to_string(pipeline_configs.size()) + ")");
        consumer::BufferConsumerService::printResult(test_name, result);
        return result.success ? 0 : 1;
    }

    // SINGLE
    auto result = test::ExecuteMode::single(pipeline_configs[0], flags, test_name);
    consumer::BufferConsumerService::printResult(test_name, result);
    return result.success ? 0 : 1;
}
