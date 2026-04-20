/**
 * @file ComponentsBridge.cpp
 * @brief Components 桥接层 —— API 参数 → WorkerConfig 构建
 *
 * 等价于 test_module_main.cpp 的步骤 1-7.5：
 *   1. 从 API 参数构造 CLI argv
 *   2. 创建插件实例 + CLI11 解析
 *   3. applyTo → buildPipelineConfigs → inheritCompanionSettings
 *   4. 设置 on_frame 回调
 *   5. buildConsumeFlags
 *
 * 不再内部启动 BufferConsumerService，由 WorkerManager 统一 PARALLEL 启动。
 */

#include "../include/ComponentsBridge.hpp"
#include "../include/PreviewService.hpp"

#include "common/ExecuteMode.hpp"
#include "common/IOptionPlugin.hpp"
#include "common/third_party/CLI11.hpp"
#include "vdec/VdecPlugin.hpp"
#include "display/DisplayPlugin.hpp"
#include "npu/NpuPlugin.hpp"
#include "preview/PreviewPlugin.hpp"
#include "save/SavePlugin.hpp"
#include "pp/PPPlugin.hpp"
#include "opencv/OpencvPlugin.hpp"
#include "venc/VencPlugin.hpp"

#include "consumptionline/core/BufferConsumerService.hpp"

#include <iostream>
#include <sstream>
#include <unordered_map>

namespace webui {

// ============================================================
// API 参数 → CLI argv 构建（与旧版完全一致）
// ============================================================

namespace {

static const std::unordered_map<std::string, std::string> kKeyToCliFlag = {
    {"target_fps",       "--fps"},
    {"encoder_name",     "--encoder"},
    {"model_path",       "--model"},
    {"conf_threshold",   "--conf-threshold"},
    {"nms_threshold",    "--nms-threshold"},
    {"output_path",      "--output"},
    {"view_type",        "--view-type"},
    {"screen_width",     "--screen-width"},
    {"screen_height",    "--screen-height"},
    {"frame_width",      "--frame-width"},
    {"frame_height",     "--frame-height"},
    {"osd_fps",          "--osd-fps"},
    {"slot_assignment",  "--slot-assignment"},
    {"main_ratio",       "--main-ratio"},
    {"npu_core",         "--npu-core"},
    {"physical_addr",    "--physical-addr"},
    {"draw_detections",  "--draw-detections"},
    {"inference_interval", "--inference-interval"},
    {"min_psnr",         "--min-psnr"},
    {"min_ssim",         "--min-ssim"},
    {"max_frames",       "--max-frames"},
    {"buffer_count",     "--buffer-count"},
    {"decode_threads",   "--threads"},
    {"input_format",     "--input-format"},
    {"color_std",        "--color-std"},
    {"all_formats",      "--all-formats"},
    {"all_rgb",          "--all-rgb"},
    {"all_yuv",          "--all-yuv"},
};

static const std::unordered_map<int, std::string> kConsumerTypeToSubcmd = {
    {static_cast<int>(ConsumerType::DISPLAY),       "display"},
    {static_cast<int>(ConsumerType::NPU_INFERENCE),  "npu"},
    {static_cast<int>(ConsumerType::JPEG_PREVIEW),   "preview"},
    {static_cast<int>(ConsumerType::SAVE_RAW),       "save"},
    {static_cast<int>(ConsumerType::SAVE_ENCODED),   "save"},
    {static_cast<int>(ConsumerType::OPENCV),         "opencv"},
    {static_cast<int>(ConsumerType::COUNT),          ""},
    {static_cast<int>(ConsumerType::COMPARE),        ""},
};

static std::string keyToFlag(const std::string& key) {
    auto it = kKeyToCliFlag.find(key);
    if (it != kKeyToCliFlag.end()) return it->second;
    std::string flag = "--";
    for (char ch : key) {
        flag += (ch == '_') ? '-' : ch;
    }
    return flag;
}

static void appendConfigAsFlags(std::vector<std::string>& args, const json& config) {
    if (!config.is_object()) return;
    for (auto& [key, val] : config.items()) {
        if (val.is_null()) continue;
        std::string flag = keyToFlag(key);

        if (val.is_boolean()) {
            if (val.get<bool>()) args.push_back(flag);
        } else if (val.is_string()) {
            auto s = val.get<std::string>();
            if (!s.empty()) {
                args.push_back(flag);
                args.push_back(s);
            }
        } else if (val.is_number_integer()) {
            args.push_back(flag);
            args.push_back(std::to_string(val.get<int64_t>()));
        } else if (val.is_number_float()) {
            args.push_back(flag);
            args.push_back(std::to_string(val.get<double>()));
        } else if (val.is_array()) {
            std::string joined;
            for (size_t i = 0; i < val.size(); ++i) {
                if (i > 0) joined += ",";
                if (val[i].is_string()) joined += val[i].get<std::string>();
                else joined += val[i].dump();
            }
            if (!joined.empty()) {
                args.push_back(flag);
                args.push_back(joined);
            }
        }
    }
}

static std::vector<std::string> buildCliArgs(
    const DataSourceInfo& ds,
    const ApiDecoderConfig& decoder,
    const std::vector<ConsumerInfo>& consumers)
{
    std::vector<std::string> args;
    args.push_back("webui");

    args.push_back("vdec");
    args.push_back("--file");
    args.push_back(ds.path);

    if (ds.max_frames > 0) {
        args.push_back("--max-frames");
        args.push_back(std::to_string(ds.max_frames));
    }
    if (ds.loop) {
        args.push_back("--loop");
    }
    if (ds.buffer_count > 0) {
        args.push_back("--buffer-count");
        args.push_back(std::to_string(ds.buffer_count));
    }
    if (decoder.name.has_value() && !decoder.name->empty()) {
        args.push_back("--codec");
        args.push_back(decoder.name.value());
    }
    if (!decoder.enable_hardware) {
        args.push_back("--decoder");
        args.push_back("sw");
    }
    if (decoder.decode_threads > 0) {
        args.push_back("--threads");
        args.push_back(std::to_string(decoder.decode_threads));
    }

    for (auto& c : consumers) {
        auto it = kConsumerTypeToSubcmd.find(static_cast<int>(c.type));
        std::string subcmd;
        if (it != kConsumerTypeToSubcmd.end()) subcmd = it->second;

        if (subcmd.empty()) continue;
        args.push_back(subcmd);

        json cfg = c.config;
        if (c.type == ConsumerType::NPU_INFERENCE) {
            if (cfg.contains("draw") && cfg["draw"].is_boolean()) {
                cfg["draw_detections"] = cfg["draw"];
                cfg.erase("draw");
            }
        }

        appendConfigAsFlags(args, cfg);
    }

    return args;
}

static std::string describeArgs(const std::vector<std::string>& args) {
    std::ostringstream oss;
    for (size_t i = 1; i < args.size(); ++i) {
        if (i > 1) oss << " ";
        oss << args[i];
    }
    return oss.str();
}

} // anonymous namespace

// ============================================================
// buildWorkerConfig — 公开的纯函数
// ============================================================

BuildResult buildWorkerConfig(
    const DataSourceInfo& ds,
    const ApiDecoderConfig& decoder,
    const std::vector<ConsumerInfo>& consumers,
    PreviewService* preview_service,
    const std::string& worker_id)
{
    BuildResult result;

    // 1. API 参数 → CLI argv
    auto cli_args = buildCliArgs(ds, decoder, consumers);
    result.description = describeArgs(cli_args);

    // 2. 创建与 qa_cases 完全一致的插件实例
    auto vdec_plugin    = std::make_unique<test::vdec::VdecPlugin>();
    auto display_plugin = std::make_unique<test::display::DisplayPlugin>();
    auto npu_plugin     = std::make_unique<test::npu::NpuPlugin>();
    auto preview_plugin = std::make_unique<test::preview::PreviewPlugin>();
    auto save_plugin    = std::make_unique<test::save::SavePlugin>();
    auto pp_plugin      = std::make_unique<test::pp::PPPlugin>();
    auto opencv_plugin  = std::make_unique<test::opencv::OpencvPlugin>();
    auto venc_plugin    = std::make_unique<test::venc::VencPlugin>();

    // 3. CLI11 注册 + 解析
    CLI::App app{"webui-worker"};
    app.require_subcommand(1, 0);

    struct PluginEntry {
        test::IOptionPlugin* plugin;
        CLI::App* cmd;
    };
    std::vector<PluginEntry> all_entries;

    auto register_plugin = [&](test::IOptionPlugin* p) {
        auto* cmd = app.add_subcommand(p->getName(), p->getDescription());
        p->registerOptions(*cmd);
        all_entries.push_back({p, cmd});
    };

    register_plugin(vdec_plugin.get());
    register_plugin(venc_plugin.get());
    register_plugin(pp_plugin.get());
    register_plugin(save_plugin.get());
    register_plugin(display_plugin.get());
    register_plugin(npu_plugin.get());
    register_plugin(opencv_plugin.get());
    register_plugin(preview_plugin.get());

    std::vector<char*> argv_ptrs;
    for (auto& s : cli_args) argv_ptrs.push_back(s.data());
    int argc = static_cast<int>(argv_ptrs.size());

    try {
        app.parse(argc, argv_ptrs.data());
    } catch (const CLI::ParseError& e) {
        result.error = std::string("CLI 解析错误: ") + e.what();
        return result;
    }

    // 4. 收集被解析到的插件
    std::vector<test::IOptionPlugin*> active_plugins;
    for (auto& entry : all_entries) {
        if (entry.cmd->parsed())
            active_plugins.push_back(entry.plugin);
    }

    if (active_plugins.empty()) {
        result.error = "没有激活的插件";
        return result;
    }

    // 5. 预处理
    for (auto* p : active_plugins) {
        int rc = p->handlePreActions();
        if (rc >= 0) {
            result.error = "插件预处理返回 " + std::to_string(rc);
            return result;
        }
    }

    // 6. 所有插件 applyTo → 构建共享 WorkerConfig
    WorkerConfig config;
    for (auto* p : active_plugins) {
        p->applyTo(config);
    }

    // 7. 获取管线配置
    std::vector<WorkerConfig> pipeline_configs;
    for (auto* p : active_plugins) {
        auto configs = p->buildPipelineConfigs(config);
        if (!configs.empty()) {
            pipeline_configs = std::move(configs);
            break;
        }
    }

    if (pipeline_configs.empty()) {
        result.error = "无法生成管线配置";
        return result;
    }

    // 7.5. 继承伴随设置
    for (auto& pc : pipeline_configs) {
        pc.consumer_type.inheritCompanionSettings(config.consumer_type);
    }

    // 8. 同进程 JPEG 预览回调
    if (preview_service) {
        for (auto& pc : pipeline_configs) {
            if (pc.consumer_type.jpeg_encode.enable) {
                pc.consumer_type.jpeg_encode.on_frame =
                    [preview_service, worker_id](const uint8_t* data, size_t size) {
                        preview_service->onJpegFrame(worker_id, data, size);
                    };
            }
        }
    }

    // 9. 构建 consume flags
    result.flags = test::ExecuteMode::buildConsumeFlags(config);
    result.flags |= test::ExecuteMode::buildConsumeFlags(pipeline_configs[0]);

    result.config = std::move(pipeline_configs[0]);
    result.success = true;
    return result;
}

} // namespace webui
