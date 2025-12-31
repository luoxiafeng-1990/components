#ifndef WORKER_CONFIG_HPP
#define WORKER_CONFIG_HPP

#include <string>
#include <string_view>
#include <optional>

/**
 * @brief Worker 类型枚举
 * 
 * 注意：此枚举独立定义，避免与 BufferFillingWorkerFactory 的循环依赖
 */
enum class WorkerType {
    AUTO,                 // 自动检测（默认）
    MMAP_RAW,             // Mmap Raw 视频文件
    IOURING_RAW,          // IoUring Raw 视频文件
    FFMPEG_RTSP,          // FFmpeg RTSP 流
    FFMPEG_RTSP_RECORD,   // FFmpeg RTSP 原始码流录制
    FFMPEG_VIDEO_FILE     // FFmpeg 视频文件
};

/**
 * @brief Worker 配置（完整版）
 * 
 * 设计理念：
 * - 完全独立：包含 Worker 需要的所有配置
 * - 配置分离：文件、输出、解码器配置独立
 * - Builder 构建：链式调用，易用易读
 * - 职责清晰：每个 Builder 只负责自己层级的配置
 * 
 * 配置结构：
 * - FileConfig: 文件路径和导航参数
 * - OutputConfig: 输出分辨率和格式
 * - DecoderConfig: 解码器类型和参数
 * - worker_type: Worker 实现类型
 */
struct WorkerConfig {
    // ========================================
    // 文件配置
    // ========================================
    struct FileConfig {
        std::string file_path;                // 文件路径（使用 std::string 保证生命周期安全）
        int start_frame = 0;                   // 起始帧
        int end_frame = -1;                    // 结束帧（-1=全部）
        
        FileConfig() = default;
        FileConfig(const FileConfig&) = default;
        FileConfig& operator=(const FileConfig&) = default;
        FileConfig(FileConfig&&) = default;
        FileConfig& operator=(FileConfig&&) = default;
    } file;
    
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
        // h264_taco 特定配置（子子结构体）
        // ========================================
        struct TacoConfig {
            bool reorder_disable = true;               // 禁用重排序
            bool ch0_enable = true;                    // 启用通道0（YUV）
            bool ch1_enable = true;                    // 启用通道1（RGB）
            bool ch1_rgb = true;                       // 通道1输出RGB
            std::string ch1_rgb_format = "argb888";   // RGB格式（使用 std::string）
            std::string ch1_rgb_std = "bt601";        // 色彩标准（使用 std::string）
            int ch1_crop_x = 0;                        // 裁剪参数
            int ch1_crop_y = 0;
            int ch1_crop_width = 0;
            int ch1_crop_height = 0;
            int ch1_scale_width = 0;                   // 缩放参数
            int ch1_scale_height = 0;
            
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
 * @brief 文件配置构建器
 */
class FileConfigBuilder {
public:
    FileConfigBuilder() = default;
    
    // 接受 std::string_view（推荐，C++17+）
    FileConfigBuilder& setFilePath(std::string_view path) {
        config_.file_path = std::string(path);
        return *this;
    }
    
    // 兼容 const char*（保持向后兼容）
    FileConfigBuilder& setFilePath(const char* path) {
        if (path) {
            config_.file_path = path;
        } else {
            config_.file_path.clear();
        }
        return *this;
    }
    
    // 兼容 std::string
    FileConfigBuilder& setFilePath(const std::string& path) {
        config_.file_path = path;
        return *this;
    }
    
    FileConfigBuilder& setStartFrame(int frame) {
        config_.start_frame = frame;
        return *this;
    }
    
    FileConfigBuilder& setEndFrame(int frame) {
        config_.end_frame = frame;
        return *this;
    }
    
    FileConfigBuilder& setFrameRange(int start, int end) {
        config_.start_frame = start;
        config_.end_frame = end;
        return *this;
    }
    
    WorkerConfig::FileConfig build() const {
        return config_;
    }
    
private:
    WorkerConfig::FileConfig config_;
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
        config_.width = width;
        return *this;
    }
    
    /**
     * @brief 设置显示设备高度
     * @param height 显示设备高度（像素）
     */
    DisplayConfigBuilder& setDisplayHeight(int height) {
        config_.height = height;
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
        config_.width = width;
        config_.height = height;
        return *this;
    }
    
    /**
     * @brief 设置每像素位数
     * @param bpp 每像素位数（如 32 表示 ARGB8888）
     */
    DisplayConfigBuilder& setBitsPerPixel(int bpp) {
        config_.bits_per_pixel = bpp;
        return *this;
    }
    
    WorkerConfig::DisplayConfig build() const {
        return config_;
    }
    
private:
    WorkerConfig::DisplayConfig config_;
};

/**
 * @brief h264_taco 特定配置构建器
 */
class TacoConfigBuilder {
public:
    TacoConfigBuilder() = default;
    
    TacoConfigBuilder& setReorderDisable(bool disable = true) {
        config_.reorder_disable = disable;
        return *this;
    }
    
    TacoConfigBuilder& setChannels(bool ch0, bool ch1) {
        config_.ch0_enable = ch0;
        config_.ch1_enable = ch1;
        return *this;
    }
    
    // 接受 std::string_view（推荐）
    TacoConfigBuilder& setRgbConfig(
        bool enable, 
        std::string_view format = "argb888", 
        std::string_view std = "bt601"
    ) {
        config_.ch1_rgb = enable;
        config_.ch1_rgb_format = std::string(format);
        config_.ch1_rgb_std = std::string(std);
        return *this;
    }
    
    // 兼容 const char*（保持向后兼容）
    TacoConfigBuilder& setRgbConfig(bool enable, const char* format, const char* std) {
        config_.ch1_rgb = enable;
        if (format) {
            config_.ch1_rgb_format = format;
        }
        if (std) {
            config_.ch1_rgb_std = std;
        }
        return *this;
    }
    
    TacoConfigBuilder& setCropRegion(int x, int y, int width, int height) {
        config_.ch1_crop_x = x;
        config_.ch1_crop_y = y;
        config_.ch1_crop_width = width;
        config_.ch1_crop_height = height;
        return *this;
    }
    
    /**
     * @brief 设置解码器输出分辨率（硬件缩放）
     * 
     * ⚠️ 重要：此方法设置的是解码器输出分辨率，不是显示设备分辨率！
     * 
     * TACO 解码器会将视频通过硬件缩放到指定分辨率后输出。
     * 
     * @param width  解码器输出宽度（像素）
     * @param height 解码器输出高度（像素）
     * @return TacoConfigBuilder& 链式调用
     * 
     * @example
     * ```cpp
     * TacoConfigBuilder()
     *     .setRgbConfig(true, "argb888", "bt601")
     *     .setDecoderOutputResolution(1920, 1080)  // 解码器输出 1920×1080
     *     .build()
     * ```
     */
    TacoConfigBuilder& setDecoderOutputResolution(int width, int height) {
        config_.ch1_scale_width = width;
        config_.ch1_scale_height = height;
        return *this;
    }
    
    WorkerConfig::DecoderConfig::TacoConfig build() const {
        return config_;
    }
    
private:
    WorkerConfig::DecoderConfig::TacoConfig config_;
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
        config_.name = std::string(name);
        return *this;
    }
    
    // 兼容 const char*（保持向后兼容）
    DecoderConfigBuilder& setDecoderName(const char* name) {
        if (name) {
            config_.name = name;
        } else {
            config_.name = std::nullopt;
        }
        return *this;
    }
    
    // 清除解码器名称（使用自动选择）
    DecoderConfigBuilder& clearDecoderName() {
        config_.name = std::nullopt;
        return *this;
    }
    
    DecoderConfigBuilder& enableHardware(bool enable = true) {
        config_.enable_hardware = enable;
        return *this;
    }
    
    // 接受 std::string_view（推荐）
    DecoderConfigBuilder& setHwaccelDevice(std::string_view device) {
        config_.hwaccel_device = std::string(device);
        return *this;
    }
    
    // 兼容 const char*（保持向后兼容）
    DecoderConfigBuilder& setHwaccelDevice(const char* device) {
        if (device) {
            config_.hwaccel_device = device;
        } else {
            config_.hwaccel_device = std::nullopt;
        }
        return *this;
    }
    
    DecoderConfigBuilder& setDecodeThreads(int threads) {
        config_.decode_threads = threads;
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
        config_.name = std::string(codec) + "_taco";
        config_.enable_hardware = true;
        
        // 设置默认 taco 配置
        config_.taco.reorder_disable = true;
        config_.taco.ch0_enable = true;
        config_.taco.ch1_enable = true;
        config_.taco.ch1_rgb = true;
        config_.taco.ch1_rgb_format = "argb888";
        config_.taco.ch1_rgb_std = "bt601";
        
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
        config_.name = std::string(codec) + "_taco";
        config_.enable_hardware = true;
        config_.taco = taco_config;
        return *this;
    }
    
    /**
     * @brief 预设：软件解码（自动选择）
     */
    DecoderConfigBuilder& useSoftware() {
        config_.name = std::nullopt;
        config_.enable_hardware = false;
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
        config_.name = std::string(codec) + "_cuvid";
        config_.enable_hardware = true;
        config_.hwaccel_device = "cuda";
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
        config_.name = std::string(codec) + "_qsv";
        config_.enable_hardware = true;
        config_.hwaccel_device = "qsv";
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
        config_.name = std::string(codec) + "_vaapi";
        config_.enable_hardware = true;
        config_.hwaccel_device = "vaapi";
        return *this;
    }
    
    WorkerConfig::DecoderConfig build() const {
        return config_;
    }
    
private:
    WorkerConfig::DecoderConfig config_;
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
     * @brief 设置文件配置
     */
    WorkerConfigBuilder& setFileConfig(const WorkerConfig::FileConfig& file_config) {
        config_.file = file_config;
        return *this;
    }
    
    /**
     * @brief 设置显示设备配置
     * @param display_config 显示设备配置
     */
    WorkerConfigBuilder& setDisplayConfig(const WorkerConfig::DisplayConfig& display_config) {
        config_.display = display_config;
        return *this;
    }
    
    /**
     * @brief 设置解码器配置
     */
    WorkerConfigBuilder& setDecoderConfig(const WorkerConfig::DecoderConfig& decoder_config) {
        config_.decoder = decoder_config;
        return *this;
    }
    
    /**
     * @brief 设置 Worker 类型
     */
    WorkerConfigBuilder& setWorkerType(WorkerType type) {
        config_.worker_type = type;
        return *this;
    }
    
    /**
     * @brief 构建最终配置
     */
    WorkerConfig build() const {
        return config_;
    }
    
private:
    WorkerConfig config_;
};

#endif // WORKER_CONFIG_HPP



