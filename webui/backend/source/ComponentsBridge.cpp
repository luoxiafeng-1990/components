#include "../include/ComponentsBridge.hpp"
#include "../include/PreviewService.hpp"
#include <thread>
#include <chrono>
#include <atomic>
#include <iostream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <mutex>
#include <deque>
#include <regex>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace webui {

namespace bridge {

std::vector<std::string> buildQaCasesArgs(
    const DataSourceInfo& ds,
    const ApiDecoderConfig& decoder,
    const std::vector<ConsumerInfo>& consumers)
{
    std::vector<std::string> args;
    args.push_back("qa_cases");

    // 驱动子命令 = vdec
    args.push_back("vdec");

    // 数据源
    if (ds.type == DataSourceType::RTSP) {
        args.push_back("--rtsp");
    } else {
        args.push_back("--file");
    }
    args.push_back(ds.path);

    // 解码器
    if (decoder.name.has_value() && !decoder.name.value().empty()) {
        args.push_back("--codec");
        args.push_back(decoder.name.value());
    }
    if (!decoder.enable_hardware) {
        args.push_back("--decoder");
        args.push_back("sw");
    }

    // 最大帧数
    if (ds.max_frames > 0) {
        args.push_back("--max-frames");
        args.push_back(std::to_string(ds.max_frames));
    }

    // 循环播放
    if (ds.loop) {
        args.push_back("--loop");
    }

    // 消费者子命令
    for (auto& c : consumers) {
        switch (c.type) {
            case ConsumerType::DISPLAY: {
                args.push_back("display");
                std::string vendor = "tacopro";
                if (c.config.contains("vendor"))
                    vendor = c.config["vendor"].get<std::string>();
                args.push_back("--vendor");
                args.push_back(vendor);
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
                break;
            }

            case ConsumerType::SAVE_RAW: {
                args.push_back("save");
                if (c.config.contains("output_path")) {
                    args.push_back("--output");
                    args.push_back(c.config["output_path"].get<std::string>());
                }
                if (c.config.contains("format")) {
                    args.push_back("--format");
                    args.push_back(c.config["format"].get<std::string>());
                }
                if (c.config.contains("frames")) {
                    args.push_back("--frames");
                    args.push_back(std::to_string(c.config["frames"].get<int>()));
                }
                break;
            }

            case ConsumerType::SAVE_ENCODED: {
                args.push_back("save");
                if (c.config.contains("output_path")) {
                    args.push_back("--output");
                    args.push_back(c.config["output_path"].get<std::string>());
                }
                if (c.config.contains("format")) {
                    args.push_back("--format");
                    args.push_back(c.config["format"].get<std::string>());
                } else {
                    args.push_back("--format");
                    args.push_back("mp4");
                }
                if (c.config.contains("duration")) {
                    args.push_back("--duration");
                    args.push_back(std::to_string(c.config["duration"].get<int>()));
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

            case ConsumerType::COUNT:
            case ConsumerType::COMPARE:
            case ConsumerType::OPENCV:
            case ConsumerType::JPEG_PREVIEW:
                break;
        }
    }

    return args;
}

std::string argsToString(const std::vector<std::string>& args) {
    std::ostringstream oss;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) oss << " ";
        // 如果参数包含空格，加引号
        if (args[i].find(' ') != std::string::npos) {
            oss << "\"" << args[i] << "\"";
        } else {
            oss << args[i];
        }
    }
    return oss.str();
}

} // namespace bridge

// ============================================================
// ComponentsWorkerInstance —— 管理 qa_cases 子进程
// ============================================================

struct ComponentsWorkerInstance::Impl {
    pid_t child_pid = -1;
    int pipe_fd = -1;                      // 读取子进程 stdout+stderr 的管道
    std::thread reader_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};
    std::chrono::steady_clock::time_point start_time;

    // 输出解析
    std::atomic<int64_t> decoded_frames{0};
    std::atomic<int64_t> dropped_frames{0};
    std::atomic<double> fps{0.0};

    // 输出缓存（最近 200 行）
    mutable std::mutex output_mutex;
    std::deque<std::string> output_lines;
    static constexpr size_t MAX_OUTPUT_LINES = 200;

    OutputCallback output_callback;
    std::string command_line;
    std::string worker_id;

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

    void parseOutputLine(const std::string& line) {
        // 解析 qa_cases 的 printResult 输出
        // 格式示例: "  Frames consumed: 1234"  "  Average FPS: 30.12"
        static std::regex frames_re(R"(Frames consumed:\s*(\d+))");
        static std::regex fps_re(R"(Average FPS:\s*([\d.]+))");
        static std::regex produced_re(R"(produced_frames[=:]\s*(\d+))");

        std::smatch match;
        if (std::regex_search(line, match, frames_re)) {
            decoded_frames.store(std::stoll(match[1].str()));
        }
        if (std::regex_search(line, match, fps_re)) {
            fps.store(std::stod(match[1].str()));
        }
        if (std::regex_search(line, match, produced_re)) {
            decoded_frames.store(std::stoll(match[1].str()));
        }
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
    PreviewService* /*preview_service*/,
    const std::string& worker_id)
{
    if (impl_->running.load()) return false;

    auto args = bridge::buildQaCasesArgs(ds, decoder, consumers);
    impl_->command_line = bridge::argsToString(args);
    impl_->worker_id = worker_id;

    std::cout << "[WebUI] Worker " << worker_id
              << " 执行: " << impl_->command_line << std::endl;

    // 创建管道
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        std::cerr << "[WebUI] Worker " << worker_id << " pipe() 失败" << std::endl;
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "[WebUI] Worker " << worker_id << " fork() 失败" << std::endl;
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        // 子进程
        close(pipefd[0]);  // 关闭读端
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        // 构造 execvp 参数
        std::vector<char*> c_args;
        for (auto& a : args) {
            c_args.push_back(const_cast<char*>(a.c_str()));
        }
        c_args.push_back(nullptr);

        execvp(c_args[0], c_args.data());
        // 如果 exec 失败
        perror("execvp qa_cases failed");
        _exit(127);
    }

    // 父进程
    close(pipefd[1]);  // 关闭写端
    impl_->child_pid = pid;
    impl_->pipe_fd = pipefd[0];
    impl_->running = true;
    impl_->stop_requested = false;
    impl_->decoded_frames = 0;
    impl_->dropped_frames = 0;
    impl_->fps = 0.0;
    impl_->start_time = std::chrono::steady_clock::now();

    // 启动读取线程
    impl_->reader_thread = std::thread([this]() {
        FILE* fp = fdopen(impl_->pipe_fd, "r");
        if (!fp) {
            impl_->running = false;
            return;
        }

        char buf[4096];
        while (fgets(buf, sizeof(buf), fp)) {
            std::string line(buf);
            // 去掉末尾换行
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();

            if (!line.empty()) {
                std::cout << "[qa_cases:" << impl_->worker_id << "] " << line << std::endl;
                impl_->addOutputLine(line);
                impl_->parseOutputLine(line);
            }
        }
        fclose(fp);

        // 等待子进程结束
        int status = 0;
        waitpid(impl_->child_pid, &status, 0);

        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        std::cout << "[WebUI] Worker " << impl_->worker_id
                  << " qa_cases 进程退出, exit_code=" << exit_code << std::endl;

        impl_->child_pid = -1;
        impl_->running = false;
    });

    return true;
}

void ComponentsWorkerInstance::stop() {
    if (impl_->child_pid > 0) {
        std::cout << "[WebUI] Worker " << impl_->worker_id
                  << " 发送 SIGTERM 到 pid=" << impl_->child_pid << std::endl;
        kill(impl_->child_pid, SIGTERM);

        // 等待最多 3 秒
        for (int i = 0; i < 30 && impl_->running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // 还没退出就 SIGKILL
        if (impl_->running.load() && impl_->child_pid > 0) {
            std::cout << "[WebUI] Worker " << impl_->worker_id
                      << " 发送 SIGKILL" << std::endl;
            kill(impl_->child_pid, SIGKILL);
        }
    }

    impl_->stop_requested = true;
    if (impl_->reader_thread.joinable()) {
        impl_->reader_thread.join();
    }

    impl_->running = false;
}

bool ComponentsWorkerInstance::isRunning() const {
    return impl_->running.load();
}

int64_t ComponentsWorkerInstance::getDecodedFrames() const {
    return impl_->decoded_frames.load();
}

int64_t ComponentsWorkerInstance::getDroppedFrames() const {
    return impl_->dropped_frames.load();
}

double ComponentsWorkerInstance::getFps() const {
    return impl_->fps.load();
}

double ComponentsWorkerInstance::getUptimeSeconds() const {
    if (!impl_->running.load()) return 0;
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
    return impl_->command_line;
}

} // namespace webui
