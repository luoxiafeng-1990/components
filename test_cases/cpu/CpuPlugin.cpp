/**
 * @file CpuPlugin.cpp
 * @brief CPU 性能基准测试插件实现
 *
 * CLI 选项注册与 run() 入口，分发到 CpuBenchmark 引擎。
 */

#include "cpu/CpuPlugin.hpp"
#include "cpu/CpuBenchmark.hpp"
#include "../common/third_party/CLI11.hpp"
#include <log4cplus/loggingmacros.h>
#include <sstream>

namespace test::cpu {

// ============================================================
// CLI 注册
// ============================================================

void CpuPlugin::registerOptions(CLI::App& app) {
    app.add_option("--bench,-b", bench_list_,
        "要运行的测试项（逗号分隔）: dhrystone,coremark,stream,multithread,cache\n"
        "留空则运行全部");

    app.add_option("--threads,-t", threads_,
        "线程数（默认: 0 = 自动检测 CPU 核数）")
        ->default_val(0);

    app.add_option("--iterations,-i", iterations_,
        "迭代次数（默认: 0 = 各测试项使用内置默认值）")
        ->default_val(0);

    app.add_option("--stream-size", stream_size_,
        "Stream 测试数组元素数（默认: 8388608 = 8M 元素 = 64MB 数据）")
        ->default_val(8388608);

    app.add_option("--stream-ntimes", stream_ntimes_,
        "Stream 测试重复次数（默认: 50）")
        ->default_val(50);

    app.add_option("--duration,-d", duration_sec_,
        "持续运行时长(秒)，0 = 不限时")
        ->default_val(0);

    app.add_option("--report,-r", report_path_,
        "JSON 报告输出路径（不指定则不输出文件）");

    app.add_option("--affinity,-a", affinity_,
        "CPU 核绑定列表（如 \"0,4\" 跨簇测试，或 \"auto\" 自动分配）");

    app.add_flag("--list,-l", list_tests_,
        "列出所有可用测试项并退出");
}

// ============================================================
// handlePreActions
// ============================================================

int CpuPlugin::handlePreActions() {
    if (list_tests_) {
        CpuBenchmark::listAvailableTests();
        return 0;
    }
    return -1; // 继续执行
}

// ============================================================
// run()
// ============================================================

int CpuPlugin::run() {
    LOG4CPLUS_INFO(logger_, "=== CPU 性能基准测试启动 ===");

    // 构建配置
    BenchmarkConfig config;
    config.threads = threads_;
    config.iterations = iterations_;
    config.duration_sec = duration_sec_;
    config.stream_array_size = stream_size_;
    config.stream_ntimes = stream_ntimes_;
    config.affinity = affinity_;

    // 解析 bench_list
    if (!bench_list_.empty()) {
        std::istringstream ss(bench_list_);
        std::string item;
        while (std::getline(ss, item, ',')) {
            // 去除首尾空格
            auto start = item.find_first_not_of(" \t");
            auto end = item.find_last_not_of(" \t");
            if (start != std::string::npos) {
                config.bench_list.push_back(item.substr(start, end - start + 1));
            }
        }
    }

    // 运行基准测试
    CpuBenchmark benchmark(config);
    BenchmarkResults results = benchmark.runAll();

    // 打印结果
    CpuBenchmark::printResults(results);

    // 输出 JSON 报告
    if (!report_path_.empty()) {
        if (CpuBenchmark::generateJsonReport(results, report_path_)) {
            LOG4CPLUS_INFO_FMT(logger_, "JSON 报告已写入: %s",
                               report_path_.c_str());
        } else {
            LOG4CPLUS_ERROR_FMT(logger_, "JSON 报告写入失败: %s",
                                report_path_.c_str());
            return 1;
        }
    }

    LOG4CPLUS_INFO(logger_, "=== CPU 性能基准测试完成 ===");
    return 0;
}

} // namespace test::cpu
