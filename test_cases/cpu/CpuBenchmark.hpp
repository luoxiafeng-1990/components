#pragma once

/**
 * @file CpuBenchmark.hpp
 * @brief CPU 基准测试核心引擎
 *
 * 纯 C++17 标准库实现，跨平台（ARM / RISC-V / x86 / macOS / Windows）。
 * 平台特定功能（绑核、频率检测）做优雅降级。
 */

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <log4cplus/logger.h>

namespace test::cpu {

// ================================================================
// 平台信息
// ================================================================

struct PlatformInfo {
    std::string arch;           // "aarch64", "riscv64", "x86_64", ...
    std::string os;             // "Linux", "macOS", "Windows"
    int num_cores = 0;          // 逻辑核数
    int64_t freq_mhz = 0;      // CPU 频率 (MHz)，0 = 未知
    std::string cpu_model;      // CPU 型号字符串
};

// ================================================================
// 基准测试结果
// ================================================================

struct DhrystoneResult {
    int64_t iterations = 0;
    double elapsed_sec = 0.0;
    double dmips = 0.0;
    double dmips_per_mhz = 0.0;
};

struct CoremarkResult {
    double list_ops_per_sec = 0.0;
    double matrix_ops_per_sec = 0.0;
    double state_machine_ops_per_sec = 0.0;
    double composite_per_mhz = 0.0;
    double elapsed_sec = 0.0;
};

struct StreamResult {
    struct Op {
        std::string name;
        double rate_mb_s = 0.0;     // MB/s
        double avg_time_sec = 0.0;
        double min_time_sec = 0.0;
        double max_time_sec = 0.0;
    };
    std::vector<Op> ops;            // Copy, Scale, Add, Triad, SingleCopy
    int64_t array_size = 0;         // 元素数
    int ntimes = 0;                 // 重复次数
    int num_threads = 0;            // 使用的线程数
};

struct MultiThreadResult {
    struct ScalingPoint {
        int num_threads = 0;
        double rate_mb_s = 0.0;
        double speedup = 0.0;       // 相对单线程的加速比
    };
    std::vector<ScalingPoint> points;
    int64_t data_size_bytes = 0;
};

struct CacheResult {
    struct Level {
        int64_t block_size_kb = 0;
        double ns_per_access = 0.0;
        std::string hint;           // "L1 hit", "L2 hit", "L2 miss → DDR"
    };
    std::vector<Level> levels;
};

struct BenchmarkResults {
    PlatformInfo platform;
    bool has_dhrystone = false;
    bool has_coremark = false;
    bool has_stream = false;
    bool has_multithread = false;
    bool has_cache = false;

    DhrystoneResult dhrystone;
    CoremarkResult coremark;
    StreamResult stream;
    MultiThreadResult multithread;
    CacheResult cache;
};

// ================================================================
// 配置
// ================================================================

struct BenchmarkConfig {
    // 测试项选择（空 = 全部）
    std::vector<std::string> bench_list;

    // 通用参数
    int threads = 0;                // 0 = auto
    int iterations = 0;             // 0 = 各测试默认
    int duration_sec = 0;           // 0 = 不限时

    // Stream 参数
    int64_t stream_array_size = 8388608;
    int stream_ntimes = 50;

    // 绑核
    std::string affinity;           // "0,4" 或 "auto" 或空
};

// ================================================================
// 核心引擎
// ================================================================

class CpuBenchmark {
public:
    explicit CpuBenchmark(const BenchmarkConfig& config);

    /// 运行所有选定的基准测试
    BenchmarkResults runAll();

    /// 打印格式化结果到终端
    static void printResults(const BenchmarkResults& results);

    /// 输出 JSON 报告到文件
    static bool generateJsonReport(const BenchmarkResults& results,
                                   const std::string& path);

    /// 列出所有可用测试项
    static void listAvailableTests();

private:
    // 子基准
    DhrystoneResult runDhrystone();
    CoremarkResult runCoremark();
    StreamResult runStream();
    MultiThreadResult runMultiThread();
    CacheResult runCache();

    // 平台检测
    static PlatformInfo detectPlatform();

    // 辅助
    bool shouldRun(const std::string& name) const;
    int getThreadCount() const;
    std::vector<int> parseAffinity() const;
    static void setThreadAffinity(int core_id);
    static std::string formatBytes(int64_t bytes);
    static std::string formatRate(double mb_s);

    // 防优化：确保编译器不会优化掉基准测试中的计算
    template<typename T>
    static void doNotOptimize(const T& value) {
        asm volatile("" : : "r,m"(value) : "memory");
    }

    BenchmarkConfig config_;
    log4cplus::Logger logger_ =
        log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("qa_cases.cpu.benchmark"));
};

} // namespace test::cpu
