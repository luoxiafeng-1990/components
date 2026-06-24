/**
 * @file CpuBenchmark.cpp
 * @brief CPU 基准测试核心引擎实现
 *
 * 纯 C++17 标准库实现，跨平台（ARM / RISC-V / x86）。
 *
 * 包含 5 大子基准：
 *   1. Dhrystone 2.1  — 经典整数运算基准 (Public Domain, Reinhold P. Weicker, 1984)
 *   2. CoreMark-like  — 嵌入式 CPU 核心性能（链表/矩阵/状态机）
 *   3. Stream         — 内存带宽（Copy/Scale/Add/Triad/SingleCopy）
 *   4. Multi-thread   — 多核扩展性（1→N 线程 memcpy 加速比）
 *   5. Cache          — 缓存层级性能（stride-access, ns/access）
 */

#include "cpu/CpuBenchmark.hpp"
#include <log4cplus/loggingmacros.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <thread>
#include <vector>
#include <functional>

// ── 跨平台：CPU 绑核 ──
#if defined(__linux__)
  #include <sched.h>
  #include <unistd.h>
  #define HAS_CPU_AFFINITY 1
#elif defined(_WIN32)
  #include <windows.h>
  #define HAS_CPU_AFFINITY 1
#else
  #define HAS_CPU_AFFINITY 0
#endif

namespace test::cpu {

using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double>;

// ================================================================
// 构造函数
// ================================================================

CpuBenchmark::CpuBenchmark(const BenchmarkConfig& config)
    : config_(config) {}

// ================================================================
// 平台检测
// ================================================================

PlatformInfo CpuBenchmark::detectPlatform() {
    PlatformInfo info;

    // ── 架构检测（编译期） ──
#if defined(__aarch64__) || defined(_M_ARM64)
    info.arch = "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
    info.arch = "arm";
#elif defined(__riscv)
  #if __riscv_xlen == 64
    info.arch = "riscv64";
  #else
    info.arch = "riscv32";
  #endif
#elif defined(__x86_64__) || defined(_M_X64)
    info.arch = "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    info.arch = "x86";
#else
    info.arch = "unknown";
#endif

    // ── OS 检测 ──
#if defined(__linux__)
    info.os = "Linux";
#elif defined(__APPLE__)
    info.os = "macOS";
#elif defined(_WIN32)
    info.os = "Windows";
#else
    info.os = "Unknown";
#endif

    // ── 核数 ──
    info.num_cores = static_cast<int>(std::thread::hardware_concurrency());
    if (info.num_cores == 0) info.num_cores = 1;

    // ── CPU 频率（Linux sysfs）──
#if defined(__linux__)
    {
        std::ifstream f("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
        if (f.is_open()) {
            int64_t khz = 0;
            f >> khz;
            info.freq_mhz = khz / 1000;
        }
    }

    // ── CPU 型号（Linux /proc/cpuinfo）──
    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        while (std::getline(f, line)) {
            // ARM: "model name", RISC-V: "isa" or "uarch"
            if (line.find("model name") != std::string::npos ||
                line.find("uarch") != std::string::npos) {
                auto pos = line.find(':');
                if (pos != std::string::npos) {
                    info.cpu_model = line.substr(pos + 2);
                    break;
                }
            }
        }
        if (info.cpu_model.empty()) {
            // 回退：尝试读 isa 字段（RISC-V）
            f.clear();
            f.seekg(0);
            while (std::getline(f, line)) {
                if (line.find("isa") != std::string::npos) {
                    auto pos = line.find(':');
                    if (pos != std::string::npos) {
                        info.cpu_model = line.substr(pos + 2);
                        break;
                    }
                }
            }
        }
    }
#endif

    return info;
}

// ================================================================
// CPU 绑核
// ================================================================

void CpuBenchmark::setThreadAffinity(int core_id) {
#if defined(__linux__) && HAS_CPU_AFFINITY
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
#elif defined(_WIN32) && HAS_CPU_AFFINITY
    SetThreadAffinityMask(GetCurrentThread(),
                          static_cast<DWORD_PTR>(1) << core_id);
#else
    (void)core_id; // 不支持，静默忽略
#endif
}

std::vector<int> CpuBenchmark::parseAffinity() const {
    std::vector<int> cores;
    if (config_.affinity.empty()) return cores;

    if (config_.affinity == "auto") {
        int n = getThreadCount();
        for (int i = 0; i < n; ++i) cores.push_back(i);
        return cores;
    }

    // 解析 "0,4,2,6" 格式
    std::istringstream ss(config_.affinity);
    std::string token;
    while (std::getline(ss, token, ',')) {
        try {
            cores.push_back(std::stoi(token));
        } catch (...) {
            // 忽略无效值
        }
    }
    return cores;
}

// ================================================================
// 辅助方法
// ================================================================

bool CpuBenchmark::shouldRun(const std::string& name) const {
    if (config_.bench_list.empty()) return true;
    for (const auto& b : config_.bench_list) {
        if (b == name) return true;
    }
    return false;
}

int CpuBenchmark::getThreadCount() const {
    if (config_.threads > 0) return config_.threads;
    int hw = static_cast<int>(std::thread::hardware_concurrency());
    return (hw > 0) ? hw : 1;
}

std::string CpuBenchmark::formatBytes(int64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double val = static_cast<double>(bytes);
    int unit = 0;
    while (val >= 1024.0 && unit < 4) {
        val /= 1024.0;
        ++unit;
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << val << " " << units[unit];
    return ss.str();
}

std::string CpuBenchmark::formatRate(double mb_s) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << mb_s << " MB/s";
    return ss.str();
}

void CpuBenchmark::listAvailableTests() {
    std::cout << "\n可用的 CPU 基准测试项：\n\n"
              << "  dhrystone    Dhrystone 2.1 整数运算基准 (DMIPS)\n"
              << "  coremark     CoreMark-like 嵌入式核心性能 (链表/矩阵/状态机)\n"
              << "  stream       Stream 内存带宽 (Copy/Scale/Add/Triad/SingleCopy)\n"
              << "  multithread  多核扩展性 (1→N 线程 memcpy 加速比)\n"
              << "  cache        缓存层级性能 (stride-access, ns/access)\n"
              << "\n用法示例：\n"
              << "  ./qa_cases cpu                           # 运行全部\n"
              << "  ./qa_cases cpu --bench dhrystone,stream   # 选择性运行\n"
              << "  ./qa_cases cpu --bench multithread -a 0,4 # 跨簇绑核测试\n"
              << std::endl;
}

// ================================================================
//  1. Dhrystone 2.1
// ================================================================
//
// 原始作者: Reinhold P. Weicker, 1984/1988
// 许可: Public Domain（公有领域）
//
// 这里是经典 Dhrystone 2.1 的 C++ 移植版，保留核心算法逻辑，
// 适配现代 C++ 编译器。
//

namespace dhrystone {

// Dhrystone 枚举和类型
enum Enumeration { Ident_1 = 0, Ident_2, Ident_3, Ident_4, Ident_5 };

struct Record {
    Record* ptr_comp;
    Enumeration discr;
    union {
        struct {
            Enumeration enum_comp;
            int int_comp;
            char str_comp[31];
        } var_1;
        struct {
            Enumeration enum_comp_2;
            char str_2_comp[31];
        } var_2;
        struct {
            char ch_1_comp;
            char ch_2_comp;
        } var_3;
    } variant;
};

// 全局变量（Dhrystone 规范要求）
static Record record_glob, next_record_glob;
static int int_glob;
static bool bool_glob;
static char ch_1_glob, ch_2_glob;
static int arr_1_glob[50];
static int arr_2_glob[50][50];

static Enumeration func_1(char ch_1, char ch_2) {
    char ch_1_loc = ch_1;
    char ch_2_loc = ch_1_loc;
    if (ch_2_loc != ch_2) return Ident_1;
    else return Ident_2;
}

static bool func_2(const char* str_1, const char* str_2) {
    int int_loc = 2;
    char ch_loc = 'A';
    while (int_loc <= 2) {
        if (func_1(str_1[int_loc], str_2[int_loc + 1]) == Ident_1) {
            ch_loc = 'A';
            ++int_loc;
        }
    }
    if (ch_loc >= 'W' && ch_loc < 'Z') ++int_loc;
    if (ch_loc == 'R') return true;
    else {
        if (strcmp(str_1, str_2) > 0) {
            int_loc += 7;
            return true;
        } else {
            return false;
        }
    }
}

static bool func_3(Enumeration enum_val) {
    Enumeration enum_loc = enum_val;
    if (enum_loc == Ident_3) return true;
    return false;
}

static void proc_6(Enumeration enum_val, Enumeration& enum_ref) {
    enum_ref = enum_val;
    if (!func_3(enum_val)) enum_ref = Ident_4;
    switch (enum_val) {
        case Ident_1: enum_ref = Ident_1; break;
        case Ident_2: if (int_glob > 100) enum_ref = Ident_1; else enum_ref = Ident_4; break;
        case Ident_3: enum_ref = Ident_2; break;
        case Ident_4: break;
        case Ident_5: enum_ref = Ident_3; break;
    }
}

static void proc_7(int int_1, int int_2, int& int_ref) {
    int int_loc = int_1 + 2;
    int_ref = int_2 + int_loc;
}

static void proc_8(int arr_1[50], int arr_2[50][50], int int_1, int int_2) {
    int int_loc = int_1 + 5;
    arr_1[int_loc] = int_2;
    arr_1[int_loc + 1] = arr_1[int_loc];
    arr_1[int_loc + 30] = int_loc;
    for (int int_index = int_loc; int_index <= int_loc + 1; ++int_index)
        arr_2[int_loc][int_index] = int_loc;
    arr_2[int_loc][int_loc - 1] += 1;
    arr_2[int_loc + 20][int_loc] = arr_1[int_loc];
    int_glob = 5;
}

static void proc_1(Record* ptr_val) {
    Record* next = ptr_val->ptr_comp;
    *next = record_glob;
    ptr_val->variant.var_1.int_comp = 5;
    next->variant.var_1.int_comp = ptr_val->variant.var_1.int_comp;
    next->ptr_comp = ptr_val->ptr_comp;

    if (next->discr == Ident_1) {
        next->variant.var_1.int_comp = 6;
        proc_6(ptr_val->variant.var_1.enum_comp, next->variant.var_1.enum_comp);
        next->ptr_comp = record_glob.ptr_comp;
        proc_7(next->variant.var_1.int_comp, 10, next->variant.var_1.int_comp);
    } else {
        *ptr_val = *ptr_val->ptr_comp;
    }
}

static void proc_2(int& int_ref) {
    int int_loc = int_ref + 10;
    Enumeration enum_loc = Ident_1;
    while (true) {
        if (ch_1_glob == 'A') {
            --int_loc;
            int_ref = int_loc - int_glob;
            enum_loc = Ident_1;
        }
        if (enum_loc == Ident_1) break;
    }
}

static void proc_3(Record*& ptr_ref) {
    if (record_glob.ptr_comp != nullptr)
        ptr_ref = record_glob.ptr_comp->ptr_comp;
    else
        int_glob = 100;
    proc_7(10, int_glob, record_glob.variant.var_1.int_comp);
}

static void proc_4() {
    bool bool_loc = ch_1_glob == 'A';
    bool_glob = bool_loc | bool_glob;
    ch_2_glob = 'B';
}

static void proc_5() {
    ch_1_glob = 'A';
    bool_glob = false;
}

static int64_t run(int64_t iterations) {
    // 初始化
    record_glob.ptr_comp = &next_record_glob;
    record_glob.discr = Ident_1;
    record_glob.variant.var_1.enum_comp = Ident_3;
    record_glob.variant.var_1.int_comp = 40;
    strcpy(record_glob.variant.var_1.str_comp, "DHRYSTONE PROGRAM, SOME STRING");

    char str_1_loc[31], str_2_loc[31];
    strcpy(str_1_loc, "DHRYSTONE PROGRAM, 1'ST STRING");

    arr_2_glob[8][7] = 10;

    for (int64_t i = 0; i < iterations; ++i) {
        proc_5();
        proc_4();

        int int_1_loc = 2;
        int int_2_loc = 3;
        strcpy(str_2_loc, "DHRYSTONE PROGRAM, 2'ND STRING");

        Enumeration enum_loc = Ident_2;
        bool_glob = !func_2(str_1_loc, str_2_loc);

        int int_3_loc;
        while (int_1_loc < int_2_loc) {
            int_3_loc = 5 * int_1_loc - int_2_loc;
            proc_7(int_1_loc, int_2_loc, int_3_loc);
            ++int_1_loc;
        }

        proc_8(arr_1_glob, arr_2_glob, int_1_loc, int_3_loc);
        proc_1(&record_glob);

        for (char ch_index = 'A'; ch_index <= ch_2_glob; ++ch_index) {
            if (enum_loc == func_1(ch_index, 'C'))
                proc_6(Ident_1, enum_loc);
        }

        int_2_loc = int_2_loc * int_1_loc;
        int_1_loc = int_2_loc / int_3_loc;
        int_2_loc = 7 * (int_2_loc - int_3_loc) - int_1_loc;

        proc_2(int_1_loc);
    }

    return iterations;
}

} // namespace dhrystone

DhrystoneResult CpuBenchmark::runDhrystone() {
    DhrystoneResult result;
    int64_t iterations = (config_.iterations > 0) ?
        config_.iterations : 10000000; // 默认 1000 万次

    auto start = Clock::now();
    dhrystone::run(iterations);
    auto end = Clock::now();

    result.iterations = iterations;
    result.elapsed_sec = Duration(end - start).count();

    // Dhrystone per second
    double dhrystones_per_sec = iterations / result.elapsed_sec;
    // DMIPS = Dhrystones/sec / 1757 (VAX 11/780 基准)
    result.dmips = dhrystones_per_sec / 1757.0;

    auto platform = detectPlatform();
    if (platform.freq_mhz > 0) {
        result.dmips_per_mhz = result.dmips / platform.freq_mhz;
    }

    return result;
}

// ================================================================
//  2. CoreMark-like
// ================================================================

namespace coremark_like {

// ── 2a. 链表操作 ──

struct ListNode {
    int16_t data;
    int16_t key;
    ListNode* next;
};

static ListNode* createList(int size, std::mt19937& rng) {
    ListNode* head = nullptr;
    std::uniform_int_distribution<int16_t> dist(-32768, 32767);
    for (int i = 0; i < size; ++i) {
        auto* node = new ListNode{dist(rng), static_cast<int16_t>(i), head};
        head = node;
    }
    return head;
}

static void freeList(ListNode* head) {
    while (head) {
        auto* next = head->next;
        delete head;
        head = next;
    }
}

static ListNode* reverseList(ListNode* head) {
    ListNode* prev = nullptr;
    while (head) {
        ListNode* next = head->next;
        head->next = prev;
        prev = head;
        head = next;
    }
    return prev;
}

static ListNode* sortList(ListNode* head) {
    // 插入排序（CoreMark 使用类似方法）
    ListNode dummy{0, 0, nullptr};
    while (head) {
        ListNode* next = head->next;
        ListNode* pos = &dummy;
        while (pos->next && pos->next->data < head->data) {
            pos = pos->next;
        }
        head->next = pos->next;
        pos->next = head;
        head = next;
    }
    return dummy.next;
}

static int searchList(ListNode* head, int16_t key) {
    int count = 0;
    while (head) {
        if (head->data > key) ++count;
        head = head->next;
    }
    return count;
}

static int64_t runListBench(int iterations) {
    std::mt19937 rng(42);
    const int list_size = 1000;
    int64_t ops = 0;

    for (int i = 0; i < iterations; ++i) {
        ListNode* list = createList(list_size, rng);
        list = reverseList(list);
        list = sortList(list);
        searchList(list, 0);
        freeList(list);
        ops += 4; // 4 operations per iteration
    }
    return ops;
}

// ── 2b. 矩阵运算 ──

static int64_t runMatrixBench(int iterations) {
    const int N = 32;
    std::vector<int32_t> A(N * N), B(N * N), C(N * N);

    // 初始化矩阵
    std::mt19937 rng(123);
    std::uniform_int_distribution<int32_t> dist(-100, 100);
    for (int i = 0; i < N * N; ++i) {
        A[i] = dist(rng);
        B[i] = dist(rng);
    }

    int64_t ops = 0;
    for (int iter = 0; iter < iterations; ++iter) {
        // C = A * B（整数矩阵乘法）
        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < N; ++j) {
                int32_t sum = 0;
                for (int k = 0; k < N; ++k) {
                    sum += A[i * N + k] * B[k * N + j];
                }
                C[i * N + j] = sum;
            }
        }
        ops += static_cast<int64_t>(N) * N * N * 2; // mul + add
    }

    // 防优化
    volatile int32_t sink = C[0];
    (void)sink;

    return ops;
}

// ── 2c. 状态机 ──

static int64_t runStateMachineBench(int iterations) {
    // 模拟一个简单的协议解析状态机
    enum State { IDLE, HEADER, DATA, CHECKSUM, DONE, NUM_STATES };

    // 状态转移表
    // [current_state][input] → next_state
    static const State transitions[NUM_STATES][4] = {
        /* IDLE     */ { HEADER,   IDLE,     IDLE,     IDLE     },
        /* HEADER   */ { HEADER,   DATA,     IDLE,     IDLE     },
        /* DATA     */ { DATA,     DATA,     CHECKSUM, IDLE     },
        /* CHECKSUM */ { IDLE,     IDLE,     IDLE,     DONE     },
        /* DONE     */ { IDLE,     IDLE,     IDLE,     IDLE     },
    };

    std::mt19937 rng(789);
    std::uniform_int_distribution<int> input_dist(0, 3);

    int64_t total_transitions = 0;

    for (int iter = 0; iter < iterations; ++iter) {
        State state = IDLE;
        for (int step = 0; step < 1000; ++step) {
            int input = input_dist(rng);
            state = transitions[state][input];
            ++total_transitions;
        }
        // 防优化
        volatile int s = state;
        (void)s;
    }

    return total_transitions;
}

} // namespace coremark_like

CoremarkResult CpuBenchmark::runCoremark() {
    CoremarkResult result;

    int iterations = (config_.iterations > 0) ?
        config_.iterations / 100 : 5000; // 缩放到合理范围
    if (iterations < 100) iterations = 100;

    auto start = Clock::now();

    // 链表
    auto t1 = Clock::now();
    int64_t list_ops = coremark_like::runListBench(iterations);
    auto t2 = Clock::now();
    double list_time = Duration(t2 - t1).count();
    result.list_ops_per_sec = list_ops / list_time;

    // 矩阵
    auto t3 = Clock::now();
    int64_t matrix_ops = coremark_like::runMatrixBench(iterations);
    auto t4 = Clock::now();
    double matrix_time = Duration(t4 - t3).count();
    result.matrix_ops_per_sec = matrix_ops / matrix_time;

    // 状态机
    auto t5 = Clock::now();
    int64_t sm_ops = coremark_like::runStateMachineBench(iterations);
    auto t6 = Clock::now();
    double sm_time = Duration(t6 - t5).count();
    result.state_machine_ops_per_sec = sm_ops / sm_time;

    auto end = Clock::now();
    result.elapsed_sec = Duration(end - start).count();

    // 综合分（iterations/sec/MHz）
    auto platform = detectPlatform();
    double composite = static_cast<double>(iterations) / result.elapsed_sec;
    if (platform.freq_mhz > 0) {
        result.composite_per_mhz = composite / platform.freq_mhz;
    } else {
        result.composite_per_mhz = composite; // 无频率信息时直接用 iter/s
    }

    return result;
}

// ================================================================
//  3. Stream 内存带宽
// ================================================================

StreamResult CpuBenchmark::runStream() {
    StreamResult result;
    int64_t N = config_.stream_array_size;
    int ntimes = config_.stream_ntimes;
    int num_threads = getThreadCount();
    double scalar = 3.0;

    result.array_size = N;
    result.ntimes = ntimes;
    result.num_threads = num_threads;

    // 分配数组（double 类型，与标准 Stream 一致）
    std::vector<double> a(N), b(N), c(N);

    // 初始化
    for (int64_t j = 0; j < N; ++j) {
        a[j] = 1.0;
        b[j] = 2.0;
        c[j] = 0.0;
    }

    // 数据量（字节）
    // Copy:  读 a, 写 c → 2 * N * 8
    // Scale: 读 c, 写 b → 2 * N * 8
    // Add:   读 a, 读 b, 写 c → 3 * N * 8
    // Triad: 读 b, 读 c, 写 a → 3 * N * 8
    const double bytes_copy  = 2.0 * N * sizeof(double);
    const double bytes_scale = 2.0 * N * sizeof(double);
    const double bytes_add   = 3.0 * N * sizeof(double);
    const double bytes_triad = 3.0 * N * sizeof(double);

    // 辅助：并行执行
    auto parallel_for = [&](int n_threads, int64_t total,
                            std::function<void(int64_t, int64_t)> fn) {
        if (n_threads <= 1) {
            fn(0, total);
            return;
        }
        std::vector<std::thread> threads;
        int64_t chunk = total / n_threads;
        auto affinity_list = parseAffinity();

        for (int t = 0; t < n_threads; ++t) {
            int64_t lo = t * chunk;
            int64_t hi = (t == n_threads - 1) ? total : lo + chunk;
            threads.emplace_back([&, lo, hi, t]() {
                if (t < static_cast<int>(affinity_list.size())) {
                    setThreadAffinity(affinity_list[t]);
                }
                fn(lo, hi);
            });
        }
        for (auto& th : threads) th.join();
    };

    // ── Copy: c[j] = a[j] ──
    {
        std::vector<double> times;
        for (int k = 0; k < ntimes; ++k) {
            auto t0 = Clock::now();
            parallel_for(num_threads, N, [&](int64_t lo, int64_t hi) {
                for (int64_t j = lo; j < hi; ++j) c[j] = a[j];
            });
            auto t1 = Clock::now();
            times.push_back(Duration(t1 - t0).count());
        }
        double avg = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
        double min_t = *std::min_element(times.begin(), times.end());
        double max_t = *std::max_element(times.begin(), times.end());
        result.ops.push_back({"Copy", bytes_copy / avg / 1e6, avg, min_t, max_t});
    }

    // ── Scale: b[j] = scalar * c[j] ──
    {
        std::vector<double> times;
        for (int k = 0; k < ntimes; ++k) {
            auto t0 = Clock::now();
            parallel_for(num_threads, N, [&](int64_t lo, int64_t hi) {
                for (int64_t j = lo; j < hi; ++j) b[j] = scalar * c[j];
            });
            auto t1 = Clock::now();
            times.push_back(Duration(t1 - t0).count());
        }
        double avg = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
        double min_t = *std::min_element(times.begin(), times.end());
        double max_t = *std::max_element(times.begin(), times.end());
        result.ops.push_back({"Scale", bytes_scale / avg / 1e6, avg, min_t, max_t});
    }

    // ── Add: c[j] = a[j] + b[j] ──
    {
        std::vector<double> times;
        for (int k = 0; k < ntimes; ++k) {
            auto t0 = Clock::now();
            parallel_for(num_threads, N, [&](int64_t lo, int64_t hi) {
                for (int64_t j = lo; j < hi; ++j) c[j] = a[j] + b[j];
            });
            auto t1 = Clock::now();
            times.push_back(Duration(t1 - t0).count());
        }
        double avg = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
        double min_t = *std::min_element(times.begin(), times.end());
        double max_t = *std::max_element(times.begin(), times.end());
        result.ops.push_back({"Add", bytes_add / avg / 1e6, avg, min_t, max_t});
    }

    // ── Triad: a[j] = b[j] + scalar * c[j] ──
    {
        std::vector<double> times;
        for (int k = 0; k < ntimes; ++k) {
            auto t0 = Clock::now();
            parallel_for(num_threads, N, [&](int64_t lo, int64_t hi) {
                for (int64_t j = lo; j < hi; ++j) a[j] = b[j] + scalar * c[j];
            });
            auto t1 = Clock::now();
            times.push_back(Duration(t1 - t0).count());
        }
        double avg = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
        double min_t = *std::min_element(times.begin(), times.end());
        double max_t = *std::max_element(times.begin(), times.end());
        result.ops.push_back({"Triad", bytes_triad / avg / 1e6, avg, min_t, max_t});
    }

    // ── SingleCopy (单线程): c[j] = a[j] ──
    {
        std::vector<double> times;
        for (int k = 0; k < ntimes; ++k) {
            auto t0 = Clock::now();
            for (int64_t j = 0; j < N; ++j) c[j] = a[j];
            auto t1 = Clock::now();
            times.push_back(Duration(t1 - t0).count());
        }
        double avg = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
        double min_t = *std::min_element(times.begin(), times.end());
        double max_t = *std::max_element(times.begin(), times.end());
        result.ops.push_back({"SingleCopy", bytes_copy / avg / 1e6, avg, min_t, max_t});
    }

    return result;
}

// ================================================================
//  4. Multi-thread Scaling
// ================================================================

MultiThreadResult CpuBenchmark::runMultiThread() {
    MultiThreadResult result;
    int max_threads = getThreadCount();
    int64_t N = config_.stream_array_size;
    int ntimes = std::min(config_.stream_ntimes, 20); // 缩短以减少总时间

    result.data_size_bytes = N * static_cast<int64_t>(sizeof(double));

    std::vector<double> src(N, 1.0);
    std::vector<double> dst(N, 0.0);

    const double data_bytes = 2.0 * N * sizeof(double); // 读 + 写

    auto affinity_list = parseAffinity();

    // 生成线程数序列: 1, 2, 4, ..., max_threads
    std::vector<int> thread_counts;
    for (int t = 1; t <= max_threads; t *= 2) {
        thread_counts.push_back(t);
    }
    if (thread_counts.back() != max_threads) {
        thread_counts.push_back(max_threads);
    }

    double single_rate = 0.0;

    for (int n_threads : thread_counts) {
        std::vector<double> times;

        for (int k = 0; k < ntimes; ++k) {
            auto t0 = Clock::now();

            if (n_threads == 1) {
                // 单线程
                if (!affinity_list.empty()) {
                    setThreadAffinity(affinity_list[0]);
                }
                for (int64_t j = 0; j < N; ++j) dst[j] = src[j];
            } else {
                // 多线程
                std::vector<std::thread> threads;
                int64_t chunk = N / n_threads;

                for (int t = 0; t < n_threads; ++t) {
                    int64_t lo = t * chunk;
                    int64_t hi = (t == n_threads - 1) ? N : lo + chunk;
                    threads.emplace_back([&, lo, hi, t]() {
                        if (t < static_cast<int>(affinity_list.size())) {
                            setThreadAffinity(affinity_list[t]);
                        }
                        for (int64_t j = lo; j < hi; ++j) dst[j] = src[j];
                    });
                }
                for (auto& th : threads) th.join();
            }

            auto t1 = Clock::now();
            times.push_back(Duration(t1 - t0).count());
        }

        double avg = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
        double rate = data_bytes / avg / 1e6; // MB/s

        if (n_threads == 1) single_rate = rate;

        MultiThreadResult::ScalingPoint point;
        point.num_threads = n_threads;
        point.rate_mb_s = rate;
        point.speedup = (single_rate > 0) ? rate / single_rate : 1.0;
        result.points.push_back(point);
    }

    return result;
}

// ================================================================
//  5. Cache Performance
// ================================================================

CacheResult CpuBenchmark::runCache() {
    CacheResult result;

    // 测试不同的数据块大小
    std::vector<int64_t> block_sizes_kb = {
        4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192
    };

    const int num_accesses = 1000000; // 每个块大小的访问次数

    for (int64_t size_kb : block_sizes_kb) {
        int64_t size_bytes = size_kb * 1024;
        int64_t num_elements = size_bytes / static_cast<int64_t>(sizeof(int32_t));
        if (num_elements < 16) continue;

        // 分配并初始化数据
        std::vector<int32_t> data(num_elements);
        std::iota(data.begin(), data.end(), 0);

        // 创建随机访问模式（pointer-chasing）
        // 使用随机排列生成链表式的访问模式，避免硬件预取器优化
        std::vector<int64_t> indices(num_elements);
        std::iota(indices.begin(), indices.end(), 0);
        std::mt19937 rng(42 + static_cast<unsigned>(size_kb));
        std::shuffle(indices.begin(), indices.end(), rng);

        // 构建链式访问：data[indices[i]] = indices[i+1]
        for (int64_t i = 0; i < num_elements - 1; ++i) {
            data[indices[i]] = static_cast<int32_t>(indices[i + 1]);
        }
        data[indices[num_elements - 1]] = static_cast<int32_t>(indices[0]);

        // 预热
        int32_t idx = 0;
        for (int i = 0; i < num_elements; ++i) {
            idx = data[idx];
        }

        // 正式测量
        auto t0 = Clock::now();
        idx = 0;
        for (int i = 0; i < num_accesses; ++i) {
            idx = data[idx];
        }
        auto t1 = Clock::now();

        // 防优化
        volatile int32_t sink = idx;
        (void)sink;

        double elapsed = Duration(t1 - t0).count();
        double ns_per_access = (elapsed * 1e9) / num_accesses;

        // 推断缓存层级
        std::string hint;
        if (ns_per_access < 3.0) {
            hint = "L1 hit";
        } else if (ns_per_access < 10.0) {
            hint = "L2 hit";
        } else if (ns_per_access < 30.0) {
            hint = "L2 miss -> DDR";
        } else {
            hint = "DDR";
        }

        result.levels.push_back({size_kb, ns_per_access, hint});
    }

    return result;
}

// ================================================================
// 运行入口
// ================================================================

BenchmarkResults CpuBenchmark::runAll() {
    BenchmarkResults results;
    results.platform = detectPlatform();

    LOG4CPLUS_INFO(logger_, "平台信息: arch=" << results.platform.arch
        << " os=" << results.platform.os
        << " cores=" << results.platform.num_cores
        << " freq=" << results.platform.freq_mhz << "MHz"
        << " model=" << results.platform.cpu_model);

    if (shouldRun("dhrystone")) {
        LOG4CPLUS_INFO(logger_, "[1/5] 运行 Dhrystone 2.1 ...");
        results.dhrystone = runDhrystone();
        results.has_dhrystone = true;
        LOG4CPLUS_INFO_FMT(logger_, "  Dhrystone 完成: %.0f DMIPS, 耗时 %.2f 秒",
                           results.dhrystone.dmips, results.dhrystone.elapsed_sec);
    }

    if (shouldRun("coremark")) {
        LOG4CPLUS_INFO(logger_, "[2/5] 运行 CoreMark-like ...");
        results.coremark = runCoremark();
        results.has_coremark = true;
        LOG4CPLUS_INFO_FMT(logger_, "  CoreMark-like 完成: 耗时 %.2f 秒",
                           results.coremark.elapsed_sec);
    }

    if (shouldRun("stream")) {
        LOG4CPLUS_INFO(logger_, "[3/5] 运行 Stream 内存带宽 ...");
        results.stream = runStream();
        results.has_stream = true;
        if (!results.stream.ops.empty()) {
            LOG4CPLUS_INFO_FMT(logger_, "  Stream 完成: Copy=%.1f MB/s",
                               results.stream.ops[0].rate_mb_s);
        }
    }

    if (shouldRun("multithread")) {
        LOG4CPLUS_INFO(logger_, "[4/5] 运行 Multi-thread Scaling ...");
        results.multithread = runMultiThread();
        results.has_multithread = true;
        LOG4CPLUS_INFO_FMT(logger_, "  Multi-thread 完成: %zu 个测试点",
                           results.multithread.points.size());
    }

    if (shouldRun("cache")) {
        LOG4CPLUS_INFO(logger_, "[5/5] 运行 Cache Performance ...");
        results.cache = runCache();
        results.has_cache = true;
        LOG4CPLUS_INFO_FMT(logger_, "  Cache 完成: %zu 个块大小",
                           results.cache.levels.size());
    }

    return results;
}

// ================================================================
// 终端输出
// ================================================================

void CpuBenchmark::printResults(const BenchmarkResults& r) {
    std::cout << "\n"
        "═══════════════════════════════════════════════════════════\n"
        "  CPU Performance Benchmark Results\n"
        "  Platform: " << r.platform.arch
        << " | Cores: " << r.platform.num_cores
        << " | Freq: " << (r.platform.freq_mhz > 0 ?
            std::to_string(r.platform.freq_mhz) + " MHz" : "N/A")
        << "\n";
    if (!r.platform.cpu_model.empty()) {
        std::cout << "  Model: " << r.platform.cpu_model << "\n";
    }
    std::cout <<
        "═══════════════════════════════════════════════════════════\n";

    // ── Dhrystone ──
    if (r.has_dhrystone) {
        std::cout << "\n  [1/5] Dhrystone 2.1\n"
            << std::fixed
            << "        Iterations:  " << r.dhrystone.iterations << "\n"
            << "        Time:        " << std::setprecision(3) << r.dhrystone.elapsed_sec << " sec\n"
            << "        DMIPS:       " << std::setprecision(0) << r.dhrystone.dmips << "\n";
        if (r.dhrystone.dmips_per_mhz > 0) {
            std::cout << "        DMIPS/MHz:   " << std::setprecision(2) << r.dhrystone.dmips_per_mhz << "\n";
        }
    }

    // ── CoreMark-like ──
    if (r.has_coremark) {
        std::cout << "\n  [2/5] CoreMark-like\n"
            << std::fixed
            << "        List ops:        " << std::setprecision(0) << r.coremark.list_ops_per_sec << " ops/s\n"
            << "        Matrix ops:      " << std::setprecision(0) << r.coremark.matrix_ops_per_sec << " ops/s\n"
            << "        State machine:   " << std::setprecision(0) << r.coremark.state_machine_ops_per_sec << " trans/s\n"
            << "        Composite:       " << std::setprecision(2) << r.coremark.composite_per_mhz
            << (r.platform.freq_mhz > 0 ? " iter/s/MHz" : " iter/s") << "\n"
            << "        Time:            " << std::setprecision(3) << r.coremark.elapsed_sec << " sec\n";
    }

    // ── Stream ──
    if (r.has_stream) {
        std::cout << "\n  [3/5] Stream (array: "
            << formatBytes(r.stream.array_size * static_cast<int64_t>(sizeof(double)))
            << ", " << r.stream.ntimes << " iterations, "
            << r.stream.num_threads << " threads)\n";

        std::cout << std::fixed << std::setprecision(1);
        for (const auto& op : r.stream.ops) {
            std::cout << "        " << std::left << std::setw(12) << (op.name + ":")
                      << std::right << std::setw(10) << op.rate_mb_s << " MB/s"
                      << "  (avg=" << std::setprecision(4) << op.avg_time_sec * 1000 << " ms"
                      << ", min=" << op.min_time_sec * 1000 << " ms"
                      << ", max=" << op.max_time_sec * 1000 << " ms)\n";
            std::cout << std::setprecision(1);
        }
    }

    // ── Multi-thread ──
    if (r.has_multithread) {
        std::cout << "\n  [4/5] Multi-thread Scaling (data: "
            << formatBytes(r.multithread.data_size_bytes) << ")\n"
            << std::fixed << std::setprecision(1);

        for (const auto& pt : r.multithread.points) {
            std::cout << "        " << std::setw(2) << pt.num_threads
                      << " thread" << (pt.num_threads > 1 ? "s" : " ")
                      << ":  " << std::setw(10) << pt.rate_mb_s << " MB/s"
                      << "  (" << std::setprecision(2) << pt.speedup << "x)\n";
            std::cout << std::setprecision(1);
        }
    }

    // ── Cache ──
    if (r.has_cache) {
        std::cout << "\n  [5/5] Cache Performance (pointer-chasing)\n"
            << std::fixed;

        for (const auto& lv : r.cache.levels) {
            std::cout << "        " << std::setw(6) << lv.block_size_kb << " KB: "
                      << std::setw(8) << std::setprecision(1) << lv.ns_per_access << " ns/access"
                      << "  (" << lv.hint << ")\n";
        }
    }

    std::cout << "\n═══════════════════════════════════════════════════════════\n"
              << std::endl;
}

// ================================================================
// JSON 报告
// ================================================================

bool CpuBenchmark::generateJsonReport(const BenchmarkResults& r,
                                       const std::string& path) {
    std::ofstream f(path);
    if (!f.is_open()) return false;

    f << "{\n";
    f << "  \"platform\": {\n"
      << "    \"arch\": \"" << r.platform.arch << "\",\n"
      << "    \"os\": \"" << r.platform.os << "\",\n"
      << "    \"cores\": " << r.platform.num_cores << ",\n"
      << "    \"freq_mhz\": " << r.platform.freq_mhz << ",\n"
      << "    \"cpu_model\": \"" << r.platform.cpu_model << "\"\n"
      << "  }";

    // Dhrystone
    if (r.has_dhrystone) {
        f << ",\n" << std::fixed
          << "  \"dhrystone\": {\n"
          << "    \"iterations\": " << r.dhrystone.iterations << ",\n"
          << "    \"elapsed_sec\": " << std::setprecision(6) << r.dhrystone.elapsed_sec << ",\n"
          << "    \"dmips\": " << std::setprecision(2) << r.dhrystone.dmips << ",\n"
          << "    \"dmips_per_mhz\": " << std::setprecision(4) << r.dhrystone.dmips_per_mhz << "\n"
          << "  }";
    }

    // CoreMark-like
    if (r.has_coremark) {
        f << ",\n" << std::fixed
          << "  \"coremark\": {\n"
          << "    \"list_ops_per_sec\": " << std::setprecision(2) << r.coremark.list_ops_per_sec << ",\n"
          << "    \"matrix_ops_per_sec\": " << std::setprecision(2) << r.coremark.matrix_ops_per_sec << ",\n"
          << "    \"state_machine_ops_per_sec\": " << std::setprecision(2) << r.coremark.state_machine_ops_per_sec << ",\n"
          << "    \"composite_per_mhz\": " << std::setprecision(4) << r.coremark.composite_per_mhz << ",\n"
          << "    \"elapsed_sec\": " << std::setprecision(6) << r.coremark.elapsed_sec << "\n"
          << "  }";
    }

    // Stream
    if (r.has_stream) {
        f << ",\n  \"stream\": {\n"
          << "    \"array_size\": " << r.stream.array_size << ",\n"
          << "    \"ntimes\": " << r.stream.ntimes << ",\n"
          << "    \"num_threads\": " << r.stream.num_threads << ",\n"
          << "    \"ops\": [\n";
        for (size_t i = 0; i < r.stream.ops.size(); ++i) {
            const auto& op = r.stream.ops[i];
            f << "      {\"name\": \"" << op.name << "\""
              << ", \"rate_mb_s\": " << std::setprecision(2) << op.rate_mb_s
              << ", \"avg_time_sec\": " << std::setprecision(8) << op.avg_time_sec
              << ", \"min_time_sec\": " << op.min_time_sec
              << ", \"max_time_sec\": " << op.max_time_sec
              << "}";
            if (i + 1 < r.stream.ops.size()) f << ",";
            f << "\n";
        }
        f << "    ]\n  }";
    }

    // Multi-thread
    if (r.has_multithread) {
        f << ",\n  \"multithread\": {\n"
          << "    \"data_size_bytes\": " << r.multithread.data_size_bytes << ",\n"
          << "    \"points\": [\n";
        for (size_t i = 0; i < r.multithread.points.size(); ++i) {
            const auto& pt = r.multithread.points[i];
            f << "      {\"threads\": " << pt.num_threads
              << ", \"rate_mb_s\": " << std::setprecision(2) << pt.rate_mb_s
              << ", \"speedup\": " << std::setprecision(4) << pt.speedup
              << "}";
            if (i + 1 < r.multithread.points.size()) f << ",";
            f << "\n";
        }
        f << "    ]\n  }";
    }

    // Cache
    if (r.has_cache) {
        f << ",\n  \"cache\": [\n";
        for (size_t i = 0; i < r.cache.levels.size(); ++i) {
            const auto& lv = r.cache.levels[i];
            f << "    {\"block_size_kb\": " << lv.block_size_kb
              << ", \"ns_per_access\": " << std::setprecision(2) << lv.ns_per_access
              << ", \"hint\": \"" << lv.hint << "\""
              << "}";
            if (i + 1 < r.cache.levels.size()) f << ",";
            f << "\n";
        }
        f << "  ]";
    }

    f << "\n}\n";
    return f.good();
}

} // namespace test::cpu
