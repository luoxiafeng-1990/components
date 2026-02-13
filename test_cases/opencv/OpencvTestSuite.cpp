#include "OpencvTestSuite.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "productionline/io/BufferConsumerService.hpp"

#include <iostream>
#include <getopt.h>
#include <cstring>
#include <sstream>
#include <log4cplus/loggingmacros.h>

#include "opencv2/opencv.hpp"
#include "opencv2/core/tacv.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/VideoProductionLine.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "buffer/bufferpool/Buffer.hpp"

namespace test {
namespace opencv {

uint32_t OpencvTestSuite::buildConsumeFlags(const WorkerConfig& config) {
    uint32_t flags = consumer::CONSUME_COUNT;  // 默认计数
    
    if (config.consumer_type.display.enable) {
        flags |= consumer::CONSUME_DISPLAY;
    }
    if (config.consumer_type.save_raw.enable) {
        flags |= consumer::CONSUME_SAVE_RAW;
    }
    
    return flags;
}

const std::map<std::string, OpencvTestParams>& OpencvTestSuite::getPredefinedTests() {
    static std::map<std::string, OpencvTestParams> tests = {
        {"h264_128x128_30",       {"h264", 128, 128, 30.0, "main"}},
        {"h264_320x240_30",       {"h264", 320, 240, 30.0, "high"}}
    };
    return tests;
}

std::vector<std::string> OpencvTestSuite::getTestNames() const {
    std::vector<std::string> names;
    for (const auto& pair : getPredefinedTests()) {
        names.push_back(pair.first);
    }
    return names;
}

bool test_opencv() {
    bool g_running = true;
    bool g_rtsp_interrupted = false;
    
    // 配置：只保存第一帧
    const int target_width = 480;
    const int target_height = 360;
    const std::string output_filename = "./first_resized_frame.jpg";
    bool frame_saved = false;        // 标记是否已保存第一帧
    
    WorkerConfig::DataSourceConfig g_data_source_config = DataSourceConfigBuilder()
                                    .setPath("input.mp4")
                                    .setBufferCount(16)
                                    .build();
    
    TacoConfigBuilder taco_config_builder = TacoConfigBuilder()
                                    .setReorderDisable(true)
                                    .setChannels(false, false);
    
    WorkerConfig::DecoderConfig::TacoConfig g_taco_config = taco_config_builder.build();
    WorkerConfig::DecoderConfig g_hw_decoder_config = DecoderConfigBuilder()
                                    .useTaco("h264", g_taco_config)
                                    .build();
    
    WorkerConfig g_hw_worker_config = WorkerConfigBuilder()
                                    .setDataSourceConfig(g_data_source_config)
                                    .setDecoderConfig(g_hw_decoder_config)
                                    .setWorkerType(WorkerType::OPENCV)
                                    .build();

    VideoProductionLine hw_producer = VideoProductionLine(false, 1, false);
    bool hw_result = hw_producer.start(g_hw_worker_config);
    if (!hw_result) {
        hw_producer.stop(); 
        return false;
    }

    uint64_t hw_pool_id = hw_producer.getWorkingBufferPoolId();
    if (hw_pool_id == 0) {
        hw_producer.stop(); 
        return false;
    }
    
    auto hw_pool = BufferPoolRegistry::getInstance().getPool(hw_pool_id).lock();
    if (!hw_pool) {
        hw_producer.stop(); 
        return false;
    }

    bool result_passed = true;
    int frame_processed = 0;

    // 只需要处理到保存第一帧
    while (g_running && !frame_saved) {
        Buffer* hw_buf = hw_pool->acquireFilled(true, 100);
        if (hw_buf == nullptr) {
            std::cout << "获取Buffer失败或超时" << std::endl;
            break;
        }
        
        cv::Mat* hw_mat = hw_buf->getMat();
        if (hw_mat == nullptr || hw_mat->empty()) {
            std::cout << "获取到的Mat为空" << std::endl;
            hw_pool->releaseFilled(hw_buf);
            continue;
        }
        
        frame_processed++;
        std::cout << "获取到第 " << frame_processed << " 帧" << std::endl;
        std::cout << "原始尺寸: " << hw_mat->cols << "x" << hw_mat->rows 
                  << ", 通道数: " << hw_mat->channels() << std::endl;
        
        // 处理第一帧
        if (frame_processed == 1) {
            // 直接调整尺寸，不做颜色转换（OpencvWorker已经输出BGR格式）
            cv::Mat resized_frame;
            cv::resize(*hw_mat, resized_frame,
                      cv::Size(target_width, target_height),
                      0, 0, cv::INTER_LINEAR);

            std::cout << "调整尺寸: " << hw_mat->cols << "x" << hw_mat->rows
                      << " -> " << resized_frame.cols << "x" << resized_frame.rows << std::endl;
            
            // 3. 保存到本地
            std::vector<int> compression_params;
            compression_params.push_back(cv::IMWRITE_JPEG_QUALITY);
            compression_params.push_back(95);  // JPEG质量 95%
            
            bool save_success = cv::imwrite(output_filename, resized_frame, compression_params);
            
            if (save_success) {
                std::cout << "✓ 成功保存第一帧到: " << output_filename << std::endl;
                std::cout << "  保存尺寸: " << resized_frame.cols << "x" << resized_frame.rows << std::endl;
                frame_saved = true;
            } else {
                std::cout << "✗ 保存失败: " << output_filename << std::endl;
                result_passed = false;
            }
        }
        
        hw_pool->releaseFilled(hw_buf);
        
        // 保存成功后立即退出循环
        if (frame_saved) {
            break;
        }
    }
    
    hw_producer.stop();
    
    // 输出总结
    std::cout << "\n===== 处理完成 =====" << std::endl;
    std::cout << "处理帧数: " << frame_processed << std::endl;
    std::cout << "第一帧保存: " << (frame_saved ? "成功" : "失败") << std::endl;
    if (frame_saved) {
        std::cout << "输出文件: " << output_filename << std::endl;
    }
    
    return result_passed && frame_saved;
}

int OpencvTestSuite::run(int argc, char* argv[]) {
    WorkerConfig config;
    OpencvTestParams params;
    
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
    
    return test_opencv() ? 0:1;
}

bool OpencvTestSuite::parseArgs(int argc, char* argv[], WorkerConfig& config, OpencvTestParams& params) {
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

int OpencvTestSuite::runPredefinedTest(const std::string& test_name, const std::string& path) {
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

void OpencvTestSuite::printHelp() const {
    std::cout << "\n"
              << "VDEC Module - 视频解码测试\n"
              << "\n"
              << "Usage:\n"
              << "  qa_cases opencv [options] <video_path>\n"
              << "  qa_cases opencv [options] <test_name> <video_path>\n"
              << "\n"
              << "Options:\n"
              << "  -h, --help              显示帮助信息\n"
              << "  -l, --list              列出所有预定义测试\n"
              << "  -f, --file <path>       视频文件路径\n"
              << "  -r, --rtsp <url>        RTSP URL\n"
              << "  -c, --codec <name>      编解码格式 (h264|h265|mjpeg)\n"
              << "  -D, --decoder <type>    解码方式 (hw|hardware|sw|software，默认: hw)\n"
              << "  -W, --width <n>         分辨率宽度\n"
              << "  -H, --height <n>        分辨率高度\n"
              << "  -R, --resolution <WxH>  分辨率 (如 1920x1080)\n"
              << "  -F, --fps <n>           目标帧率\n"
              << "  -m, --max-frames <n>    最大帧数 (-1=无限制)\n"
              << "  -s, --save <n>          保存帧数 (0=不保存, -1=全部)，需配合 -o 使用\n"
              << "  -o, --output <path>     输出文件路径，启用保存功能\n"
              << "  -d, --display           启用显示输出 (CONSUME_DISPLAY)\n"
              << "  -p, --psnr              启用 PSNR 验证 (ExecuteMode::COMPARE)\n"
              << "  -S, --ssim              启用 SSIM 验证 (ExecuteMode::COMPARE)\n"
              << "  -P, --min-psnr <n>      PSNR 阈值 (默认: 30.0 dB)\n"
              << "  -M, --min-ssim <n>      SSIM 阈值 (默认: 0.95)\n"
              << "  -v, --verbose           详细日志\n"
              << "  -t, --threads <n>       并发路数 (启用 PARALLEL 模式，可任意指定)\n"
              << "\n"
              << "ExecuteMode Mapping:\n"
              << "  SINGLE   - 默认单路解码，支持 --decoder hw/sw\n"
              << "  COMPARE  - --psnr/--ssim 启用时，HW vs SW 对比\n"
              << "  PARALLEL - --threads N 或预定义 multithread_N，支持 --decoder hw/sw\n"
              << "\n"
              << "Examples:\n"
              << "  qa_cases opencv video.mp4                           # SINGLE\n"
              << "  qa_cases opencv --display video.mp4                 # SINGLE + DISPLAY\n"
              << "  qa_cases opencv -s 100 -o /tmp/out.yuv video.mp4    # SINGLE + SAVE 100帧\n"
              << "  qa_cases opencv --psnr video.mp4                    # COMPARE (HW vs SW)\n"
              << "  qa_cases opencv --threads 16 video.mp4              # PARALLEL x16 (自定义路数)\n"
              << "  qa_cases opencv --threads 32 --decoder sw video.mp4 # PARALLEL x32 软解\n"
              << "  qa_cases opencv multithread_4 video.mp4             # PARALLEL x4 (预定义)\n"
              << std::endl;
}

void OpencvTestSuite::listTests() const {
    std::cout << "\n"
              << "Available VDEC tests:\n"
              << "────────────────────────────────────────────────────────\n"
              << "\n"
              << "H.264 Tests (9) - ExecuteMode::SINGLE:\n"
              << "  h264_128x128_30         H.264 128x128 30fps main\n"
              << "  h264_320x240_30         H.264 320x240 30fps high\n"
              << "  h264_640x480_30         H.264 640x480 30fps main\n"
              << "  h264_640x480_60         H.264 640x480 60fps high\n"
              << "  h264_1280x720_30        H.264 720p 30fps high\n"
              << "  h264_1920x1080_30       H.264 1080p 30fps high\n"
              << "  h264_1920x1080_60       H.264 1080p 60fps high\n"
              << "  h264_2560x1440_30       H.264 1440p 30fps high\n"
              << "  h264_3840x2160_30       H.264 4K 30fps high\n"
              << "\n"
              << "H.265/HEVC Tests (9) - ExecuteMode::SINGLE:\n"
              << "  h265_128x128_30         H.265 128x128 30fps\n"
              << "  h265_320x240_30         H.265 320x240 30fps\n"
              << "  h265_640x480_30         H.265 640x480 30fps\n"
              << "  h265_640x480_60         H.265 640x480 60fps\n"
              << "  h265_1280x720_30        H.265 720p 30fps\n"
              << "  h265_1920x1080_30       H.265 1080p 30fps\n"
              << "  h265_1920x1080_60       H.265 1080p 60fps\n"
              << "  h265_2560x1440_30       H.265 1440p 30fps\n"
              << "  h265_3840x2160_30       H.265 4K 30fps\n"
              << "\n"
              << "MJPEG Tests (9) - ExecuteMode::SINGLE:\n"
              << "  mjpeg_128x128_30        MJPEG 128x128 30fps\n"
              << "  mjpeg_320x240_30        MJPEG 320x240 30fps\n"
              << "  mjpeg_640x480_30        MJPEG 640x480 30fps\n"
              << "  mjpeg_640x480_60        MJPEG 640x480 60fps\n"
              << "  mjpeg_1280x720_30       MJPEG 720p 30fps\n"
              << "  mjpeg_1920x1080_30      MJPEG 1080p 30fps\n"
              << "  mjpeg_1920x1080_60      MJPEG 1080p 60fps\n"
              << "  mjpeg_2560x1440_30      MJPEG 1440p 30fps\n"
              << "  mjpeg_3840x2160_30      MJPEG 4K 30fps\n"
              << "\n"
              << "Software Decode Tests (2) - ExecuteMode::SINGLE:\n"
              << "  sw_h264_1920x1080_30    Software H.264 1080p 30fps\n"
              << "  sw_h265_1920x1080_30    Software H.265 1080p 30fps\n"
              << "\n"
              << "RTSP H.264 Tests (6) - ExecuteMode::SINGLE:\n"
              << "  rtsp_h264_1280x720_30_cbr   RTSP H.264 720p CBR\n"
              << "  rtsp_h264_1280x720_30_vbr   RTSP H.264 720p VBR\n"
              << "  rtsp_h264_1920x1080_30_cbr  RTSP H.264 1080p CBR\n"
              << "  rtsp_h264_1920x1080_30_vbr  RTSP H.264 1080p VBR\n"
              << "  rtsp_h264_3840x2160_30_cbr  RTSP H.264 4K CBR\n"
              << "  rtsp_h264_3840x2160_30_vbr  RTSP H.264 4K VBR\n"
              << "\n"
              << "RTSP H.265 Tests (6) - ExecuteMode::SINGLE:\n"
              << "  rtsp_h265_1280x720_30_cbr   RTSP H.265 720p CBR\n"
              << "  rtsp_h265_1280x720_30_vbr   RTSP H.265 720p VBR\n"
              << "  rtsp_h265_1920x1080_30_cbr  RTSP H.265 1080p CBR\n"
              << "  rtsp_h265_1920x1080_30_vbr  RTSP H.265 1080p VBR\n"
              << "  rtsp_h265_3840x2160_30_cbr  RTSP H.265 4K CBR\n"
              << "  rtsp_h265_3840x2160_30_vbr  RTSP H.265 4K VBR\n"
              << "\n"
              << "RTSP MJPEG Tests (1) - ExecuteMode::SINGLE:\n"
              << "  rtsp_mjpeg_32768x18432_30   RTSP MJPEG Ultra-High\n"
              << "\n"
              << "Multi-Worker Tests (2) - ExecuteMode::PARALLEL:\n"
              << "  multi_worker               HW+SW concurrent decode\n"
              << "  multi_worker_4k            HW+SW concurrent 4K decode\n"
              << "\n"
              << "Multi-Thread Tests (3) - ExecuteMode::PARALLEL:\n"
              << "  multithread_2              2-thread decode\n"
              << "  multithread_4              4-thread decode\n"
              << "  multithread_8              8-thread decode\n"
              << "\n"
              << "────────────────────────────────────────────────────────\n"
              << "Total: 47 predefined tests\n"
              << std::endl;
}


} // namespace opencv
} // namespace test
