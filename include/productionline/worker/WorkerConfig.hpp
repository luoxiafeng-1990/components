#ifndef WORKER_CONFIG_HPP
#define WORKER_CONFIG_HPP

/**
 * @brief Worker 类型枚举
 * 
 * 注意：此枚举独立定义，避免与 BufferFillingWorkerFactory 的循环依赖
 */
enum class WorkerType {
    AUTO,              // 自动检测（默认）
    MMAP_RAW,          // Mmap Raw 视频文件
    IOURING_RAW,       // IoUring Raw 视频文件
    FFMPEG_RTSP,       // FFmpeg RTSP 流
    FFMPEG_VIDEO_FILE  // FFmpeg 视频文件
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
        const char* file_path = nullptr;       // 文件路径
        int start_frame = 0;                   // 起始帧
        int end_frame = -1;                    // 结束帧（-1=全部）
        
        FileConfig() = default;
    } file;
    
    // ========================================
    // 输出配置
    // ========================================
    struct OutputConfig {
        int width = 0;                         // 输出宽度
        int height = 0;                        // 输出高度
        int bits_per_pixel = 0;                // 每像素位数
        
        OutputConfig() = default;
    } output;
    
    // ========================================
    // 解码器配置
    // ========================================
    struct DecoderConfig {
        // 通用解码器参数
        const char* name = nullptr;           // 解码器名称（nullptr=自动选择）
        bool enable_hardware = true;          // 启用硬件加速
        const char* hwaccel_device = nullptr; // 硬件设备（如 "cuda:0", "vaapi"）
        int decode_threads = 0;               // 解码线程数（0=自动）
        
        // ========================================
        // h264_taco 特定配置（子子结构体）
        // ========================================
        struct TacoConfig {
            bool reorder_disable = true;           // 禁用重排序
            bool ch0_enable = true;                // 启用通道0（YUV）
            bool ch1_enable = true;                // 启用通道1（RGB）
            bool ch1_rgb = true;                   // 通道1输出RGB
            const char* ch1_rgb_format = "argb888"; // RGB格式
            const char* ch1_rgb_std = "bt601";     // 色彩标准
            int ch1_crop_x = 0;                    // 裁剪参数
            int ch1_crop_y = 0;
            int ch1_crop_width = 0;
            int ch1_crop_height = 0;
            int ch1_scale_width = 0;               // 缩放参数
            int ch1_scale_height = 0;
            
            TacoConfig() = default;
        } taco;
        
        DecoderConfig() = default;
    } decoder;
    
    // ========================================
    // Worker 类型
    // ========================================
    WorkerType worker_type = WorkerType::AUTO;
    
    WorkerConfig() = default;
};

// ========================================
// Builder 模式实现
// ========================================

/**
 * @brief 文件配置构建器
 */
class FileConfigBuilder {
private:
    WorkerConfig::FileConfig config_;
    
public:
    FileConfigBuilder() = default;
    
    FileConfigBuilder& setFilePath(const char* path) {
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
};

/**
 * @brief 输出配置构建器
 */
class OutputConfigBuilder {
private:
    WorkerConfig::OutputConfig config_;
    
public:
    OutputConfigBuilder() = default;
    
    OutputConfigBuilder& setWidth(int width) {
        config_.width = width;
        return *this;
    }
    
    OutputConfigBuilder& setHeight(int height) {
        config_.height = height;
        return *this;
    }
    
    OutputConfigBuilder& setResolution(int width, int height) {
        config_.width = width;
        config_.height = height;
        return *this;
    }
    
    OutputConfigBuilder& setBitsPerPixel(int bpp) {
        config_.bits_per_pixel = bpp;
        return *this;
    }
    
    WorkerConfig::OutputConfig build() const {
        return config_;
    }
};

/**
 * @brief h264_taco 特定配置构建器
 */
class TacoConfigBuilder {
private:
    WorkerConfig::DecoderConfig::TacoConfig config_;
    
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
    
    TacoConfigBuilder& setRgbConfig(bool enable, const char* format = "argb888", const char* std = "bt601") {
        config_.ch1_rgb = enable;
        config_.ch1_rgb_format = format;
        config_.ch1_rgb_std = std;
        return *this;
    }
    
    TacoConfigBuilder& setCropRegion(int x, int y, int width, int height) {
        config_.ch1_crop_x = x;
        config_.ch1_crop_y = y;
        config_.ch1_crop_width = width;
        config_.ch1_crop_height = height;
        return *this;
    }
    
    TacoConfigBuilder& setScaleSize(int width, int height) {
        config_.ch1_scale_width = width;
        config_.ch1_scale_height = height;
        return *this;
    }
    
    WorkerConfig::DecoderConfig::TacoConfig build() const {
        return config_;
    }
};

/**
 * @brief 解码器配置构建器
 */
class DecoderConfigBuilder {
private:
    WorkerConfig::DecoderConfig config_;
    
public:
    DecoderConfigBuilder() = default;
    
    // ========== 通用解码器参数 ==========
    
    DecoderConfigBuilder& setDecoderName(const char* name) {
        config_.name = name;
        return *this;
    }
    
    DecoderConfigBuilder& enableHardware(bool enable = true) {
        config_.enable_hardware = enable;
        return *this;
    }
    
    DecoderConfigBuilder& setHwaccelDevice(const char* device) {
        config_.hwaccel_device = device;
        return *this;
    }
    
    DecoderConfigBuilder& setDecodeThreads(int threads) {
        config_.decode_threads = threads;
        return *this;
    }
    
    // ========== h264_taco 配置 ==========
    
    /**
     * @brief 设置 taco 配置（使用独立的配置对象）
     */
    DecoderConfigBuilder& setTacoConfig(const WorkerConfig::DecoderConfig::TacoConfig& taco_config) {
        config_.taco = taco_config;
        return *this;
    }
    
    /**
     * @brief 快速配置 taco 参数（简化版）
     */
    DecoderConfigBuilder& configureTaco(
        bool reorder_disable = true,
        bool ch0_enable = true,
        bool ch1_enable = true,
        bool ch1_rgb = true,
        const char* rgb_format = "argb888",
        const char* rgb_std = "bt601"
    ) {
        config_.taco.reorder_disable = reorder_disable;
        config_.taco.ch0_enable = ch0_enable;
        config_.taco.ch1_enable = ch1_enable;
        config_.taco.ch1_rgb = ch1_rgb;
        config_.taco.ch1_rgb_format = rgb_format;
        config_.taco.ch1_rgb_std = rgb_std;
        return *this;
    }
    
    // ========== 快捷预设 ==========
    
    /**
     * @brief 预设：h264_taco 硬件解码（默认配置）
     */
    DecoderConfigBuilder& useH264Taco() {
        config_.name = "h264_taco";
        config_.enable_hardware = true;
        configureTaco();  // 使用默认 taco 配置
        return *this;
    }
    
    /**
     * @brief 预设：h264_taco 硬件解码（自定义 taco 配置）
     */
    DecoderConfigBuilder& useH264TacoWith(const WorkerConfig::DecoderConfig::TacoConfig& taco_config) {
        config_.name = "h264_taco";
        config_.enable_hardware = true;
        config_.taco = taco_config;
        return *this;
    }
    
    /**
     * @brief 预设：软件解码（自动选择）
     */
    DecoderConfigBuilder& useSoftware() {
        config_.name = nullptr;
        config_.enable_hardware = false;
        return *this;
    }
    
    /**
     * @brief 预设：NVIDIA CUDA 解码
     */
    DecoderConfigBuilder& useH264Cuvid() {
        config_.name = "h264_cuvid";
        config_.enable_hardware = true;
        config_.hwaccel_device = "cuda";
        return *this;
    }
    
    /**
     * @brief 预设：Intel Quick Sync 解码
     */
    DecoderConfigBuilder& useH264Qsv() {
        config_.name = "h264_qsv";
        config_.enable_hardware = true;
        config_.hwaccel_device = "qsv";
        return *this;
    }
    
    WorkerConfig::DecoderConfig build() const {
        return config_;
    }
};

/**
 * @brief Worker 配置构建器（顶层）
 * 
 * 职责：只负责组装 WorkerConfig，不涉及具体配置细节
 */
class WorkerConfigBuilder {
private:
    WorkerConfig config_;
    
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
     * @brief 设置输出配置
     */
    WorkerConfigBuilder& setOutputConfig(const WorkerConfig::OutputConfig& output_config) {
        config_.output = output_config;
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
};

#endif // WORKER_CONFIG_HPP



