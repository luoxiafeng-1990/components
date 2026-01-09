#ifndef WORKER_CONFIG_HPP
#define WORKER_CONFIG_HPP

#include <string>
#include <string_view>
#include <optional>
#include "common/Logger.hpp"

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
            
            // YUV 格式由解码器自动决定（NV12/NV21/P010等），无需手动配置
            // 输出格式取决于输入流的编码格式和位深度
            
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
                                                       // 常用值：
                                                       // 9  = argb888 (ARGB8888 packed)
                                                       // 11 = abgr888 (ABGR8888 packed)
                                                       // 13 = rgba888 (RGBA8888 packed)
                                                       // 15 = bgra888 (BGRA8888 packed)
                                                       // 1  = rgb888  (RGB888 packed)
                                                       // 3  = bgr888  (BGR888 packed)
                                                       // 完整列表见 TACO 解码器文档
            
            int ch1_rgb_std = 1;                       // RGB颜色标准（默认 1=BT.601 full range）
                                                       // 0 = none (无标准)
                                                       // 1 = bt601    (BT.601 full range)
                                                       // 2 = bt601_l  (BT.601 limited range)
                                                       // 3 = bt709    (BT.709 full range)
                                                       // 4 = bt709_l  (BT.709 limited range)
                                                       // 5 = bt2020   (BT.2020 full range)
                                                       // 6 = bt2020_l (BT.2020 limited range)
            
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
 * @brief h264_taco 特定配置构建器
 */
class TacoConfigBuilder {
public:
    TacoConfigBuilder() = default;
    
    TacoConfigBuilder& setReorderDisable(bool disable = true) {
        taco_config_.reorder_disable = disable;
        return *this;
    }
    
    TacoConfigBuilder& setChannels(bool ch0, bool ch1) {
        taco_config_.ch0_enable = ch0;
        taco_config_.ch1_enable = ch1;
        return *this;
    }
    
    // ========== 通道0配置 ==========
    
    /**
     * @brief 设置通道0裁剪区域
     */
    TacoConfigBuilder& setCh0CropRegion(int x, int y, int width, int height) {
        taco_config_.ch0_crop_x = x;
        taco_config_.ch0_crop_y = y;
        taco_config_.ch0_crop_width = width;
        taco_config_.ch0_crop_height = height;
        return *this;
    }
    
    /**
     * @brief 设置通道0缩放分辨率
     */
    TacoConfigBuilder& setCh0ScaleResolution(int width, int height) {
        taco_config_.ch0_scale_width = width;
        taco_config_.ch0_scale_height = height;
        return *this;
    }
    
    // ========== 通道1配置 ==========
    
    /**
     * @brief 设置通道1 RGB配置
     * @param enable 是否输出RGB
     * @param format RGB格式类型（0-28），默认 9=argb888
     * @param std 颜色标准（0-6），默认 1=bt601
     */
    TacoConfigBuilder& setCh1RgbConfig(bool enable, int format = 9, int std = 1) {
        taco_config_.ch1_rgb = enable;
        taco_config_.ch1_rgb_format = format;
        taco_config_.ch1_rgb_std = std;
        return *this;
    }
    
    /**
     * @brief 设置通道1裁剪区域
     */
    TacoConfigBuilder& setCh1CropRegion(int x, int y, int width, int height) {
        if (taco_config_.ch1_enable) {
            taco_config_.ch1_crop_x = x;
            taco_config_.ch1_crop_y = y;
            taco_config_.ch1_crop_width = width;
            taco_config_.ch1_crop_height = height;
            return *this;
        }
        LOG_ERROR_FMT("TacoConfigBuilder::setCh1CropRegion() failed, ch1 is not enabled");
        return *this;
    }
    
    /**
     * @brief 设置通道1缩放分辨率（解码器输出分辨率）
     */
    TacoConfigBuilder& setCh1ScaleResolution(int width, int height) {
        if (taco_config_.ch1_enable) {
            taco_config_.ch1_scale_width = width;
            taco_config_.ch1_scale_height = height;
            return *this;
        }
        LOG_ERROR_FMT("TacoConfigBuilder::setCh1ScaleResolution() failed, ch1 is not enabled");
        return *this;
    }
    
    // ========== 向后兼容的快捷方法（保留旧接口） ==========
    
    /**
     * @brief 设置RGB配置（简化版，向后兼容）
     * @deprecated 请使用 setCh1RgbConfig()
     */
    TacoConfigBuilder& setRgbConfig(bool enable, std::string_view format = "argb888", std::string_view std_name = "bt601") {
        taco_config_.ch1_rgb = enable;
        taco_config_.ch1_rgb_format = mapRgbFormatNameToInt(format);
        taco_config_.ch1_rgb_std = mapRgbStdNameToInt(std_name);
        return *this;
    }
    
    /**
     * @brief 设置YUV配置（简化版，向后兼容）
     * @note YUV格式由解码器自动决定，此方法仅为兼容性保留
     * @deprecated YUV格式无需手动配置
     */
    TacoConfigBuilder& setYuvConfig(
        std::string_view format = "YUV420 8-bit NV12", 
        std::string_view std = "bt601"
    ) {
        // YUV 格式由解码器自动决定，不需要设置
        // 保留此方法仅为向后兼容
        return *this;
    }
    
    /**
     * @brief 设置裁剪区域（简化版，向后兼容，作用于ch1）
     * @deprecated 请使用 setCh1CropRegion()
     */
    TacoConfigBuilder& setCropRegion(int x, int y, int width, int height) {
        return setCh1CropRegion(x, y, width, height);
    }
    
    /**
     * @brief 设置解码器输出分辨率（简化版，向后兼容，作用于ch1）
     * @deprecated 请使用 setCh1ScaleResolution()
     */
    TacoConfigBuilder& setDecoderOutputResolution(int width, int height) {
        return setCh1ScaleResolution(width, height);
    }
    
    WorkerConfig::DecoderConfig::TacoConfig build() const {
        return taco_config_;
    }
    
    // ========== 辅助映射函数（public，供外部使用） ==========
    
    /**
     * @brief 将RGB格式名称映射为整数（向后兼容）
     */
    static int mapRgbFormatNameToInt(std::string_view format) {
        if (format == "argb888") return 9;
        if (format == "abgr888") return 11;
        if (format == "rgba888") return 13;
        if (format == "bgra888") return 15;
        if (format == "rgb888") return 1;
        if (format == "bgr888") return 3;
        if (format == "xrgb888") return 25;
        if (format == "xbgr888") return 27;
        if (format == "rgb888_planar") return 2;
        if (format == "bgr888_planar") return 4;
        if (format == "r16g16b16") return 17;
        if (format == "b16g16r16") return 19;
        if (format == "rgbx888") return 21;
        if (format == "bgrx888") return 23;
        if (format == "gbrp") return 28;
        // 默认返回 argb888
        return 9;
    }
    
    /**
     * @brief 将颜色标准名称映射为整数（向后兼容）
     */
    static int mapRgbStdNameToInt(std::string_view std_name) {
        if (std_name == "bt601") return 1;
        if (std_name == "bt601_l") return 2;
        if (std_name == "bt709") return 3;
        if (std_name == "bt709_l") return 4;
        if (std_name == "bt2020") return 5;
        if (std_name == "bt2020_l") return 6;
        // 默认返回 bt601
        return 1;
    }

private:
    WorkerConfig::DecoderConfig::TacoConfig taco_config_;
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
     * @brief 预设：TACO 硬件解码（通用，支持多种编解码器）
     * 
     * 设置解码器为 TACO 平台的指定编解码器。
     * TACO 是定制硬件解码器平台，支持多种视频编解码器。
     * 
     * @param codec 编解码器类型（如 "h264"、"h265"、"vp9" 等）
     * 
     * 示例：
     * @code
     * // H.264 解码
     * DecoderConfigBuilder().useTaco("h264").build()
     * 
     * // H.265/HEVC 解码
     * DecoderConfigBuilder().useTaco("h265").build()
     * 
     * // VP9 解码
     * DecoderConfigBuilder().useTaco("vp9").build()
     * @endcode
     * 
     * 生成的解码器名称格式为：{codec}_taco（如 "h264_taco"、"h265_taco"）
     */
    DecoderConfigBuilder& useTaco(std::string_view codec) {
        // 拼接解码器名称：codec + "_taco"
        decoder_config_.name = std::string(codec) + "_taco";
        decoder_config_.enable_hardware = true;
        
        // 设置默认 taco 配置
        decoder_config_.taco.reorder_disable = true;
        decoder_config_.taco.ch0_enable = true;
        decoder_config_.taco.ch1_enable = true;
        decoder_config_.taco.ch1_rgb = true;
        decoder_config_.taco.ch1_rgb_format = TacoConfigBuilder::mapRgbFormatNameToInt("argb888");  // 9
        decoder_config_.taco.ch1_rgb_std = TacoConfigBuilder::mapRgbStdNameToInt("bt601");          // 1
        
        return *this;
    }
    
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



