/**
 * @file TestExecutor.cpp
 * @brief TestExecutor 实现
 */

#include "TestExecutor.hpp"
#include "productionline/VideoProductionLine.hpp"
#include "productionline/MultiWorkerProductionLine.hpp"
#include "productionline/io/BufferWriter.hpp"
#include "productionline/io/BufferConsumerService.hpp"
#include "productionline/io/BufferComparator.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "buffer/bufferpool/Buffer.hpp"
#include "display/LinuxFramebufferDevice.hpp"

#include <chrono>
#include <signal.h>
#include <iostream>
#include <iomanip>
#include <ctime>
#include <thread>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace common {

// 静态成员初始化
std::atomic<bool> TestExecutor::g_running_{true};
std::atomic<bool> TestExecutor::g_interrupted_{false};

// 获取模块级日志实例
log4cplus::Logger& TestExecutor::getLogger() {
    static log4cplus::Logger logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.test.Executor"));
    return logger;
}

// 信号处理函数
static void signal_handler(int sig) {
    (void)sig;
    TestExecutor::requestStop();
}

void TestExecutor::setupSignalHandler() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    g_running_ = true;
    g_interrupted_ = false;
}

bool TestExecutor::isRunning() {
    return g_running_;
}

void TestExecutor::requestStop() {
    g_running_ = false;
    g_interrupted_ = true;
}

void TestExecutor::resetState() {
    g_running_ = true;
    g_interrupted_ = false;
}

TestResult TestExecutor::runDecode(const WorkerConfig& config) {
    TestResult result;
    
    // 设置信号处理
    setupSignalHandler();
    
    // 提取消费者参数
    const auto& consumer_cfg = config.consumer;
    
    // 1. 初始化显示设备（如果启用显示消费）
    std::unique_ptr<LinuxFramebufferDevice> display;
    if (consumer_cfg.enable_display) {
        display = std::make_unique<LinuxFramebufferDevice>();
        if (!display->initialize(0)) {
            LOG4CPLUS_ERROR(getLogger(), "Failed to initialize display device");
            result.error_message = "Failed to initialize display device";
            return result;
        }
        LOG4CPLUS_INFO(getLogger(), "Display initialized: " << display->getWidth() 
                       << "x" << display->getHeight() << " @ " << display->getBitsPerPixel() << "bpp");
    }
    
    // 2. 创建并启动 VideoProductionLine
    VideoProductionLine producer(false, 1, false);
    
    // 设置错误回调
    producer.setErrorCallback([&result](const std::string& error) {
        result.error_message = error;
        requestStop();
    });
    
    if (!producer.start(config)) {
        result.error_message = "Failed to start VideoProductionLine";
        return result;
    }
    
    // 3. 获取 BufferPool
    uint64_t pool_id = producer.getWorkingBufferPoolId();
    auto pool = BufferPoolRegistry::getInstance().getPool(pool_id).lock();
    
    if (!pool) {
        result.error_message = "Failed to get BufferPool";
        producer.stop();
        return result;
    }
    
    // 4. 创建 BufferWriter（如果需要保存）
    std::unique_ptr<productionline::io::BufferWriter> writer;
    bool save_enabled = (consumer_cfg.save_frames != 0);
    int save_limit = (consumer_cfg.save_frames == -1) ? INT32_MAX : consumer_cfg.save_frames;
    
    // 5. 使用 BufferConsumerService 消费
    productionline::io::BufferConsumerService consumer;
    productionline::io::BufferConsumerService::Options opts;
    opts.timeout_ms = consumer_cfg.timeout_ms;
    opts.max_timeout_count = consumer_cfg.max_timeout_count;
    opts.max_frames = consumer_cfg.max_frames > 0 ? consumer_cfg.max_frames : -1;
    opts.verbose = consumer_cfg.verbose;
    opts.report_interval = 100;
    
    auto start_time = std::chrono::steady_clock::now();
    
    // 显示统计
    int display_success = 0;
    int display_failed = 0;
    
    auto consumer_result = consumer.consumeWithProgress(
        pool,
        [&](Buffer* buffer, int frame_index) -> bool {
            // 检查运行状态
            if (!g_running_) return false;
            
            // ========================================
            // 消费类型1：显示输出
            // ========================================
            if (display) {
                display->waitVerticalSync();
                if (display->displayBufferByDMA(buffer)) {
                    display_success++;
                    result.frames_displayed++;
                } else {
                    // DMA 失败，尝试回退到普通显示
                    if (display->displayFilledFramebuffer(buffer)) {
                        display_success++;
                        result.frames_displayed++;
                    } else {
                        display_failed++;
                    }
                }
            }
            
            // ========================================
            // 消费类型2：存储到文件
            // ========================================
            // 首帧时初始化 BufferWriter
            if (save_enabled && !writer && frame_index == 0) {
                writer = std::make_unique<productionline::io::BufferWriter>();
                
                // 获取格式信息
                AVPixelFormat fmt = buffer->hasImageMetadata() ? 
                    buffer->getImageFormat() : AV_PIX_FMT_NV12;
                int w = buffer->hasImageMetadata() ? buffer->getImageWidth() : 1920;
                int h = buffer->hasImageMetadata() ? buffer->getImageHeight() : 1080;
                
                // 确定输出路径
                std::string output = consumer_cfg.output_path;
                if (output.empty()) {
                    output = "/tmp/qa_output_" + std::to_string(time(nullptr)) + ".yuv";
                }
                
                if (!writer->openRaw(output.c_str(), fmt, w, h)) {
                    result.error_message = "Failed to open BufferWriter: " + output;
                    return false;
                }
                
                result.output_file = output;
            }
            
            // 保存帧
            if (writer && result.frames_saved < save_limit) {
                if (writer->write(buffer)) {
                    result.frames_saved++;
                }
            }
            
            return true;
        },
        [&](int frames_consumed, double elapsed_seconds) {
            if (consumer_cfg.verbose) {
                double fps = frames_consumed / elapsed_seconds;
                std::ostringstream msg;
                msg << "Progress: " << frames_consumed << " frames, " 
                    << std::fixed << std::setprecision(2) << fps << " fps";
                if (display) {
                    msg << ", displayed: " << display_success;
                }
                LOG4CPLUS_INFO(getLogger(), msg.str());
            }
        },
        opts
    );
    
    // 6. 计算结果
    auto end_time = std::chrono::steady_clock::now();
    result.duration_seconds = std::chrono::duration<double>(end_time - start_time).count();
    result.frames_decoded = consumer_result.frames_consumed;
    result.average_fps = consumer_result.average_fps;
    
    // 显示统计日志
    if (display) {
        LOG4CPLUS_INFO(getLogger(), "Display stats: success=" << display_success 
                       << ", failed=" << display_failed);
    }
    
    // FPS 验证
    if (consumer_cfg.target_fps > 0) {
        result.fps_passed = (result.average_fps >= consumer_cfg.target_fps * 0.95); // 允许 5% 误差
    }
    
    // 判断成功
    result.success = (result.frames_decoded > 0) && 
                     result.error_message.empty() &&
                     result.fps_passed &&
                     result.psnr_passed;
    
    // 7. 清理
    if (writer) {
        writer->close();
    }
    producer.stop();
    
    return result;
}

TestResult TestExecutor::runRecord(
    const WorkerConfig& config,
    const std::string& output_path,
    double max_duration_seconds
) {
    TestResult result;
    
    setupSignalHandler();
    
    // 1. 确保使用录制 Worker 类型
    WorkerConfig record_config = config;
    record_config.worker_type = WorkerType::FFMPEG_PACKET_RECORDER;
    
    // 2. 创建并启动 VideoProductionLine
    VideoProductionLine producer(false, 1, false);
    
    producer.setErrorCallback([&result](const std::string& error) {
        result.error_message = error;
        requestStop();
    });
    
    if (!producer.start(record_config)) {
        result.error_message = "Failed to start VideoProductionLine for recording";
        return result;
    }
    
    // 3. 获取 BufferPool（录制模式下存放 Packet 数据）
    uint64_t pool_id = producer.getWorkingBufferPoolId();
    auto pool = BufferPoolRegistry::getInstance().getPool(pool_id).lock();
    
    if (!pool) {
        result.error_message = "Failed to get BufferPool";
        producer.stop();
        return result;
    }
    
    // 4. 创建 BufferWriter（编码流模式）
    productionline::io::BufferWriter writer;
    
    // 从 Worker Facade 获取编解码器参数
    auto worker_facade = producer.getWorkerFacade();
    if (!worker_facade) {
        result.error_message = "Failed to get WorkerFacade";
        producer.stop();
        return result;
    }
    
    auto codec_params = worker_facade->getSourceCodecParameters();
    auto time_base = worker_facade->getTimeBase();
    
    if (!codec_params) {
        result.error_message = "Failed to get codec parameters";
        producer.stop();
        return result;
    }
    
    if (!writer.openEncoded(output_path.c_str(), codec_params, time_base)) {
        result.error_message = "Failed to open output file: " + output_path;
        producer.stop();
        return result;
    }
    
    result.output_file = output_path;
    
    // 5. 消费 Packet 并写入文件
    auto start_time = std::chrono::steady_clock::now();
    const auto& consumer_cfg = config.consumer;
    
    productionline::io::BufferConsumerService consumer;
    productionline::io::BufferConsumerService::Options opts;
    opts.timeout_ms = consumer_cfg.timeout_ms;
    opts.max_timeout_count = consumer_cfg.max_timeout_count;
    opts.max_frames = consumer_cfg.max_frames;
    opts.verbose = consumer_cfg.verbose;
    
    auto consumer_result = consumer.consumeWithProgress(
        pool,
        [&](Buffer* buffer, int packet_index) -> bool {
            if (!g_running_) return false;
            
            // 检查时长限制
            if (max_duration_seconds > 0) {
                auto now = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(now - start_time).count();
                if (elapsed >= max_duration_seconds) {
                    return false;
                }
            }
            
            // 写入 Packet
            if (writer.write(buffer)) {
                result.packets_recorded++;
                result.bytes_recorded += buffer->size();
            }
            
            return true;
        },
        [&](int packets_consumed, double elapsed_seconds) {
            if (consumer_cfg.verbose) {
                double rate = result.bytes_recorded / elapsed_seconds / 1024.0;
                LOG4CPLUS_INFO(getLogger(), "Recording: " << packets_consumed 
                               << " packets, " << std::fixed << std::setprecision(2) << rate << " KB/s");
            }
        },
        opts
    );
    
    // 6. 计算结果
    auto end_time = std::chrono::steady_clock::now();
    result.duration_seconds = std::chrono::duration<double>(end_time - start_time).count();
    result.frames_decoded = consumer_result.frames_consumed;
    result.average_fps = consumer_result.average_fps;
    
    // 判断成功
    result.success = (result.packets_recorded > 0) && result.error_message.empty();
    
    // 7. 清理
    writer.close();
    producer.stop();
    
    return result;
}

TestResult TestExecutor::runPsnrValidation(
    const WorkerConfig& hw_config,
    const WorkerConfig& sw_config
) {
    TestResult result;
    
    setupSignalHandler();
    
    const auto& consumer_cfg = hw_config.consumer;
    
    // ========================================
    // 方案 A：使用 MultiWorkerProductionLine + 帧同步
    // 
    // 架构：
    // - 一个生产者（Record Worker）读取数据源
    // - 两个消费者（HW 解码器 + SW 解码器）共享 PacketSource
    // - 使用 WorkerSyncCoordinator 同步两个解码器的输出
    // - 在回调中使用 BufferComparator 对比同一帧
    // ========================================
    
    // 1. 创建 BufferComparator（需要在回调中使用）
    auto comparator = std::make_shared<productionline::io::BufferComparator>();
    productionline::io::CompareConfig compare_cfg;
    compare_cfg.enable_psnr = consumer_cfg.enable_psnr || true;  // 至少启用一个
    compare_cfg.enable_ssim = consumer_cfg.enable_ssim;
    compare_cfg.quick_psnr_threshold = consumer_cfg.min_psnr;
    compare_cfg.ssim_threshold = consumer_cfg.min_ssim;
    compare_cfg.verbose = consumer_cfg.verbose;
    compare_cfg.save_report = true;
    compare_cfg.report_path = consumer_cfg.output_path.empty() ? 
                              "/tmp/psnr_report.txt" : consumer_cfg.output_path;
    
    if (!comparator->open(compare_cfg)) {
        result.error_message = "Failed to open BufferComparator";
        return result;
    }
    
    // 2. 创建帧同步回调
    // 当两个解码器都完成同一帧时，此回调被调用
    auto frame_count = std::make_shared<std::atomic<int>>(0);
    int max_frames = consumer_cfg.max_frames > 0 ? consumer_cfg.max_frames : INT32_MAX;
    
    FrameSyncCallback compare_callback = [comparator, frame_count, max_frames, &consumer_cfg](
        uint64_t frame_version,
        const std::map<std::string, Buffer*>& worker_buffers,
        void* context
    ) -> bool {
        (void)context;
        
        // 查找硬件和软件解码器的 Buffer
        Buffer* hw_buf = nullptr;
        Buffer* sw_buf = nullptr;
        
        for (const auto& [name, buf] : worker_buffers) {
            if (name.find("hw") != std::string::npos || 
                name.find("taco") != std::string::npos ||
                name.find("hardware") != std::string::npos) {
                hw_buf = buf;
            } else if (name.find("sw") != std::string::npos || 
                       name.find("software") != std::string::npos) {
                sw_buf = buf;
            }
        }
        
        // 如果只有两个，按顺序分配
        if (worker_buffers.size() == 2 && (!hw_buf || !sw_buf)) {
            auto it = worker_buffers.begin();
            sw_buf = it->second;  // 第一个作为参考（软件）
            ++it;
            hw_buf = it->second;  // 第二个作为测试（硬件）
        }
        
        if (!hw_buf || !sw_buf) {
            LOG4CPLUS_WARN(TestExecutor::getLogger(), 
                           "PSNR callback: Missing buffer (hw=" << (hw_buf ? "yes" : "no") 
                           << ", sw=" << (sw_buf ? "yes" : "no") << ")");
            return true;  // 继续
        }
        
        // 执行比较
        auto frame_result = comparator->compare(sw_buf, hw_buf);
        
        int count = ++(*frame_count);
        
        if (consumer_cfg.verbose && count % 100 == 0) {
            LOG4CPLUS_INFO(TestExecutor::getLogger(), 
                           "Frame " << count << ": PSNR Y=" 
                           << std::fixed << std::setprecision(2) << frame_result.psnr_y 
                           << " dB, SSIM Y=" << std::setprecision(4) << frame_result.ssim_y);
        }
        
        // 检查是否达到最大帧数
        return count < max_frames;
    };
    
    // 3. 构建 MultiWorkerConfig
    MultiWorkerConfig multi_config;
    multi_config.thread_pool_size = 4;
    
    // 创建 WorkerGroup
    WorkerGroupConfig group;
    group.group_id = "psnr_validation";
    
    // 生产者配置（Record Worker，读取数据源）
    ProducerConfig producer_cfg;
    producer_cfg.producer_name = "packet_source";
    producer_cfg.worker_config = hw_config;  // 使用硬件配置的数据源
    producer_cfg.worker_config.worker_type = WorkerType::FFMPEG_PACKET_RECORDER;
    group.producer_configs.push_back(producer_cfg);
    
    // 消费者配置 - 硬件解码器
    ConsumerConfig hw_consumer;
    hw_consumer.consumer_name = "hw_decoder";
    hw_consumer.worker_config = hw_config;
    hw_consumer.worker_config.worker_type = WorkerType::FFMPEG_DECODE;
    group.consumer_configs.push_back(hw_consumer);
    
    // 消费者配置 - 软件解码器
    ConsumerConfig sw_consumer;
    sw_consumer.consumer_name = "sw_decoder";
    sw_consumer.worker_config = sw_config;
    sw_consumer.worker_config.worker_type = WorkerType::FFMPEG_DECODE;
    group.consumer_configs.push_back(sw_consumer);
    
    // 连接器配置（ONE_TO_MANY：一个生产者，多个消费者）
    ConnectorConfig connector;
    connector.mode = Connector::Mode::ONE_TO_MANY;
    connector.producer_names = {"packet_source"};
    connector.consumer_names = {"hw_decoder", "sw_decoder"};
    connector.enable_frame_sync = true;
    
    // 添加帧同步回调
    CallbackChainItem callback_item(compare_callback, nullptr, "psnr_compare");
    connector.callback_chain.push_back(callback_item);
    
    group.connector_configs.push_back(connector);
    
    multi_config.groups.push_back(group);
    
    // 4. 创建并启动 MultiWorkerProductionLine
    MultiWorkerProductionLine producer(multi_config, false, 1, false);
    
    producer.setErrorCallback([&result](const std::string& error) {
        result.error_message = error;
        requestStop();
    });
    
    auto start_time = std::chrono::steady_clock::now();
    
    if (!producer.start()) {
        result.error_message = "Failed to start MultiWorkerProductionLine";
        comparator->close();
        return result;
    }
    
    // 5. 等待完成（通过检查帧数或运行状态）
    while (g_running_ && *frame_count < max_frames) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // 检查是否还在运行
        if (comparator->getCompareCount() > 0 && 
            comparator->getCompareCount() == *frame_count) {
            // 可能已经完成，使用循环短等待检测
            // 每 100ms 检查一次，最多等 5 秒（50 次），有新帧立即继续
            int idle_check_count = 0;
            const int MAX_IDLE_CHECKS = 50;  // 50 * 100ms = 5秒
            int last_count = comparator->getCompareCount();
            
            while (idle_check_count < MAX_IDLE_CHECKS && g_running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (comparator->getCompareCount() > last_count) {
                    break;  // 有新帧，立即继续处理
                }
                idle_check_count++;
            }
            
            if (idle_check_count >= MAX_IDLE_CHECKS) {
                // 5秒内无新帧，认为完成
                break;
            }
        }
    }
    
    // 6. 停止生产线
    producer.stop();
    
    // 7. 关闭对比器并获取统计
    comparator->close();
    
    // 8. 计算结果
    auto end_time = std::chrono::steady_clock::now();
    result.duration_seconds = std::chrono::duration<double>(end_time - start_time).count();
    result.frames_decoded = comparator->getCompareCount();
    result.average_fps = result.frames_decoded / result.duration_seconds;
    
    // 从 Comparator 获取统计结果
    result.psnr_passed = comparator->isPassed();
    result.success = result.psnr_passed && result.error_message.empty() && 
                     result.frames_decoded > 0;
    
    // 打印详细报告
    comparator->printSummary();
    
    result.output_file = compare_cfg.report_path;
    
    return result;
}

TestResult TestExecutor::runMultiWorker(
    const std::vector<WorkerConfig>& configs
) {
    TestResult result;
    
    if (configs.empty()) {
        result.error_message = "No worker configs provided";
        return result;
    }
    
    setupSignalHandler();
    
    // 使用第一个配置的 consumer 参数
    const auto& consumer_cfg = configs[0].consumer;
    
    // 构建 MultiWorkerConfig
    MultiWorkerConfig multi_config;
    multi_config.thread_pool_size = static_cast<int>(configs.size());
    
    // 创建一个 WorkerGroup
    WorkerGroupConfig group;
    group.group_id = "multi_worker_test";
    
    // 添加所有 Worker 作为消费者（共享同一个生产者）
    // 第一个 Worker 作为生产者
    if (!configs.empty()) {
        ProducerConfig producer_cfg;
        producer_cfg.producer_name = "source";
        producer_cfg.worker_config = configs[0];
        producer_cfg.worker_config.worker_type = WorkerType::FFMPEG_PACKET_RECORDER;
        group.producer_configs.push_back(producer_cfg);
    }
    
    // 其他 Worker 作为消费者
    for (size_t i = 0; i < configs.size(); i++) {
        ConsumerConfig consumer;
        consumer.consumer_name = "worker_" + std::to_string(i);
        consumer.worker_config = configs[i];
        consumer.worker_config.worker_type = WorkerType::FFMPEG_DECODE;
        group.consumer_configs.push_back(consumer);
    }
    
    // 添加连接器
    ConnectorConfig connector;
    connector.mode = Connector::Mode::ONE_TO_MANY;
    connector.producer_names = {"source"};
    for (size_t i = 0; i < configs.size(); i++) {
        connector.consumer_names.push_back("worker_" + std::to_string(i));
    }
    group.connector_configs.push_back(connector);
    
    multi_config.groups.push_back(group);
    
    // 创建并启动 MultiWorkerProductionLine
    MultiWorkerProductionLine multi_producer(multi_config, false, 1, false);
    
    multi_producer.setErrorCallback([&result](const std::string& error) {
        result.error_message = error;
        requestStop();
    });
    
    auto start_time = std::chrono::steady_clock::now();
    
    if (!multi_producer.start()) {
        result.error_message = "Failed to start MultiWorkerProductionLine";
        return result;
    }
    
    // 简单等待完成
    int max_frames = consumer_cfg.max_frames > 0 ? consumer_cfg.max_frames : 300;
    int wait_count = 0;
    int max_wait = max_frames / 30 + 10;  // 估计等待时间（秒）
    
    while (g_running_ && wait_count < max_wait) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        wait_count++;
        
        // 检查是否完成
        if (multi_producer.getAllLineFramesProduced() >= max_frames * static_cast<int64_t>(configs.size())) {
            break;
        }
    }
    
    // 获取统计
    int64_t total_frames = multi_producer.getAllLineFramesProduced();
    
    // 停止
    multi_producer.stop();
    
    // 计算结果
    auto end_time = std::chrono::steady_clock::now();
    result.duration_seconds = std::chrono::duration<double>(end_time - start_time).count();
    result.frames_decoded = static_cast<int>(total_frames);
    result.average_fps = total_frames / result.duration_seconds;
    
    result.success = (total_frames > 0) && result.error_message.empty();
    
    return result;
}

void TestExecutor::printHeader(const std::string& test_name, const WorkerConfig& config) {
    std::cout << "\n";
    printSeparator();
    std::cout << "  Test: " << test_name << "\n";
    printSeparator();
    std::cout << "  Input:    " << config.data_source.path << "\n";
    
    if (config.decoder.name.has_value()) {
        std::cout << "  Decoder:  " << config.decoder.name.value() << "\n";
    } else {
        std::cout << "  Decoder:  auto (software)\n";
    }
    
    std::cout << "  Display:  " << config.display.width << "x" 
              << config.display.height << "\n";
    
    if (config.consumer.max_frames > 0) {
        std::cout << "  Max frames: " << config.consumer.max_frames << "\n";
    }
    if (config.consumer.target_fps > 0) {
        std::cout << "  Target FPS: " << config.consumer.target_fps << "\n";
    }
    
    // 消费类型
    std::cout << "  Consume:  ";
    bool first = true;
    if (config.consumer.enable_display) {
        std::cout << "display";
        first = false;
    }
    if (config.consumer.save_frames != 0) {
        if (!first) std::cout << " + ";
        std::cout << "save";
        first = false;
    }
    if (config.consumer.enable_psnr) {
        if (!first) std::cout << " + ";
        std::cout << "psnr";
        first = false;
    }
    if (config.consumer.enable_ssim) {
        if (!first) std::cout << " + ";
        std::cout << "ssim";
        first = false;
    }
    if (first) {
        std::cout << "(none)";
    }
    std::cout << "\n";
    
    if (config.consumer.enable_psnr) {
        std::cout << "  PSNR:     enabled (min=" << config.consumer.min_psnr << " dB)\n";
    }
    if (config.consumer.enable_ssim) {
        std::cout << "  SSIM:     enabled (min=" << config.consumer.min_ssim << ")\n";
    }
    
    printSeparator();
}

void TestExecutor::printResult(const std::string& test_name, const TestResult& result) {
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  Test Result: " << test_name << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    
    // 状态
    std::cout << "  Status:         " 
              << (result.success ? "\033[32mPASS\033[0m" : "\033[31mFAIL\033[0m") 
              << "\n";
    
    // 帧统计
    std::cout << "  Frames decoded: " << result.frames_decoded << "\n";
    if (result.frames_displayed > 0) {
        std::cout << "  Frames displayed: " << result.frames_displayed << "\n";
    }
    if (result.frames_saved > 0) {
        std::cout << "  Frames saved:   " << result.frames_saved << "\n";
    }
    
    // 性能
    std::cout << "  Duration:       " << std::fixed << std::setprecision(2) 
              << result.duration_seconds << " seconds\n";
    std::cout << "  Average FPS:    " << std::fixed << std::setprecision(2) 
              << result.average_fps << "\n";
    std::cout << "  FPS check:      " 
              << (result.fps_passed ? "\033[32mPASS\033[0m" : "\033[31mFAIL\033[0m") 
              << "\n";
    
    // PSNR
    if (result.psnr_average > 0) {
        std::cout << "  PSNR average:   " << std::fixed << std::setprecision(2) 
                  << result.psnr_average << " dB\n";
        std::cout << "  PSNR check:     " 
                  << (result.psnr_passed ? "\033[32mPASS\033[0m" : "\033[31mFAIL\033[0m") 
                  << "\n";
    }
    
    // SSIM
    if (result.ssim_average > 0) {
        std::cout << "  SSIM average:   " << std::fixed << std::setprecision(4) 
                  << result.ssim_average << "\n";
        std::cout << "  SSIM check:     " 
                  << (result.ssim_passed ? "\033[32mPASS\033[0m" : "\033[31mFAIL\033[0m") 
                  << "\n";
    }
    
    // 输出文件
    if (!result.output_file.empty()) {
        std::cout << "  Output file:    " << result.output_file << "\n";
    }
    
    // 错误信息
    if (!result.error_message.empty()) {
        std::cout << "  Error:          \033[31m" << result.error_message << "\033[0m\n";
    }
    
    std::cout << "═══════════════════════════════════════════════════════\n";
}

void TestExecutor::printSeparator() {
    std::cout << "───────────────────────────────────────────────────────\n";
}

} // namespace common
} // namespace test
