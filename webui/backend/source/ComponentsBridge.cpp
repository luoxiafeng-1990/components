/**
 * @file ComponentsBridge.cpp
 * @brief Components 桥接层 —— 复用 qa_cases 插件体系，同进程调用 libcomponents
 *
 * 等价于 test_module_main.cpp，区别只在于参数来源：
 *   qa_cases  ← CLI 命令行参数
 *   webui     ← HTTP API 参数（转换为 CLI argv 后走同一套插件）
 *
 * 流程：
 *   1. 从 API 参数构造 CLI argv（如 "vdec --file /path display --vendor tacopro"）
 *   2. 创建同样的插件实例（VdecPlugin, DisplayPlugin, NpuPlugin, PreviewPlugin...）
 *   3. CLI11 解析 → applyTo(config) → buildPipelineConfigs(config)
 *   4. 设置 on_frame 回调（同进程 JPEG 预览，无需 FIFO）
 *   5. ExecuteMode::buildConsumeFlags → BufferConsumerService::start()
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

#include "consumptionline/BufferConsumerService.hpp"
#include "productionline/worker/WorkerConfig.hpp"

#include <thread>
#include <chrono>
#include <atomic>
#include <iostream>
#include <sstream>
#include <mutex>
#include <deque>
#include <unordered_map>

namespace webui {

// ============================================================
// API 参数 → CLI argv 构建
// ============================================================

namespace {

/**
 * JSON config key → CLI 参数名映射。
 * 当 JSON key 与 CLI 参数名不一致时（如下划线 vs 连字符），
 * 需要在此映射表中注册。未映射的 key 自动转换：下划线 → 连字符。
 */
static const std::unordered_map<std::string, std::string> kKeyToCliFlag = {
    // vdec 相关
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

/**
 * ConsumerType → CLI 子命令名
 */
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

/**
 * 将 JSON key 转为 CLI 参数名。
 * 优先查映射表，否则自动将下划线替换为连字符加 -- 前缀。
 */
static std::string keyToFlag(const std::string& key) {
    auto it = kKeyToCliFlag.find(key);
    if (it != kKeyToCliFlag.end()) return it->second;
    // 自动转换: target_fps → --target-fps
    std::string flag = "--";
    for (char ch : key) {
        flag += (ch == '_') ? '-' : ch;
    }
    return flag;
}

/**
 * 将 JSON config 中的所有 key-value 对转换为 CLI 参数追加到 args。
 * - bool true  → --flag（无值）
 * - bool false → 跳过
 * - string     → --flag value
 * - number     → --flag value
 * - array      → --flag v1,v2,v3（逗号分隔）
 * - null       → 跳过
 */
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

/**
 * 从 WebUI API 参数构造等效的 CLI 参数列表。
 *
 * 设计原则：config JSON 中的所有 key 自动透传为 CLI 参数，
 * 不再逐个硬编码，确保与 qa_cases 插件系统的参数完全一致。
 */
std::vector<std::string> buildCliArgs(
    const DataSourceInfo& ds,
    const ApiDecoderConfig& decoder,
    const std::vector<ConsumerInfo>& consumers)
{
    std::vector<std::string> args;
    args.push_back("webui");

    // ── vdec 子命令 ──
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

    // ── 消费者子命令：通用透传 ──
    for (auto& c : consumers) {
        auto it = kConsumerTypeToSubcmd.find(static_cast<int>(c.type));
        std::string subcmd;
        if (it != kConsumerTypeToSubcmd.end()) subcmd = it->second;

        if (subcmd.empty()) continue;
        args.push_back(subcmd);

        // 特殊处理：布尔 flag 的别名（draw → draw_detections, physical_addr）
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

std::string describeArgs(const std::vector<std::string>& args) {
    std::ostringstream oss;
    for (size_t i = 1; i < args.size(); ++i) {
        if (i > 1) oss << " ";
        oss << args[i];
    }
    return oss.str();
}

} // anonymous namespace

// ============================================================
// ComponentsWorkerInstance
// ============================================================

struct ComponentsWorkerInstance::Impl {
    std::thread worker_thread;
    std::atomic<bool> running{false};
    std::chrono::steady_clock::time_point start_time;

    consumer::BufferConsumerService service;
    consumer::ConsumeResult result;
    std::string description;
    std::string worker_id;

    OutputCallback output_callback;

    mutable std::mutex output_mutex;
    std::deque<std::string> output_lines;
    static constexpr size_t MAX_OUTPUT_LINES = 200;

    void addOutputLine(const std::string& line) {
        {
            std::lock_guard<std::mutex> lock(output_mutex);
            output_lines.push_back(line);
            if (output_lines.size() > MAX_OUTPUT_LINES)
                output_lines.pop_front();
        }
        if (output_callback)
            output_callback(line);
    }
};

ComponentsWorkerInstance::ComponentsWorkerInstance()
    : impl_(std::make_unique<Impl>()) {}

ComponentsWorkerInstance::~ComponentsWorkerInstance() { stop(); }

void ComponentsWorkerInstance::setOutputCallback(OutputCallback cb) {
    impl_->output_callback = std::move(cb);
}

bool ComponentsWorkerInstance::start(
    const DataSourceInfo& ds,
    const ApiDecoderConfig& decoder,
    const std::vector<ConsumerInfo>& consumers,
    PreviewService* preview_service,
    const std::string& worker_id)
{
    if (impl_->running.load()) return false;

    impl_->worker_id = worker_id;

    // ── 1. API 参数 → CLI argv ──
    auto cli_args = buildCliArgs(ds, decoder, consumers);
    impl_->description = describeArgs(cli_args);

    std::cout << "[WebUI] Worker " << worker_id
              << " 启动: " << impl_->description << std::endl;

    // ── 2. 创建与 qa_cases 完全一致的插件实例 ──
    auto vdec_plugin    = std::make_unique<test::vdec::VdecPlugin>();
    auto display_plugin = std::make_unique<test::display::DisplayPlugin>();
    auto npu_plugin     = std::make_unique<test::npu::NpuPlugin>();
    auto preview_plugin = std::make_unique<test::preview::PreviewPlugin>();
    auto save_plugin    = std::make_unique<test::save::SavePlugin>();
    auto pp_plugin      = std::make_unique<test::pp::PPPlugin>();
    auto opencv_plugin  = std::make_unique<test::opencv::OpencvPlugin>();
    auto venc_plugin    = std::make_unique<test::venc::VencPlugin>();

    // ── 3. CLI11 注册 + 解析（与 test_module_main.cpp 完全一致） ──
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

    // 转换为 argc/argv
    std::vector<char*> argv_ptrs;
    for (auto& s : cli_args) argv_ptrs.push_back(s.data());
    int argc = static_cast<int>(argv_ptrs.size());

    try {
        app.parse(argc, argv_ptrs.data());
    } catch (const CLI::ParseError& e) {
        std::cerr << "[WebUI] CLI 解析错误: " << e.what() << std::endl;
        return false;
    }

    // ── 4. 收集被解析到的插件（与 test_module_main.cpp 一致） ──
    std::vector<test::IOptionPlugin*> active_plugins;
    for (auto& entry : all_entries) {
        if (entry.cmd->parsed())
            active_plugins.push_back(entry.plugin);
    }

    if (active_plugins.empty()) {
        std::cerr << "[WebUI] 没有激活的插件" << std::endl;
        return false;
    }

    // ── 5. 预处理 ──
    for (auto* p : active_plugins) {
        int rc = p->handlePreActions();
        if (rc >= 0) {
            std::cerr << "[WebUI] 插件预处理返回 " << rc << std::endl;
            return false;
        }
    }

    // ── 6. 所有插件 applyTo → 构建共享 WorkerConfig ──
    WorkerConfig config;
    for (auto* p : active_plugins) {
        p->applyTo(config);
    }

    // ── 7. 获取管线配置 ──
    std::vector<WorkerConfig> pipeline_configs;
    std::string test_name;
    for (auto* p : active_plugins) {
        auto configs = p->buildPipelineConfigs(config);
        if (!configs.empty()) {
            pipeline_configs = std::move(configs);
            test_name = p->getTestName();
            break;
        }
    }

    if (pipeline_configs.empty()) {
        std::cerr << "[WebUI] 无法生成管线配置" << std::endl;
        return false;
    }

    // ── 7.5. 继承伴随设置（与 test_module_main.cpp 一致） ──
    for (auto& pc : pipeline_configs) {
        pc.consumer_type.inheritCompanionSettings(config.consumer_type);
    }

    // ── 8. 同进程 JPEG 预览回调 ──
    // 仅当 preview 插件被用户显式启用时，才挂载 WebUI 的内存回调。
    // 不再强制 enable —— 避免用户未请求预览时触发硬件 JPEG 编码。
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

    // ── 9. 构建 consume flags（与 test_module_main.cpp 一致） ──
    uint32_t flags = test::ExecuteMode::buildConsumeFlags(config);
    flags |= test::ExecuteMode::buildConsumeFlags(pipeline_configs[0]);

    // ── 10. 启动后台线程执行 ──
    impl_->running = true;
    impl_->start_time = std::chrono::steady_clock::now();

    impl_->worker_thread = std::thread(
        [this, pipeline_configs = std::move(pipeline_configs), flags, test_name]() {

        std::cout << "[WebUI] Worker " << impl_->worker_id
                  << " BufferConsumerService starting..."
                  << " flags=0x" << std::hex << flags << std::dec
                  << " pipelines=" << pipeline_configs.size()
                  << " jpeg_encode=" << (pipeline_configs[0].consumer_type.jpeg_encode.enable ? "ON" : "OFF")
                  << " display=" << (pipeline_configs[0].consumer_type.display.enable ? "ON" : "OFF")
                  << " on_frame=" << (pipeline_configs[0].consumer_type.jpeg_encode.on_frame ? "SET" : "NULL")
                  << std::endl;

        impl_->result = impl_->service.start(
            pipeline_configs, consumer::ExecuteMode::SINGLE, flags);

        std::cout << "[WebUI] Worker " << impl_->worker_id
                  << " 完成: " << (impl_->result.success ? "OK" : "FAILED")
                  << " frames=" << impl_->result.frames_consumed
                  << " fps=" << impl_->result.average_fps;
        if (!impl_->result.error_message.empty())
            std::cout << " error=" << impl_->result.error_message;
        std::cout << std::endl;

        impl_->running = false;
    });

    return true;
}

void ComponentsWorkerInstance::stop() {
    if (impl_->running.load()) {
        std::cout << "[WebUI] Worker " << impl_->worker_id
                  << " 请求停止..." << std::endl;
        impl_->service.requestStop();
    }

    if (impl_->worker_thread.joinable()) {
        impl_->worker_thread.join();
    }

    impl_->running = false;
}

bool ComponentsWorkerInstance::isRunning() const {
    return impl_->running.load();
}

int64_t ComponentsWorkerInstance::getDecodedFrames() const {
    return impl_->result.frames_consumed;
}

int64_t ComponentsWorkerInstance::getDroppedFrames() const {
    return 0;
}

double ComponentsWorkerInstance::getFps() const {
    return impl_->result.average_fps;
}

double ComponentsWorkerInstance::getUptimeSeconds() const {
    if (!impl_->running.load()) return impl_->result.duration_seconds;
    auto elapsed = std::chrono::steady_clock::now() - impl_->start_time;
    return std::chrono::duration<double>(elapsed).count();
}

std::string ComponentsWorkerInstance::getLastOutput() const {
    std::lock_guard<std::mutex> lock(impl_->output_mutex);
    if (impl_->output_lines.empty()) return "";
    std::ostringstream oss;
    for (auto& line : impl_->output_lines) {
        oss << line << "\n";
    }
    return oss.str();
}

std::string ComponentsWorkerInstance::getCommandLine() const {
    return impl_->description;
}

} // namespace webui
