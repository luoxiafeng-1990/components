#include "productionline/worker/WorkerConfig.hpp"
#include "vendor/taco/decode/TacoDecoderExtension.hpp"
#include <stdexcept>
#include <algorithm>
#include <string>
#include <cstdio>

extern "C" {
#include <libavutil/pixfmt.h>
}

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

// ============================================================
// 统一像素格式字符串 → AVPixelFormat 映射
// ============================================================

int mapPixFmtStringToAVPixFmt(std::string_view format_name) {
    std::string fmt(format_name);
    std::transform(fmt.begin(), fmt.end(), fmt.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (fmt == "nv12")        return AV_PIX_FMT_NV12;
    if (fmt == "nv21")        return AV_PIX_FMT_NV21;
    if (fmt == "yuv420p")     return AV_PIX_FMT_YUV420P;
    if (fmt == "yuvj420p")    return AV_PIX_FMT_YUVJ420P;
    if (fmt == "yuyv422" || fmt == "yuyv")   return AV_PIX_FMT_YUYV422;
    if (fmt == "yvyu")        return AV_PIX_FMT_YVYU422;
    if (fmt == "uyvy422" || fmt == "uyvy")   return AV_PIX_FMT_UYVY422;
    if (fmt == "rgb24"  || fmt == "rgb888")  return AV_PIX_FMT_RGB24;
    if (fmt == "bgr24"  || fmt == "bgr888")  return AV_PIX_FMT_BGR24;
    if (fmt == "argb"   || fmt == "argb888") return AV_PIX_FMT_ARGB;
    if (fmt == "bgra"   || fmt == "bgra888") return AV_PIX_FMT_BGRA;
    if (fmt == "rgba"   || fmt == "rgba888") return AV_PIX_FMT_RGBA;
    if (fmt == "abgr"   || fmt == "abgr888") return AV_PIX_FMT_ABGR;
    if (fmt == "rgb0"   || fmt == "rgbx888") return AV_PIX_FMT_RGB0;
    if (fmt == "bgr0"   || fmt == "bgrx888") return AV_PIX_FMT_BGR0;
    if (fmt == "rgb565")      return AV_PIX_FMT_RGB565LE;
    if (fmt == "bgr565")      return AV_PIX_FMT_BGR565LE;
    if (fmt == "rgb555")      return AV_PIX_FMT_RGB555LE;
    if (fmt == "bgr555")      return AV_PIX_FMT_BGR555LE;
#if defined(AV_PIX_FMT_X2RGB10LE)
    if (fmt == "x2rgb10le" || fmt == "rgbx101010" || fmt == "rgb101010")
        return AV_PIX_FMT_X2RGB10LE;
#endif
#if defined(AV_PIX_FMT_X2BGR10LE)
    if (fmt == "x2bgr10le" || fmt == "bgrx101010" || fmt == "bgr101010")
        return AV_PIX_FMT_X2BGR10LE;
#endif

    fprintf(stderr, "[WARN] mapPixFmtStringToAVPixFmt: unrecognized format \"%s\", fallback to NV12\n",
            std::string(format_name).c_str());
    return AV_PIX_FMT_NV12;
}
