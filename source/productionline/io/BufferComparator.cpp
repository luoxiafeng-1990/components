#include "productionline/io/BufferComparator.hpp"
#include "common/Logger.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace productionline {
namespace io {

// ============================================================================
// 构造/析构
// ============================================================================

BufferComparator::BufferComparator()
    : is_open_(false)
    , compare_count_(0)
    , passed_count_(0)
    , warned_count_(0)
    , failed_count_(0)
    , sum_psnr_y_(0.0)
    , sum_psnr_u_(0.0)
    , sum_psnr_v_(0.0)
    , min_psnr_y_(100.0)
    , max_psnr_y_(0.0)
    , report_file_(nullptr)
{
}

BufferComparator::~BufferComparator() {
    if (is_open_) {
        close();
    }
}

// ============================================================================
// 公共接口
// ============================================================================

bool BufferComparator::open(const CompareConfig& config) {
    if (is_open_) {
        LOG_WARN("[BufferComparator] Already opened");
        return false;
    }
    
    config_ = config;
    
    // 打开报告文件
    if (config_.save_report) {
        report_file_ = fopen(config_.report_path.c_str(), "w");
        if (!report_file_) {
            LOG_ERROR_FMT("[BufferComparator] Failed to open report file: %s", 
                         config_.report_path.c_str());
            return false;
        }
        
        // 写入报告头
        fprintf(report_file_, "═══════════════════════════════════════════════════════\n");
        fprintf(report_file_, "  Decoder Comparison Report\n");
        fprintf(report_file_, "═══════════════════════════════════════════════════════\n");
        fprintf(report_file_, "Strategy: %s\n", 
                config_.strategy == CompareConfig::FAST_ONLY ? "FAST_ONLY" :
                config_.strategy == CompareConfig::AUTO_LAYERED ? "AUTO_LAYERED" : "DEEP_ALWAYS");
        fprintf(report_file_, "Format Strategy: %s\n",
                config_.format_strategy == CompareConfig::AUTO ? "AUTO" :
                config_.format_strategy == CompareConfig::FORCE_YUV ? "FORCE_YUV" :
                config_.format_strategy == CompareConfig::FORCE_RGB ? "FORCE_RGB" : "NATIVE");
        fprintf(report_file_, "PSNR Threshold: Pass >= %.1f dB, Warn >= %.1f dB\n",
                config_.quick_psnr_threshold, config_.quick_warn_threshold);
        fprintf(report_file_, "═══════════════════════════════════════════════════════\n\n");
        fflush(report_file_);
    }
    
    // 重置统计
    compare_count_ = 0;
    passed_count_ = 0;
    warned_count_ = 0;
    failed_count_ = 0;
    sum_psnr_y_ = 0.0;
    sum_psnr_u_ = 0.0;
    sum_psnr_v_ = 0.0;
    min_psnr_y_ = 100.0;
    max_psnr_y_ = 0.0;
    failures_.clear();
    warnings_.clear();
    
    is_open_ = true;
    LOG_INFO("[BufferComparator] Opened successfully");
    return true;
}

void BufferComparator::close() {
    if (!is_open_) {
        return;
    }
    
    // 写入报告尾
    if (report_file_) {
        fprintf(report_file_, "\n═══════════════════════════════════════════════════════\n");
        fprintf(report_file_, "  Summary\n");
        fprintf(report_file_, "═══════════════════════════════════════════════════════\n");
        fprintf(report_file_, "Total frames compared: %d\n", compare_count_.load());
        fprintf(report_file_, "Passed: %d (%.1f%%)\n", 
                passed_count_.load(), 
                compare_count_ > 0 ? 100.0 * passed_count_.load() / compare_count_.load() : 0.0);
        fprintf(report_file_, "Warned: %d (%.1f%%)\n",
                warned_count_.load(),
                compare_count_ > 0 ? 100.0 * warned_count_.load() / compare_count_.load() : 0.0);
        fprintf(report_file_, "Failed: %d (%.1f%%)\n",
                failed_count_.load(),
                compare_count_ > 0 ? 100.0 * failed_count_.load() / compare_count_.load() : 0.0);
        
        if (compare_count_ > 0) {
            fprintf(report_file_, "\nPSNR Statistics:\n");
            fprintf(report_file_, "  Average: Y=%.2f U=%.2f V=%.2f dB\n",
                    sum_psnr_y_ / compare_count_.load(),
                    sum_psnr_u_ / compare_count_.load(),
                    sum_psnr_v_ / compare_count_.load());
            fprintf(report_file_, "  Min Y: %.2f dB\n", min_psnr_y_);
            fprintf(report_file_, "  Max Y: %.2f dB\n", max_psnr_y_);
        }
        
        fprintf(report_file_, "\n%s\n", 
                failed_count_.load() == 0 ? "✅ ALL TESTS PASSED" : "❌ SOME TESTS FAILED");
        fprintf(report_file_, "═══════════════════════════════════════════════════════\n");
        
        fclose(report_file_);
        report_file_ = nullptr;
        
        LOG_INFO_FMT("[BufferComparator] Report saved to: %s", config_.report_path.c_str());
    }
    
    is_open_ = false;
    LOG_INFO("[BufferComparator] Closed");
}

FrameCompareResult BufferComparator::compare(
    Buffer* reference_buffer, 
    Buffer* test_buffer
) {
    FrameCompareResult result;
    result.frame_index = compare_count_++;
    
    if (!is_open_) {
        LOG_ERROR("[BufferComparator] Not opened");
        result.error_message = "Comparator not opened";
        result.passed = false;
        result.level = FrameCompareResult::FAIL;
        return result;
    }
    
    if (!reference_buffer || !test_buffer) {
        LOG_ERROR("[BufferComparator] Null buffer");
        result.error_message = "Null buffer";
        result.passed = false;
        result.level = FrameCompareResult::FAIL;
        return result;
    }
    
    // 步骤1：分析两个Buffer的格式
    FormatInfo ref_info = analyzeFormat(reference_buffer);
    FormatInfo test_info = analyzeFormat(test_buffer);
    
    result.ref_format_name = ref_info.name;
    result.test_format_name = test_info.name;
    
    if (config_.verbose && result.frame_index == 0) {
        LOG_INFO_FMT("[BufferComparator] Frame %d format detected:", result.frame_index);
        LOG_INFO_FMT("  Reference: %s (%dx%d, %s, %d planes)", 
                     ref_info.name.c_str(), ref_info.width, ref_info.height,
                     ref_info.is_yuv ? "YUV" : ref_info.is_rgb ? "RGB" : "Unknown",
                     ref_info.num_planes);
        LOG_INFO_FMT("  Test:      %s (%dx%d, %s, %d planes)",
                     test_info.name.c_str(), test_info.width, test_info.height,
                     test_info.is_yuv ? "YUV" : test_info.is_rgb ? "RGB" : "Unknown",
                     test_info.num_planes);
    }
    
    // 步骤2：元数据检查
    if (!compareMetadata(ref_info, test_info, result)) {
        failed_count_++;
        updateStatistics(result);
        writeReport(result);
        return result;
    }
    
    // 步骤3：根据配置选择对比策略
    switch (config_.format_strategy) {
        case CompareConfig::AUTO:
            result = compareAuto(reference_buffer, ref_info, test_buffer, test_info);
            break;
            
        case CompareConfig::FORCE_YUV:
            result = compareMixed(reference_buffer, ref_info, test_buffer, test_info);
            break;
            
        case CompareConfig::NATIVE:
            if (ref_info.format != test_info.format) {
                result.error_message = "Format mismatch in NATIVE mode";
                result.passed = false;
                result.level = FrameCompareResult::FAIL;
                failed_count_++;
                updateStatistics(result);
                writeReport(result);
                return result;
            }
            
            if (ref_info.is_yuv) {
                result = compareYUV(reference_buffer, ref_info, test_buffer, test_info);
            } else if (ref_info.is_rgb) {
                result = compareRGB(reference_buffer, ref_info, test_buffer, test_info);
            }
            break;
            
        default:
            result.error_message = "Unknown format strategy";
            result.passed = false;
            result.level = FrameCompareResult::FAIL;
            failed_count_++;
            break;
    }
    
    result.frame_index = compare_count_.load() - 1;  // 修正索引
    updateStatistics(result);
    writeReport(result);
    
    return result;
}

void BufferComparator::printSummary() const {
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  BufferComparator Summary                              ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO_FMT("  Total frames compared: %d", compare_count_.load());
    LOG_INFO_FMT("  Passed: %d ✅ (%.1f%%)", 
                 passed_count_.load(),
                 compare_count_ > 0 ? 100.0 * passed_count_.load() / compare_count_.load() : 0.0);
    LOG_INFO_FMT("  Warned: %d ⚠️  (%.1f%%)",
                 warned_count_.load(),
                 compare_count_ > 0 ? 100.0 * warned_count_.load() / compare_count_.load() : 0.0);
    LOG_INFO_FMT("  Failed: %d ❌ (%.1f%%)",
                 failed_count_.load(),
                 compare_count_ > 0 ? 100.0 * failed_count_.load() / compare_count_.load() : 0.0);
    
    if (compare_count_ > 0) {
        LOG_INFO("");
        LOG_INFO("  PSNR Statistics:");
        LOG_INFO_FMT("    Average: Y=%.2f U=%.2f V=%.2f dB",
                     sum_psnr_y_ / compare_count_.load(),
                     sum_psnr_u_ / compare_count_.load(),
                     sum_psnr_v_ / compare_count_.load());
        LOG_INFO_FMT("    Range:   Y=[%.2f, %.2f] dB", min_psnr_y_, max_psnr_y_);
    }
    
    LOG_INFO("");
    if (failed_count_.load() == 0) {
        LOG_INFO("  ✅ Result: ALL TESTS PASSED");
    } else {
        LOG_WARN_FMT("  ❌ Result: %d TESTS FAILED", failed_count_.load());
    }
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
}

// ============================================================================
// 格式分析
// ============================================================================

BufferComparator::FormatInfo BufferComparator::analyzeFormat(Buffer* buffer) {
    FormatInfo info = {};
    info.format = AV_PIX_FMT_NONE;
    info.name = "Unknown";
    
    if (!buffer || !buffer->hasImageMetadata()) {
        return info;
    }
    
    info.format = buffer->getImageFormat();
    info.width = buffer->getImageWidth();
    info.height = buffer->getImageHeight();
    info.name = av_get_pix_fmt_name(info.format);
    
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(info.format);
    if (desc) {
        info.num_planes = desc->nb_components;
        info.is_planar = !(desc->flags & AV_PIX_FMT_FLAG_RGB);
        
        // YUV格式检测
        info.is_yuv = (info.format == AV_PIX_FMT_YUV420P ||
                      info.format == AV_PIX_FMT_NV12 ||
                      info.format == AV_PIX_FMT_NV21 ||
                      info.format == AV_PIX_FMT_YUV422P ||
                      info.format == AV_PIX_FMT_YUV444P ||
                      info.format == AV_PIX_FMT_YUV410P ||
                      info.format == AV_PIX_FMT_YUV411P ||
                      info.format == AV_PIX_FMT_P010LE ||
                      info.format == AV_PIX_FMT_P016LE);
        
        // RGB格式检测
        info.is_rgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    }
    
    return info;
}

// ============================================================================
// 元数据对比
// ============================================================================

bool BufferComparator::compareMetadata(
    const FormatInfo& ref_info,
    const FormatInfo& test_info,
    FrameCompareResult& result
) {
    // 检查分辨率
    if (ref_info.width != test_info.width || ref_info.height != test_info.height) {
        result.error_message = "Resolution mismatch: " + 
                              std::to_string(ref_info.width) + "x" + std::to_string(ref_info.height) + 
                              " vs " + 
                              std::to_string(test_info.width) + "x" + std::to_string(test_info.height);
        result.passed = false;
        result.level = FrameCompareResult::FAIL;
        LOG_ERROR_FMT("[BufferComparator] %s", result.error_message.c_str());
        return false;
    }
    
    return true;
}

// ============================================================================
// 自动选择对比策略
// ============================================================================

FrameCompareResult BufferComparator::compareAuto(
    Buffer* ref_buffer, const FormatInfo& ref_info,
    Buffer* test_buffer, const FormatInfo& test_info
) {
    // 情况1：格式完全一致 → 直接对比（最快）
    if (ref_info.format == test_info.format) {
        if (config_.verbose && compare_count_.load() == 1) {
            LOG_DEBUG("[BufferComparator] Strategy: SAME_FORMAT (fastest)");
        }
        
        if (ref_info.is_yuv) {
            return compareYUV(ref_buffer, ref_info, test_buffer, test_info);
        } else if (ref_info.is_rgb) {
            return compareRGB(ref_buffer, ref_info, test_buffer, test_info);
        }
    }
    
    // 情况2：都是YUV家族 → YUV空间对比
    if (ref_info.is_yuv && test_info.is_yuv) {
        if (config_.verbose && compare_count_.load() == 1) {
            LOG_DEBUG("[BufferComparator] Strategy: YUV_FAMILY");
            if (ref_info.format != test_info.format) {
                LOG_WARN("  YUV formats differ, will convert if needed");
            }
        }
        
        // 尝试直接对比（如果都是planar YUV）
        return compareYUV(ref_buffer, ref_info, test_buffer, test_info);
    }
    
    // 情况3：都是RGB家族 → RGB空间对比
    if (ref_info.is_rgb && test_info.is_rgb) {
        if (config_.verbose && compare_count_.load() == 1) {
            LOG_DEBUG("[BufferComparator] Strategy: RGB_FAMILY");
        }
        
        return compareRGB(ref_buffer, ref_info, test_buffer, test_info);
    }
    
    // 情况4：YUV vs RGB → 转换到YUV空间对比（业界标准）
    if ((ref_info.is_yuv && test_info.is_rgb) || (ref_info.is_rgb && test_info.is_yuv)) {
        if (config_.verbose && compare_count_.load() == 1) {
            LOG_WARN("[BufferComparator] Strategy: MIXED_FORMAT (YUV vs RGB)");
            LOG_WARN("  Converting to YUV420P for comparison (industry standard)");
        }
        
        return compareMixed(ref_buffer, ref_info, test_buffer, test_info);
    }
    
    // 未知格式
    FrameCompareResult result;
    result.error_message = "Unsupported format combination";
    result.passed = false;
    result.level = FrameCompareResult::FAIL;
    return result;
}

// ============================================================================
// YUV格式对比
// ============================================================================

FrameCompareResult BufferComparator::compareYUV(
    Buffer* ref_buffer, const FormatInfo& ref_info,
    Buffer* test_buffer, const FormatInfo& test_info
) {
    FrameCompareResult result;
    
    // 层1：快速验证（仅Y平面）
    result.psnr_y = calculatePSNR_YUV_Y(ref_buffer, test_buffer, ref_info, test_info);
    
    if (result.psnr_y >= config_.quick_psnr_threshold) {
        // ✅ 快速通过
        result.passed = true;
        result.level = FrameCompareResult::PASS;
        passed_count_++;
        
        if (config_.verbose && result.frame_index % 50 == 0) {
            LOG_DEBUG_FMT("  Frame %d: PASS (PSNR-Y: %.2f dB) ⚡ quick", 
                         result.frame_index, result.psnr_y);
        }
        
        return result;
    }
    
    // 层2：深度验证（U/V平面）
    if (config_.strategy == CompareConfig::AUTO_LAYERED ||
        config_.strategy == CompareConfig::DEEP_ALWAYS) {
        
        if (config_.verbose) {
            LOG_WARN_FMT("  Frame %d: PSNR-Y=%.2f dB < %.2f dB, deep validation...",
                         result.frame_index, result.psnr_y, config_.quick_psnr_threshold);
        }
        
        result.psnr_u = calculatePSNR_YUV_U(ref_buffer, test_buffer, ref_info, test_info);
        result.psnr_v = calculatePSNR_YUV_V(ref_buffer, test_buffer, ref_info, test_info);
        
        // 加权平均
        if (config_.use_perceptual_weighting) {
            result.psnr_avg = (result.psnr_y * 4.0 + result.psnr_u + result.psnr_v) / 6.0;
        } else {
            result.psnr_avg = (result.psnr_y + result.psnr_u + result.psnr_v) / 3.0;
        }
        
        // 判定
        if (result.psnr_y >= config_.quick_warn_threshold) {
            result.level = FrameCompareResult::WARN;
            result.passed = true;
            warned_count_++;
            
            if (config_.verbose) {
                LOG_WARN_FMT("  Frame %d: WARN (PSNR: Y=%.2f U=%.2f V=%.2f dB)",
                             result.frame_index, result.psnr_y, result.psnr_u, result.psnr_v);
            }
        } else {
            result.level = FrameCompareResult::FAIL;
            result.passed = false;
            failed_count_++;
            
            LOG_ERROR_FMT("  Frame %d: FAIL (PSNR: Y=%.2f U=%.2f V=%.2f dB)",
                          result.frame_index, result.psnr_y, result.psnr_u, result.psnr_v);
        }
    } else {
        // FAST_ONLY 模式
        result.level = FrameCompareResult::FAIL;
        result.passed = false;
        failed_count_++;
    }
    
    return result;
}

// ============================================================================
// RGB格式对比
// ============================================================================

FrameCompareResult BufferComparator::compareRGB(
    Buffer* ref_buffer, const FormatInfo& ref_info,
    Buffer* test_buffer, const FormatInfo& test_info
) {
    FrameCompareResult result;
    
    // 层1：快速验证（G通道，人眼最敏感）
    double psnr_g = calculatePSNR_RGB_G(ref_buffer, test_buffer, ref_info, test_info);
    result.psnr_y = psnr_g;  // 复用psnr_y字段
    
    if (psnr_g >= config_.quick_psnr_threshold) {
        // ✅ 快速通过
        result.passed = true;
        result.level = FrameCompareResult::PASS;
        passed_count_++;
        
        if (config_.verbose && result.frame_index % 50 == 0) {
            LOG_DEBUG_FMT("  Frame %d: PASS (PSNR-G: %.2f dB) ⚡ quick",
                         result.frame_index, psnr_g);
        }
        
        return result;
    }
    
    // 层2：深度验证（R/G/B全通道）
    if (config_.strategy == CompareConfig::AUTO_LAYERED ||
        config_.strategy == CompareConfig::DEEP_ALWAYS) {
        
        if (config_.verbose) {
            LOG_WARN_FMT("  Frame %d: PSNR-G=%.2f dB < %.2f dB, deep validation...",
                         result.frame_index, psnr_g, config_.quick_psnr_threshold);
        }
        
        double psnr_r = calculatePSNR_RGB_R(ref_buffer, test_buffer, ref_info, test_info);
        double psnr_b = calculatePSNR_RGB_B(ref_buffer, test_buffer, ref_info, test_info);
        
        // 加权平均（G通道权重更高）
        if (config_.use_perceptual_weighting) {
            result.psnr_avg = (psnr_r + psnr_g * 2.0 + psnr_b) / 4.0;
        } else {
            result.psnr_avg = (psnr_r + psnr_g + psnr_b) / 3.0;
        }
        
        // 存储到结果（复用YUV字段）
        result.psnr_y = psnr_g;  // G → Y
        result.psnr_u = psnr_r;  // R → U
        result.psnr_v = psnr_b;  // B → V
        
        // 判定
        if (psnr_g >= config_.quick_warn_threshold) {
            result.level = FrameCompareResult::WARN;
            result.passed = true;
            warned_count_++;
            
            if (config_.verbose) {
                LOG_WARN_FMT("  Frame %d: WARN (PSNR: R=%.2f G=%.2f B=%.2f dB)",
                             result.frame_index, psnr_r, psnr_g, psnr_b);
            }
        } else {
            result.level = FrameCompareResult::FAIL;
            result.passed = false;
            failed_count_++;
            
            LOG_ERROR_FMT("  Frame %d: FAIL (PSNR: R=%.2f G=%.2f B=%.2f dB)",
                          result.frame_index, psnr_r, psnr_g, psnr_b);
        }
    } else {
        result.level = FrameCompareResult::FAIL;
        result.passed = false;
        failed_count_++;
    }
    
    return result;
}

// ============================================================================
// 混合格式对比（需要转换）
// ============================================================================

FrameCompareResult BufferComparator::compareMixed(
    Buffer* ref_buffer, const FormatInfo& ref_info,
    Buffer* test_buffer, const FormatInfo& test_info
) {
    // 转换到YUV420P（标准格式）
    AVFrame* ref_yuv = convertToYUV420P(ref_buffer, ref_info);
    AVFrame* test_yuv = convertToYUV420P(test_buffer, test_info);
    
    if (!ref_yuv || !test_yuv) {
        LOG_ERROR("[BufferComparator] Format conversion failed");
        
        if (ref_yuv) freeConvertedFrame(ref_yuv);
        if (test_yuv) freeConvertedFrame(test_yuv);
        
        FrameCompareResult result;
        result.error_message = "Format conversion failed";
        result.passed = false;
        result.level = FrameCompareResult::FAIL;
        return result;
    }
    
    // 创建临时Buffer包装AVFrame
    // TODO: 实现转换后的对比
    // 由于需要创建临时Buffer，这里暂时返回错误
    
    freeConvertedFrame(ref_yuv);
    freeConvertedFrame(test_yuv);
    
    FrameCompareResult result;
    result.error_message = "Mixed format comparison not fully implemented";
    result.passed = false;
    result.level = FrameCompareResult::FAIL;
    LOG_WARN("[BufferComparator] Mixed format comparison not fully implemented");
    
    return result;
}

// ============================================================================
// PSNR计算 - 通用方法
// ============================================================================

double BufferComparator::calculatePSNR(
    const uint8_t* data1, const uint8_t* data2,
    int width, int height,
    int stride1, int stride2
) {
    if (!data1 || !data2 || width <= 0 || height <= 0) {
        return 0.0;
    }
    
    uint64_t mse = 0;
    
    // 逐行对比（考虑stride）
    for (int y = 0; y < height; y++) {
        const uint8_t* row1 = data1 + y * stride1;
        const uint8_t* row2 = data2 + y * stride2;
        
        for (int x = 0; x < width; x++) {
            int diff = (int)row1[x] - (int)row2[x];
            mse += (uint64_t)(diff * diff);
        }
    }
    
    if (mse == 0) {
        return 100.0;  // 完全一致
    }
    
    double mean_mse = (double)mse / (width * height);
    double psnr = 10.0 * log10((255.0 * 255.0) / mean_mse);
    
    return psnr;
}

// ============================================================================
// PSNR计算 - YUV格式
// ============================================================================

double BufferComparator::calculatePSNR_YUV_Y(
    Buffer* buf1, Buffer* buf2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    // 获取Y平面数据
    uint8_t* data1 = buf1->getImagePlaneData(0);
    uint8_t* data2 = buf2->getImagePlaneData(0);
    
    if (!data1 || !data2) {
        return 0.0;
    }
    
    const int* linesize1 = buf1->getImageLinesize();
    const int* linesize2 = buf2->getImageLinesize();
    
    return calculatePSNR(data1, data2, info1.width, info1.height,
                        linesize1[0], linesize2[0]);
}

double BufferComparator::calculatePSNR_YUV_U(
    Buffer* buf1, Buffer* buf2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    // 获取U平面数据
    uint8_t* data1 = buf1->getImagePlaneData(1);
    uint8_t* data2 = buf2->getImagePlaneData(1);
    
    if (!data1 || !data2) {
        return 100.0;  // 无U平面，认为一致
    }
    
    const int* linesize1 = buf1->getImageLinesize();
    const int* linesize2 = buf2->getImageLinesize();
    
    // U平面通常是原始分辨率的一半
    int uv_width = info1.width / 2;
    int uv_height = info1.height / 2;
    
    return calculatePSNR(data1, data2, uv_width, uv_height,
                        linesize1[1], linesize2[1]);
}

double BufferComparator::calculatePSNR_YUV_V(
    Buffer* buf1, Buffer* buf2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    // 获取V平面数据
    uint8_t* data1 = buf1->getImagePlaneData(2);
    uint8_t* data2 = buf2->getImagePlaneData(2);
    
    if (!data1 || !data2) {
        return 100.0;  // 无V平面，认为一致
    }
    
    const int* linesize1 = buf1->getImageLinesize();
    const int* linesize2 = buf2->getImageLinesize();
    
    int uv_width = info1.width / 2;
    int uv_height = info1.height / 2;
    
    return calculatePSNR(data1, data2, uv_width, uv_height,
                        linesize1[2], linesize2[2]);
}

// ============================================================================
// PSNR计算 - RGB格式
// ============================================================================

double BufferComparator::calculatePSNR_RGB_R(
    Buffer* buf1, Buffer* buf2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    // 简化实现：暂时使用plane 0
    // TODO: 实现RGB通道分离
    uint8_t* data1 = buf1->getImagePlaneData(0);
    uint8_t* data2 = buf2->getImagePlaneData(0);
    
    if (!data1 || !data2) {
        return 0.0;
    }
    
    const int* linesize1 = buf1->getImageLinesize();
    const int* linesize2 = buf2->getImageLinesize();
    
    // 简化：使用整体PSNR
    return calculatePSNR(data1, data2, info1.width * 3, info1.height,
                        linesize1[0], linesize2[0]);
}

double BufferComparator::calculatePSNR_RGB_G(
    Buffer* buf1, Buffer* buf2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    // G通道最重要，这里简化为使用整体PSNR
    return calculatePSNR_RGB_R(buf1, buf2, info1, info2);
}

double BufferComparator::calculatePSNR_RGB_B(
    Buffer* buf1, Buffer* buf2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    return calculatePSNR_RGB_R(buf1, buf2, info1, info2);
}

// ============================================================================
// 格式转换
// ============================================================================

AVFrame* BufferComparator::convertToYUV420P(Buffer* buffer, const FormatInfo& info) {
    if (!buffer || !buffer->hasImageMetadata()) {
        return nullptr;
    }
    
    AVFrame* src_frame = buffer->getAVFrame();
    if (!src_frame) {
        return nullptr;
    }
    
    // 如果已经是YUV420P，直接返回
    if (info.format == AV_PIX_FMT_YUV420P) {
        return av_frame_clone(src_frame);
    }
    
    // 创建目标frame
    AVFrame* dst_frame = av_frame_alloc();
    if (!dst_frame) {
        return nullptr;
    }
    
    dst_frame->format = AV_PIX_FMT_YUV420P;
    dst_frame->width = info.width;
    dst_frame->height = info.height;
    
    if (av_frame_get_buffer(dst_frame, 0) < 0) {
        av_frame_free(&dst_frame);
        return nullptr;
    }
    
    // 创建转换上下文
    SwsContext* sws_ctx = sws_getContext(
        info.width, info.height, info.format,
        info.width, info.height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!sws_ctx) {
        av_frame_free(&dst_frame);
        return nullptr;
    }
    
    // 执行转换
    sws_scale(sws_ctx, src_frame->data, src_frame->linesize, 0, info.height,
              dst_frame->data, dst_frame->linesize);
    
    sws_freeContext(sws_ctx);
    
    return dst_frame;
}

void BufferComparator::freeConvertedFrame(AVFrame* frame) {
    if (frame) {
        av_frame_free(&frame);
    }
}

// ============================================================================
// 辅助方法
// ============================================================================

void BufferComparator::updateStatistics(const FrameCompareResult& result) {
    if (result.psnr_y > 0.0) {
        sum_psnr_y_ += result.psnr_y;
        sum_psnr_u_ += result.psnr_u;
        sum_psnr_v_ += result.psnr_v;
        
        if (result.psnr_y < min_psnr_y_) {
            min_psnr_y_ = result.psnr_y;
        }
        if (result.psnr_y > max_psnr_y_) {
            max_psnr_y_ = result.psnr_y;
        }
    }
    
    // 记录失败和警告帧
    if (result.level == FrameCompareResult::FAIL) {
        failures_.push_back(result);
    } else if (result.level == FrameCompareResult::WARN) {
        warnings_.push_back(result);
    }
}

void BufferComparator::writeReport(const FrameCompareResult& result) {
    if (!report_file_) {
        return;
    }
    
    // 只记录失败和警告帧
    if (result.level == FrameCompareResult::FAIL || 
        result.level == FrameCompareResult::WARN) {
        
        fprintf(report_file_, "Frame %d: %s\n", 
                result.frame_index,
                result.level == FrameCompareResult::FAIL ? "FAIL ❌" : "WARN ⚠️");
        fprintf(report_file_, "  Formats: %s vs %s\n",
                result.ref_format_name.c_str(),
                result.test_format_name.c_str());
        fprintf(report_file_, "  PSNR: Y=%.2f U=%.2f V=%.2f dB (avg=%.2f dB)\n",
                result.psnr_y, result.psnr_u, result.psnr_v, result.psnr_avg);
        
        if (!result.error_message.empty()) {
            fprintf(report_file_, "  Error: %s\n", result.error_message.c_str());
        }
        
        fprintf(report_file_, "\n");
        fflush(report_file_);
    }
}

} // namespace io
} // namespace productionline
