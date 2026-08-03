#pragma once

/**
 * @file CpuPlugin.hpp
 * @brief CPU 性能基准测试插件
 *
 * 分类：UTILITY（不走消费策略，由 main 直接调用 run()）
 *
 * 内置 5 大基准测试：
 *   1. Dhrystone 2.1  — 整数运算 (DMIPS)
 *   2. CoreMark-like  — 嵌入式核心性能 (链表/矩阵/状态机)
 *   3. Stream         — 内存带宽 (Copy/Scale/Add/Triad)
 *   4. Multi-thread   — 多核扩展性 (1→N 线程加速比)
 *   5. Cache          — 缓存层级性能 (ns/access)
 *
 * 跨平台：纯 C++17 标准库实现，ARM / RISC-V / x86 通用。
 *
 * 命令行用法：
 *   ./qa_cases cpu                                    ← 运行全部基准
 *   ./qa_cases cpu --bench dhrystone,stream            ← 选择性运行
 *   ./qa_cases cpu --threads 8 --iterations 10000000   ← 配置参数
 *   ./qa_cases cpu --bench multithread --affinity 0,4  ← 跨簇绑核测试
 *   ./qa_cases cpu --report /tmp/cpu_bench.json        ← JSON 报告
 *   ./qa_cases cpu --list                              ← 列出所有测试项
 */

#include "common/IOptionPlugin.hpp"
#include <string>
#include <vector>
#include <log4cplus/logger.h>

namespace test::cpu {

class CpuPlugin : public IOptionPlugin {
public:
    std::string getName() const override { return "cpu"; }
    std::string getDescription() const override {
        return "CPU 性能基准测试 (Dhrystone/CoreMark/Stream/MultiThread/Cache)";
    }

    PluginCategory getCategory() const override { return PluginCategory::UTILITY; }

    void registerOptions(CLI::App& app) override;
    void applyCliToConfig(WorkerConfig& /*config*/) const override {}

    int handlePreActions() override;
    int run() override;

private:
    // CLI options
    std::string bench_list_;              // 逗号分隔的测试项列表，空=全部
    int threads_ = 0;                     // 0 = auto (hardware_concurrency)
    int iterations_ = 0;                  // 0 = 使用各测试项的默认值
    int stream_size_ = 8388608;           // Stream 数组元素数 (默认 8M = 64MB)
    int stream_ntimes_ = 50;              // Stream 重复次数
    int duration_sec_ = 0;                // 持续时间（秒），0 = 不限
    std::string report_path_;             // JSON 报告路径，空=不输出
    std::string affinity_;                // 核绑定列表，如 "0,4" 或 "auto"
    bool list_tests_ = false;             // 列出所有可用测试项

    log4cplus::Logger logger_ =
        log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("qa_cases.cpu"));
};

} // namespace test::cpu
