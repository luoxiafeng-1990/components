/**
 * @file test_module_main.cpp
 * @brief 插件化测试入口（CLI11 版本）
 *
 * 架构：所有功能（vdec、pp、save、display、npu）均为 IOptionPlugin，
 * 地位平等，统一注册为子命令。用户可在一条命令中组合多个子命令：
 *
 *   ./qa_cases vdec --file video.mp4 display --vendor taco npu --model m.nb
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "common/IOptionPlugin.hpp"
#include "common/ExecuteMode.hpp"
#include "common/PerfReportPlugin.hpp"
#include "common/third_party/CLI11.hpp"
#include "display/DisplayPlugin.hpp"
#include "npu/NpuPlugin.hpp"
#include "vdec/VdecPlugin.hpp"
#include "venc/VencPlugin.hpp"
#include "pp/PPPlugin.hpp"
#include "save/SavePlugin.hpp"
#include "opencv/OpencvPlugin.hpp"
#include "preview/PreviewPlugin.hpp"
#include "memleak/MemleakPlugin.hpp"
#include "logconfig/LogConfigPlugin.hpp"
#include "cpu/CpuPlugin.hpp"
#include "common/Logger.hpp"

#include "productionline/line/WorkerSyncCoordinator.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"

#include <iostream>
#include <memory>
#include <vector>
#include <string>
#include <thread>

int main(int argc, char* argv[]) {
    LoggerGuard logger_guard;

#ifdef PACKAGE_VERSION
    const std::string version_str = PACKAGE_VERSION;
#else
    const std::string version_str = "unknown";
#endif
    const std::string version_detail = "qa_cases v" + version_str
        + " (built " + __DATE__ + " " + __TIME__ + ")";

    CLI::App app{"qa_cases - Component 测试套件 v" + version_str};
    app.set_version_flag("-v,--version", version_detail, "显示版本号及编译时间并退出");
    app.require_subcommand(1, 0);

    bool show_topology = false;
    app.add_flag("--topology", show_topology, "执行完毕后打印组件拓扑关系");

    // ── 性能报告全局选项 ──
    test::PerfReportController perf_ctrl;
    perf_ctrl.registerGlobalOptions(app);

    // ── 1. 创建所有插件 ──
    auto vdec_plugin    = std::make_unique<test::vdec::VdecPlugin>();
    auto venc_plugin    = std::make_unique<test::venc::VencPlugin>();
    auto pp_plugin      = std::make_unique<test::pp::PPPlugin>();
    auto save_plugin    = std::make_unique<test::save::SavePlugin>();
    auto display_plugin = std::make_unique<test::display::DisplayPlugin>();
    auto npu_plugin     = std::make_unique<test::npu::NpuPlugin>();
    auto opencv_plugin  = std::make_unique<test::opencv::OpencvPlugin>();
    auto preview_plugin = std::make_unique<test::preview::PreviewPlugin>();
    auto memleak_plugin   = std::make_unique<test::memleak::MemleakPlugin>();
    auto logconfig_plugin = std::make_unique<test::logconfig::LogConfigPlugin>();
    auto cpu_plugin       = std::make_unique<test::cpu::CpuPlugin>();

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
    register_plugin(venc_plugin.get());
    register_plugin(pp_plugin.get());
    register_plugin(save_plugin.get());
    register_plugin(display_plugin.get());
    register_plugin(npu_plugin.get());
    register_plugin(opencv_plugin.get());
    register_plugin(preview_plugin.get());
    register_plugin(memleak_plugin.get());
    register_plugin(logconfig_plugin.get());
    register_plugin(cpu_plugin.get());

    // ── 3. 解析命令行（支持多子命令） ──
    // 对所有子命令开启 fallthrough：子命令未识别的选项（如 --perf、--json、--topology）
    // 会自动回退到父 app 解析，使全局选项可放在命令行任意位置
    for (auto* sub : app.get_subcommands({})) {
        sub->fallthrough();
    }
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

    // ── --perf-only: 静默所有日志（仅保留 stdout 性能表格） ──
    if (perf_ctrl.isPerfOnly()) {
        log4cplus::Logger::getRoot().setLogLevel(log4cplus::OFF_LOG_LEVEL);
    }

    // ── 开始性能采集 ──
    perf_ctrl.beginCapture(argc, argv);

    if (actived_plugins.empty()) {
        std::cout << app.help() << std::endl;
        return 0;
    }

    // ── 5. 预处理（list、校验等） ──
    for (auto* p : actived_plugins) {
        int rc = p->handlePreActions();
        if (rc >= 0) return rc;
    }

    // ── 5.5 UTILITY 插件分流 ──
    // UTILITY 类型的插件直接调用 run()，不走消费策略
    for (auto* p : actived_plugins) {
        if (p->getCategory() == test::PluginCategory::UTILITY) {
            int ret = p->run();
            // UTILITY 插件（如 cpu）有自己的报告机制，直接返回
            return ret;
        }
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

    // ── 7.5. 将 applyTo 阶段的伴随设置继承到管线配置 ──
    for (auto& pc : pipeline_configs) {
        pc.consumer_type.inheritCompanionSettings(config.consumer_type);
        // 将 --perf-file 路径传入消费配置（consumeLoop 会周期性写入快照）
        if (!perf_ctrl.getPerfFilePath().empty()) {
            pc.consumer_type.perf_file_path = perf_ctrl.getPerfFilePath();
        }
    }

    // ── 8. 从 config 推断执行模式，统一执行 ──
    // RAII: 无论哪条 return 路径，析构时自动打印拓扑
    struct TopologyGuard {
        bool enabled;
        ~TopologyGuard() { if (enabled) ComponentTopology::getInstance().printTopology(); }
    } topo_guard{show_topology};
    // 合并共享 config 与管线 config 的 flags，确保管线特有的标志（如 CONSUME_SAVE_ENCODED）不丢失
    uint32_t flags = test::ExecuteMode::buildConsumeFlags(config);
    flags |= test::ExecuteMode::buildConsumeFlags(pipeline_configs[0]);

    // ── 辅助 lambda：统一处理 result → printResult → perf report → return ──
    auto finalize_result = [&](const consumer::ConsumeResult& result,
                               const std::string& name) -> int {
        consumer::BufferConsumerService::printResult(name, result);
        if (perf_ctrl.isEnabled()) {
            std::string module = actived_plugins.empty() ? "unknown" : actived_plugins[0]->getName();
            auto report = perf_ctrl.buildReport(result, name, module);
            int exit_code = perf_ctrl.finalize(report);
            // 基线对比检测到性能回归时返回 exit_code=2
            if (exit_code == 2) return 2;
        }
        return result.getOverallResult() ? 0 : 1;
    };

    // CHANNEL COMPARE
    if (config.consumer_type.compare.enable_channel_compare) {
        auto result = test::ExecuteMode::channelCompare(pipeline_configs[0], test_name + " (CHANNEL_COMPARE)");
        return finalize_result(result, test_name);
    }

    const bool compare_enabled = config.consumer_type.compare.enable_psnr
                              || config.consumer_type.compare.enable_ssim;

    // 编码质量对比：单路 FFMPEG_ENCODE + PSNR/SSIM（源 YUV vs 编码→软解，非解码双路 COMPARE）
    if (pipeline_configs.size() == 1
        && pipeline_configs[0].global.worker_type == WorkerType::FFMPEG_ENCODE
        && (config.consumer_type.compare.enable_psnr || config.consumer_type.compare.enable_ssim)) {
        auto result = test::venc::runEncodeQualityCompare(
            pipeline_configs[0], config, test_name + " (ENC_COMPARE)");
        return finalize_result(result, test_name);
    }

    // 单路编码 + 显示：编码 -> 解码 -> 显示（码流不可直接显示）
    if (pipeline_configs.size() == 1
        && pipeline_configs[0].global.worker_type == WorkerType::FFMPEG_ENCODE
        && config.consumer_type.display.enable
        && !config.consumer_type.compare.enable_psnr
        && !config.consumer_type.compare.enable_ssim) {
        auto result = test::venc::runEncodeDecodeDisplay(
            pipeline_configs[0], config, test_name);
        return finalize_result(result, test_name);
    }

    // PARALLEL COMPARE (N 组 hw vs sw 并发对比)
    if (compare_enabled && pipeline_configs.size() > 2 && pipeline_configs.size() % 2 == 0)
    {
        const int groups = static_cast<int>(pipeline_configs.size()) / 2;

        std::vector<std::thread> threads(groups);
        std::vector<consumer::ConsumeResult> results(groups);

        std::cout << "\n═══════════════════════════════════════════════════════\n"
                  << "  " << test_name << " (PARALLEL COMPARE x" << groups << ")\n"
                  << "═══════════════════════════════════════════════════════\n\n";

        for (int i = 0; i < groups; i++) {
            threads[i] = std::thread([&, i]() {
                std::vector<WorkerConfig> pair = {
                    pipeline_configs[i * 2],
                    pipeline_configs[i * 2 + 1]
                };
                auto name = test_name + " [" + std::to_string(i + 1)
                          + "/" + std::to_string(groups) + "]";
                results[i] = test::ExecuteMode::compare(pair, flags, name);
            });
        }

        for (auto& t : threads) t.join();

        bool all_ok = true;
        int total_frames = 0;
        for (int i = 0; i < groups; i++) {
            if (!results[i].getOverallResult()) all_ok = false;
            total_frames += results[i].frames_consumed;
            std::cout << "  [" << (i + 1) << "/" << groups << "] "
                      << (results[i].getOverallResult() ? "PASSED" : "FAILED")
                      << " (" << results[i].frames_consumed << " frames)\n";
        }
        std::cout << "\n  Summary: " << (all_ok ? "ALL PASSED" : "SOME FAILED")
                  << " (" << total_frames << " total frames)\n";
        return all_ok ? 0 : 1;
    }

    if (pipeline_configs.size() >= 1 &&
        pipeline_configs[0].consumer_type.opencv.enable) {

        auto result = test::ExecuteMode::single(pipeline_configs[0], flags, test_name);
        return finalize_result(result, test_name);
    }

    // COMPARE (PSNR/SSIM, 2 configs = hw vs sw)
    if (compare_enabled && pipeline_configs.size() == 2) {
        auto result = test::ExecuteMode::compare(pipeline_configs, flags, test_name + " (COMPARE)");
        return finalize_result(result, test_name);
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
                if (!r.getOverallResult()) all_ok = false;
                total_frames += r.frames_consumed;
                std::cout << "  [" << (i + 1) << "/" << pipeline_configs.size() << "] "
                          << (r.getOverallResult() ? "PASSED" : "FAILED") << " (" << r.frames_consumed << " frames)\n";
            }
            std::cout << "\n  Summary: " << (all_ok ? "ALL PASSED" : "SOME FAILED")
                      << " (" << total_frames << " total frames)\n";
            return all_ok ? 0 : 1;
        }

        auto result = test::ExecuteMode::parallel(pipeline_configs, flags,
            test_name + " (PARALLEL x" + std::to_string(pipeline_configs.size()) + ")");
        return finalize_result(result, test_name);
    }

    // SINGLE
    auto result = test::ExecuteMode::single(pipeline_configs[0], flags, test_name);
    return finalize_result(result, test_name);
}
