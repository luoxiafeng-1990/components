#pragma once

#include "buffer/bufferpool/Buffer.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <cstdio>

// FFmpeg前向声明
extern "C" {
#include <libavutil/pixfmt.h>
}

struct AVFrame;
struct SwsContext;
struct AVPixFmtDescriptor;

namespace productionline {
namespace io {

/**
 * @brief 对比配置
 */
struct CompareConfig {
    // ========== 验证策略 ==========
    enum Strategy {
        FAST_ONLY,          // 仅快速验证（每帧PSNR-Y/G）
        AUTO_LAYERED,       // ⭐ 自动分层（推荐）
        DEEP_ALWAYS         // 总是深度验证（慢但详细）
    };
    
    Strategy strategy = AUTO_LAYERED;
    
    // ========== 格式处理策略 ⭐ 新增 ==========
    enum FormatStrategy {
        AUTO,               // 自动检测并选择最优策略（推荐）
        FORCE_YUV,          // 强制转换到YUV空间对比
        FORCE_RGB,          // 强制转换到RGB空间对比
        NATIVE              // 原生格式对比（要求两边格式一致）
    };
    
    FormatStrategy format_strategy = AUTO;
    
    // ========== 色彩空间转换配置 ==========
    enum ColorStandard {
        BT601,              // 标清：Rec.601（默认）
        BT709,              // 高清：Rec.709
        BT2020              // 4K/HDR：Rec.2020
    };
    
    ColorStandard color_std = BT601;
    
    // ========== 指标开关 ==========
    bool enable_psnr = true;               // 是否启用 PSNR 计算
    bool enable_ssim = true;               // 是否启用 SSIM 计算（计算量约为 PSNR 的 1.5-2 倍）
    
    // ========== 并行计算配置 ==========
    bool enable_parallel = true;           // 是否启用并行计算（使用全局线程池）
    // 注意：并行计算可提速 40-60%（快速验证）到 2-3 倍（深度验证）
    //      但会增加线程调度开销，建议在高分辨率视频时启用
    
    // ========== 阈值配置 ==========
    double quick_psnr_threshold = 38.0;    // >= 38dB 快速通过
    double quick_warn_threshold = 35.0;    // < 35dB 触发深度验证
    double ssim_threshold = 0.95;          // >= 0.95 认为质量优秀
    double ssim_warn_threshold = 0.90;     // < 0.90 触发警告
    int max_pixel_diff = 3;                // 最大差3灰度级
    float diff_pixel_ratio = 0.05;         // 差异像素<5%
    
    // ========== 感知加权 ==========
    // YUV格式：Y平面权重更高（人眼对亮度敏感）
    // RGB格式：G通道权重更高（人眼对绿色敏感）
    bool use_perceptual_weighting = true;
    
    // ========== 输出选项 ==========
    bool verbose = true;                   // 详细日志
    bool save_report = true;               // 保存报告到文件
    std::string report_path = "./decoder_compare_report.txt";
    bool save_failed_frames = false;       // 保存失败帧的差异图（未实现）
    std::string output_dir = "./validation_output";
};

/**
 * @brief 单帧对比结果
 */
struct FrameCompareResult {
    int frame_index = 0;
    bool passed = false;
    
    // ========== PSNR结果 ==========
    // YUV格式：psnr_y/u/v 表示 Y/U/V 平面
    // RGB格式：复用字段 psnr_y=G, psnr_u=R, psnr_v=B
    double psnr_y = 0.0;    // Y平面 或 G通道（主要指标）
    double psnr_u = 0.0;    // U平面 或 R通道
    double psnr_v = 0.0;    // V平面 或 B通道
    double psnr_avg = 0.0;  // 加权平均
    
    // ========== SSIM结果 ==========
    // YUV格式：ssim_y/u/v 表示 Y/U/V 平面
    // RGB格式：复用字段 ssim_y=G, ssim_u=R, ssim_v=B
    double ssim_y = 0.0;    // Y平面 或 G通道（主要指标）范围 [0.0, 1.0]
    double ssim_u = 0.0;    // U平面 或 R通道，范围 [0.0, 1.0]
    double ssim_v = 0.0;    // V平面 或 B通道，范围 [0.0, 1.0]
    double ssim_avg = 0.0;  // 加权平均，范围 [0.0, 1.0]
    
    // ========== 像素差异统计 ==========
    int max_pixel_diff = 0;      // 最大像素差值
    int diff_pixel_count = 0;    // 差异像素数量
    float diff_pixel_ratio = 0.0; // 差异像素比例
    
    // ========== 判定级别 ==========
    enum Level {
        PASS,    // 通过
        WARN,    // 警告（边界）
        FAIL     // 失败
    } level = PASS;
    
    std::string error_message;
    
    // ========== 格式信息 ==========
    std::string ref_format_name;
    std::string test_format_name;
};

/**
 * @brief Buffer对比器（IO后处理，支持多格式）
 * 
 * 架构角色：IO后处理（与BufferWriter并列）
 * 
 * 职责：
 * - 对比两个解码器的输出Buffer（参考 vs 测试）
 * - 自动检测格式并选择最优对比策略
 * - 支持YUV/RGB格式及混合格式对比
 * - 计算PSNR等质量指标
 * - 收集统计数据并生成报告
 * 
 * 使用方式（类似BufferWriter）：
 * ```cpp
 * using namespace productionline::io;
 * 
 * BufferComparator comparator;
 * CompareConfig config;
 * config.strategy = CompareConfig::AUTO_LAYERED;
 * config.format_strategy = CompareConfig::AUTO;
 * 
 * comparator.open(config);
 * 
 * while (running) {
 *     Buffer* sw_buf = sw_pool->acquireFilled(...);
 *     Buffer* hw_buf = hw_pool->acquireFilled(...);
 *     
 *     FrameCompareResult result = comparator.compare(sw_buf, hw_buf);
 *     
 *     sw_pool->releaseFilled(sw_buf);
 *     hw_pool->releaseFilled(hw_buf);
 * }
 * 
 * comparator.close();
 * comparator.printSummary();
 * ```
 * 
 * 特性：
 * - 格式自适应：自动检测YUV/RGB并选择算法
 * - 2层验证：快速验证（PSNR-Y/G）→ 深度验证（完整PSNR）
 * - 零转换优化：相同格式直接对比
 * - 感知加权：Y/G通道权重更高
 */
class BufferComparator {
public:
    BufferComparator();
    ~BufferComparator();
    
    // 禁止拷贝
    BufferComparator(const BufferComparator&) = delete;
    BufferComparator& operator=(const BufferComparator&) = delete;
    
    /**
     * @brief 打开对比器（初始化）
     * @param config 对比配置
     * @return 成功返回true
     */
    bool open(const CompareConfig& config = CompareConfig());
    
    /**
     * @brief 对比两个Buffer
     * @param reference_buffer 参考Buffer（通常是软件解码）
     * @param test_buffer 测试Buffer（通常是硬件解码）
     * @return 对比结果
     * 
     * @note 自动推断帧索引（从0开始递增）
     * @note 线程安全（内部使用原子计数）
     * @note 自动检测格式并选择最优对比策略
     */
    FrameCompareResult compare(Buffer* reference_buffer, Buffer* test_buffer);
    
    /**
     * @brief 关闭对比器（输出报告）
     */
    void close();
    
    /**
     * @brief 打印统计摘要（类似BufferWriter::printStats）
     */
    void printSummary() const;
    
    /**
     * @brief 获取对比帧数
     */
    int getCompareCount() const { return compare_count_.load(); }
    
    /**
     * @brief 获取通过帧数
     */
    int getPassedCount() const { return passed_count_.load(); }
    
    /**
     * @brief 获取失败帧数
     */
    int getFailedCount() const { return failed_count_.load(); }
    
    /**
     * @brief 是否全部通过
     */
    bool isPassed() const { return failed_count_.load() == 0; }
    
private:
    CompareConfig config_;
    bool is_open_;
    
    // ========== 统计信息（原子变量，线程安全）==========
    std::atomic<int> compare_count_;
    std::atomic<int> passed_count_;
    std::atomic<int> warned_count_;
    std::atomic<int> failed_count_;
    
    // ========== 质量统计 ==========
    double sum_psnr_y_;
    double sum_psnr_u_;
    double sum_psnr_v_;
    double min_psnr_y_;
    double max_psnr_y_;
    
    double sum_ssim_y_;
    double sum_ssim_u_;
    double sum_ssim_v_;
    double min_ssim_y_;
    double max_ssim_y_;
    
    // ========== 失败帧列表 ==========
    std::vector<FrameCompareResult> failures_;
    std::vector<FrameCompareResult> warnings_;
    
    // ========== 报告文件 ==========
    FILE* report_file_;
    
    // ========== 格式信息 ==========
    struct FormatInfo {
        AVPixelFormat format;
        bool is_yuv;
        bool is_rgb;
        bool is_planar;
        int num_planes;
        int width;
        int height;
        std::string name;
    };
    
    /**
     * @brief 分析Buffer格式
     */
    FormatInfo analyzeFormat(Buffer* buffer);
    
    // ========== 多格式对比策略（核心）==========
    
    /**
     * @brief 自动选择对比策略
     */
    FrameCompareResult compareAuto(
        Buffer* ref_buffer, const FormatInfo& ref_info,
        Buffer* test_buffer, const FormatInfo& test_info
    );
    
    /**
     * @brief YUV格式对比（分平面）
     */
    FrameCompareResult compareYUV(
        Buffer* ref_buffer, const FormatInfo& ref_info,
        Buffer* test_buffer, const FormatInfo& test_info
    );
    
    /**
     * @brief RGB格式对比（分通道）
     */
    FrameCompareResult compareRGB(
        Buffer* ref_buffer, const FormatInfo& ref_info,
        Buffer* test_buffer, const FormatInfo& test_info
    );
    
    /**
     * @brief 混合格式对比（需要转换）
     */
    FrameCompareResult compareMixed(
        Buffer* ref_buffer, const FormatInfo& ref_info,
        Buffer* test_buffer, const FormatInfo& test_info
    );
    
    // ========== PSNR计算 ==========
    
    /**
     * @brief 通用PSNR计算（单平面/通道）
     */
    double calculatePSNR(
        const uint8_t* data1, const uint8_t* data2,
        int width, int height,
        int stride1, int stride2
    );
    
    /**
     * @brief YUV格式：Y平面PSNR
     */
    double calculatePSNR_YUV_Y(Buffer* buf1, Buffer* buf2, 
                               const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief YUV格式：U平面PSNR
     */
    double calculatePSNR_YUV_U(Buffer* buf1, Buffer* buf2,
                               const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief YUV格式：V平面PSNR
     */
    double calculatePSNR_YUV_V(Buffer* buf1, Buffer* buf2,
                               const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief RGB格式：R通道PSNR
     */
    double calculatePSNR_RGB_R(Buffer* buf1, Buffer* buf2,
                               const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief RGB格式：G通道PSNR
     */
    double calculatePSNR_RGB_G(Buffer* buf1, Buffer* buf2,
                               const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief RGB格式：B通道PSNR
     */
    double calculatePSNR_RGB_B(Buffer* buf1, Buffer* buf2,
                               const FormatInfo& info1, const FormatInfo& info2);
    
    // ========== SSIM计算 ==========
    
    /**
     * @brief 通用 SSIM 计算（单平面/通道）
     * @param data1 第一个图像数据
     * @param data2 第二个图像数据
     * @param width 图像宽度
     * @param height 图像高度
     * @param stride1 第一个图像的行跨度
     * @param stride2 第二个图像的行跨度
     * @return SSIM 值，范围 [0.0, 1.0]，1.0 表示完全相同
     * 
     * @note 使用 8x8 滑动窗口和标准 SSIM 算法
     * @note 计算复杂度约为 PSNR 的 1.5-2 倍
     */
    double calculateSSIM(
        const uint8_t* data1, const uint8_t* data2,
        int width, int height,
        int stride1, int stride2
    );
    
    /**
     * @brief YUV格式：Y平面 SSIM
     */
    double calculateSSIM_YUV_Y(Buffer* buf1, Buffer* buf2, 
                              const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief YUV格式：U平面 SSIM
     */
    double calculateSSIM_YUV_U(Buffer* buf1, Buffer* buf2,
                              const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief YUV格式：V平面 SSIM
     */
    double calculateSSIM_YUV_V(Buffer* buf1, Buffer* buf2,
                              const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief RGB格式：R通道 SSIM
     */
    double calculateSSIM_RGB_R(Buffer* buf1, Buffer* buf2,
                              const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief RGB格式：G通道 SSIM
     */
    double calculateSSIM_RGB_G(Buffer* buf1, Buffer* buf2,
                              const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief RGB格式：B通道 SSIM
     */
    double calculateSSIM_RGB_B(Buffer* buf1, Buffer* buf2,
                              const FormatInfo& info1, const FormatInfo& info2);
    
    // ========== 辅助方法 ==========
    
    /**
     * @brief 元数据对比
     */
    bool compareMetadata(
        const FormatInfo& ref_info,
        const FormatInfo& test_info,
        FrameCompareResult& result
    );
    
    /**
     * @brief 更新统计信息
     */
    void updateStatistics(const FrameCompareResult& result);
    
    /**
     * @brief 写入报告
     */
    void writeReport(const FrameCompareResult& result);
    
    /**
     * @brief 格式转换到YUV420P
     */
    AVFrame* convertToYUV420P(Buffer* buffer, const FormatInfo& info);
    
    /**
     * @brief 将YUV格式转换为RGB格式（用于RGB对比）
     * @param buffer YUV格式的Buffer
     * @param info YUV格式信息
     * @param target_rgb_format 目标RGB格式
     * @return 转换后的AVFrame，失败返回nullptr
     */
    AVFrame* convertYUVToRGB(Buffer* buffer, const FormatInfo& info, AVPixelFormat target_rgb_format);
    
    /**
     * @brief 释放转换后的AVFrame
     */
    void freeConvertedFrame(AVFrame* frame);
};

} // namespace io
} // namespace productionline
