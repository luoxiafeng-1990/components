#ifndef WORKER_CONFIG_HPP
#define WORKER_CONFIG_HPP

#include <string>
#include <string_view>
#include <optional>
#include "common/Logger.hpp"

// FFmpeg 头文件（用于 AVRational 和 AVCodecParameters）
extern "C" {
#include <libavutil/rational.h>
}

/**
 * @brief Worker 类型枚举
 * 
 * 注意：此枚举独立定义，避免与 BufferFillingWorkerFactory 的循环依赖
 */
enum class WorkerType {
    AUTO,                   // 自动检测（默认）
    FFMPEG_RTSP,            // FFmpeg RTSP 流
    FFMPEG_PACKET_RECORDER, // FFmpeg Packet 录制器（支持 RTSP/文件/HTTP 等多种数据源）
    FFMPEG_VIDEO_FILE       // FFmpeg 视频文件
};

/**
 * @brief 通道枚举
 * 
 * TACO 解码器支持两个输出通道：
 * - CH0: YUV 格式输出通道
 * - CH1: RGB/YUV 格式输出通道（支持格式转换）
 */
enum class Channel { 
    CH0 = 0,  // 通道0（仅支持 YUV 格式输出）
    CH1 = 1   // 通道1（支持 RGB 和 YUV 格式输出）
};

/**
 * @brief 输出格式枚举
 * 
 * 包含 TACO 解码器支持的所有输出格式（YUV 和 RGB）
 */
enum class OutputFormat {
    // ========================================
    // YUV 格式（通道0和通道1都支持）
    // ========================================
    YUV_AUTO = -1,        // 自动选择 YUV 格式（由解码器根据输入流决定）
    YUV_NV12 = 0,         // NV12: YUV420 semi-planar, UV interleaved
    YUV_NV21 = 1,         // NV21: YUV420 semi-planar, VU interleaved
    YUV_I420 = 2,         // I420/YUV420P: YUV420 planar
    YUV_YV12 = 3,         // YV12: YUV420 planar, V before U
    YUV_P010 = 4,         // P010: 10-bit YUV420 semi-planar
    YUV_NV16 = 5,         // NV16: YUV422 semi-planar
    YUV_NV61 = 6,         // NV61: YUV422 semi-planar, VU interleaved
    YUV_I422 = 7,         // I422: YUV422 planar
    YUV_NV24 = 8,         // NV24: YUV444 semi-planar
    YUV_I444 = 9,         // I444: YUV444 planar
    
    // ========================================
    // RGB 格式（仅通道1支持）
    // ========================================
    // 注意：枚举值 >= 1000 用于与 YUV 格式区分
    RGB_ARGB888 = 1000,      // ARGB8888 packed (驱动值: 9)
    RGB_ABGR888 = 1001,      // ABGR8888 packed (驱动值: 11)
    RGB_RGBA888 = 1002,      // RGBA8888 packed (驱动值: 13)
    RGB_BGRA888 = 1003,      // BGRA8888 packed (驱动值: 15)
    RGB_RGB888 = 1004,       // RGB888 packed (驱动值: 1)
    RGB_BGR888 = 1005,       // BGR888 packed (驱动值: 3)
    RGB_XRGB888 = 1006,      // XRGB8888 packed (驱动值: 25)
    RGB_XBGR888 = 1007,      // XBGR8888 packed (驱动值: 27)
    RGB_RGBX888 = 1008,      // RGBX8888 packed (驱动值: 21)
    RGB_BGRX888 = 1009,      // BGRX8888 packed (驱动值: 23)
    RGB_RGB888_PLANAR = 1010,   // RGB888 planar (驱动值: 2)
    RGB_BGR888_PLANAR = 1011,   // BGR888 planar (驱动值: 4)
    RGB_R16G16B16 = 1012,    // RGB 16-bit per channel (驱动值: 17)
    RGB_B16G16R16 = 1013,    // BGR 16-bit per channel (驱动值: 19)
    RGB_GBRP = 1014          // GBR planar (驱动值: 28)
};

/**
 * @brief 颜色标准枚举
 * 
 * 定义视频颜色空间标准和范围
 */
enum class ColorStandard {
    NONE = 0,               // 无颜色标准
    BT601 = 1,              // BT.601 full range (SD)
    BT601_LIMITED = 2,      // BT.601 limited range
    BT709 = 3,              // BT.709 full range (HD)
    BT709_LIMITED = 4,      // BT.709 limited range
    BT2020 = 5,             // BT.2020 full range (UHD/HDR)
    BT2020_LIMITED = 6      // BT.2020 limited range
};

/**
 * @brief Worker 配置（完整版）
 * 
 * 设计理念：
 * - 完全独立：包含 Worker 需要的所有配置
 * - 配置分离：数据源、显示设备、解码器配置独立
 * - Builder 构建：链式调用，易用易读
 * - 职责清晰：每个 Builder 只负责自己层级的配置
 * 
 * 配置结构：
 * - DataSourceConfig: 数据源路径和 BufferPool 参数
 * - DisplayConfig: 显示设备分辨率和格式
 * - DecoderConfig: 解码器类型和参数
 * - worker_type: Worker 实现类型
 */
struct WorkerConfig {
    // ========================================
    // 数据源配置
    // ========================================
    /**
     * @brief 数据源配置
     * 
     * 用于配置 Worker 的数据源（RTSP 流、HTTP 流、本地文件等）及其相关参数。
     */
    struct DataSourceConfig {
        std::string path;                     ///< 数据源路径/URL（RTSP/HTTP/文件等）
        int buffer_count = 0;                 ///< BufferPool 的 Buffer 数量（0=使用 Worker 默认值）
        
        DataSourceConfig() = default;
        DataSourceConfig(const DataSourceConfig&) = default;
        DataSourceConfig& operator=(const DataSourceConfig&) = default;
        DataSourceConfig(DataSourceConfig&&) = default;
        DataSourceConfig& operator=(DataSourceConfig&&) = default;
    } data_source;
    
    // ========================================
    // 显示设备配置
    // ========================================
    /**
     * @brief 显示设备配置
     * 
     * 用于配置目标显示设备（如 Framebuffer、显示器）的参数。
     * 
     * ⚠️ 注意：此配置指定的是显示设备分辨率，不是解码器输出分辨率！
     * 解码器输出分辨率请使用 TacoConfig::setDecoderOutputResolution()
     */
    struct DisplayConfig {
        int width = 0;                         ///< 显示设备宽度（像素）
        int height = 0;                        ///< 显示设备高度（像素）
        int bits_per_pixel = 0;                ///< 每像素位数（用于BufferPool内存计算）
        
        DisplayConfig() = default;
    } display;
    
    // ========================================
    // 解码器配置
    // ========================================
    struct DecoderConfig {
        // 通用解码器参数
        std::optional<std::string> name;              // 解码器名称（std::nullopt=自动选择）
        bool enable_hardware = true;                   // 启用硬件加速
        std::optional<std::string> hwaccel_device;     // 硬件设备（如 "cuda:0", "vaapi"）
        int decode_threads = 0;                        // 解码线程数（0=自动）
        
        // ========================================
        // 数据源配置（v2.9新增：支持数据源抽象模式）
        // ========================================
        bool datasource_buffer_mode = false;           // true=从Buffer数据源获取packet, false=从文件数据源读取
        const struct AVCodecParameters* codec_params = nullptr;  // Buffer模式下的编解码器参数（从Record Worker获取）
        AVRational time_base = {0, 1};                 // 时间基准（从Record Worker获取，用于同步）
        
        // ========================================
        // h264_taco 特定配置（子子结构体）
        // ========================================
        struct TacoConfig {
            // ========================================
            // 解码器行为配置
            // ========================================
            bool reorder_disable = true;  // 禁用重排序（推荐保持 true）

            // ========================================
            // 通道0配置（Channel 0 - YUV Output）
            // ========================================
            bool ch0_enable = true;                    // 启用通道0（YUV 格式输出）
            
            // YUV 格式配置
            int ch0_yuv_format = -1;                   // YUV格式类型（-1=自动，0=NV12, 1=NV21, 等）
            int ch0_yuv_std = 1;                       // YUV颜色标准（默认 1=BT.601）
            
            // 裁剪参数（Crop）
            int ch0_crop_x = 0;                        // 裁剪起始X坐标（0=不裁剪）
            int ch0_crop_y = 0;                        // 裁剪起始Y坐标（0=不裁剪）
            int ch0_crop_width = 0;                    // 裁剪宽度（0=不裁剪）
            int ch0_crop_height = 0;                   // 裁剪高度（0=不裁剪）
            
            // 缩放参数（Scale）
            int ch0_scale_width = 0;                   // 缩放目标宽度（0=不缩放）
            int ch0_scale_height = 0;                  // 缩放目标高度（0=不缩放）

            // ========================================
            // 通道1配置（Channel 1 - RGB/YUV Output）
            // ========================================
            bool ch1_enable = false;                   // 启用通道1（默认禁用）
            bool ch1_rgb = false;                      // 是否输出RGB格式（false=YUV）
            
            // RGB 格式配置（仅当 ch1_rgb=true 时有效）
            int ch1_rgb_format = 9;                    // RGB格式类型（默认 9=argb888 packed）
            int ch1_rgb_std = 1;                       // RGB颜色标准（默认 1=BT.601 full range）
            
            // YUV 格式配置（仅当 ch1_rgb=false 时有效）
            int ch1_yuv_format = -1;                   // YUV格式类型（-1=自动）
            int ch1_yuv_std = 1;                       // YUV颜色标准（默认 1=BT.601）
            
            // 裁剪参数（Crop）
            int ch1_crop_x = 0;                        // 裁剪起始X坐标（0=不裁剪）
            int ch1_crop_y = 0;                        // 裁剪起始Y坐标（0=不裁剪）
            int ch1_crop_width = 0;                    // 裁剪宽度（0=不裁剪）
            int ch1_crop_height = 0;                   // 裁剪高度（0=不裁剪）
            
            // 缩放参数（Scale）
            int ch1_scale_width = 0;                   // 缩放目标宽度（0=不缩放）
            int ch1_scale_height = 0;                  // 缩放目标高度（0=不缩放）
            
            TacoConfig() = default;
            TacoConfig(const TacoConfig&) = default;
            TacoConfig& operator=(const TacoConfig&) = default;
            TacoConfig(TacoConfig&&) = default;
            TacoConfig& operator=(TacoConfig&&) = default;
        } taco;
        
        DecoderConfig() = default;
        DecoderConfig(const DecoderConfig&) = default;
        DecoderConfig& operator=(const DecoderConfig&) = default;
        DecoderConfig(DecoderConfig&&) = default;
        DecoderConfig& operator=(DecoderConfig&&) = default;
    } decoder;
    
    // ========================================
    // Worker 类型
    // ========================================
    WorkerType worker_type = WorkerType::AUTO;
    
    WorkerConfig() = default;
    WorkerConfig(const WorkerConfig&) = default;
    WorkerConfig& operator=(const WorkerConfig&) = default;
    WorkerConfig(WorkerConfig&&) = default;
    WorkerConfig& operator=(WorkerConfig&&) = default;
};

// ========================================
// Builder 模式实现
// ========================================

/**
 * @brief 数据源配置构建器
 */
class DataSourceConfigBuilder {
public:
    DataSourceConfigBuilder() = default;
    
    /**
     * @brief 设置数据源路径/URL
     * @param path 数据源路径（支持 RTSP、HTTP、本地文件等）
     * 
     * @example
     * - RTSP 流：`rtsp://192.168.1.100/stream`
     * - HTTP/HLS 流：`http://example.com/playlist.m3u8`
     * - 本地文件：`/data/video.mp4`
     */
    DataSourceConfigBuilder& setPath(std::string_view path) {
        data_source_config_.path = std::string(path);
        return *this;
    }
    
    // 兼容 const char*（保持向后兼容）
    DataSourceConfigBuilder& setPath(const char* path) {
        if (path) {
            data_source_config_.path = path;
        } else {
            data_source_config_.path.clear();
        }
        return *this;
    }
    
    // 兼容 std::string
    DataSourceConfigBuilder& setPath(const std::string& path) {
        data_source_config_.path = path;
        return *this;
    }
    
    /**
     * @brief 设置 BufferPool 的 Buffer 数量
     * @param count Buffer 数量（0=使用 Worker 默认值）
     * 
     * 建议值：
     * - RTSP 流解码：4-8
     * - 本地文件解码：128
     * - Packet 录制：64
     */
    DataSourceConfigBuilder& setBufferCount(int count) {
        data_source_config_.buffer_count = count;
        return *this;
    }
    
    WorkerConfig::DataSourceConfig build() const {
        return data_source_config_;
    }
    
private:
    WorkerConfig::DataSourceConfig data_source_config_;
};

/**
 * @brief 显示设备配置构建器
 */
class DisplayConfigBuilder {
public:
    DisplayConfigBuilder() = default;
    
    /**
     * @brief 设置显示设备宽度
     * @param width 显示设备宽度（像素）
     */
    DisplayConfigBuilder& setDisplayWidth(int width) {
        display_config_.width = width;
        return *this;
    }
    
    /**
     * @brief 设置显示设备高度
     * @param height 显示设备高度（像素）
     */
    DisplayConfigBuilder& setDisplayHeight(int height) {
        display_config_.height = height;
        return *this;
    }
    
    /**
     * @brief 设置显示设备分辨率
     * 
     * @param width  显示设备宽度（像素）
     * @param height 显示设备高度（像素）
     * @return DisplayConfigBuilder& 链式调用
     * 
     * @example
     * ```cpp
     * DisplayConfigBuilder()
     *     .setDisplayResolution(1920, 1080)  // Framebuffer 分辨率
     *     .setBitsPerPixel(32)
     *     .build()
     * ```
     */
    DisplayConfigBuilder& setDisplayResolution(int width, int height) {
        display_config_.width = width;
        display_config_.height = height;
        return *this;
    }
    
    /**
     * @brief 设置每像素位数
     * @param bpp 每像素位数（如 32 表示 ARGB8888）
     */
    DisplayConfigBuilder& setBitsPerPixel(int bpp) {
        display_config_.bits_per_pixel = bpp;
        return *this;
    }
    
    WorkerConfig::DisplayConfig build() const {
        return display_config_;
    }
    
private:
    WorkerConfig::DisplayConfig display_config_;
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
 *     .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT709)
 *     .setScale(Channel::CH0, 1920, 1080)
 *     .build();
 * 
 * // 通道1: RGB BGRA888 输出
 * auto taco = TacoConfigBuilder()
 *     .setChannels(false, true)
 *     .setOutputFormat(Channel::CH1, OutputFormat::RGB_BGRA888, ColorStandard::BT709)
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
    
    /**
     * @brief 设置是否禁用重排序
     * @param disable true=禁用重排序（推荐），false=启用重排序
     */
    TacoConfigBuilder& setReorderDisable(bool disable = true) {
        taco_config_.reorder_disable = disable;
        return *this;
    }
    
    /**
     * @brief 同时设置两个通道的启用状态（快捷方法）
     * @param ch0 是否启用通道0
     * @param ch1 是否启用通道1
     */
    TacoConfigBuilder& setChannels(bool ch0, bool ch1) {
        taco_config_.ch0_enable = ch0;
        taco_config_.ch1_enable = ch1;
        return *this;
    }
    
    // ========================================
    // 通用配置接口（支持任意通道）
    // ========================================
    
    /**
     * @brief 设置通道输出格式（通用接口）
     * 
     * @param ch 通道选择（CH0 或 CH1）
     * @param format 输出格式
     *               - YUV_*: YUV 格式（两个通道都支持）
     *               - RGB_*: RGB 格式（仅 CH1 支持）
     * @param std 颜色标准（默认 BT601）
     * 
     * @note 通道0仅支持 YUV 格式，传入 RGB 格式会记录错误并忽略
     * @note 通道1支持 RGB 和 YUV 格式
     * @note 自动根据 format 判断是 RGB 还是 YUV 并设置对应参数
     * 
     * @example
     * // 通道0设置 YUV NV12
     * .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT601)
     * 
     * // 通道1设置 RGB ARGB888
     * .setOutputFormat(Channel::CH1, OutputFormat::RGB_ARGB888, ColorStandard::BT709)
     * 
     * // 通道1设置 YUV P010 (10-bit)
     * .setOutputFormat(Channel::CH1, OutputFormat::YUV_P010, ColorStandard::BT2020)
     */
    TacoConfigBuilder& setOutputFormat(
        Channel ch,
        OutputFormat format = OutputFormat::YUV_AUTO,
        ColorStandard std = ColorStandard::BT601
    ) {
        int format_value = static_cast<int>(format);
        int std_value = static_cast<int>(std);
        
        // 判断是 RGB 还是 YUV（RGB 格式枚举值 >= 1000）
        bool is_rgb = (format_value >= 1000);
        
        if (ch == Channel::CH0) {
            // 通道0仅支持 YUV
            if (is_rgb) {
                LOG_ERROR_FMT("TacoConfigBuilder: Channel 0 only supports YUV format, RGB format ignored");
                return *this;
            }
            // 设置 YUV 格式
            taco_config_.ch0_yuv_format = format_value;
            taco_config_.ch0_yuv_std = std_value;
            
        } else if (ch == Channel::CH1) {
            // 通道1支持 RGB 和 YUV
            taco_config_.ch1_rgb = is_rgb;
            
            if (is_rgb) {
                // RGB 格式：需要映射回驱动的原始值
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
    
    /**
     * @brief 设置通道裁剪区域（通用接口）
     * 
     * @param ch 通道选择（CH0 或 CH1）
     * @param x 裁剪起始 X 坐标（0=不裁剪）
     * @param y 裁剪起始 Y 坐标（0=不裁剪）
     * @param width 裁剪宽度（0=不裁剪）
     * @param height 裁剪高度（0=不裁剪）
     * 
     * @note 无需预先启用通道，可以先配置后启用
     * 
     * @example
     * // 通道0裁剪
     * .setCrop(Channel::CH0, 100, 100, 1920, 1080)
     * 
     * // 通道1裁剪
     * .setCrop(Channel::CH1, 0, 0, 1280, 720)
     */
    TacoConfigBuilder& setCrop(Channel ch, int x, int y, int width, int height) {
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
    
    /**
     * @brief 设置通道缩放分辨率（通用接口）
     * 
     * @param ch 通道选择（CH0 或 CH1）
     * @param width 缩放目标宽度（0=不缩放）
     * @param height 缩放目标高度（0=不缩放）
     * 
     * @note 无需预先启用通道，可以先配置后启用
     * 
     * @example
     * // 通道0缩放到 1920x1080
     * .setScale(Channel::CH0, 1920, 1080)
     * 
     * // 通道1缩放到 1280x720
     * .setScale(Channel::CH1, 1280, 720)
     */
    TacoConfigBuilder& setScale(Channel ch, int width, int height) {
        if (ch == Channel::CH0) {
            taco_config_.ch0_scale_width = width;
            taco_config_.ch0_scale_height = height;
        } else if (ch == Channel::CH1) {
            taco_config_.ch1_scale_width = width;
            taco_config_.ch1_scale_height = height;
        }
        return *this;
    }
    
    /**
     * @brief 构建最终的 TacoConfig 对象
     */
    WorkerConfig::DecoderConfig::TacoConfig build() const {
        return taco_config_;
    }
    
    // ========================================
    // 辅助映射函数（向后兼容，供外部使用）
    // ========================================
    
    /**
     * @brief 将格式名称字符串映射为 OutputFormat 枚举
     * @param format_name 格式名称（如 "nv12", "argb888" 等）
     * @return OutputFormat 枚举值
     * 
     * 向后兼容旧代码使用字符串配置的情况。
     */
    static OutputFormat mapFormatNameToEnum(std::string_view format_name) {
        // YUV 格式
        if (format_name == "auto" || format_name == "yuv_auto") return OutputFormat::YUV_AUTO;
        if (format_name == "nv12") return OutputFormat::YUV_NV12;
        if (format_name == "nv21") return OutputFormat::YUV_NV21;
        if (format_name == "i420" || format_name == "yuv420p") return OutputFormat::YUV_I420;
        if (format_name == "yv12") return OutputFormat::YUV_YV12;
        if (format_name == "p010") return OutputFormat::YUV_P010;
        if (format_name == "nv16") return OutputFormat::YUV_NV16;
        if (format_name == "nv61") return OutputFormat::YUV_NV61;
        if (format_name == "i422" || format_name == "yuv422p") return OutputFormat::YUV_I422;
        if (format_name == "nv24") return OutputFormat::YUV_NV24;
        if (format_name == "i444" || format_name == "yuv444p") return OutputFormat::YUV_I444;
        
        // RGB 格式
        if (format_name == "argb888") return OutputFormat::RGB_ARGB888;
        if (format_name == "abgr888") return OutputFormat::RGB_ABGR888;
        if (format_name == "rgba888") return OutputFormat::RGB_RGBA888;
        if (format_name == "bgra888") return OutputFormat::RGB_BGRA888;
        if (format_name == "rgb888") return OutputFormat::RGB_RGB888;
        if (format_name == "bgr888") return OutputFormat::RGB_BGR888;
        if (format_name == "xrgb888") return OutputFormat::RGB_XRGB888;
        if (format_name == "xbgr888") return OutputFormat::RGB_XBGR888;
        if (format_name == "rgbx888") return OutputFormat::RGB_RGBX888;
        if (format_name == "bgrx888") return OutputFormat::RGB_BGRX888;
        if (format_name == "rgb888_planar") return OutputFormat::RGB_RGB888_PLANAR;
        if (format_name == "bgr888_planar") return OutputFormat::RGB_BGR888_PLANAR;
        if (format_name == "r16g16b16") return OutputFormat::RGB_R16G16B16;
        if (format_name == "b16g16r16") return OutputFormat::RGB_B16G16R16;
        if (format_name == "gbrp") return OutputFormat::RGB_GBRP;
        
        // 默认返回 YUV_AUTO
        return OutputFormat::YUV_AUTO;
    }
    
    /**
     * @brief 将颜色标准名称字符串映射为 ColorStandard 枚举
     * @param std_name 颜色标准名称（如 "bt601", "bt709" 等）
     * @return ColorStandard 枚举值
     * 
     * 向后兼容旧代码使用字符串配置的情况。
     */
    static ColorStandard mapColorStdNameToEnum(std::string_view std_name) {
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
    
    /**
     * @brief 将 OutputFormat 枚举映射为格式名称字符串
     * @param format OutputFormat 枚举值
     * @return 格式名称字符串
     */
    static std::string_view mapFormatEnumToName(OutputFormat format) {
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
            
            default: return "unknown";
        }
    }
    
    /**
     * @brief 将 ColorStandard 枚举映射为颜色标准名称字符串
     * @param std ColorStandard 枚举值
     * @return 颜色标准名称字符串
     */
    static std::string_view mapColorStdEnumToName(ColorStandard std) {
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

private:
    WorkerConfig::DecoderConfig::TacoConfig taco_config_;
    
    /**
     * @brief 将 OutputFormat 枚举值映射回 TACO 驱动的原始 RGB 格式值
     * 
     * OutputFormat 枚举使用 1000+ 的值来区分 RGB 和 YUV，
     * 但 TACO 驱动需要原始的格式值（如 9 表示 ARGB888）。
     */
    static int mapEnumToRgbDriverValue(OutputFormat format) {
        switch (format) {
            case OutputFormat::RGB_ARGB888: return 9;
            case OutputFormat::RGB_ABGR888: return 11;
            case OutputFormat::RGB_RGBA888: return 13;
            case OutputFormat::RGB_BGRA888: return 15;
            case OutputFormat::RGB_RGB888: return 1;
            case OutputFormat::RGB_BGR888: return 3;
            case OutputFormat::RGB_XRGB888: return 25;
            case OutputFormat::RGB_XBGR888: return 27;
            case OutputFormat::RGB_RGBX888: return 21;
            case OutputFormat::RGB_BGRX888: return 23;
            case OutputFormat::RGB_RGB888_PLANAR: return 2;
            case OutputFormat::RGB_BGR888_PLANAR: return 4;
            case OutputFormat::RGB_R16G16B16: return 17;
            case OutputFormat::RGB_B16G16R16: return 19;
            case OutputFormat::RGB_GBRP: return 28;
            default: return 9; // 默认 ARGB888
        }
    }
};

/**
 * @brief 解码器配置构建器
 */
class DecoderConfigBuilder {
public:
    DecoderConfigBuilder() = default;
    
    // ========== 通用解码器参数 ==========
    
    // 接受 std::string_view（推荐）
    DecoderConfigBuilder& setDecoderName(std::string_view name) {
        decoder_config_.name = std::string(name);
        return *this;
    }
    
    // 兼容 const char*（保持向后兼容）
    DecoderConfigBuilder& setDecoderName(const char* name) {
        if (name) {
            decoder_config_.name = name;
        } else {
            decoder_config_.name = std::nullopt;
        }
        return *this;
    }
    
    // 清除解码器名称（使用自动选择）
    DecoderConfigBuilder& clearDecoderName() {
        decoder_config_.name = std::nullopt;
        return *this;
    }
    
    // 接受 std::string_view（推荐）
    DecoderConfigBuilder& setHwaccelDevice(std::string_view device) {
        decoder_config_.hwaccel_device = std::string(device);
        return *this;
    }
    
    // 兼容 const char*（保持向后兼容）
    DecoderConfigBuilder& setHwaccelDevice(const char* device) {
        if (device) {
            decoder_config_.hwaccel_device = device;
        } else {
            decoder_config_.hwaccel_device = std::nullopt;
        }
        return *this;
    }
    
    DecoderConfigBuilder& setDecodeThreads(int threads) {
        decoder_config_.decode_threads = threads;
        return *this;
    }
    
    // ========== 快捷预设 ==========
    /**
     * @brief 预设：TACO 硬件解码（通用，支持自定义配置）
     * 
     * 设置解码器为 TACO 平台的指定编解码器，并使用自定义 TACO 配置。
     * 
     * @param codec 编解码器类型（如 "h264"、"h265"、"vp9" 等）
     * @param taco_config 自定义的 TACO 配置对象
     * 
     * 示例：
     * @code
     * auto tacoConfig = TacoConfigBuilder()
     *     .setRgbConfig(true, "bgra888", "bt709")
     *     .setDecoderOutputResolution(1920, 1080)
     *     .build();
     * 
     * DecoderConfigBuilder().useTaco("h264", tacoConfig).build()
     * @endcode
     */
    DecoderConfigBuilder& useTaco(
        std::string_view codec,
        const WorkerConfig::DecoderConfig::TacoConfig& taco_config
    ) {
        // 拼接解码器名称：codec + "_taco"
        decoder_config_.name = std::string(codec) + "_taco";
        decoder_config_.enable_hardware = true;
        decoder_config_.taco = taco_config;
        return *this;
    }
    
    /**
     * @brief 预设：软件解码（自动选择）
     */
    DecoderConfigBuilder& useSoftware() {
        decoder_config_.name = std::nullopt;
        decoder_config_.enable_hardware = false;
        return *this;
    }
    
    /**
     * @brief 预设：NVIDIA CUDA 硬件解码（通用，支持多种编解码器）
     * 
     * 设置解码器为 NVIDIA CUDA 平台的指定编解码器。
     * CUDA 是 NVIDIA GPU 硬件加速平台，支持 H.264、H.265、VP9、AV1 等。
     * 
     * @param codec 编解码器类型（如 "h264"、"h265"、"vp9"、"av1" 等）
     * 
     * 示例：
     * @code
     * // H.264 CUDA 解码
     * DecoderConfigBuilder().useCuvid("h264").build()
     * 
     * // H.265/HEVC CUDA 解码
     * DecoderConfigBuilder().useCuvid("h265").build()
     * 
     * // VP9 CUDA 解码
     * DecoderConfigBuilder().useCuvid("vp9").build()
     * 
     * // AV1 CUDA 解码
     * DecoderConfigBuilder().useCuvid("av1").build()
     * @endcode
     * 
     * 生成的解码器名称格式为：{codec}_cuvid（如 "h264_cuvid"、"h265_cuvid"）
     */
    DecoderConfigBuilder& useCuvid(std::string_view codec) {
        decoder_config_.name = std::string(codec) + "_cuvid";
        decoder_config_.enable_hardware = true;
        decoder_config_.hwaccel_device = "cuda";
        return *this;
    }
    
    /**
     * @brief 预设：Intel Quick Sync Video 硬件解码（通用，支持多种编解码器）
     * 
     * 设置解码器为 Intel QSV 平台的指定编解码器。
     * QSV 是 Intel 集成显卡硬件加速平台，支持 H.264、H.265、VP9、AV1 等。
     * 
     * @param codec 编解码器类型（如 "h264"、"h265"、"vp9"、"av1" 等）
     * 
     * 示例：
     * @code
     * // H.264 QSV 解码
     * DecoderConfigBuilder().useQsv("h264").build()
     * 
     * // H.265/HEVC QSV 解码
     * DecoderConfigBuilder().useQsv("h265").build()
     * 
     * // VP9 QSV 解码
     * DecoderConfigBuilder().useQsv("vp9").build()
     * 
     * // AV1 QSV 解码
     * DecoderConfigBuilder().useQsv("av1").build()
     * @endcode
     * 
     * 生成的解码器名称格式为：{codec}_qsv（如 "h264_qsv"、"h265_qsv"）
     */
    DecoderConfigBuilder& useQsv(std::string_view codec) {
        decoder_config_.name = std::string(codec) + "_qsv";
        decoder_config_.enable_hardware = true;
        decoder_config_.hwaccel_device = "qsv";
        return *this;
    }
    
    /**
     * @brief 预设：VA-API 硬件解码（通用，支持多种编解码器）
     * 
     * 设置解码器为 VA-API 平台的指定编解码器。
     * VA-API 是 Linux 视频加速 API，支持 Intel/AMD GPU 硬件加速。
     * 
     * @param codec 编解码器类型（如 "h264"、"h265"、"vp9"、"av1" 等）
     * 
     * 示例：
     * @code
     * // H.264 VA-API 解码
     * DecoderConfigBuilder().useVaapi("h264").build()
     * 
     * // H.265/HEVC VA-API 解码
     * DecoderConfigBuilder().useVaapi("h265").build()
     * 
     * // VP9 VA-API 解码
     * DecoderConfigBuilder().useVaapi("vp9").build()
     * @endcode
     * 
     * 生成的解码器名称格式为：{codec}_vaapi（如 "h264_vaapi"、"h265_vaapi"）
     */
    DecoderConfigBuilder& useVaapi(std::string_view codec) {
        decoder_config_.name = std::string(codec) + "_vaapi";
        decoder_config_.enable_hardware = true;
        decoder_config_.hwaccel_device = "vaapi";
        return *this;
    }
    
    WorkerConfig::DecoderConfig build() const {
        return decoder_config_;
    }
    
private:
    WorkerConfig::DecoderConfig decoder_config_;
};

/**
 * @brief Worker 配置构建器（顶层）
 * 
 * 职责：只负责组装 WorkerConfig，不涉及具体配置细节
 */
class WorkerConfigBuilder {
public:
    WorkerConfigBuilder() = default;
    
    /**
     * @brief 设置数据源配置
     */
    WorkerConfigBuilder& setDataSourceConfig(const WorkerConfig::DataSourceConfig& data_source_config) {
        worker_config_.data_source = data_source_config;
        return *this;
    }
    
    /**
     * @brief 设置显示设备配置
     * @param display_config 显示设备配置
     */
    WorkerConfigBuilder& setDisplayConfig(const WorkerConfig::DisplayConfig& display_config) {
        worker_config_.display = display_config;
        return *this;
    }
    
    /**
     * @brief 设置解码器配置
     */
    WorkerConfigBuilder& setDecoderConfig(const WorkerConfig::DecoderConfig& decoder_config) {
        worker_config_.decoder = decoder_config;
        return *this;
    }
    
    /**
     * @brief 设置 Worker 类型
     */
    WorkerConfigBuilder& setWorkerType(WorkerType type) {
        worker_config_.worker_type = type;
        return *this;
    }
    
    /**
     * @brief 构建最终配置
     */
    WorkerConfig build() const {
        return worker_config_;
    }
    
private:
    WorkerConfig worker_config_;
};

#endif // WORKER_CONFIG_HPP



