#include <gtest/gtest.h>

#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/VideoProductionLine.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "buffer/bufferpool/Buffer.hpp"
#include "productionline/io/BufferComparator.hpp"
static volatile bool g_running = true;
extern std::string g_input_file;

int test_single_pp (
    std::string input_file,
    OutputFormat pix_fmt,
    Channel pp_channel_id,
    int crop_x,
    int crop_y,
    int crop_width,
    int crop_height,
    int scale_width,
    int scale_height,
    std::string codec
){
    WorkerConfig::DataSourceConfig g_data_source_config = DataSourceConfigBuilder().
                                    setPath(input_file).
                                    setBufferCount(16).
                                    build();
    TacoConfigBuilder taco_config_builder = TacoConfigBuilder().
                                    setReorderDisable(true);
    bool enable_pp = false;
    if (crop_x >0 && crop_y >0 && crop_height >0 && crop_width >0) {
        enable_pp = true;
        taco_config_builder = taco_config_builder.setCrop(pp_channel_id, crop_width, crop_y, crop_width, crop_height);
    }
    if (scale_height >0 && scale_width >0){
        enable_pp = true;
        taco_config_builder = taco_config_builder.setScale(pp_channel_id, scale_width, scale_height);
    }
    if (pix_fmt != OutputFormat::YUV_AUTO){
        enable_pp = true;
        taco_config_builder = taco_config_builder.setOutputFormat(pp_channel_id,pix_fmt);
    }
    if (enable_pp) {
        if (pp_channel_id == Channel::CH0){taco_config_builder = taco_config_builder.setChannels(true,false);}
        else if (pp_channel_id == Channel::CH1){taco_config_builder = taco_config_builder.setChannels(false,true);}
    }
    WorkerConfig::DecoderConfig::TacoConfig g_taco_config = taco_config_builder.build();
    WorkerConfig::DecoderConfig g_hw_decoder_config = DecoderConfigBuilder().useTaco(codec,g_taco_config).build();
    WorkerConfig::DecoderConfig g_sw_decoder_config = DecoderConfigBuilder().useSoftware().build();
    WorkerConfig g_hw_worker_config = WorkerConfigBuilder().
                                    setDataSourceConfig(g_data_source_config).
                                    setDecoderConfig(g_hw_decoder_config).
                                    setWorkerType(WorkerType::FFMPEG_VIDEO_FILE).
                                    build();
    WorkerConfig g_sw_worker_config = WorkerConfigBuilder().
                                    setDataSourceConfig(g_data_source_config).
                                    setDecoderConfig(g_sw_decoder_config).
                                    setWorkerType(WorkerType::FFMPEG_VIDEO_FILE).
                                    build();

    productionline::io::CompareConfig comparatorConfig;
    comparatorConfig.strategy = productionline::io::CompareConfig::Strategy::AUTO_LAYERED;
    comparatorConfig.format_strategy = productionline::io::CompareConfig::FormatStrategy::AUTO;
    comparatorConfig.quick_psnr_threshold = 38.0;
    comparatorConfig.quick_warn_threshold = 35.0;
    comparatorConfig.enable_psnr = true;
    comparatorConfig.enable_ssim = true;
    comparatorConfig.ssim_threshold = 0.95;
    comparatorConfig.ssim_warn_threshold = 0.90;
    comparatorConfig.enable_parallel = true;
    comparatorConfig.use_perceptual_weighting = true;
    comparatorConfig.verbose = true;
    productionline::io::BufferComparator comparator;
    if (! comparator.open(comparatorConfig)){return -1;}

    VideoProductionLine hw_producer = VideoProductionLine(false,1,false);
    VideoProductionLine sw_producer = VideoProductionLine(false,1,false);
    bool hw_result = hw_producer.start(g_hw_worker_config);
    if (!hw_result) {hw_producer.stop(); return -1;}
    bool sw_result = sw_producer.start(g_sw_worker_config);
    if (!sw_result) {sw_producer.stop(); return -1;}

    uint64_t hw_pool_id = hw_producer.getWorkingBufferPoolId();
    uint64_t sw_pool_id = sw_producer.getWorkingBufferPoolId();
    if (hw_pool_id ==0 || sw_pool_id == 0){hw_producer.stop(); sw_producer.stop(); return -1;}
    auto hw_pool = BufferPoolRegistry::getInstance().getPool(hw_pool_id).lock();
    auto sw_pool = BufferPoolRegistry::getInstance().getPool(sw_pool_id).lock();
    if (! hw_pool || ! sw_pool){hw_producer.stop(); sw_producer.stop(); return -1;}

    bool result_passed = true;

    Buffer* hw_buf = hw_pool->acquireFilled(true,100);
    Buffer* sw_buf = sw_pool->acquireFilled(true, 100);
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
        if (sw_pts == hw_pts){
            productionline::io::FrameCompareResult compare_result = comparator.compare(sw_buf,hw_buf);
            result_passed &= compare_result.passed;
            sw_pool->releaseFilled(sw_buf);
            hw_pool->releaseFilled(hw_buf);
            sw_buf = sw_pool->acquireFilled(true,100);
            hw_buf = hw_pool->acquireFilled(true,100);
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
    comparator.printSummary();
    comparator.close();
    hw_producer.stop();
    sw_producer.stop();

    return result_passed;
}

// 自定义测试名称生成器
std::string CustomTestName(
    const testing::TestParamInfo<std::tuple<
        OutputFormat, Channel, int, int, int, int, int, int, std::string
    >>& info) {
    
    auto [pix_fmt, pp_channel, crop_x, crop_y, 
          crop_width, crop_height, scale_width, scale_height, codec] = info.param;
    
    std::ostringstream name;
    
    // 添加像素格式名称
    switch (pix_fmt) {
        case OutputFormat::YUV_NV12: name << "NV12"; break;
        case OutputFormat::YUV_NV21: name << "NV21"; break;
        case OutputFormat::YUV_I420: name << "I420"; break;
        case OutputFormat::YUV_YV12: name << "YV12"; break;
        case OutputFormat::YUV_P010: name << "P010"; break;
        case OutputFormat::YUV_NV16: name << "NV16"; break;
        case OutputFormat::YUV_NV61: name << "NV61"; break;
        case OutputFormat::YUV_I422: name << "I422"; break;
        case OutputFormat::YUV_NV24: name << "NV24"; break;
        case OutputFormat::YUV_I444: name << "I444"; break;
        case OutputFormat::RGB_ARGB888: name << "ARGB888"; break;
        case OutputFormat::RGB_ABGR888: name << "ABGR888"; break;
        case OutputFormat::RGB_RGBA888: name << "RGBA888"; break;
        case OutputFormat::RGB_BGRA888: name << "BGRA888"; break;
        case OutputFormat::RGB_RGB888: name << "RGB888"; break;
        case OutputFormat::RGB_BGR888: name << "BGR888"; break;
        case OutputFormat::RGB_XRGB888: name << "XRGB888"; break;
        case OutputFormat::RGB_XBGR888: name << "XBGR888"; break;
        case OutputFormat::RGB_RGBX888: name << "RGBX888"; break;
        case OutputFormat::RGB_BGRX888: name << "BGRX888"; break;
        case OutputFormat::RGB_RGB888_PLANAR: name << "RGB888P"; break;
        case OutputFormat::RGB_BGR888_PLANAR: name << "BGR888P"; break;
        case OutputFormat::RGB_R16G16B16: name << "R16G16B16"; break;
        case OutputFormat::RGB_B16G16R16: name << "B16G16R16"; break;
        case OutputFormat::RGB_GBRP: name << "GBRP"; break;
        default: name << "UnknownFormat"; break;
    }
    
    // 添加通道信息
    // name << "_" << (pp_channel == Channel::CH0 ? "CH0" : "CH1");
    
    // 添加裁剪信息
    bool has_crop = (crop_x > 0 && crop_y > 0 && crop_width > 0 && crop_height > 0);
    bool has_scale = (scale_width >0 && scale_height >0);
    
    if (has_crop) {
        name << "_crop" << crop_width << "x" << crop_height;
    }
    
    // 添加缩放信息
    if (scale_width > 0 && scale_height > 0) {
        name << "_scale" << scale_width << "x" << scale_height;
    }
    
    // 添加编码器信息
    if (! codec.empty()){
        name << "_" << codec;
    }
    
    // 替换无效字符（Google Test要求名称只包含字母数字字符）
    std::string result = name.str();
    for (char& c : result) {
        if (!std::isalnum(c) && c != '_') {
            c = '_';
        }
    }
    
    return result;
}

class FfmpegTest : public testing::TestWithParam<std::tuple<OutputFormat,Channel,int,int,int,int,int,int,std::string>> {
protected:
    static void SetUpTestSuite() {
        INIT_LOGGER();
        signal(SIGINT, [](int) { g_running = false; });
        signal(SIGTERM, [](int) { g_running = false; });
    }
    static void TearDownTestSuite() {
        //
    }
    void SetUp() override {
        //
    }
    void TearDown() override {
        //
    }
};

TEST_P(FfmpegTest, PostProcessTest) {
    auto [pix_fmt,pp_channel_id,crop_x,crop_y,crop_width,crop_height,scale_width,scale_height,codec] = GetParam();
    
    int result = -1;
    result = test_single_pp(g_input_file,pix_fmt,pp_channel_id,crop_x,crop_y,crop_width,crop_height,scale_width,scale_height,codec);
    
    EXPECT_EQ(result, true);
};

constexpr auto all_ch1_pix_formats = std::array{
    // YUV 格式
    OutputFormat::YUV_NV12,
    OutputFormat::YUV_NV21,
    OutputFormat::YUV_I420,
    OutputFormat::YUV_YV12,
    OutputFormat::YUV_P010,
    OutputFormat::YUV_NV16,
    OutputFormat::YUV_NV61,
    OutputFormat::YUV_I422,
    OutputFormat::YUV_NV24,
    OutputFormat::YUV_I444,
    // RGB 格式
    OutputFormat::RGB_ARGB888,
    OutputFormat::RGB_ABGR888,
    OutputFormat::RGB_RGBA888,
    OutputFormat::RGB_BGRA888,
    OutputFormat::RGB_RGB888,
    OutputFormat::RGB_BGR888,
    OutputFormat::RGB_XRGB888,
    OutputFormat::RGB_XBGR888,
    OutputFormat::RGB_RGBX888,
    OutputFormat::RGB_BGRX888,
    OutputFormat::RGB_RGB888_PLANAR,
    OutputFormat::RGB_BGR888_PLANAR,
    OutputFormat::RGB_R16G16B16,
    OutputFormat::RGB_B16G16R16,
    OutputFormat::RGB_GBRP
};

constexpr auto all_ch0_pix_formats = std::array{
    // YUV 格式
    OutputFormat::YUV_NV12,
    OutputFormat::YUV_NV21,
    OutputFormat::YUV_I420,
    OutputFormat::YUV_YV12,
    OutputFormat::YUV_P010,
    OutputFormat::YUV_NV16,
    OutputFormat::YUV_NV61,
    OutputFormat::YUV_I422,
    OutputFormat::YUV_NV24,
    OutputFormat::YUV_I444
};

//测试数据的分辨率、帧率是由视频文件控制的，不再这里指定

INSTANTIATE_TEST_SUITE_P(
  Ch0PixFormatConversionTest,
  FfmpegTest,
  testing::Combine(
    testing::ValuesIn(all_ch0_pix_formats),
    testing::Values(Channel::CH0),
    testing::Values(0),
    testing::Values(0),
    testing::Values(0),
    testing::Values(0),
    testing::Values(0),
    testing::Values(0),
    testing::Values(std::string(""))
  ),
  CustomTestName
);

INSTANTIATE_TEST_SUITE_P(
  Ch1PixFormatConversionTest,
  FfmpegTest,
  testing::Combine(
    testing::ValuesIn(all_ch1_pix_formats),
    testing::Values(Channel::CH1),
    testing::Values(0),
    testing::Values(0),
    testing::Values(0),
    testing::Values(0),
    testing::Values(0),
    testing::Values(0),
    testing::Values(std::string(""))
  ),
  CustomTestName
);

INSTANTIATE_TEST_SUITE_P(
  DecodeFileTest,
  FfmpegTest,
  testing::Combine(
    testing::ValuesIn(all_ch0_pix_formats),
    testing::Values(Channel::CH0),
    testing::Values(0),
    testing::Values(0),
    testing::Values(0),
    testing::Values(0),
    testing::Values(0),
    testing::Values(0),
    testing::Values(std::string("h264"),std::string("hevc"),std::string("jpeg"))
  ),
  CustomTestName
);