/**
 * @file ExecuteMode.hpp
 * @brief 执行模式静态工具类
 *
 * 从原 ITestModule.cpp 提取的三种执行模式：
 * - single()   → BufferConsumerService::ExecuteMode::SINGLE
 * - compare()  → BufferConsumerService::ExecuteMode::COMPARE
 * - parallel() → BufferConsumerService::ExecuteMode::PARALLEL
 *
 * @version 5.0
 */

#ifndef EXECUTE_MODE_HPP
#define EXECUTE_MODE_HPP

#include "consumptionline/core/BufferConsumerService.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"

#include <string>
#include <vector>
#include <cstdint>

namespace test {

class ExecuteMode {
public:
    static consumer::ConsumeResult single(
        const WorkerConfig& config,
        uint32_t flags,
        const std::string& test_name = "");

    static consumer::ConsumeResult compare(
        const std::vector<WorkerConfig>& configs,
        uint32_t flags,
        const std::string& test_name = "");

    static consumer::ConsumeResult parallel(
        const std::vector<WorkerConfig>& configs,
        uint32_t flags,
        const std::string& test_name = "");

    /**
     * @brief PP 多通道比较（channel compare）
     *
     * 使用 VideoProductionLine + ChannelCompareConsumer 直接对比两个 PP 通道。
     * 独立于 BufferConsumerService 的执行路径。
     */
    static consumer::ConsumeResult channelCompare(
        const WorkerConfig& config,
        const std::string& test_name = "");

    static uint32_t buildConsumeFlags(const WorkerConfig& config);
};

} // namespace test

#endif // EXECUTE_MODE_HPP
