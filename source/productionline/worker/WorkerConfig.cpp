#include "productionline/worker/WorkerConfig.hpp"
#include "vendor/taco/decode/TacoDecoderExtension.hpp"
#include <stdexcept>
#include <algorithm>
#include <string>
#include <cstdio>

// ============================================================
// DataSourceConfigBuilder 实现
// ============================================================

DataSourceConfigBuilder& DataSourceConfigBuilder::setPath(std::string_view path) {
    data_source_config_.path = std::string(path);
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setPath(const char* path) {
    if (path) {
        data_source_config_.path = path;
    } else {
        data_source_config_.path.clear();
    }
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setPath(const std::string& path) {
    data_source_config_.path = path;
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setPathIfNonEmpty(std::string_view path) {
    if (!path.empty()) {
        data_source_config_.path = std::string(path);
    }
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setBufferCount(int count) {
    data_source_config_.buffer_count = count;
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setBufferCountIfNonZero(int count) {
    if (count > 0) {
        data_source_config_.buffer_count = count;
    }
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setMaxFrames(int max_frames) {
    data_source_config_.max_frames = max_frames;
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setMaxFramesIfNonZero(int max_frames) {
    if (max_frames != 0) {
        data_source_config_.max_frames = max_frames;
    }
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setBufferMode(bool mode) {
    data_source_config_.buffer_mode = mode;
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setCodecParams(const AVCodecParameters* params) {
    data_source_config_.codec_params = params;
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setTimeBase(AVRational tb) {
    data_source_config_.time_base = tb;
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setSharedPacketSource(std::shared_ptr<IEncodedPacketSource> source) {
    data_source_config_.shared_packet_source = std::move(source);
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setDeferredCommit(bool deferred) {
    data_source_config_.deferred_commit = deferred;
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setLoop(bool loop) {
    data_source_config_.loop = loop;
    return *this;
}

WorkerConfig::DataSourceConfig DataSourceConfigBuilder::build() const {
    return data_source_config_;
}

// ============================================================
// DisplayConfigBuilder 实现
// ============================================================

DisplayConfigBuilder& DisplayConfigBuilder::setDisplayWidth(int width) {
    display_config_.width = width;
    return *this;
}

DisplayConfigBuilder& DisplayConfigBuilder::setDisplayHeight(int height) {
    display_config_.height = height;
    return *this;
}

DisplayConfigBuilder& DisplayConfigBuilder::setDisplayResolution(int width, int height) {
    display_config_.width = width;
    display_config_.height = height;
    return *this;
}

DisplayConfigBuilder& DisplayConfigBuilder::setBitsPerPixel(int bpp) {
    display_config_.bits_per_pixel = bpp;
    return *this;
}

WorkerConfig::DisplayConfig DisplayConfigBuilder::build() const {
    return display_config_;
}

// ============================================================
// TacoConfigBuilder 实现
// ============================================================

TacoConfigBuilder& TacoConfigBuilder::setChannels(bool ch0, bool ch1) {
    taco_config_.ch0_enable = ch0;
    taco_config_.ch1_enable = ch1;
    return *this;
}

TacoConfigBuilder& TacoConfigBuilder::setOutputFormat(
    Channel ch,
    OutputFormat format,
    ColorStandard std
) {
    int format_value = static_cast<int>(format);
    int std_value = static_cast<int>(std);
    
    // 判断是 RGB 还是 YUV（RGB 格式枚举值 >= 1000）
    bool is_rgb = (format_value >= 1000);
    
    if (ch == Channel::CH0) {
        // 通道0仅支持 YUV
        if (is_rgb) {
            // 通道0不支持 RGB 格式，忽略此配置
            return *this;
        }
        // 设置 YUV 格式
        taco_config_.ch0_yuv_format = format_value;
        taco_config_.ch0_yuv_std = std_value;
        
    } else if (ch == Channel::CH1) {
        // 通道1支持 RGB 和 YUV
        taco_config_.ch1_rgb = is_rgb;
        
        if (is_rgb) {
            taco_config_.ch1_rgb_format = mapEnumToRgbDriverValue(format);
            taco_config_.ch1_rgb_std = std_value;
        } else {
            // YUV 格式
            taco_config_.ch1_yuv_format = format_value;
            taco_config_.ch1_yuv_std = std_value;
        }
    }
    
    return *this;
}

TacoConfigBuilder& TacoConfigBuilder::setCrop(Channel ch, int x, int y, int width, int height) {
    if (ch == Channel::CH0) {
        taco_config_.ch0_crop_x = x;
        taco_config_.ch0_crop_y = y;
        taco_config_.ch0_crop_width = width;
        taco_config_.ch0_crop_height = height;
    } else if (ch == Channel::CH1) {
        taco_config_.ch1_crop_x = x;
        taco_config_.ch1_crop_y = y;
        taco_config_.ch1_crop_width = width;
        taco_config_.ch1_crop_height = height;
    }
    return *this;
}

TacoConfigBuilder& TacoConfigBuilder::setScale(Channel ch, int width, int height) {
    if (ch == Channel::CH0) {
        taco_config_.ch0_scale_width = width;
        taco_config_.ch0_scale_height = height;
    } else if (ch == Channel::CH1) {
        taco_config_.ch1_scale_width = width;
        taco_config_.ch1_scale_height = height;
    }
    return *this;
}

TacoConfig TacoConfigBuilder::build() const {
    return taco_config_;
}

// ============================================================
// TacoConfigBuilder 静态辅助函数实现
// ============================================================

OutputFormat TacoConfigBuilder::mapFormatNameToEnum(std::string_view format_name) {
    std::string name(format_name);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // YUV 格式
    if (name == "auto" || name == "yuv_auto") return OutputFormat::YUV_AUTO;
    if (name == "nv12") return OutputFormat::YUV_NV12;
    if (name == "nv21") return OutputFormat::YUV_NV21;
    if (name == "i420" || name == "yuv420p") return OutputFormat::YUV_I420;
    if (name == "yv12") return OutputFormat::YUV_YV12;
    if (name == "p010") return OutputFormat::YUV_P010;
    if (name == "nv16") return OutputFormat::YUV_NV16;
    if (name == "nv61") return OutputFormat::YUV_NV61;
    if (name == "i422" || name == "yuv422p") return OutputFormat::YUV_I422;
    if (name == "nv24") return OutputFormat::YUV_NV24;
    if (name == "i444" || name == "yuv444p") return OutputFormat::YUV_I444;

    // RGB 格式（含 ffmpeg 风格别名）
    if (name == "argb888" || name == "argb") return OutputFormat::RGB_ARGB888;
    if (name == "abgr888" || name == "abgr") return OutputFormat::RGB_ABGR888;
    if (name == "rgba888" || name == "rgba") return OutputFormat::RGB_RGBA888;
    if (name == "bgra888" || name == "bgra") return OutputFormat::RGB_BGRA888;
    if (name == "rgb888" || name == "rgb24") return OutputFormat::RGB_RGB888;
    if (name == "bgr888" || name == "bgr24") return OutputFormat::RGB_BGR888;
    if (name == "xrgb888" || name == "0rgb") return OutputFormat::RGB_XRGB888;
    if (name == "xbgr888" || name == "0bgr") return OutputFormat::RGB_XBGR888;
    if (name == "rgbx888" || name == "rgb0") return OutputFormat::RGB_RGBX888;
    if (name == "bgrx888" || name == "bgr0") return OutputFormat::RGB_BGRX888;
    if (name == "rgb888_planar") return OutputFormat::RGB_RGB888_PLANAR;
    if (name == "bgr888_planar") return OutputFormat::RGB_BGR888_PLANAR;
    if (name == "r16g16b16") return OutputFormat::RGB_R16G16B16;
    if (name == "b16g16r16") return OutputFormat::RGB_B16G16R16;
    if (name == "gbrp") return OutputFormat::RGB_GBRP;
    if (name == "argb2101010" || name == "a2r10g10b10" || name == "rgbx101010" || name == "rgb101010")
        return OutputFormat::RGB_A2R10G10B10;
    if (name == "abgr2101010" || name == "a2b10g10r10" || name == "bgrx101010" || name == "bgr101010")
        return OutputFormat::RGB_A2B10G10R10;
    if (name == "rgba2101010" || name == "r10g10b10a2") return OutputFormat::RGB_R10G10B10A2;
    if (name == "bgra2101010" || name == "b10g10r10a2") return OutputFormat::RGB_B10G10R10A2;

    fprintf(stderr, "[WARN] mapFormatNameToEnum: unrecognized format \"%s\", fallback to YUV_AUTO\n",
            std::string(format_name).c_str());
    return OutputFormat::YUV_AUTO;
}

ColorStandard TacoConfigBuilder::mapColorStdNameToEnum(std::string_view std_name) {
    if (std_name == "none") return ColorStandard::NONE;
    if (std_name == "bt601") return ColorStandard::BT601;
    if (std_name == "bt601_l" || std_name == "bt601_limited") return ColorStandard::BT601_LIMITED;
    if (std_name == "bt709") return ColorStandard::BT709;
    if (std_name == "bt709_l" || std_name == "bt709_limited") return ColorStandard::BT709_LIMITED;
    if (std_name == "bt2020") return ColorStandard::BT2020;
    if (std_name == "bt2020_l" || std_name == "bt2020_limited") return ColorStandard::BT2020_LIMITED;
    
    // 默认返回 BT601
    return ColorStandard::BT601;
}

std::string_view TacoConfigBuilder::mapFormatEnumToName(OutputFormat format) {
    switch (format) {
        // YUV 格式
        case OutputFormat::YUV_AUTO: return "yuv_auto";
        case OutputFormat::YUV_NV12: return "nv12";
        case OutputFormat::YUV_NV21: return "nv21";
        case OutputFormat::YUV_I420: return "i420";
        case OutputFormat::YUV_YV12: return "yv12";
        case OutputFormat::YUV_P010: return "p010";
        case OutputFormat::YUV_NV16: return "nv16";
        case OutputFormat::YUV_NV61: return "nv61";
        case OutputFormat::YUV_I422: return "i422";
        case OutputFormat::YUV_NV24: return "nv24";
        case OutputFormat::YUV_I444: return "i444";
        
        // RGB 格式
        case OutputFormat::RGB_ARGB888: return "argb888";
        case OutputFormat::RGB_ABGR888: return "abgr888";
        case OutputFormat::RGB_RGBA888: return "rgba888";
        case OutputFormat::RGB_BGRA888: return "bgra888";
        case OutputFormat::RGB_RGB888: return "rgb888";
        case OutputFormat::RGB_BGR888: return "bgr888";
        case OutputFormat::RGB_XRGB888: return "xrgb888";
        case OutputFormat::RGB_XBGR888: return "xbgr888";
        case OutputFormat::RGB_RGBX888: return "rgbx888";
        case OutputFormat::RGB_BGRX888: return "bgrx888";
        case OutputFormat::RGB_RGB888_PLANAR: return "rgb888_planar";
        case OutputFormat::RGB_BGR888_PLANAR: return "bgr888_planar";
        case OutputFormat::RGB_R16G16B16: return "r16g16b16";
        case OutputFormat::RGB_B16G16R16: return "b16g16r16";
        case OutputFormat::RGB_GBRP: return "gbrp";
        case OutputFormat::RGB_A2R10G10B10: return "argb2101010";
        case OutputFormat::RGB_A2B10G10R10: return "abgr2101010";
        case OutputFormat::RGB_R10G10B10A2: return "rgba2101010";
        case OutputFormat::RGB_B10G10R10A2: return "bgra2101010";
        
        default: return "unknown";
    }
}

std::string_view TacoConfigBuilder::mapColorStdEnumToName(ColorStandard std) {
    switch (std) {
        case ColorStandard::NONE: return "none";
        case ColorStandard::BT601: return "bt601";
        case ColorStandard::BT601_LIMITED: return "bt601_limited";
        case ColorStandard::BT709: return "bt709";
        case ColorStandard::BT709_LIMITED: return "bt709_limited";
        case ColorStandard::BT2020: return "bt2020";
        case ColorStandard::BT2020_LIMITED: return "bt2020_limited";
        default: return "unknown";
    }
}

int TacoConfigBuilder::mapEnumToRgbDriverValue(OutputFormat format) {
    switch (format) {
        case OutputFormat::RGB_RGB888: return 1;
        case OutputFormat::RGB_RGB888_PLANAR: return 2;
        case OutputFormat::RGB_BGR888: return 3;
        case OutputFormat::RGB_BGR888_PLANAR: return 4;
        case OutputFormat::RGB_R16G16B16: return 5;
        case OutputFormat::RGB_B16G16R16: return 7;
        case OutputFormat::RGB_ARGB888: return 9;
        case OutputFormat::RGB_ABGR888: return 11;
        case OutputFormat::RGB_RGBA888: return 13;
        case OutputFormat::RGB_BGRA888: return 15;
        case OutputFormat::RGB_RGBX888: return -1;  // 驱动不支持 RGBX
        case OutputFormat::RGB_BGRX888: return -1;  // 驱动不支持 BGRX
        case OutputFormat::RGB_A2R10G10B10: return 17;
        case OutputFormat::RGB_A2B10G10R10: return 19;
        case OutputFormat::RGB_R10G10B10A2: return 21;
        case OutputFormat::RGB_B10G10R10A2: return 23;
        case OutputFormat::RGB_XRGB888: return 25;
        case OutputFormat::RGB_XBGR888: return 27;
        case OutputFormat::RGB_GBRP: return 28;
        default: return 9;
    }
}

// ============================================================
// DecoderConfigBuilder 实现
// ============================================================

DecoderConfigBuilder& DecoderConfigBuilder::setDecoderName(std::string_view name) {
    decoder_config_.name = std::string(name);
    return *this;
}

DecoderConfigBuilder& DecoderConfigBuilder::setDecoderName(const char* name) {
    if (name) {
        decoder_config_.name = name;
    } else {
        decoder_config_.name = std::nullopt;
    }
    return *this;
}

DecoderConfigBuilder& DecoderConfigBuilder::setHwaccelDevice(std::string_view device) {
    decoder_config_.hwaccel_device = std::string(device);
    return *this;
}

DecoderConfigBuilder& DecoderConfigBuilder::setHwaccelDevice(const char* device) {
    if (device) {
        decoder_config_.hwaccel_device = device;
    } else {
        decoder_config_.hwaccel_device = std::nullopt;
    }
    return *this;
}

DecoderConfigBuilder& DecoderConfigBuilder::setDecodeThreads(int threads) {
    decoder_config_.decode_threads = threads;
    return *this;
}

DecoderConfigBuilder& DecoderConfigBuilder::useVendor(
        std::string_view codec,
        std::unique_ptr<IDecoderVendorExtension> extension) {
    decoder_config_.name = std::string(codec) + "_" + extension->kind();
    decoder_config_.enable_hardware = true;
    decoder_config_.vendor = std::move(extension);
    return *this;
}

DecoderConfigBuilder& DecoderConfigBuilder::useTaco(std::string_view codec, const TacoConfig& taco_config) {
    return useVendor(codec, makeTacoDecoderExtension(taco_config));
}

DecoderConfigBuilder& DecoderConfigBuilder::useSoftware() {
    decoder_config_.name = std::nullopt;
    decoder_config_.enable_hardware = false;
    decoder_config_.vendor = nullptr;
    return *this;
}

WorkerConfig::DecoderConfig DecoderConfigBuilder::build() const {
    return decoder_config_;
}

// ============================================================
// WorkerGlobalConfigBuilder 实现
// ============================================================

WorkerGlobalConfigBuilder& WorkerGlobalConfigBuilder::setWorkerType(WorkerType type) {
    global_config_.worker_type = type;
    return *this;
}

WorkerGlobalConfigBuilder& WorkerGlobalConfigBuilder::setThreadPoolSize(int size) {
    global_config_.thread_pool_size = size;
    return *this;
}

WorkerConfig::GlobalConfig WorkerGlobalConfigBuilder::build() const {
    return global_config_;
}

// ============================================================
// ConsumerTypeConfigBuilder 实现
// ============================================================

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setConsumerMaxFrames(int frames) {
    consumer_type_config_.max_frames = frames;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setVerbose(bool verbose) {
    consumer_type_config_.verbose = verbose;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::enableDisplay(bool enable, int device_id) {
    consumer_type_config_.display.enable = enable;
    consumer_type_config_.display.device_id = device_id;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::enableSaveRaw(bool enable,
                                                                    const std::string& output_path,
                                                                    int max_frames) {
    consumer_type_config_.save_raw.enable = enable;
    consumer_type_config_.save_raw.setOutputPath(output_path);
    consumer_type_config_.save_raw.max_frames_per_channel = {max_frames};
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::enableSaveEncoded(bool enable,
                                                                        const std::string& output_path) {
    consumer_type_config_.save_encoded.enable = enable;
    consumer_type_config_.save_encoded.output_path = output_path;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::enableCompare(bool enable,
                                                                    double min_psnr,
                                                                    double min_ssim) {
    consumer_type_config_.compare.enable_psnr = enable;
    consumer_type_config_.compare.enable_ssim = enable;
    consumer_type_config_.compare.min_psnr = min_psnr;
    consumer_type_config_.compare.min_ssim = min_ssim;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::enablePerformance(bool enable, double target_fps) {
    consumer_type_config_.performance.enable = enable;
    consumer_type_config_.performance.target_fps = target_fps;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::enableNpuInference(const std::string& model_path,
                                                                       float conf_threshold,
                                                                       float nms_threshold,
                                                                       bool enable_draw) {
    consumer_type_config_.npu_inference.enable = true;
    consumer_type_config_.npu_inference.model_path = model_path;
    consumer_type_config_.npu_inference.conf_threshold = conf_threshold;
    consumer_type_config_.npu_inference.nms_threshold = nms_threshold;
    consumer_type_config_.npu_inference.enable_draw = enable_draw;
    return *this;
}

WorkerConfig::ConsumerTypeConfig ConsumerTypeConfigBuilder::build() const {
    return consumer_type_config_;
}

// ============================================================
// ConsumerTypeConfig::inheritCompanionSettings 实现
// ============================================================

void WorkerConfig::ConsumerTypeConfig::inheritCompanionSettings(const ConsumerTypeConfig& shared) {
    if (shared.display.enable && !display.enable)
        display = shared.display;

    if (shared.npu_inference.enable && !npu_inference.enable)
        npu_inference = shared.npu_inference;

    if ((shared.compare.enable_psnr || shared.compare.enable_ssim)
        && !compare.enable_psnr && !compare.enable_ssim)
        compare = shared.compare;

    if (shared.verbose)
        verbose = true;
}

// ============================================================
// WorkerConfigBuilder 实现
// ============================================================

WorkerConfigBuilder& WorkerConfigBuilder::setGlobalConfig(const WorkerConfig::GlobalConfig& global_config) {
    worker_config_.global = global_config;
    return *this;
}

WorkerConfigBuilder& WorkerConfigBuilder::setDataSourceConfig(const WorkerConfig::DataSourceConfig& data_source_config) {
    worker_config_.data_source = data_source_config;
    return *this;
}

WorkerConfigBuilder& WorkerConfigBuilder::setDisplayConfig(const WorkerConfig::DisplayConfig& display_config) {
    worker_config_.display = display_config;
    return *this;
}

WorkerConfigBuilder& WorkerConfigBuilder::setDecoderConfig(const WorkerConfig::DecoderConfig& decoder_config) {
    worker_config_.decoder = decoder_config;
    return *this;
}

WorkerConfigBuilder& WorkerConfigBuilder::setConsumerTypeConfig(const WorkerConfig::ConsumerTypeConfig& consumer_type_config) {
    worker_config_.consumer_type = consumer_type_config;
    return *this;
}

WorkerConfig WorkerConfigBuilder::build() const {
    return worker_config_;
}

// ============================================================
// Connector 类实现（v2.20：从 Connector.cpp 移动）
// ============================================================

Connector::Connector(Mode mode,
                     const std::vector<std::string>& producer_names,
                     const std::vector<std::string>& consumer_names)
    : mode_(mode)
    , producer_names_(producer_names)
    , consumer_names_(consumer_names)
{
    if (producer_names_.empty()) {
        throw std::invalid_argument("Connector: producer_names cannot be empty");
    }
    if (consumer_names_.empty()) {
        throw std::invalid_argument("Connector: consumer_names cannot be empty");
    }
    
    // 验证模式约束
    switch (mode_) {
        case Mode::ONE_TO_ONE: {
            if (producer_names_.size() != consumer_names_.size()) {
                throw std::invalid_argument("Connector ONE_TO_ONE: producer_names.size() must equal consumer_names.size()");
            }
            break;
        }
        case Mode::ONE_TO_MANY: {
            if (producer_names_.size() != 1) {
                throw std::invalid_argument("Connector ONE_TO_MANY: producer_names.size() must be 1");
            }
            break;
        }
        case Mode::MANY_TO_ONE: {
            if (consumer_names_.size() != 1) {
                throw std::invalid_argument("Connector MANY_TO_ONE: consumer_names.size() must be 1");
            }
            break;
        }
        case Mode::MANY_TO_MANY: {
            // 无特殊约束
            break;
        }
    }
}

std::string Connector::getProducerNameForConsumer(const std::string& consumer_name) const {
    // 查找 consumer_name 在 consumer_names_ 中的位置
    size_t consumer_idx = SIZE_MAX;
    for (size_t i = 0; i < consumer_names_.size(); i++) {
        if (consumer_names_[i] == consumer_name) {
            consumer_idx = i;
            break;
        }
    }
    
    if (consumer_idx == SIZE_MAX) {
        return "";  // 消费者不存在
    }
    
    // 根据模式动态计算对应的生产者
    switch (mode_) {
        case Mode::ONE_TO_ONE: {
            // 1:1 映射：consumer_names[i] -> producer_names[i]
            return producer_names_[consumer_idx];
        }
        
        case Mode::ONE_TO_MANY: {
            // 1:N 映射：所有消费者都绑定到同一个生产者（第一个）
            return producer_names_[0];
        }
        
        case Mode::MANY_TO_ONE: {
            // N:1 映射：第一个消费者绑定到第一个生产者
            return producer_names_[0];
        }
        
        case Mode::MANY_TO_MANY: {
            // N:M 映射：轮询策略
            // consumer_names[i] 绑定到 producer_names[i % producer_names_.size()]
            size_t producer_idx = consumer_idx % producer_names_.size();
            return producer_names_[producer_idx];
        }
    }
    
    return "";  // 不应该到达这里
}

// ============================================================
// Connector 访问器实现
// ============================================================

Connector::Mode Connector::getMode() const {
    return mode_;
}

const std::vector<std::string>& Connector::getProducerNames() const {
    return producer_names_;
}

const std::vector<std::string>& Connector::getConsumerNames() const {
    return consumer_names_;
}

bool Connector::containsProducer(const std::string& producer_name) const {
    for (const auto& name : producer_names_) {
        if (name == producer_name) {
            return true;
        }
    }
    return false;
}

bool Connector::containsConsumer(const std::string& consumer_name) const {
    for (const auto& name : consumer_names_) {
        if (name == consumer_name) {
            return true;
        }
    }
    return false;
}

void Connector::setSharedSource(const std::string& producer_name, std::shared_ptr<class IEncodedPacketSource> source) {
    shared_sources_[producer_name] = source;
}

std::shared_ptr<class IEncodedPacketSource> Connector::getSharedSource(const std::string& producer_name) const {
    auto it = shared_sources_.find(producer_name);
    if (it != shared_sources_.end()) {
        return it->second;
    }
    return nullptr;
}
