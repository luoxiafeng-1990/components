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

namespace webui {

// ============================================================
// API 参数 → CLI argv 构建
// ============================================================

namespace {

/**
 * 从 WebUI API 参数构造等效的 CLI 参数列表。
 *
 * 示例输出：
 *   {"webui", "vdec", "--file", "/path/to/video.mp4", "--max-frames", "300",
 *    "display", "--vendor", "tacopro", "--fps", "30",
 *    "preview", "--quality", "80"}
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

    // ── 消费者子命令 ──
    for (auto& c : consumers) {
        switch (c.type) {
            case ConsumerType::DISPLAY: {
                args.push_back("display");
                if (c.config.contains("vendor")) {
                    args.push_back("--vendor");
                    args.push_back(c.config["vendor"].get<std::string>());
                }
                if (c.config.contains("target_fps")) {
                    args.push_back("--fps");
                    args.push_back(std::to_string(c.config["target_fps"].get<int>()));
                }
                if (c.config.contains("osd") && c.config["osd"].get<bool>()) {
                    args.push_back("--osd");
                }
                if (c.config.contains("view_type")) {
                    args.push_back("--view-type");
                    args.push_back(c.config["view_type"].get<std::string>());
                }
                if (c.config.contains("screen_width")) {
                    args.push_back("--screen-width");
                    args.push_back(std::to_string(c.config["screen_width"].get<int>()));
                }
                if (c.config.contains("screen_height")) {
                    args.push_back("--screen-height");
                    args.push_back(std::to_string(c.config["screen_height"].get<int>()));
                }
                break;
            }

            case ConsumerType::NPU_INFERENCE: {
                args.push_back("npu");
                if (c.config.contains("model_path")) {
                    args.push_back("--model");
                    args.push_back(c.config["model_path"].get<std::string>());
                }
                if (c.config.contains("conf_threshold")) {
                    args.push_back("--conf-threshold");
                    args.push_back(std::to_string(c.config["conf_threshold"].get<float>()));
                }
                if (c.config.contains("nms_threshold")) {
                    args.push_back("--nms-threshold");
                    args.push_back(std::to_string(c.config["nms_threshold"].get<float>()));
                }
                if (c.config.contains("draw") && c.config["draw"].get<bool>()) {
                    args.push_back("--draw-detections");
                }
                break;
            }

            case ConsumerType::JPEG_PREVIEW: {
                args.push_back("preview");
                if (c.config.contains("quality")) {
                    args.push_back("--quality");
                    args.push_back(std::to_string(c.config["quality"].get<int>()));
                }
                if (c.config.contains("target_fps")) {
                    args.push_back("--fps");
                    args.push_back(std::to_string(c.config["target_fps"].get<int>()));
                }
                if (c.config.contains("encoder_name")) {
                    args.push_back("--encoder");
                    args.push_back(c.config["encoder_name"].get<std::string>());
                }
                break;
            }

            case ConsumerType::SAVE_RAW: {
                args.push_back("save");
                if (c.config.contains("output_path")) {
                    args.push_back("--output");
                    args.push_back(c.config["output_path"].get<std::string>());
                }
                if (c.config.contains("frames")) {
                    args.push_back("--frames");
                    args.push_back(std::to_string(c.config["frames"].get<int>()));
                }
                break;
            }

            case ConsumerType::SAVE_ENCODED:
            case ConsumerType::COUNT:
            case ConsumerType::COMPARE:
            case ConsumerType::OPENCV:
                break;
        }
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
