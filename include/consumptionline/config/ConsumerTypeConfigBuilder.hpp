#ifndef CONSUMPTIONLINE_CONFIG_CONSUMER_TYPE_CONFIG_BUILDER_HPP
#define CONSUMPTIONLINE_CONFIG_CONSUMER_TYPE_CONFIG_BUILDER_HPP

#include <memory>
#include <string>
#include <vector>
#include "consumptionline/config/ConsumerTypeConfig.hpp"

class DisplayConsumerConfigBuilder {
public:
    DisplayConsumerConfigBuilder() = default;
    explicit DisplayConsumerConfigBuilder(const ConsumerTypeConfig::DisplayConsumerConfig& seed)
        : config_(seed) {}

    DisplayConsumerConfigBuilder& setEnable(bool enable);
    DisplayConsumerConfigBuilder& setDeviceId(int id);
    DisplayConsumerConfigBuilder& setVendor(std::unique_ptr<IDisplayVendorExtension> vendor);

    ConsumerTypeConfig::DisplayConsumerConfig build() const;

private:
    ConsumerTypeConfig::DisplayConsumerConfig config_;
};

class SaveRawConfigBuilder {
public:
    SaveRawConfigBuilder() = default;
    explicit SaveRawConfigBuilder(const ConsumerTypeConfig::SaveRawType& seed)
        : config_(seed) {}

    SaveRawConfigBuilder& setEnable(bool enable);
    SaveRawConfigBuilder& setOutputPaths(const std::vector<std::string>& paths);
    SaveRawConfigBuilder& setMaxFramesPerChannel(const std::vector<int>& frames);

    ConsumerTypeConfig::SaveRawType build() const;

private:
    ConsumerTypeConfig::SaveRawType config_;
};

class SaveEncodedConfigBuilder {
public:
    SaveEncodedConfigBuilder() = default;
    explicit SaveEncodedConfigBuilder(const ConsumerTypeConfig::SaveEncodedType& seed)
        : config_(seed) {}

    SaveEncodedConfigBuilder& setEnable(bool enable);
    SaveEncodedConfigBuilder& setOutputPath(const std::string& path);

    ConsumerTypeConfig::SaveEncodedType build() const;

private:
    ConsumerTypeConfig::SaveEncodedType config_;
};

class NpuInferenceConfigBuilder {
public:
    NpuInferenceConfigBuilder() = default;
    explicit NpuInferenceConfigBuilder(const ConsumerTypeConfig::NpuInferenceType& seed)
        : config_(seed) {}

    NpuInferenceConfigBuilder& setEnable(bool enable);
    NpuInferenceConfigBuilder& setModelPath(const std::string& path);
    NpuInferenceConfigBuilder& setAlgorithm(consumer::NpuAlgorithm algorithm);
    NpuInferenceConfigBuilder& setConfThreshold(float threshold);
    NpuInferenceConfigBuilder& setNmsThreshold(float threshold);
    NpuInferenceConfigBuilder& setNpuCoreIndex(int index);
    NpuInferenceConfigBuilder& setUsePhysicalAddr(bool use);
    NpuInferenceConfigBuilder& setEnableDraw(bool enable);
    NpuInferenceConfigBuilder& setInferenceInterval(int interval);
    NpuInferenceConfigBuilder& setVendor(std::unique_ptr<INpuInferenceVendorExtension> vendor);

    ConsumerTypeConfig::NpuInferenceType build() const;

private:
    ConsumerTypeConfig::NpuInferenceType config_;
};

class CompareConfigBuilder {
public:
    CompareConfigBuilder() = default;
    explicit CompareConfigBuilder(const ConsumerTypeConfig::CompareType& seed)
        : config_(seed) {}

    CompareConfigBuilder& setEnablePsnr(bool enable);
    CompareConfigBuilder& setEnableSsim(bool enable);
    CompareConfigBuilder& setMinPsnr(double psnr);
    CompareConfigBuilder& setMinSsim(double ssim);
    CompareConfigBuilder& setVerbose(bool verbose);
    CompareConfigBuilder& setEnableChannelCompare(bool enable);
    CompareConfigBuilder& setReferenceChannel(int ch);
    CompareConfigBuilder& setCompareChannel(int ch);

    ConsumerTypeConfig::CompareType build() const;

private:
    ConsumerTypeConfig::CompareType config_;
};

class PerformanceConfigBuilder {
public:
    PerformanceConfigBuilder() = default;
    explicit PerformanceConfigBuilder(const ConsumerTypeConfig::PerformanceType& seed)
        : config_(seed) {}

    PerformanceConfigBuilder& setEnable(bool enable);
    PerformanceConfigBuilder& setTargetFps(double fps);

    ConsumerTypeConfig::PerformanceType build() const;

private:
    ConsumerTypeConfig::PerformanceType config_;
};

class JpegEncodeConfigBuilder {
public:
    JpegEncodeConfigBuilder() = default;
    explicit JpegEncodeConfigBuilder(const ConsumerTypeConfig::JpegEncodeType& seed)
        : config_(seed) {}

    JpegEncodeConfigBuilder& setEnable(bool enable);
    JpegEncodeConfigBuilder& setOutputPipe(const std::string& pipe);
    JpegEncodeConfigBuilder& setQuality(int quality);
    JpegEncodeConfigBuilder& setTargetFps(int fps);
    JpegEncodeConfigBuilder& setEncoderName(const std::string& name);

    ConsumerTypeConfig::JpegEncodeType build() const;

private:
    ConsumerTypeConfig::JpegEncodeType config_;
};

class OpencvConfigBuilder {
public:
    OpencvConfigBuilder() = default;
    explicit OpencvConfigBuilder(const ConsumerTypeConfig::OpencvType& seed)
        : config_(seed) {}

    OpencvConfigBuilder& setEnable(bool enable);
    OpencvConfigBuilder& setOpType(ConsumerTypeConfig::OpencvType::OpType type);
    OpencvConfigBuilder& setResize(const ConsumerTypeConfig::OpencvType::Resize& r);
    OpencvConfigBuilder& setCrop(const ConsumerTypeConfig::OpencvType::Crop& c);
    OpencvConfigBuilder& setMorph(const ConsumerTypeConfig::OpencvType::Morph& m);
    OpencvConfigBuilder& setSobel(const ConsumerTypeConfig::OpencvType::Sobel& s);
    OpencvConfigBuilder& setCanny(const ConsumerTypeConfig::OpencvType::Canny& c);
    OpencvConfigBuilder& setLaplacian(const ConsumerTypeConfig::OpencvType::Laplacian& l);
    OpencvConfigBuilder& setTranslate(const ConsumerTypeConfig::OpencvType::Translate& t);
    OpencvConfigBuilder& setRotate(const ConsumerTypeConfig::OpencvType::Rotate& r);
    OpencvConfigBuilder& setPerspective(const ConsumerTypeConfig::OpencvType::Perspective& p);
    OpencvConfigBuilder& setDrawLine(const ConsumerTypeConfig::OpencvType::DrawLine& d);
    OpencvConfigBuilder& setDrawRect(const ConsumerTypeConfig::OpencvType::DrawRect& d);
    OpencvConfigBuilder& setPutText(const ConsumerTypeConfig::OpencvType::PutText& p);
    OpencvConfigBuilder& setGaussianBlur(const ConsumerTypeConfig::OpencvType::GaussianBlur& g);
    OpencvConfigBuilder& setThreshold(const ConsumerTypeConfig::OpencvType::Threshold& t);
    OpencvConfigBuilder& setSplitMerge(const ConsumerTypeConfig::OpencvType::SplitMerge& s);
    OpencvConfigBuilder& setCvtColor(const ConsumerTypeConfig::OpencvType::ColorConvert& c);
    OpencvConfigBuilder& setVendor(std::unique_ptr<IOpencvVendorExtension> vendor);

    ConsumerTypeConfig::OpencvType build() const;

private:
    ConsumerTypeConfig::OpencvType config_;
};

class ConsumerTypeConfigBuilder {
public:
    ConsumerTypeConfigBuilder() = default;
    explicit ConsumerTypeConfigBuilder(const ConsumerTypeConfig& seed)
        : config_(seed) {}

    ConsumerTypeConfigBuilder& setMaxFrames(int frames);
    ConsumerTypeConfigBuilder& setMaxDurationSeconds(double seconds);
    ConsumerTypeConfigBuilder& setTimeoutMs(int ms);
    ConsumerTypeConfigBuilder& setMaxTimeoutCount(int count);
    ConsumerTypeConfigBuilder& setVerbose(bool verbose);

    ConsumerTypeConfigBuilder& setDisplayConfig(const ConsumerTypeConfig::DisplayConsumerConfig& config);
    ConsumerTypeConfigBuilder& setSaveRawConfig(const ConsumerTypeConfig::SaveRawType& config);
    ConsumerTypeConfigBuilder& setSaveEncodedConfig(const ConsumerTypeConfig::SaveEncodedType& config);
    ConsumerTypeConfigBuilder& setNpuInferenceConfig(const ConsumerTypeConfig::NpuInferenceType& config);
    ConsumerTypeConfigBuilder& setCompareConfig(const ConsumerTypeConfig::CompareType& config);
    ConsumerTypeConfigBuilder& setPerformanceConfig(const ConsumerTypeConfig::PerformanceType& config);
    ConsumerTypeConfigBuilder& setJpegEncodeConfig(const ConsumerTypeConfig::JpegEncodeType& config);
    ConsumerTypeConfigBuilder& setOpencvConfig(const ConsumerTypeConfig::OpencvType& config);
    ConsumerTypeConfigBuilder& setCountConfig(const ConsumerTypeConfig::CountType& config);

    ConsumerTypeConfig build() const;

private:
    ConsumerTypeConfig config_;
};

#endif // CONSUMPTIONLINE_CONFIG_CONSUMER_TYPE_CONFIG_BUILDER_HPP
