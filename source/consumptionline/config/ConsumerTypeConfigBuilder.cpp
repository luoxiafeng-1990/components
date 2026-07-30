#include "consumptionline/config/ConsumerTypeConfigBuilder.hpp"

DisplayConsumerConfigBuilder& DisplayConsumerConfigBuilder::setEnable(bool enable) {
    config_.enable = enable;
    return *this;
}

DisplayConsumerConfigBuilder& DisplayConsumerConfigBuilder::setDeviceId(int id) {
    config_.device_id = id;
    return *this;
}

DisplayConsumerConfigBuilder& DisplayConsumerConfigBuilder::setVendor(
    std::unique_ptr<IDisplayVendorExtension> vendor) {
    config_.vendor = std::move(vendor);
    return *this;
}

ConsumerTypeConfig::DisplayConsumerConfig DisplayConsumerConfigBuilder::build() const {
    return config_;
}

SaveRawConfigBuilder& SaveRawConfigBuilder::setEnable(bool enable) {
    config_.enable = enable;
    return *this;
}

SaveRawConfigBuilder& SaveRawConfigBuilder::setOutputPaths(const std::vector<std::string>& paths) {
    config_.output_paths = paths;
    return *this;
}

SaveRawConfigBuilder& SaveRawConfigBuilder::setMaxFramesPerChannel(const std::vector<int>& frames) {
    config_.max_frames_per_channel = frames;
    return *this;
}

ConsumerTypeConfig::SaveRawType SaveRawConfigBuilder::build() const {
    return config_;
}

SaveEncodedConfigBuilder& SaveEncodedConfigBuilder::setEnable(bool enable) {
    config_.enable = enable;
    return *this;
}

SaveEncodedConfigBuilder& SaveEncodedConfigBuilder::setOutputPath(const std::string& path) {
    config_.output_path = path;
    return *this;
}

ConsumerTypeConfig::SaveEncodedType SaveEncodedConfigBuilder::build() const {
    return config_;
}

NpuInferenceConfigBuilder& NpuInferenceConfigBuilder::setEnable(bool enable) {
    config_.enable = enable;
    return *this;
}

NpuInferenceConfigBuilder& NpuInferenceConfigBuilder::setModelPath(const std::string& path) {
    config_.model_path = path;
    return *this;
}

NpuInferenceConfigBuilder& NpuInferenceConfigBuilder::setAlgorithm(consumer::NpuAlgorithm algorithm) {
    config_.algorithm = algorithm;
    return *this;
}

NpuInferenceConfigBuilder& NpuInferenceConfigBuilder::setConfThreshold(float threshold) {
    config_.conf_threshold = threshold;
    return *this;
}

NpuInferenceConfigBuilder& NpuInferenceConfigBuilder::setNmsThreshold(float threshold) {
    config_.nms_threshold = threshold;
    return *this;
}

NpuInferenceConfigBuilder& NpuInferenceConfigBuilder::setNpuCoreIndex(int index) {
    config_.npu_core_index = index;
    return *this;
}

NpuInferenceConfigBuilder& NpuInferenceConfigBuilder::setUsePhysicalAddr(bool use) {
    config_.use_physical_addr = use;
    return *this;
}

NpuInferenceConfigBuilder& NpuInferenceConfigBuilder::setEnableDraw(bool enable) {
    config_.enable_draw = enable;
    return *this;
}

NpuInferenceConfigBuilder& NpuInferenceConfigBuilder::setInferenceInterval(int interval) {
    config_.inference_interval = interval;
    return *this;
}

NpuInferenceConfigBuilder& NpuInferenceConfigBuilder::setVendor(
    std::unique_ptr<INpuInferenceVendorExtension> vendor) {
    config_.vendor = std::move(vendor);
    return *this;
}

ConsumerTypeConfig::NpuInferenceType NpuInferenceConfigBuilder::build() const {
    return config_;
}

CompareConfigBuilder& CompareConfigBuilder::setEnablePsnr(bool enable) {
    config_.enable_psnr = enable;
    return *this;
}

CompareConfigBuilder& CompareConfigBuilder::setEnableSsim(bool enable) {
    config_.enable_ssim = enable;
    return *this;
}

CompareConfigBuilder& CompareConfigBuilder::setMinPsnr(double psnr) {
    config_.min_psnr = psnr;
    return *this;
}

CompareConfigBuilder& CompareConfigBuilder::setMinSsim(double ssim) {
    config_.min_ssim = ssim;
    return *this;
}

CompareConfigBuilder& CompareConfigBuilder::setVerbose(bool verbose) {
    config_.verbose = verbose;
    return *this;
}

CompareConfigBuilder& CompareConfigBuilder::setEnableChannelCompare(bool enable) {
    config_.enable_channel_compare = enable;
    return *this;
}

CompareConfigBuilder& CompareConfigBuilder::setReferenceChannel(int ch) {
    config_.reference_channel = ch;
    return *this;
}

CompareConfigBuilder& CompareConfigBuilder::setCompareChannel(int ch) {
    config_.compare_channel = ch;
    return *this;
}

ConsumerTypeConfig::CompareType CompareConfigBuilder::build() const {
    return config_;
}

PerformanceConfigBuilder& PerformanceConfigBuilder::setEnable(bool enable) {
    config_.enable = enable;
    return *this;
}

PerformanceConfigBuilder& PerformanceConfigBuilder::setTargetFps(double fps) {
    config_.target_fps = fps;
    return *this;
}

ConsumerTypeConfig::PerformanceType PerformanceConfigBuilder::build() const {
    return config_;
}

JpegEncodeConfigBuilder& JpegEncodeConfigBuilder::setEnable(bool enable) {
    config_.enable = enable;
    return *this;
}

JpegEncodeConfigBuilder& JpegEncodeConfigBuilder::setOutputPipe(const std::string& pipe) {
    config_.output_pipe = pipe;
    return *this;
}

JpegEncodeConfigBuilder& JpegEncodeConfigBuilder::setQuality(int quality) {
    config_.quality = quality;
    return *this;
}

JpegEncodeConfigBuilder& JpegEncodeConfigBuilder::setTargetFps(int fps) {
    config_.target_fps = fps;
    return *this;
}

JpegEncodeConfigBuilder& JpegEncodeConfigBuilder::setEncoderName(const std::string& name) {
    config_.encoder_name = name;
    return *this;
}

ConsumerTypeConfig::JpegEncodeType JpegEncodeConfigBuilder::build() const {
    return config_;
}

VideoEncodeConfigBuilder& VideoEncodeConfigBuilder::setEnable(bool enable) {
    config_.enable = enable;
    return *this;
}

VideoEncodeConfigBuilder& VideoEncodeConfigBuilder::setEncoderName(const std::string& name) {
    config_.encoder_name = name;
    return *this;
}

VideoEncodeConfigBuilder& VideoEncodeConfigBuilder::setBitRate(int64_t bit_rate) {
    config_.bit_rate = bit_rate;
    return *this;
}

VideoEncodeConfigBuilder& VideoEncodeConfigBuilder::setGopSize(int gop_size) {
    config_.gop_size = gop_size;
    return *this;
}

VideoEncodeConfigBuilder& VideoEncodeConfigBuilder::setFramerate(int num, int den) {
    config_.framerate_num = num;
    config_.framerate_den = den > 0 ? den : 1;
    return *this;
}

VideoEncodeConfigBuilder& VideoEncodeConfigBuilder::setRcMode(int rc_mode) {
    config_.rc_mode = rc_mode;
    return *this;
}

VideoEncodeConfigBuilder& VideoEncodeConfigBuilder::setMaxBFrames(int max_b_frames) {
    config_.max_b_frames = max_b_frames;
    return *this;
}

VideoEncodeConfigBuilder& VideoEncodeConfigBuilder::setBufferCount(int buffer_count) {
    config_.buffer_count = buffer_count;
    return *this;
}

ConsumerTypeConfig::VideoEncodeType VideoEncodeConfigBuilder::build() const {
    return config_;
}

OpencvConfigBuilder& OpencvConfigBuilder::setEnable(bool enable) {
    config_.enable = enable;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setOpType(ConsumerTypeConfig::OpencvType::OpType type) {
    config_.op_type = type;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setResize(const ConsumerTypeConfig::OpencvType::Resize& r) {
    config_.resize = r;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setCrop(const ConsumerTypeConfig::OpencvType::Crop& c) {
    config_.crop = c;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setMorph(const ConsumerTypeConfig::OpencvType::Morph& m) {
    config_.morph = m;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setSobel(const ConsumerTypeConfig::OpencvType::Sobel& s) {
    config_.sobel = s;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setCanny(const ConsumerTypeConfig::OpencvType::Canny& c) {
    config_.canny = c;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setLaplacian(const ConsumerTypeConfig::OpencvType::Laplacian& l) {
    config_.laplacian = l;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setTranslate(const ConsumerTypeConfig::OpencvType::Translate& t) {
    config_.translate = t;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setRotate(const ConsumerTypeConfig::OpencvType::Rotate& r) {
    config_.rotate = r;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setPerspective(
    const ConsumerTypeConfig::OpencvType::Perspective& p) {
    config_.perspective = p;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setDrawLine(const ConsumerTypeConfig::OpencvType::DrawLine& d) {
    config_.draw_line = d;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setDrawRect(const ConsumerTypeConfig::OpencvType::DrawRect& d) {
    config_.draw_rect = d;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setPutText(const ConsumerTypeConfig::OpencvType::PutText& p) {
    config_.put_text = p;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setGaussianBlur(
    const ConsumerTypeConfig::OpencvType::GaussianBlur& g) {
    config_.gaussian_blur = g;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setThreshold(const ConsumerTypeConfig::OpencvType::Threshold& t) {
    config_.threshold = t;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setSplitMerge(const ConsumerTypeConfig::OpencvType::SplitMerge& s) {
    config_.split_merge = s;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setCvtColor(const ConsumerTypeConfig::OpencvType::ColorConvert& c) {
    config_.cvtcolor = c;
    return *this;
}

OpencvConfigBuilder& OpencvConfigBuilder::setVendor(std::unique_ptr<IOpencvVendorExtension> vendor) {
    config_.vendor = std::move(vendor);
    return *this;
}

ConsumerTypeConfig::OpencvType OpencvConfigBuilder::build() const {
    return config_;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setMaxFrames(int frames) {
    config_.max_frames = frames;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setMaxDurationSeconds(double seconds) {
    config_.max_duration_seconds = seconds;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setTimeoutMs(int ms) {
    config_.timeout_ms = ms;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setMaxTimeoutCount(int count) {
    config_.max_timeout_count = count;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setVerbose(bool verbose) {
    config_.verbose = verbose;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setDisplayConfig(
    const ConsumerTypeConfig::DisplayConsumerConfig& config) {
    config_.display = config;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setSaveRawConfig(
    const ConsumerTypeConfig::SaveRawType& config) {
    config_.save_raw = config;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setSaveEncodedConfig(
    const ConsumerTypeConfig::SaveEncodedType& config) {
    config_.save_encoded = config;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setNpuInferenceConfig(
    const ConsumerTypeConfig::NpuInferenceType& config) {
    config_.npu_inference = config;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setCompareConfig(
    const ConsumerTypeConfig::CompareType& config) {
    config_.compare = config;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setPerformanceConfig(
    const ConsumerTypeConfig::PerformanceType& config) {
    config_.performance = config;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setJpegEncodeConfig(
    const ConsumerTypeConfig::JpegEncodeType& config) {
    config_.jpeg_encode = config;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setVideoEncodeConfig(
    const ConsumerTypeConfig::VideoEncodeType& config) {
    config_.video_encode = config;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setOpencvConfig(
    const ConsumerTypeConfig::OpencvType& config) {
    config_.opencv = config;
    return *this;
}

ConsumerTypeConfigBuilder& ConsumerTypeConfigBuilder::setCountConfig(
    const ConsumerTypeConfig::CountType& config) {
    config_.count = config;
    return *this;
}

ConsumerTypeConfig ConsumerTypeConfigBuilder::build() const {
    return config_;
}
