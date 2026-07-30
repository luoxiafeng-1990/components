/**
 * @file ExecuteMode.cpp
 * @brief ExecuteMode 静态方法实现（从原 ITestModule.cpp 提取）
 */

#include "ExecuteMode.hpp"
#include "productionline/line/VideoProductionLine.hpp"
#include "consumptionline/core/BufferConsumerStrategies.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

namespace test {

static log4cplus::Logger& getLogger() {
    static log4cplus::Logger logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.test.ExecuteMode"));
    return logger;
}

consumer::ConsumeResult ExecuteMode::single(
    const WorkerConfig& config,
    uint32_t flags,
    const std::string& test_name)
{
    if (!test_name.empty()) {
        consumer::BufferConsumerService::printHeader(test_name, config);
    }
    LOG4CPLUS_DEBUG_FMT(getLogger(), "single: mode=SINGLE, flags=0x%X", flags);

    consumer::BufferConsumerService service;
    return service.start({config}, consumer::ExecuteMode::SINGLE, flags);
}

consumer::ConsumeResult ExecuteMode::compare(
    const std::vector<WorkerConfig>& configs,
    uint32_t flags,
    const std::string& test_name)
{
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
            if (flags & consumer::CONSUME_DISPLAY) {
                LOG4CPLUS_INFO(logger, "  Display:    enabled (stacked)");
            }
            if (flags & consumer::CONSUME_SAVE_RAW) {
                LOG4CPLUS_INFO_FMT(logger, "  Save:       enabled to %s (stacked)",
                                  configs[0].consumer_type.save_raw.output_paths.empty()
                                      ? "" : configs[0].consumer_type.save_raw.output_paths[0].c_str());
            }
        }
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
    }

    LOG4CPLUS_DEBUG_FMT(logger, "compare: mode=COMPARE, workers=%zu, flags=0x%X",
                        configs.size(), flags);

    consumer::BufferConsumerService service;
    return service.start(configs, consumer::ExecuteMode::COMPARE, flags);
}

consumer::ConsumeResult ExecuteMode::parallel(
    const std::vector<WorkerConfig>& configs,
    uint32_t flags,
    const std::string& test_name)
{
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

    LOG4CPLUS_DEBUG_FMT(logger, "parallel: mode=PARALLEL, workers=%zu, flags=0x%X",
                        configs.size(), flags);

    consumer::BufferConsumerService service;
    return service.start(configs, consumer::ExecuteMode::PARALLEL, flags);
}

consumer::ConsumeResult ExecuteMode::channelCompare(
    const WorkerConfig& config,
    const std::string& test_name)
{
    auto& logger = getLogger();
    consumer::ConsumeResult result;

    LOG4CPLUS_INFO_FMT(logger, "channelCompare: ref_ch=%d, cmp_ch=%d",
        config.consumer_type.compare.reference_channel,
        config.consumer_type.compare.compare_channel);

    VideoProductionLine producer;
    if (!producer.start(config)) {
        result.success = false;
        result.error_message = "Failed to start production line";
        return result;
    }

    auto pool_id = producer.getWorkingBufferPoolId();
    auto pool = ComponentTopology::getInstance().getPool(pool_id).lock();
    if (!pool) {
        result.success = false;
        result.error_message = "Failed to get BufferPool";
        producer.stop();
        return result;
    }

    auto consumer_ptr = std::make_shared<consumer::ChannelCompareConsumer>(
        pool, config.consumer_type.compare);

    int max_frames = config.consumer_type.max_frames;
    consumer_ptr->run(max_frames);
    producer.stop();

    result.success = consumer_ptr->isPassed();
    result.frames_consumed = consumer_ptr->getComparedCount();
    if (!result.success) {
        result.error_message = "Channel compare failed: " + consumer_ptr->getStats();
    }

    std::cout << "\n" << test_name << ": "
              << (result.success ? "PASSED" : "FAILED") << "\n"
              << "  Compared: " << consumer_ptr->getComparedCount() << " frames\n"
              << "  Avg PSNR: " << consumer_ptr->getAveragePsnr() << " dB\n"
              << "  Avg SSIM: " << consumer_ptr->getAverageSsim() << "\n"
              << "  Mismatch: " << consumer_ptr->getMismatchCount() << "\n";

    return result;
}

uint32_t ExecuteMode::buildConsumeFlags(const WorkerConfig& config) {
    uint32_t flags = consumer::CONSUME_COUNT;
    if (config.consumer_type.display.enable)
        flags |= consumer::CONSUME_DISPLAY;
    if (config.consumer_type.save_raw.enable)
        flags |= consumer::CONSUME_SAVE_RAW;
    if (config.consumer_type.save_encoded.enable)
        flags |= consumer::CONSUME_SAVE_ENCODED;
    if (config.consumer_type.npu_inference.enable)
        flags |= consumer::CONSUME_NPU_INFERENCE;
    if (config.consumer_type.jpeg_encode.enable)
        flags |= consumer::CONSUME_JPEG_ENCODE;
    if (config.consumer_type.video_encode.enable)
        flags |= consumer::CONSUME_VIDEO_ENCODE;
    if (config.consumer_type.opencv.enable) {
        flags |= consumer::CONSUME_OPENCV;
    }
    return flags;
}

} // namespace test
