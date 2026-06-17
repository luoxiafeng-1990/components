#include "memleak/MemleakPlugin.hpp"
#include "../common/third_party/CLI11.hpp"
#include <log4cplus/loggingmacros.h>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <chrono>
#include <regex>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

namespace fs = std::filesystem;

namespace test::memleak {

// ============================================================
// CLI 注册
// ============================================================

void MemleakPlugin::registerOptions(CLI::App& app) {
    app.add_option("--target,-t", target_cmd_,
        "被检测的 qa_cases 子命令（如 \"vdec --input foo.h264 --loop\"）")
        ->required();

    app.add_option("--tool", tool_,
        "valgrind 工具 (memcheck | massif)")
        ->default_val("memcheck");

    app.add_option("--duration,-d", duration_sec_,
        "运行时长(秒)，0=直到子进程自然退出")
        ->default_val(0);

    app.add_option("--report,-r", report_path_,
        "报告输出路径 (JSON)")
        ->default_val("/tmp/memleak_report.json");

    app.add_flag("--verbose,-v", verbose_, "详细输出");
}

// ============================================================
// run()
// ============================================================

int MemleakPlugin::run() {
    LOG4CPLUS_INFO(logger_, "=== 内存泄漏检测启动 ===");
    LOG4CPLUS_INFO_FMT(logger_, "target: %s", target_cmd_.c_str());
    LOG4CPLUS_INFO_FMT(logger_, "tool:   %s", tool_.c_str());
    LOG4CPLUS_INFO_FMT(logger_, "duration: %d sec", duration_sec_);

    if (!isValgrindAvailable()) {
        LOG4CPLUS_ERROR(logger_,
            "valgrind 未安装。请执行: sudo apt install valgrind");
        return 1;
    }

    if (tool_ == "memcheck") {
        return runMemcheck();
    } else if (tool_ == "massif") {
        return runMassif();
    } else {
        LOG4CPLUS_ERROR_FMT(logger_, "未知工具: %s (支持 memcheck / massif)",
                            tool_.c_str());
        return 1;
    }
}

// ============================================================
// valgrind 可用性检查
// ============================================================

bool MemleakPlugin::isValgrindAvailable() const {
    return system("which valgrind > /dev/null 2>&1") == 0;
}

std::string MemleakPlugin::buildChildCommand() const {
    auto self = fs::read_symlink("/proc/self/exe").string();
    return self + " " + target_cmd_;
}

// ============================================================
// memcheck 模式
// ============================================================

int MemleakPlugin::runMemcheck() {
    std::string log_path = "/tmp/memleak_memcheck_" +
        std::to_string(getpid()) + ".log";

    std::ostringstream cmd;
    cmd << "valgrind"
        << " --tool=memcheck"
        << " --leak-check=full"
        << " --show-reachable=yes"
        << " --track-origins=yes"
        << " --log-file=" << log_path
        << " " << buildChildCommand();

    LOG4CPLUS_INFO_FMT(logger_, "执行: %s", cmd.str().c_str());

    pid_t child = fork();
    if (child == 0) {
        execl("/bin/sh", "sh", "-c", cmd.str().c_str(), nullptr);
        _exit(127);
    }

    if (child < 0) {
        LOG4CPLUS_ERROR(logger_, "fork() 失败");
        return 1;
    }

    if (duration_sec_ > 0) {
        LOG4CPLUS_INFO_FMT(logger_, "等待 %d 秒后终止子进程...", duration_sec_);
        sleep(static_cast<unsigned>(duration_sec_));
        kill(child, SIGTERM);
        sleep(2);
        kill(child, SIGKILL);
    }

    int status = 0;
    waitpid(child, &status, 0);
    LOG4CPLUS_INFO_FMT(logger_, "子进程退出码: %d", WEXITSTATUS(status));

    return parseMemcheckOutput(log_path);
}

int MemleakPlugin::parseMemcheckOutput(const std::string& log_path) {
    std::ifstream f(log_path);
    if (!f.is_open()) {
        LOG4CPLUS_ERROR_FMT(logger_, "无法读取 valgrind 日志: %s",
                            log_path.c_str());
        return 1;
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    std::regex definitely_re(R"(definitely lost: ([\d,]+) bytes)");
    std::regex indirectly_re(R"(indirectly lost: ([\d,]+) bytes)");
    std::regex possibly_re(R"(possibly lost: ([\d,]+) bytes)");
    std::regex still_re(R"(still reachable: ([\d,]+) bytes)");

    auto extractBytes = [](const std::string& s, const std::regex& re) -> std::string {
        std::smatch m;
        if (std::regex_search(s, m, re)) {
            std::string v = m[1].str();
            v.erase(std::remove(v.begin(), v.end(), ','), v.end());
            return v;
        }
        return "0";
    };

    std::string def_lost  = extractBytes(content, definitely_re);
    std::string ind_lost  = extractBytes(content, indirectly_re);
    std::string pos_lost  = extractBytes(content, possibly_re);
    std::string reachable = extractBytes(content, still_re);

    LOG4CPLUS_INFO(logger_, "====== Memcheck 摘要 ======");
    LOG4CPLUS_INFO_FMT(logger_, "  definitely lost: %s bytes", def_lost.c_str());
    LOG4CPLUS_INFO_FMT(logger_, "  indirectly lost: %s bytes", ind_lost.c_str());
    LOG4CPLUS_INFO_FMT(logger_, "  possibly lost:   %s bytes", pos_lost.c_str());
    LOG4CPLUS_INFO_FMT(logger_, "  still reachable: %s bytes", reachable.c_str());

    std::ofstream rpt(report_path_);
    if (rpt.is_open()) {
        rpt << "{\n"
            << "  \"tool\": \"memcheck\",\n"
            << "  \"target\": \"" << target_cmd_ << "\",\n"
            << "  \"definitely_lost_bytes\": " << def_lost << ",\n"
            << "  \"indirectly_lost_bytes\": " << ind_lost << ",\n"
            << "  \"possibly_lost_bytes\": "   << pos_lost << ",\n"
            << "  \"still_reachable_bytes\": "  << reachable << ",\n"
            << "  \"log_file\": \"" << log_path << "\"\n"
            << "}\n";
        LOG4CPLUS_INFO_FMT(logger_, "报告已写入: %s", report_path_.c_str());
    }

    return (std::stoll(def_lost) > 0) ? 1 : 0;
}

// ============================================================
// massif 模式
// ============================================================

int MemleakPlugin::runMassif() {
    std::string out_path = "/tmp/memleak_massif_" +
        std::to_string(getpid()) + ".out";

    std::ostringstream cmd;
    cmd << "valgrind"
        << " --tool=massif"
        << " --time-unit=ms"
        << " --detailed-freq=10"
        << " --massif-out-file=" << out_path
        << " " << buildChildCommand();

    LOG4CPLUS_INFO_FMT(logger_, "执行: %s", cmd.str().c_str());

    pid_t child = fork();
    if (child == 0) {
        execl("/bin/sh", "sh", "-c", cmd.str().c_str(), nullptr);
        _exit(127);
    }

    if (child < 0) {
        LOG4CPLUS_ERROR(logger_, "fork() 失败");
        return 1;
    }

    if (duration_sec_ > 0) {
        LOG4CPLUS_INFO_FMT(logger_, "等待 %d 秒后终止子进程...", duration_sec_);
        sleep(static_cast<unsigned>(duration_sec_));
        kill(child, SIGTERM);
        sleep(2);
        kill(child, SIGKILL);
    }

    int status = 0;
    waitpid(child, &status, 0);
    LOG4CPLUS_INFO_FMT(logger_, "子进程退出码: %d", WEXITSTATUS(status));

    return parseMassifOutput(out_path);
}

int MemleakPlugin::parseMassifOutput(const std::string& output_path) {
    std::string print_cmd = "ms_print " + output_path;
    std::string text_path = output_path + ".txt";

    std::string full_cmd = print_cmd + " > " + text_path + " 2>&1";
    int rc = system(full_cmd.c_str());

    if (rc != 0) {
        LOG4CPLUS_WARN(logger_,
            "ms_print 不可用，请安装 valgrind 完整包。跳过文本报告。");
    } else {
        LOG4CPLUS_INFO_FMT(logger_, "Massif 文本报告: %s", text_path.c_str());
    }

    std::ifstream f(output_path);
    if (!f.is_open()) {
        LOG4CPLUS_ERROR_FMT(logger_, "无法读取 massif 输出: %s",
                            output_path.c_str());
        return 1;
    }

    long long peak_mem = 0;
    std::string line;
    std::regex snap_re(R"(mem_heap_B=(\d+))");
    while (std::getline(f, line)) {
        std::smatch m;
        if (std::regex_search(line, m, snap_re)) {
            long long val = std::stoll(m[1].str());
            if (val > peak_mem) peak_mem = val;
        }
    }

    LOG4CPLUS_INFO(logger_, "====== Massif 摘要 ======");
    LOG4CPLUS_INFO_FMT(logger_, "  峰值堆内存: %lld bytes (%.2f MB)",
                        peak_mem, peak_mem / 1048576.0);

    std::ofstream rpt(report_path_);
    if (rpt.is_open()) {
        rpt << "{\n"
            << "  \"tool\": \"massif\",\n"
            << "  \"target\": \"" << target_cmd_ << "\",\n"
            << "  \"peak_heap_bytes\": " << peak_mem << ",\n"
            << "  \"massif_out\": \"" << output_path << "\",\n"
            << "  \"massif_txt\": \"" << text_path << "\"\n"
            << "}\n";
        LOG4CPLUS_INFO_FMT(logger_, "报告已写入: %s", report_path_.c_str());
    }

    return 0;
}

} // namespace test::memleak
