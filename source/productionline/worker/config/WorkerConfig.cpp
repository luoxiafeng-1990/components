/**
 * @file WorkerConfig.cpp
 * @brief Builder 实现
 *
 * ⭐ v3.4 重构：Connector 实现拆分到 config/Connector.cpp
 *              ConsumerTypeConfig::inheritCompanionSettings 拆分到 config/ConsumerTypeConfig.cpp
 */
#include "productionline/worker/config/ConfigBuilders.hpp"
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

DataSourceConfigBuilder& DataSourceConfigBuilder::setLoopCount(int loop_count) {
    data_source_config_.loop_count = loop_count < 1 ? 1 : loop_count;
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setRawFrameWidth(int width) {
    data_source_config_.raw_frame_width = width;
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setRawFrameHeight(int height) {
    data_source_config_.raw_frame_height = height;
    return *this;
}

DataSourceConfigBuilder& DataSourceConfigBuilder::setRawFrameSize(int width, int height) {
    if (width > 0 && height > 0) {
        data_source_config_.raw_frame_width = width;
        data_source_config_.raw_frame_height = height;
    }
    return *this;
}

WorkerConfig::DataSourceConfig DataSourceConfigBuilder::build() const {
    return data_source_config_;
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

WorkerConfigBuilder& WorkerConfigBuilder::setDecoderConfig(const WorkerConfig::DecoderConfig& decoder_config) {
    worker_config_.decoder = decoder_config;
    return *this;
}

WorkerConfigBuilder& WorkerConfigBuilder::setEncoderConfig(const WorkerConfig::EncoderConfig& encoder_config) {
    worker_config_.encoder = encoder_config;
    return *this;
}

WorkerConfigBuilder& WorkerConfigBuilder::setConsumerTypeConfig(const ConsumerTypeConfig& consumer_type_config) {
    worker_config_.consumer_type = consumer_type_config;
    return *this;
}

WorkerConfig WorkerConfigBuilder::build() const {
    return worker_config_;
}

