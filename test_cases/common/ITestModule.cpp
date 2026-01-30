/**
 * @file ITestModule.cpp
 * @brief ITestModule 公共方法实现
 * 
 * 实现所有测试模块共享的 runSingle/runCompare/runParallel 方法
 */

#include "ITestModule.hpp"
#include <log4cplus/loggingmacros.h>

namespace test {
namespace common {

// ========================================
// 公共执行方法实现
// ========================================

consumer::ConsumeResult ITestModule::runSingle(
    const WorkerConfig& config,
    uint32_t flags,
    const std::string& test_name
) {
    auto& logger = getLogger();
    
    if (!test_name.empty()) {
        consumer::BufferConsumerService::printHeader(test_name, config);
    }
    
    LOG4CPLUS_DEBUG_FMT(logger, "runSingle: mode=SINGLE, flags=0x%X", flags);
    
    consumer::BufferConsumerService service;
    return service.start({config}, consumer::ExecuteMode::SINGLE, flags);
}

consumer::ConsumeResult ITestModule::runCompare(
    const std::vector<WorkerConfig>& configs,
    uint32_t flags,
    const std::string& test_name
) {
    auto& logger = getLogger();
    
    if (!test_name.empty()) {
        LOG4CPLUS_INFO(logger, "");
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
        LOG4CPLUS_INFO_FMT(logger, "  %s", test_name.c_str());
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
        LOG4CPLUS_INFO_FMT(logger, "  Mode:       ExecuteMode::COMPARE");
        LOG4CPLUS_INFO_FMT(logger, "  Workers:    %zu", configs.size());
        LOG4CPLUS_INFO_FMT(logger, "  Flags:      0x%X", flags);
        if (!configs.empty()) {
            LOG4CPLUS_INFO_FMT(logger, "  Input:      %s", configs[0].data_source.path.c_str());
            if (configs[0].consumer_type.compare.enable_psnr) {
                LOG4CPLUS_INFO_FMT(logger, "  PSNR:       enabled (min: %.1f dB)", configs[0].consumer_type.compare.min_psnr);
            }
            if (configs[0].consumer_type.compare.enable_ssim) {
                LOG4CPLUS_INFO_FMT(logger, "  SSIM:       enabled (min: %.2f)", configs[0].consumer_type.compare.min_ssim);
            }
            // 显示叠加的消费类型
            if (flags & consumer::CONSUME_DISPLAY) {
                LOG4CPLUS_INFO(logger, "  Display:    enabled (stacked)");
            }
            if (flags & consumer::CONSUME_SAVE_RAW) {
                LOG4CPLUS_INFO_FMT(logger, "  Save:       enabled to %s (stacked)", 
                                  configs[0].consumer_type.save_raw.getOutputPath().c_str());
            }
        }
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
    }
    
    LOG4CPLUS_DEBUG_FMT(logger, "runCompare: mode=COMPARE, workers=%zu, flags=0x%X", 
                        configs.size(), flags);
    
    consumer::BufferConsumerService service;
    return service.start(configs, consumer::ExecuteMode::COMPARE, flags);
}

consumer::ConsumeResult ITestModule::runParallel(
    const std::vector<WorkerConfig>& configs,
    uint32_t flags,
    const std::string& test_name
) {
    auto& logger = getLogger();
    
    if (!test_name.empty()) {
        LOG4CPLUS_INFO(logger, "");
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
        LOG4CPLUS_INFO_FMT(logger, "  %s", test_name.c_str());
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
        LOG4CPLUS_INFO_FMT(logger, "  Mode:       ExecuteMode::PARALLEL");
        LOG4CPLUS_INFO_FMT(logger, "  Workers:    %zu", configs.size());
        LOG4CPLUS_INFO_FMT(logger, "  Flags:      0x%X", flags);
        if (!configs.empty()) {
            LOG4CPLUS_INFO_FMT(logger, "  Input:      %s", configs[0].data_source.path.c_str());
        }
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
    }
    
    LOG4CPLUS_DEBUG_FMT(logger, "runParallel: mode=PARALLEL, workers=%zu, flags=0x%X", 
                        configs.size(), flags);
    
    consumer::BufferConsumerService service;
    return service.start(configs, consumer::ExecuteMode::PARALLEL, flags);
}

} // namespace common
} // namespace test
