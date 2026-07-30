#ifndef WORKER_CONFIG_BUILDERS_HPP
#define WORKER_CONFIG_BUILDERS_HPP

#include <string>
#include <string_view>
#include <memory>
#include "productionline/worker/config/WorkerConfigs.hpp"
#include "consumptionline/config/ConsumerTypeConfigBuilder.hpp"
#include "vendor/taco/decode/TacoDecoderConfig.hpp"
#include "vendor/taco/encode/TacoEncoderExtension.hpp"

// 前向声明
class IEncodedPacketSource;
class IDecoderVendorExtension;
struct AVCodecParameters;
struct AVRational;

/**
 * @brief 数据源配置构建器
 */
class DataSourceConfigBuilder {
public:
    DataSourceConfigBuilder() = default;

    /// 从现有配置拷贝为起点，再链式 set* 只覆盖需要修改的字段（插件/工厂补丁场景）
    explicit DataSourceConfigBuilder(const WorkerConfig::DataSourceConfig& seed)
        : data_source_config_(seed) {}
    
    /**
     * @brief 设置数据源路径/URL
     * @param path 数据源路径（支持 RTSP、HTTP、本地文件等）
     * 
     * @example
     * - RTSP 流：`rtsp://192.168.1.100/stream`
     * - HTTP/HLS 流：`http://example.com/playlist.m3u8`
     * - 本地文件：`/data/video.mp4`
     */
    DataSourceConfigBuilder& setPath(std::string_view path);
    
    // 兼容 const char*（保持向后兼容）
    DataSourceConfigBuilder& setPath(const char* path);
    
    // 兼容 std::string
    DataSourceConfigBuilder& setPath(const std::string& path);

    /// path 非空时才写入，避免用空串清空已有路径（与插件 applyTo 语义一致）
    DataSourceConfigBuilder& setPathIfNonEmpty(std::string_view path);
    
    /**
     * @brief 设置 BufferPool 的 Buffer 数量
     * @param count Buffer 数量（0=使用 Worker 默认值）
     */
    DataSourceConfigBuilder& setBufferCount(int count);
    
    /// count > 0 时才写入，避免覆盖工厂已设定的默认值（CLI 未指定时 count=0）
    DataSourceConfigBuilder& setBufferCountIfNonZero(int count);
    
    /**
     * @brief 设置最大帧数限制
     * @param max_frames 最大读取帧数（-1=无限制）
     */
    DataSourceConfigBuilder& setMaxFrames(int max_frames);

    /// max_frames==0 时不修改（PP 约定：0 表示 CLI 未指定覆盖）
    DataSourceConfigBuilder& setMaxFramesIfNonZero(int max_frames);
    
    DataSourceConfigBuilder& setBufferMode(bool mode);
    DataSourceConfigBuilder& setCodecParams(const struct AVCodecParameters* params);
    DataSourceConfigBuilder& setTimeBase(AVRational tb);
    DataSourceConfigBuilder& setSharedPacketSource(std::shared_ptr<IEncodedPacketSource> source);
    DataSourceConfigBuilder& setDeferredCommit(bool deferred);
    DataSourceConfigBuilder& setLoop(bool loop);
    /// 裸帧文件循环遍数（<1 时按 1 处理）
    DataSourceConfigBuilder& setLoopCount(int loop_count);
    /// 裸帧（YUV）文件实际宽度（供 FFMPEG_ENCODE / RawFrameSourceFromFile）
    DataSourceConfigBuilder& setRawFrameWidth(int width);
    /// 裸帧（YUV）文件实际高度
    DataSourceConfigBuilder& setRawFrameHeight(int height);
    /// 同时设置裸帧宽高（width/height 均 >0 时写入）
    DataSourceConfigBuilder& setRawFrameSize(int width, int height);
    
    WorkerConfig::DataSourceConfig build() const;
    
private:
    WorkerConfig::DataSourceConfig data_source_config_;
};

/**
 * @brief TACO 解码器特定配置构建器
 * 
 * 提供统一的通用接口来配置 TACO 解码器的两个输出通道。
 * 
 * 设计理念：
 * - 通用接口：所有通道使用相同的配置接口（setCrop/setScale/setOutputFormat）
 * - 类型安全：使用枚举类型代替魔法数字
 * - 无前置检查：可以先配置参数，后启用通道（Builder 模式的正常用法）
 * 
 * 使用示例：
 * @code
 * // 通道0: YUV NV12 输出
 * auto taco = TacoConfigBuilder()
 *     .setChannels(true, false)
 *     .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, TacoColorSpace::BT709_FULL)
 *     .setScale(Channel::CH0, 1920, 1080)
 *     .build();
 * 
 * // 通道1: RGB BGRA888 输出
 * auto taco = TacoConfigBuilder()
 *     .setChannels(false, true)
 *     .setOutputFormat(Channel::CH1, OutputFormat::RGB_BGRA888, TacoColorSpace::BT709_FULL)
 *     .setCrop(Channel::CH1, 0, 0, 1920, 1080)
 *     .setScale(Channel::CH1, 1280, 720)
 *     .build();
 * @endcode
 */
class TacoConfigBuilder {
public:
    TacoConfigBuilder() = default;
    
    // ========================================
    // 解码器行为配置
    // ========================================
    
    TacoConfigBuilder& setChannels(bool ch0, bool ch1);
    
    // ========================================
    // 通用配置接口（支持任意通道）
    // ========================================
    
    TacoConfigBuilder& setOutputFormat(
        Channel ch,
        OutputFormat format = OutputFormat::YUV_AUTO,
        TacoColorSpace std = TacoColorSpace::BT601_FULL
    );
    
    TacoConfigBuilder& setCrop(Channel ch, int x, int y, int width, int height);
    TacoConfigBuilder& setScale(Channel ch, int width, int height);
    
    TacoConfig build() const;
    
    // ========================================
    // 辅助映射函数（向后兼容，供外部使用）
    // ========================================
    
    static OutputFormat mapFormatNameToEnum(std::string_view format_name);
    static TacoColorSpace mapColorStdNameToEnum(std::string_view std_name);
    static std::string_view mapFormatEnumToName(OutputFormat format);
    static std::string_view mapColorStdEnumToName(TacoColorSpace std);

private:
    TacoConfig taco_config_;
    
    static int mapEnumToRgbDriverValue(OutputFormat format);
};

/**
 * @brief 解码器配置构建器
 */
class DecoderConfigBuilder {
public:
    DecoderConfigBuilder() = default;
    
    // ========== 通用解码器参数 ==========
    DecoderConfigBuilder& setDecoderName(std::string_view name);
    DecoderConfigBuilder& setDecoderName(const char* name);
    DecoderConfigBuilder& setHwaccelDevice(std::string_view device);
    DecoderConfigBuilder& setHwaccelDevice(const char* device);
    DecoderConfigBuilder& setDecodeThreads(int threads);
    
    // ========== 快捷预设 ==========
    DecoderConfigBuilder& useVendor(std::string_view codec,
                                     std::unique_ptr<IDecoderVendorExtension> extension);
    DecoderConfigBuilder& useTaco(std::string_view codec, const TacoConfig& taco_config);
    DecoderConfigBuilder& useSoftware();
    
    WorkerConfig::DecoderConfig build() const;
    
private:
    WorkerConfig::DecoderConfig decoder_config_;
};

/**
 * @brief 全局配置构建器（Worker 类型、线程池规模等）
 */
class WorkerGlobalConfigBuilder {
public:
    WorkerGlobalConfigBuilder() = default;

    WorkerGlobalConfigBuilder& setWorkerType(WorkerType type);
    WorkerGlobalConfigBuilder& setThreadPoolSize(int size);

    WorkerConfig::GlobalConfig build() const;

private:
    WorkerConfig::GlobalConfig global_config_;
};

/**
 * @brief Worker 配置构建器（顶层）
 * 
 * 职责：只负责组装 WorkerConfig，不涉及具体配置细节
 */
class WorkerConfigBuilder {
public:
    WorkerConfigBuilder() = default;

    WorkerConfigBuilder& setGlobalConfig(const WorkerConfig::GlobalConfig& global_config);
    WorkerConfigBuilder& setDataSourceConfig(const WorkerConfig::DataSourceConfig& data_source_config);
    WorkerConfigBuilder& setDecoderConfig(const WorkerConfig::DecoderConfig& decoder_config);
    WorkerConfigBuilder& setEncoderConfig(const WorkerConfig::EncoderConfig& encoder_config);
    WorkerConfigBuilder& setConsumerTypeConfig(const ConsumerTypeConfig& consumer_type_config);
    
    WorkerConfig build() const;
    
private:
    WorkerConfig worker_config_;
};

#endif // WORKER_CONFIG_BUILDERS_HPP
