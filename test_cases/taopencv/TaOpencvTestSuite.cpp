#include "TaOpencvTestSuite.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "productionline/io/BufferConsumerService.hpp"

#include <iostream>
#include <getopt.h>
#include <cstring>
#include <sstream>
#include <log4cplus/loggingmacros.h>
#include <chrono>

#include "opencv2/opencv.hpp"
#include "opencv2/core/tacv.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/VideoProductionLine.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "buffer/bufferpool/Buffer.hpp"

namespace test {
namespace taopencv {

uint32_t TaOpencvTestSuite::buildConsumeFlags(const WorkerConfig& config) {
    uint32_t flags = consumer::CONSUME_COUNT;  // 默认计数
    
    if (config.consumer_type.display.enable) {
        flags |= consumer::CONSUME_DISPLAY;
    }
    if (config.consumer_type.save_raw.enable) {
        flags |= consumer::CONSUME_SAVE_RAW;
    }
    
    return flags;
}

const std::map<std::string, TaOpencvTestParams>& TaOpencvTestSuite::getPredefinedTests() {
    static std::map<std::string, TaOpencvTestParams> tests = {
        {"h264_128x128_30",       {"h264", 128, 128, 30.0, true}},
        {"h264_320x240_30",       {"h264", 320, 240, 30.0, true}}
    };
    return tests;
}

std::vector<std::string> TaOpencvTestSuite::getTestNames() const {
    std::vector<std::string> names;
    for (const auto& pair : getPredefinedTests()) {
        names.push_back(pair.first);
    }
    return names;
}

bool test_opencv() {
    bool g_running = true;
    bool g_rtsp_interrupted = false;
    
    WorkerConfig::DataSourceConfig data_source_config = DataSourceConfigBuilder()
                                    .setPath("input.mp4")
                                    .setBufferCount(16)
                                    .build();
    
    WorkerConfig::DecoderConfig::TacoConfig taco_config = TacoConfigBuilder()
                                    .setReorderDisable(true)
                                    .setChannels(false, false)
                                    .build();
    
    WorkerConfig::DecoderConfig hw_decoder_config = DecoderConfigBuilder()
                                    .useTaco("h264", taco_config)
                                    .build();
    WorkerConfig::DecoderConfig sw_decoder_config = DecoderConfigBuilder()
                                    .useSoftware()
                                    .build();
    
    WorkerConfig hw_worker_config = WorkerConfigBuilder()
                                    .setDataSourceConfig(data_source_config)
                                    .setDecoderConfig(hw_decoder_config)
                                    .setWorkerType(WorkerType::FFMPEG_DECODE)
                                    .build();
    
    WorkerConfig sw_worker_config = WorkerConfigBuilder()
                                    .setDataSourceConfig(data_source_config)
                                    .setDecoderConfig(sw_decoder_config)
                                    .setWorkerType(WorkerType::FFMPEG_DECODE)
                                    .build();

    VideoProductionLine hw_producer = VideoProductionLine(false, 1, false);
    VideoProductionLine sw_producer = VideoProductionLine(false, 1, false);

    bool hw_result = hw_producer.start(hw_worker_config);
    if (!hw_result) {hw_producer.stop(); return false;}
    bool sw_result = sw_producer.start(sw_worker_config);
    if (!sw_result) {sw_producer.stop(); return false;}

    uint64_t hw_pool_id = hw_producer.getWorkingBufferPoolId();
    uint64_t sw_pool_id = sw_producer.getWorkingBufferPoolId();
    if (hw_pool_id ==0 || sw_pool_id == 0){hw_producer.stop(); sw_producer.stop(); return false;}

    auto hw_pool = BufferPoolRegistry::getInstance().getPool(hw_pool_id).lock();
    auto sw_pool = BufferPoolRegistry::getInstance().getPool(sw_pool_id).lock();
    if (! hw_pool || ! sw_pool){hw_producer.stop(); sw_producer.stop(); return false;}

    bool result_passed = true;
    const int target_width = 480;
    const int target_height = 360;
    const std::string hw_output = "hw_output.jpg";
    const std::string sw_output = "sw_output.jpg";

    Buffer* hw_buf = hw_pool->acquireFilled(true,100);
    Buffer* sw_buf = sw_pool->acquireFilled(true,100);
    while (g_running) {
        if (hw_buf == nullptr && sw_buf == nullptr){
            break;
        }
        else if (hw_buf == nullptr){
            sw_pool->releaseFilled(sw_buf);
            sw_buf = sw_pool->acquireFilled(true,100);
            continue;
        }
        else if (sw_buf == nullptr){
            hw_pool->releaseFilled(hw_buf);
            hw_buf = hw_pool->acquireFilled(true,100);
            continue;
        }
        int64_t hw_pts = hw_buf->getAVFrame()->pts;
        int64_t sw_pts = sw_buf->getAVFrame()->pts;
        std::cout << "hw_pts: " << hw_pts << std::endl;
        std::cout << "sw_pts: " << sw_pts << std::endl;
        if (sw_pts == hw_pts){
            cv::MatAllocator* sw_allocator = cv::Mat::getDefaultAllocator();
            cv::MatAllocator* hw_allocator = cv::hal::getAllocator();
            // 因为mat和avframe内存共享，所以对象的析构顺序必须是mat->avframe->buffer
            {
            cv::Mat::setDefaultAllocator(hw_allocator);
            cv::Mat original_mat = cv::Mat(hw_buf->getAVFrame());
            cv::Mat::setDefaultAllocator(sw_allocator);
            cv::Mat hw_resized_mat;
            cv::Mat sw_resized_mat;
            
            cv::Mat::setDefaultAllocator(hw_allocator);
            auto hw_start = std::chrono::high_resolution_clock::now();
            cv::resize(original_mat, hw_resized_mat,cv::Size(target_width, target_height));
            auto hw_end = std::chrono::high_resolution_clock::now();
            
            cv::Mat::setDefaultAllocator(sw_allocator);
            auto sw_start = std::chrono::high_resolution_clock::now();
            cv::resize(original_mat, sw_resized_mat,cv::Size(target_width, target_height));
            auto sw_end = std::chrono::high_resolution_clock::now();
            auto hw_duration = std::chrono::duration_cast<std::chrono::milliseconds>(hw_end - hw_start);
            auto sw_duration = std::chrono::duration_cast<std::chrono::milliseconds>(sw_end - sw_start);
            std::cout << "hw_duration: " << hw_duration.count() << std::endl;
            std::cout << "sw_duration: " << sw_duration.count() << std::endl;

            std::cout << "[DEBUG] Before hw_imwrite" << std::endl;
            cv::imwrite(hw_output,hw_resized_mat);
            std::cout << "[DEBUG] After hw_imwrite, before sw_imwrite" << std::endl;
            cv::imwrite(sw_output,sw_resized_mat);
            std::cout << "[DEBUG] After sw_imwrite, before scope end" << std::endl;
            }
            std::cout << "[DEBUG] After scope end" << std::endl;
            sw_pool->releaseFilled(sw_buf);
            hw_pool->releaseFilled(hw_buf);
            std::cout << "[1]" << std::endl;
            sw_buf = sw_pool->acquireFilled(true,100);
            hw_buf = hw_pool->acquireFilled(true,100);
            std::cout << "[2]" << std::endl;
            break;
        }
        else if (sw_pts < hw_pts){
            sw_pool->releaseFilled(sw_buf);
            sw_buf = sw_pool->acquireFilled(true,100);
        }
        else if (sw_pts > hw_pts){
            hw_pool->releaseFilled(hw_buf);
            hw_buf = hw_pool->acquireFilled(true,100);
        }
    }

    std::cout << "[3]" << std::endl;
    
    hw_producer.stop();
    sw_producer.stop();

    std::cout << "[4]" << std::endl;
    return result_passed;
}


bool test_single_pp (
    std::string data_source_path,
    std::string codec
){
    bool g_running = true;

    WorkerConfig::DataSourceConfig g_data_source_config = DataSourceConfigBuilder().
                                    setPath(data_source_path).
                                    setBufferCount(16).
                                    build();
    TacoConfigBuilder taco_config_builder = TacoConfigBuilder().
                                    setReorderDisable(false);
    WorkerConfig::DecoderConfig::TacoConfig g_taco_config = taco_config_builder.build();
    WorkerConfig::DecoderConfig g_hw_decoder_config = DecoderConfigBuilder().useTaco(codec,g_taco_config).build();
    WorkerConfig::DecoderConfig g_sw_decoder_config = DecoderConfigBuilder().useSoftware().build();
    WorkerConfig g_hw_worker_config = WorkerConfigBuilder().
                                    setDataSourceConfig(g_data_source_config).
                                    setDecoderConfig(g_hw_decoder_config).
                                    setWorkerType(WorkerType::FFMPEG_DECODE).
                                    build();
    WorkerConfig g_sw_worker_config = WorkerConfigBuilder().
                                    setDataSourceConfig(g_data_source_config).
                                    setDecoderConfig(g_sw_decoder_config).
                                    setWorkerType(WorkerType::FFMPEG_DECODE).
                                    build();

    VideoProductionLine hw_producer = VideoProductionLine(false,1,false);
    VideoProductionLine sw_producer = VideoProductionLine(false,1,false);
    bool hw_result = hw_producer.start(g_hw_worker_config);
    if (!hw_result) {hw_producer.stop(); return false;}
    bool sw_result = sw_producer.start(g_sw_worker_config);
    if (!sw_result) {sw_producer.stop(); return false;}

    uint64_t hw_pool_id = hw_producer.getWorkingBufferPoolId();
    uint64_t sw_pool_id = sw_producer.getWorkingBufferPoolId();
    if (hw_pool_id ==0 || sw_pool_id == 0){hw_producer.stop(); sw_producer.stop(); return false;}
    auto hw_pool = BufferPoolRegistry::getInstance().getPool(hw_pool_id).lock();
    auto sw_pool = BufferPoolRegistry::getInstance().getPool(sw_pool_id).lock();
    if (! hw_pool || ! sw_pool){hw_producer.stop(); sw_producer.stop(); return false;}

    bool result_passed = true;

    Buffer* hw_buf = hw_pool->acquireFilled(true, 100);
    Buffer* sw_buf = sw_pool->acquireFilled(true, 100);
    while (g_running) {
        if (hw_buf == nullptr && sw_buf == nullptr){
            std::cout << "no more avframe" << std::endl;
            break;
        }
        else if (hw_buf == nullptr){
            std::cout << "sw_pts: " << sw_buf->getAVFrame()->pts << std::endl;
            hw_pool->releaseFilled(hw_buf);
            hw_buf = hw_pool->acquireFilled(true, 100);
            continue;
        }
        else if (sw_buf == nullptr){
            std::cout << "hw_pts: " << hw_buf->getAVFrame()->pts << std::endl;
            sw_pool->releaseFilled(sw_buf);
            sw_buf = sw_pool->acquireFilled(true, 100);
            continue;
        }
        else {
            int64_t hw_pts = hw_buf->getAVFrame()->pts;
            int64_t sw_pts = sw_buf->getAVFrame()->pts;
            std::cout << "hw_pts: " << hw_pts
                      << "sw_pts: " << sw_pts
                      << "same? " << (hw_pts==sw_pts ? "true":"false")
                      << std::endl;
            sw_pool->releaseFilled(sw_buf);
            hw_pool->releaseFilled(hw_buf);
            sw_buf = sw_pool->acquireFilled(true,100);
            hw_buf = hw_pool->acquireFilled(true,100);
        }
    }
    hw_producer.stop();
    sw_producer.stop();

    return result_passed;
}

int TaOpencvTestSuite::run(int argc, char* argv[]) {
    WorkerConfig config;
    TaOpencvTestParams params;
    
    if (!parseArgs(argc, argv, config, params)) {
        return 1;
    }
    
    std::ostringstream test_name;
    if (params.isPredefined()) {
        // 使用预定义测试名称 + 参数信息
        test_name << params.predefined_name << " (" 
                  << params.codec << " " << params.width << "x" << params.height 
                  << " " << static_cast<int>(params.fps) << "fps)";
    } else {
        // 未匹配预定义测试，使用文件名 + 实际配置
        // 提取文件名（去掉路径）
        std::string filename = config.data_source.path;
        size_t pos = filename.find_last_of("/\\");
        if (pos != std::string::npos) {
            filename = filename.substr(pos + 1);
        }
        test_name << "Custom: " << filename << " (" 
                  << params.codec << " " << params.width << "x" << params.height 
                  << " " << static_cast<int>(params.fps) << "fps)";
    }
    
    return test_single_pp("input.mp4","h264") ? 0:1;
}

bool TaOpencvTestSuite::parseArgs(int argc, char* argv[], WorkerConfig& config, TaOpencvTestParams& params) {
    optind = 1;
    
    static struct option long_options[] = {
        {"help",       no_argument,       0, 'h'},
        {"list",       no_argument,       0, 'l'},
        {"file",       required_argument, 0, 'f'},
        {"rtsp",       required_argument, 0, 'r'},
        {"codec",      required_argument, 0, 'c'},
        {"decoder",    required_argument, 0, 'D'},
        {"width",      required_argument, 0, 'W'},
        {"height",     required_argument, 0, 'H'},
        {"resolution", required_argument, 0, 'R'},
        {"fps",        required_argument, 0, 'F'},
        {"max-frames", required_argument, 0, 'm'},
        {"save",       required_argument, 0, 's'},
        {"output",     required_argument, 0, 'o'},
        {"display",    no_argument,       0, 'd'},
        {"psnr",       no_argument,       0, 'p'},
        {"ssim",       no_argument,       0, 'S'},
        {"min-psnr",   required_argument, 0, 'P'},
        {"min-ssim",   required_argument, 0, 'M'},
        {"verbose",    no_argument,       0, 'v'},
        {"threads",    required_argument, 0, 't'},  // 并发路数（PARALLEL 模式）
        {0, 0, 0, 0}
    };
    
    std::string input_path;
    
    int opt;
    while ((opt = getopt_long(argc, argv, "hlf:r:c:D:W:H:R:F:m:s:o:dpSP:M:vt:", 
                              long_options, nullptr)) != -1) {
        switch (opt) {
            case 'h':
                printHelp();
                return false;
            
            case 'l':
                listTests();
                return false;
            
            case 'f':
            case 'r':
                input_path = optarg;
                break;
            
            case 'c':
                params.codec = optarg;
                break;
            
            case 'D': {
                std::string decoder_type = optarg;
                if (decoder_type == "hw" || decoder_type == "hardware") {
                    params.use_hardware = true;
                } else if (decoder_type == "sw" || decoder_type == "software") {
                    params.use_hardware = false;
                } else {
                    LOG4CPLUS_ERROR_FMT(getLogger(), 
                        "Invalid decoder type '%s', use 'hw' or 'sw'", optarg);
                    return false;
                }
                break;
            }
            
            case 'W':
                params.width = std::stoi(optarg);
                break;
            
            case 'H':
                params.height = std::stoi(optarg);
                break;
            
            case 'R': {
                // 解析 WxH 格式
                std::string res = optarg;
                size_t pos = res.find('x');
                if (pos != std::string::npos) {
                    params.width = std::stoi(res.substr(0, pos));
                    params.height = std::stoi(res.substr(pos + 1));
                }
                break;
            }
            
            case 'F':
                params.fps = std::stod(optarg);
                break;
            
            case 'm':
                // v2.23：从数据源层面限制帧数，而不是消费层面
                config.data_source.max_frames = std::stoi(optarg);
                break;
            
            case 's':
                config.consumer_type.save_raw.max_frames_per_channel = {std::stoi(optarg)};  // 只设置帧数
                break;
            
            case 'o':
                config.consumer_type.save_raw.enable = true;  // 指定路径即启用保存
                config.consumer_type.save_raw.setOutputPath(optarg);
                break;
            
            case 'd':
                config.consumer_type.display.enable = true;
                break;
            
            case 'p':
                config.consumer_type.compare.enable_psnr = true;
                break;
            
            case 'S':
                config.consumer_type.compare.enable_ssim = true;
                break;
            
            case 'P':
                config.consumer_type.compare.min_psnr = std::stod(optarg);
                break;
            
            case 'M':
                config.consumer_type.compare.min_ssim = std::stod(optarg);
                break;
            
            case 'v':
                config.consumer_type.verbose = true;
                break;
            
            case 't': {
                int thread_count = std::stoi(optarg);
                if (thread_count < 1) {
                    LOG4CPLUS_ERROR_FMT(getLogger(), 
                        "Invalid thread count '%s', must be >= 1", optarg);
                    return false;
                }
                break;
            }
            
            default:
                printHelp();
                return false;
        }
    }
    
    // 处理剩余参数
    for (int i = optind; i < argc; i++) {
        std::string arg = argv[i];
        
        // 检查是否是预定义测试名称
        const auto& tests = getPredefinedTests();
        auto it = tests.find(arg);
        if (it != tests.end()) {
            params = it->second;
            params.predefined_name = arg;  // 记录匹配的预定义测试名称
            // 下一个参数作为路径
            if (i + 1 < argc) {
                input_path = argv[++i];
            }
            continue;
        }
        
        // 否则作为输入路径
        if (input_path.empty()) {
            input_path = arg;
        } else {
            LOG4CPLUS_WARN_FMT(getLogger(), 
                "Extra positional argument ignored: '%s' (input already set to: '%s')",
                arg.c_str(), input_path.c_str());
        }
    }
    
    if (input_path.empty()) {
        std::cerr << "Error: No input file or RTSP URL specified\n" << std::endl;
        printHelp();
        return false;
    }
    
    // 验证：如果指定了 -s 保存帧数，必须同时指定 -o 输出路径
    if (!config.consumer_type.save_raw.max_frames_per_channel.empty() && 
        config.consumer_type.save_raw.max_frames_per_channel[0] != 0 &&
        !config.consumer_type.save_raw.enable) {
        std::cerr << "Error: -s/--save requires -o/--output to specify output file path\n" << std::endl;
        std::cerr << "Example: qa_cases opencv -s 100 -o /tmp/output.yuv video.mp4\n" << std::endl;
        return false;
    }
    
    config.data_source.path = input_path;
    
    return true;
}

int TaOpencvTestSuite::runPredefinedTest(const std::string& test_name, const std::string& path) {
    const auto& tests = getPredefinedTests();
    auto it = tests.find(test_name);
    if (it == tests.end()) {
        LOG4CPLUS_ERROR_FMT(getLogger(), "Unknown test '%s'", test_name.c_str());
        return 1;
    }
    
    const auto& params = it->second;
    auto config = common::WorkerConfigFactory::createDecode(
        path, params.codec, params.width, params.height);
    config.consumer_type.performance.target_fps = params.fps;
    
    auto result = runSingle(config, consumer::CONSUME_COUNT, test_name);
    consumer::BufferConsumerService::printResult(test_name, result);
    return result.success ? 0 : 1;
}

void TaOpencvTestSuite::printHelp() const {
    std::cout << ""
              << std::endl;
}

void TaOpencvTestSuite::listTests() const {
    std::cout << ""
              << std::endl;
}


}
}
