#include "productionline/io/BufferComparator.hpp"
#include "common/Logger.hpp"
#include "common/GlobalThreadPool.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <future>
#include <vector>

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
    , sum_ssim_y_(0.0)
    , sum_ssim_u_(0.0)
    , sum_ssim_v_(0.0)
    , min_ssim_y_(1.0)
    , max_ssim_y_(0.0)
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
    
    // 检查：至少启用一个指标
    if (!config_.enable_psnr && !config_.enable_ssim) {
        LOG_ERROR("[BufferComparator] At least one metric (PSNR or SSIM) must be enabled");
        return false;
    }
    
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
        fprintf(report_file_, "Enabled Metrics: %s%s%s\n",
                config_.enable_psnr ? "PSNR" : "",
                (config_.enable_psnr && config_.enable_ssim) ? " + " : "",
                config_.enable_ssim ? "SSIM" : "");
        fprintf(report_file_, "Parallel Computing: %s\n",
                config_.enable_parallel ? "Enabled (using GlobalThreadPool)" : "Disabled");
        if (config_.enable_psnr) {
            fprintf(report_file_, "PSNR Threshold: Pass >= %.1f dB, Warn >= %.1f dB\n",
                    config_.min_psnr, config_.warn_psnr);
        }
        if (config_.enable_ssim) {
            fprintf(report_file_, "SSIM Threshold: Pass >= %.4f, Warn >= %.4f\n",
                    config_.min_ssim, config_.warn_ssim);
        }
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
    sum_ssim_y_ = 0.0;
    sum_ssim_u_ = 0.0;
    sum_ssim_v_ = 0.0;
    min_ssim_y_ = 1.0;
    max_ssim_y_ = 0.0;
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
            if (config_.enable_psnr) {
                fprintf(report_file_, "\nPSNR Statistics:\n");
                fprintf(report_file_, "  Average: Y=%.2f U=%.2f V=%.2f dB\n",
                        sum_psnr_y_ / compare_count_.load(),
                        sum_psnr_u_ / compare_count_.load(),
                        sum_psnr_v_ / compare_count_.load());
                fprintf(report_file_, "  Min Y: %.2f dB\n", min_psnr_y_);
                fprintf(report_file_, "  Max Y: %.2f dB\n", max_psnr_y_);
            }
            
            if (config_.enable_ssim) {
                fprintf(report_file_, "\nSSIM Statistics:\n");
                fprintf(report_file_, "  Average: Y=%.4f U=%.4f V=%.4f\n",
                        sum_ssim_y_ / compare_count_.load(),
                        sum_ssim_u_ / compare_count_.load(),
                        sum_ssim_v_ / compare_count_.load());
                fprintf(report_file_, "  Min Y: %.4f\n", min_ssim_y_);
                fprintf(report_file_, "  Max Y: %.4f\n", max_ssim_y_);
            }
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
    
    // ⭐ 统一在 compare() 返回前打印日志（修复 frame_index 不一致问题）
    if (config_.verbose) {
        // 只在失败/警告时打印，或每 50 帧打印一次进度
        if (result.level == FrameCompareResult::FAIL) {
            if (config_.enable_psnr && config_.enable_ssim) {
                LOG_ERROR_FMT("  Frame %d: FAIL (PSNR-Y: %.2f dB, SSIM-Y: %.4f)", 
                             result.frame_index, result.psnr_y, result.ssim_y);
            } else if (config_.enable_psnr) {
                LOG_ERROR_FMT("  Frame %d: FAIL (PSNR-Y: %.2f dB)", 
                             result.frame_index, result.psnr_y);
            } else {
                LOG_ERROR_FMT("  Frame %d: FAIL (SSIM-Y: %.4f)", 
                             result.frame_index, result.ssim_y);
            }
        } else if (result.level == FrameCompareResult::WARN) {
            if (config_.enable_psnr && config_.enable_ssim) {
                LOG_WARN_FMT("  Frame %d: WARN (PSNR-Y: %.2f dB, SSIM-Y: %.4f)", 
                            result.frame_index, result.psnr_y, result.ssim_y);
            } else if (config_.enable_psnr) {
                LOG_WARN_FMT("  Frame %d: WARN (PSNR-Y: %.2f dB)", 
                            result.frame_index, result.psnr_y);
            } else {
                LOG_WARN_FMT("  Frame %d: WARN (SSIM-Y: %.4f)", 
                            result.frame_index, result.ssim_y);
            }
        } else if (result.frame_index % 50 == 0) {
            // PASS: 每 50 帧打印一次进度
            if (config_.enable_psnr && config_.enable_ssim) {
                LOG_DEBUG_FMT("  Frame %d: PASS (PSNR-Y: %.2f dB, SSIM-Y: %.4f)", 
                             result.frame_index, result.psnr_y, result.ssim_y);
            } else if (config_.enable_psnr) {
                LOG_DEBUG_FMT("  Frame %d: PASS (PSNR-Y: %.2f dB)", 
                             result.frame_index, result.psnr_y);
            } else {
                LOG_DEBUG_FMT("  Frame %d: PASS (SSIM-Y: %.4f)", 
                             result.frame_index, result.ssim_y);
            }
        }
    }
    
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
        if (config_.enable_psnr) {
            LOG_INFO("");
            LOG_INFO("  PSNR Statistics:");
            LOG_INFO_FMT("    Average: Y=%.2f U=%.2f V=%.2f dB",
                         sum_psnr_y_ / compare_count_.load(),
                         sum_psnr_u_ / compare_count_.load(),
                         sum_psnr_v_ / compare_count_.load());
            LOG_INFO_FMT("    Range:   Y=[%.2f, %.2f] dB", min_psnr_y_, max_psnr_y_);
        }
        
        if (config_.enable_ssim) {
            LOG_INFO("");
            LOG_INFO("  SSIM Statistics:");
            LOG_INFO_FMT("    Average: Y=%.4f U=%.4f V=%.4f",
                         sum_ssim_y_ / compare_count_.load(),
                         sum_ssim_u_ / compare_count_.load(),
                         sum_ssim_v_ / compare_count_.load());
            LOG_INFO_FMT("    Range:   Y=[%.4f, %.4f]", min_ssim_y_, max_ssim_y_);
        }
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
    
    // ============================================================================
    // 层1：快速验证（仅Y平面）
    // ============================================================================
    
    bool quick_pass = false;
    
    // 🚀 并行计算 PSNR-Y 和 SSIM-Y（如果都启用）
    if (config_.enable_parallel && config_.enable_psnr && config_.enable_ssim) {
        // 方案B：完全并行 - 使用全局线程池
        auto& pool = GlobalThreadPool::getInstance().getThreadPool();
        
        // 提交两个异步任务
        auto future_psnr = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
            return this->calculatePSNR_YUV_Y(ref_buffer, test_buffer, ref_info, test_info);
        });
        
        auto future_ssim = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
            return this->calculateSSIM_YUV_Y(ref_buffer, test_buffer, ref_info, test_info);
        });
        
        // 等待结果
        result.psnr_y = future_psnr.get();
        result.ssim_y = future_ssim.get();
        
        // 两者都要达标
        quick_pass = (result.psnr_y >= config_.min_psnr) && 
                     (result.ssim_y >= config_.min_ssim);
    } 
    else {
        // 串行计算（兼容模式或只启用一个指标）
        if (config_.enable_psnr) {
            result.psnr_y = calculatePSNR_YUV_Y(ref_buffer, test_buffer, ref_info, test_info);
        }
        
        if (config_.enable_ssim) {
            result.ssim_y = calculateSSIM_YUV_Y(ref_buffer, test_buffer, ref_info, test_info);
        }
        
        // 判定快速通过条件（严格模式：都要满足）
        if (config_.enable_psnr && config_.enable_ssim) {
            quick_pass = (result.psnr_y >= config_.min_psnr) && 
                         (result.ssim_y >= config_.min_ssim);
        } else if (config_.enable_psnr) {
            quick_pass = (result.psnr_y >= config_.min_psnr);
        } else if (config_.enable_ssim) {
            quick_pass = (result.ssim_y >= config_.min_ssim);
        }
    }
    
    if (quick_pass) {
        // ✅ 快速通过（日志在 compare() 返回前统一打印）
        result.passed = true;
        result.level = FrameCompareResult::PASS;
        // 快速通过时，使用 Y 平面值作为平均值（因为未计算 U/V）
        result.psnr_avg = result.psnr_y;
        result.ssim_avg = result.ssim_y;
        passed_count_++;
        return result;
    }
    
    // ============================================================================
    // 层2：深度验证（U/V平面）
    // ============================================================================
    
    if (config_.strategy == CompareConfig::AUTO_LAYERED ||
        config_.strategy == CompareConfig::DEEP_ALWAYS) {
        
        // 日志在 compare() 返回前统一打印
        
        // 🚀 方案B：完全并行 - 并行计算所有 U/V 平面（最多4个任务）
        if (config_.enable_parallel && config_.enable_psnr && config_.enable_ssim) {
            // 完全并行：4 个任务（PSNR-U, PSNR-V, SSIM-U, SSIM-V）
            auto& pool = GlobalThreadPool::getInstance().getThreadPool();
            
            auto future_psnr_u = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                return this->calculatePSNR_YUV_U(ref_buffer, test_buffer, ref_info, test_info);
            });
            
            auto future_psnr_v = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                return this->calculatePSNR_YUV_V(ref_buffer, test_buffer, ref_info, test_info);
            });
            
            auto future_ssim_u = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                return this->calculateSSIM_YUV_U(ref_buffer, test_buffer, ref_info, test_info);
            });
            
            auto future_ssim_v = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                return this->calculateSSIM_YUV_V(ref_buffer, test_buffer, ref_info, test_info);
            });
            
            // 等待所有结果
            result.psnr_u = future_psnr_u.get();
            result.psnr_v = future_psnr_v.get();
            result.ssim_u = future_ssim_u.get();
            result.ssim_v = future_ssim_v.get();
            
            // 加权平均
            if (config_.use_perceptual_weighting) {
                result.psnr_avg = (result.psnr_y * 4.0 + result.psnr_u + result.psnr_v) / 6.0;
                result.ssim_avg = (result.ssim_y * 4.0 + result.ssim_u + result.ssim_v) / 6.0;
            } else {
                result.psnr_avg = (result.psnr_y + result.psnr_u + result.psnr_v) / 3.0;
                result.ssim_avg = (result.ssim_y + result.ssim_u + result.ssim_v) / 3.0;
            }
        }
        else if (config_.enable_parallel && (config_.enable_psnr || config_.enable_ssim)) {
            // 部分并行：2 个任务（PSNR-UV 或 SSIM-UV）
            auto& pool = GlobalThreadPool::getInstance().getThreadPool();
            
            if (config_.enable_psnr) {
                auto future_psnr_u = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                    return this->calculatePSNR_YUV_U(ref_buffer, test_buffer, ref_info, test_info);
                });
                
                auto future_psnr_v = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                    return this->calculatePSNR_YUV_V(ref_buffer, test_buffer, ref_info, test_info);
                });
                
                result.psnr_u = future_psnr_u.get();
                result.psnr_v = future_psnr_v.get();
                
                // 加权平均
                if (config_.use_perceptual_weighting) {
                    result.psnr_avg = (result.psnr_y * 4.0 + result.psnr_u + result.psnr_v) / 6.0;
                } else {
                    result.psnr_avg = (result.psnr_y + result.psnr_u + result.psnr_v) / 3.0;
                }
            }
            
            if (config_.enable_ssim) {
                auto future_ssim_u = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                    return this->calculateSSIM_YUV_U(ref_buffer, test_buffer, ref_info, test_info);
                });
                
                auto future_ssim_v = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                    return this->calculateSSIM_YUV_V(ref_buffer, test_buffer, ref_info, test_info);
                });
                
                result.ssim_u = future_ssim_u.get();
                result.ssim_v = future_ssim_v.get();
                
                // 加权平均
                if (config_.use_perceptual_weighting) {
                    result.ssim_avg = (result.ssim_y * 4.0 + result.ssim_u + result.ssim_v) / 6.0;
                } else {
                    result.ssim_avg = (result.ssim_y + result.ssim_u + result.ssim_v) / 3.0;
                }
            }
        }
        else {
            // 串行计算（兼容模式）
            if (config_.enable_psnr) {
                result.psnr_u = calculatePSNR_YUV_U(ref_buffer, test_buffer, ref_info, test_info);
                result.psnr_v = calculatePSNR_YUV_V(ref_buffer, test_buffer, ref_info, test_info);
                
                // 加权平均
                if (config_.use_perceptual_weighting) {
                    result.psnr_avg = (result.psnr_y * 4.0 + result.psnr_u + result.psnr_v) / 6.0;
                } else {
                    result.psnr_avg = (result.psnr_y + result.psnr_u + result.psnr_v) / 3.0;
                }
            }
            
            if (config_.enable_ssim) {
                result.ssim_u = calculateSSIM_YUV_U(ref_buffer, test_buffer, ref_info, test_info);
                result.ssim_v = calculateSSIM_YUV_V(ref_buffer, test_buffer, ref_info, test_info);
                
                // 加权平均
                if (config_.use_perceptual_weighting) {
                    result.ssim_avg = (result.ssim_y * 4.0 + result.ssim_u + result.ssim_v) / 6.0;
                } else {
                    result.ssim_avg = (result.ssim_y + result.ssim_u + result.ssim_v) / 3.0;
                }
            }
        }
        
        // 判定（严格模式：都要满足）
        bool is_warn = false;
        
        if (config_.enable_psnr && config_.enable_ssim) {
            // 两者都启用：都要满足警告阈值
            is_warn = (result.psnr_y >= config_.warn_psnr) && 
                      (result.ssim_y >= config_.warn_ssim);
        } else if (config_.enable_psnr) {
            is_warn = (result.psnr_y >= config_.warn_psnr);
        } else if (config_.enable_ssim) {
            is_warn = (result.ssim_y >= config_.warn_ssim);
        }
        
        if (is_warn) {
            result.level = FrameCompareResult::WARN;
            result.passed = true;
            warned_count_++;
            // 日志在 compare() 返回前统一打印
        } else {
            result.level = FrameCompareResult::FAIL;
            result.passed = false;
            failed_count_++;
            // 日志在 compare() 返回前统一打印
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
    
    // ============================================================================
    // 层1：快速验证（G通道，人眼最敏感）
    // ============================================================================
    
    bool quick_pass = false;
    
    // 🚀 并行计算 PSNR-G 和 SSIM-G（如果都启用）
    if (config_.enable_parallel && config_.enable_psnr && config_.enable_ssim) {
        // 方案B：完全并行 - 使用全局线程池
        auto& pool = GlobalThreadPool::getInstance().getThreadPool();
        
        auto future_psnr = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
            return this->calculatePSNR_RGB_G(ref_buffer, test_buffer, ref_info, test_info);
        });
        
        auto future_ssim = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
            return this->calculateSSIM_RGB_G(ref_buffer, test_buffer, ref_info, test_info);
        });
        
        // 等待结果
        result.psnr_y = future_psnr.get();  // G → Y
        result.ssim_y = future_ssim.get();  // G → Y
        
        // 两者都要达标
        quick_pass = (result.psnr_y >= config_.min_psnr) && 
                     (result.ssim_y >= config_.min_ssim);
    }
    else {
        // 串行计算
        if (config_.enable_psnr) {
            double psnr_g = calculatePSNR_RGB_G(ref_buffer, test_buffer, ref_info, test_info);
            result.psnr_y = psnr_g;  // 复用psnr_y字段
        }
        
        if (config_.enable_ssim) {
            double ssim_g = calculateSSIM_RGB_G(ref_buffer, test_buffer, ref_info, test_info);
            result.ssim_y = ssim_g;  // 复用ssim_y字段
        }
        
        // 判定快速通过条件（严格模式：都要满足）
        if (config_.enable_psnr && config_.enable_ssim) {
            quick_pass = (result.psnr_y >= config_.min_psnr) && 
                         (result.ssim_y >= config_.min_ssim);
        } else if (config_.enable_psnr) {
            quick_pass = (result.psnr_y >= config_.min_psnr);
        } else if (config_.enable_ssim) {
            quick_pass = (result.ssim_y >= config_.min_ssim);
        }
    }
    
    if (quick_pass) {
        // ✅ 快速通过（日志在 compare() 返回前统一打印）
        result.passed = true;
        result.level = FrameCompareResult::PASS;
        // 快速通过时，使用 G 通道值作为平均值（因为未计算 R/B）
        result.psnr_avg = result.psnr_y;  // G → psnr_y
        result.ssim_avg = result.ssim_y;  // G → ssim_y
        passed_count_++;
        return result;
    }
    
    // ============================================================================
    // 层2：深度验证（R/B通道）
    // ============================================================================
    
    if (config_.strategy == CompareConfig::AUTO_LAYERED ||
        config_.strategy == CompareConfig::DEEP_ALWAYS) {
        
        // 日志在 compare() 返回前统一打印
        if (false) {  // 保留代码结构，但不执行日志打印
            if (config_.enable_psnr && config_.enable_ssim) {
                LOG_WARN_FMT("  Frame %d: PSNR-G=%.2f dB, SSIM-G=%.4f, deep validation...",
                             result.frame_index, result.psnr_y, result.ssim_y);
            } else if (config_.enable_psnr) {
                LOG_WARN_FMT("  Frame %d: PSNR-G=%.2f dB < %.2f dB, deep validation...",
                             result.frame_index, result.psnr_y, config_.min_psnr);
            } else {
                LOG_WARN_FMT("  Frame %d: SSIM-G=%.4f < %.4f, deep validation...",
                             result.frame_index, result.ssim_y, config_.min_ssim);
            }
        }
        
        double psnr_g = result.psnr_y;  // 保存已计算的 G 通道值
        double ssim_g = result.ssim_y;
        
        // 🚀 方案B：完全并行 - 并行计算 R/B 通道（最多4个任务）
        if (config_.enable_parallel && config_.enable_psnr && config_.enable_ssim) {
            // 完全并行：4 个任务（PSNR-R, PSNR-B, SSIM-R, SSIM-B）
            auto& pool = GlobalThreadPool::getInstance().getThreadPool();
            
            auto future_psnr_r = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                return this->calculatePSNR_RGB_R(ref_buffer, test_buffer, ref_info, test_info);
            });
            
            auto future_psnr_b = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                return this->calculatePSNR_RGB_B(ref_buffer, test_buffer, ref_info, test_info);
            });
            
            auto future_ssim_r = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                return this->calculateSSIM_RGB_R(ref_buffer, test_buffer, ref_info, test_info);
            });
            
            auto future_ssim_b = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                return this->calculateSSIM_RGB_B(ref_buffer, test_buffer, ref_info, test_info);
            });
            
            // 等待所有结果
            double psnr_r = future_psnr_r.get();
            double psnr_b = future_psnr_b.get();
            double ssim_r = future_ssim_r.get();
            double ssim_b = future_ssim_b.get();
            
            // 加权平均（G通道权重更高）
            if (config_.use_perceptual_weighting) {
                result.psnr_avg = (psnr_r + psnr_g * 2.0 + psnr_b) / 4.0;
                result.ssim_avg = (ssim_r + ssim_g * 2.0 + ssim_b) / 4.0;
            } else {
                result.psnr_avg = (psnr_r + psnr_g + psnr_b) / 3.0;
                result.ssim_avg = (ssim_r + ssim_g + ssim_b) / 3.0;
            }
            
            // 存储到结果（复用YUV字段）
            result.psnr_y = psnr_g;  // G → Y
            result.psnr_u = psnr_r;  // R → U
            result.psnr_v = psnr_b;  // B → V
            result.ssim_y = ssim_g;  // G → Y
            result.ssim_u = ssim_r;  // R → U
            result.ssim_v = ssim_b;  // B → V
        }
        else if (config_.enable_parallel && (config_.enable_psnr || config_.enable_ssim)) {
            // 部分并行：2 个任务
            auto& pool = GlobalThreadPool::getInstance().getThreadPool();
            
            if (config_.enable_psnr) {
                auto future_psnr_r = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                    return this->calculatePSNR_RGB_R(ref_buffer, test_buffer, ref_info, test_info);
                });
                
                auto future_psnr_b = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                    return this->calculatePSNR_RGB_B(ref_buffer, test_buffer, ref_info, test_info);
                });
                
                double psnr_r = future_psnr_r.get();
                double psnr_b = future_psnr_b.get();
                
                // 加权平均
                if (config_.use_perceptual_weighting) {
                    result.psnr_avg = (psnr_r + psnr_g * 2.0 + psnr_b) / 4.0;
                } else {
                    result.psnr_avg = (psnr_r + psnr_g + psnr_b) / 3.0;
                }
                
                // 存储到结果
                result.psnr_y = psnr_g;  // G → Y
                result.psnr_u = psnr_r;  // R → U
                result.psnr_v = psnr_b;  // B → V
            }
            
            if (config_.enable_ssim) {
                auto future_ssim_r = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                    return this->calculateSSIM_RGB_R(ref_buffer, test_buffer, ref_info, test_info);
                });
                
                auto future_ssim_b = pool.submit_task([this, ref_buffer, test_buffer, &ref_info, &test_info]() {
                    return this->calculateSSIM_RGB_B(ref_buffer, test_buffer, ref_info, test_info);
                });
                
                double ssim_r = future_ssim_r.get();
                double ssim_b = future_ssim_b.get();
                
                // 加权平均
                if (config_.use_perceptual_weighting) {
                    result.ssim_avg = (ssim_r + ssim_g * 2.0 + ssim_b) / 4.0;
                } else {
                    result.ssim_avg = (ssim_r + ssim_g + ssim_b) / 3.0;
                }
                
                // 存储到结果
                result.ssim_y = ssim_g;  // G → Y
                result.ssim_u = ssim_r;  // R → U
                result.ssim_v = ssim_b;  // B → V
            }
        }
        else {
            // 串行计算（兼容模式）
            if (config_.enable_psnr) {
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
            }
            
            if (config_.enable_ssim) {
                double ssim_r = calculateSSIM_RGB_R(ref_buffer, test_buffer, ref_info, test_info);
                double ssim_b = calculateSSIM_RGB_B(ref_buffer, test_buffer, ref_info, test_info);
                
                // 加权平均（G通道权重更高）
                if (config_.use_perceptual_weighting) {
                    result.ssim_avg = (ssim_r + ssim_g * 2.0 + ssim_b) / 4.0;
                } else {
                    result.ssim_avg = (ssim_r + ssim_g + ssim_b) / 3.0;
                }
                
                // 存储到结果（复用YUV字段）
                result.ssim_y = ssim_g;  // G → Y
                result.ssim_u = ssim_r;  // R → U
                result.ssim_v = ssim_b;  // B → V
            }
        }
        
        // 判定（严格模式：都要满足）
        bool is_warn = false;
        
        if (config_.enable_psnr && config_.enable_ssim) {
            is_warn = (result.psnr_y >= config_.warn_psnr) && 
                      (result.ssim_y >= config_.warn_ssim);
        } else if (config_.enable_psnr) {
            is_warn = (result.psnr_y >= config_.warn_psnr);
        } else if (config_.enable_ssim) {
            is_warn = (result.ssim_y >= config_.warn_ssim);
        }
        
        if (is_warn) {
            result.level = FrameCompareResult::WARN;
            result.passed = true;
            warned_count_++;
            // 日志在 compare() 返回前统一打印
        } else {
            result.level = FrameCompareResult::FAIL;
            result.passed = false;
            failed_count_++;
            // 日志在 compare() 返回前统一打印
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
    FrameCompareResult result;
    
    // 情况1：ref是YUV，test是RGB → 将ref转换为RGB后对比
    if (ref_info.is_yuv && test_info.is_rgb) {
        if (config_.verbose) {
            LOG_DEBUG("[BufferComparator] Converting YUV (ref) to RGB for comparison");
        }
        
        // 将YUV转换为RGB（使用test的RGB格式）
        AVFrame* ref_rgb = convertYUVToRGB(ref_buffer, ref_info, test_info.format);
        
        if (!ref_rgb) {
            LOG_ERROR("[BufferComparator] Failed to convert YUV to RGB");
            result.error_message = "YUV to RGB conversion failed";
            result.passed = false;
            result.level = FrameCompareResult::FAIL;
            return result;
        }
        
        // 创建临时Buffer包装转换后的AVFrame
        Buffer temp_ref_buffer(
            0,  // 临时ID
            ref_rgb->data[0],  // 虚拟地址
            0,  // 物理地址
            ref_rgb->linesize[0] * ref_rgb->height,  // 大小
            Buffer::Ownership::EXTERNAL  // 外部管理（AVFrame）
        );
        
        // 设置AVFrame关联
        temp_ref_buffer.setAVFrame(ref_rgb);
        
        // 设置图像元数据
        temp_ref_buffer.setImageMetadataFromAVFrame(ref_rgb);
        
        // 创建转换后的FormatInfo（使用analyzeFormat的方式初始化）
        FormatInfo ref_rgb_info = {};
        ref_rgb_info.format = test_info.format;  // 使用test的RGB格式
        ref_rgb_info.width = ref_rgb->width;
        ref_rgb_info.height = ref_rgb->height;
        ref_rgb_info.name = av_get_pix_fmt_name(ref_rgb_info.format);
        
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(ref_rgb_info.format);
        if (desc) {
            ref_rgb_info.num_planes = desc->nb_components;
            ref_rgb_info.is_planar = !(desc->flags & AV_PIX_FMT_FLAG_RGB);
            ref_rgb_info.is_rgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
            ref_rgb_info.is_yuv = false;
        } else {
            ref_rgb_info.is_rgb = true;
            ref_rgb_info.is_yuv = false;
            ref_rgb_info.is_planar = (ref_rgb_info.format == AV_PIX_FMT_GBRP);
            ref_rgb_info.num_planes = ref_rgb_info.is_planar ? 3 : 1;
        }
        
        // 使用RGB对比函数进行对比
        result = compareRGB(&temp_ref_buffer, ref_rgb_info, test_buffer, test_info);
        
        // 清理：释放转换后的AVFrame（Buffer析构时不会释放，因为Ownership::EXTERNAL）
        freeConvertedFrame(ref_rgb);
        
        return result;
    }
    
    // 情况2：ref是RGB，test是YUV → 将test转换为RGB后对比
    if (ref_info.is_rgb && test_info.is_yuv) {
        if (config_.verbose) {
            LOG_DEBUG("[BufferComparator] Converting YUV (test) to RGB for comparison");
        }
        
        // 将YUV转换为RGB（使用ref的RGB格式）
        AVFrame* test_rgb = convertYUVToRGB(test_buffer, test_info, ref_info.format);
        
        if (!test_rgb) {
            LOG_ERROR("[BufferComparator] Failed to convert YUV to RGB");
            result.error_message = "YUV to RGB conversion failed";
            result.passed = false;
            result.level = FrameCompareResult::FAIL;
            return result;
        }
        
        // 创建临时Buffer包装转换后的AVFrame
        Buffer temp_test_buffer(
            0,  // 临时ID
            test_rgb->data[0],  // 虚拟地址
            0,  // 物理地址
            test_rgb->linesize[0] * test_rgb->height,  // 大小
            Buffer::Ownership::EXTERNAL  // 外部管理（AVFrame）
        );
        
        // 设置AVFrame关联
        temp_test_buffer.setAVFrame(test_rgb);
        
        // 设置图像元数据
        temp_test_buffer.setImageMetadataFromAVFrame(test_rgb);
        
        // 创建转换后的FormatInfo（使用analyzeFormat的方式初始化）
        FormatInfo test_rgb_info = {};
        test_rgb_info.format = ref_info.format;  // 使用ref的RGB格式
        test_rgb_info.width = test_rgb->width;
        test_rgb_info.height = test_rgb->height;
        test_rgb_info.name = av_get_pix_fmt_name(test_rgb_info.format);
        
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(test_rgb_info.format);
        if (desc) {
            test_rgb_info.num_planes = desc->nb_components;
            test_rgb_info.is_planar = !(desc->flags & AV_PIX_FMT_FLAG_RGB);
            test_rgb_info.is_rgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
            test_rgb_info.is_yuv = false;
        } else {
            test_rgb_info.is_rgb = true;
            test_rgb_info.is_yuv = false;
            test_rgb_info.is_planar = (test_rgb_info.format == AV_PIX_FMT_GBRP);
            test_rgb_info.num_planes = test_rgb_info.is_planar ? 3 : 1;
        }
        
        // 使用RGB对比函数进行对比
        result = compareRGB(ref_buffer, ref_info, &temp_test_buffer, test_rgb_info);
        
        // 清理：释放转换后的AVFrame
        freeConvertedFrame(test_rgb);
        
        return result;
    }
    
    // 其他情况：都转换为YUV420P（用于其他混合格式对比）
    AVFrame* ref_yuv = convertToYUV420P(ref_buffer, ref_info);
    AVFrame* test_yuv = convertToYUV420P(test_buffer, test_info);
    
    if (!ref_yuv || !test_yuv) {
        LOG_ERROR("[BufferComparator] Format conversion failed");
        
        if (ref_yuv) freeConvertedFrame(ref_yuv);
        if (test_yuv) freeConvertedFrame(test_yuv);
        
        result.error_message = "Format conversion failed";
        result.passed = false;
        result.level = FrameCompareResult::FAIL;
        return result;
    }
    
    // 创建临时Buffer包装转换后的AVFrame
    Buffer temp_ref_yuv(
        0,
        ref_yuv->data[0],
        0,
        ref_yuv->linesize[0] * ref_yuv->height,
        Buffer::Ownership::EXTERNAL
    );
    temp_ref_yuv.setAVFrame(ref_yuv);
    temp_ref_yuv.setImageMetadataFromAVFrame(ref_yuv);
    
    Buffer temp_test_yuv(
        0,
        test_yuv->data[0],
        0,
        test_yuv->linesize[0] * test_yuv->height,
        Buffer::Ownership::EXTERNAL
    );
    temp_test_yuv.setAVFrame(test_yuv);
    temp_test_yuv.setImageMetadataFromAVFrame(test_yuv);
    
    // 创建转换后的FormatInfo（使用analyzeFormat的方式初始化）
    FormatInfo ref_yuv_info = {};
    ref_yuv_info.format = AV_PIX_FMT_YUV420P;
    ref_yuv_info.width = ref_yuv->width;
    ref_yuv_info.height = ref_yuv->height;
    ref_yuv_info.name = av_get_pix_fmt_name(ref_yuv_info.format);
    
    const AVPixFmtDescriptor* desc_yuv = av_pix_fmt_desc_get(ref_yuv_info.format);
    if (desc_yuv) {
        ref_yuv_info.num_planes = desc_yuv->nb_components;
        ref_yuv_info.is_planar = !(desc_yuv->flags & AV_PIX_FMT_FLAG_RGB);
        ref_yuv_info.is_yuv = true;
        ref_yuv_info.is_rgb = false;
    } else {
        ref_yuv_info.is_yuv = true;
        ref_yuv_info.is_rgb = false;
        ref_yuv_info.is_planar = true;
        ref_yuv_info.num_planes = 3;
    }
    
    FormatInfo test_yuv_info = ref_yuv_info;
    test_yuv_info.width = test_yuv->width;
    test_yuv_info.height = test_yuv->height;
    
    // 使用YUV对比函数进行对比
    result = compareYUV(&temp_ref_yuv, ref_yuv_info, &temp_test_yuv, test_yuv_info);
    
    // 清理：释放转换后的AVFrame
    freeConvertedFrame(ref_yuv);
    freeConvertedFrame(test_yuv);
    
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

/**
 * @brief 提取RGB通道到临时缓冲区（用于通道分离计算）
 * @param buffer Buffer指针
 * @param format 像素格式
 * @param width 宽度
 * @param height 高度
 * @param channel 通道索引：0=R, 1=G, 2=B
 * @param out_data 输出缓冲区（需要预先分配 width*height 字节）
 * @return 成功返回true
 */
static bool extractRGBChannel(
    Buffer* buffer, AVPixelFormat format, int width, int height,
    int channel, uint8_t* out_data
) {
    if (!buffer || !out_data || channel < 0 || channel > 2) {
        return false;
    }
    
    uint8_t* src_data = buffer->getImagePlaneData(0);
    const int* linesize = buffer->getImageLinesize();
    
    if (!src_data || !linesize) {
        return false;
    }
    
    int src_stride = linesize[0];
    
    // Planar格式（GBRP）：直接使用对应plane
    if (format == AV_PIX_FMT_GBRP) {
        uint8_t* plane_data = buffer->getImagePlaneData(channel);
        if (!plane_data) return false;
        int plane_stride = linesize[channel];
        
        for (int y = 0; y < height; y++) {
            memcpy(out_data + y * width, plane_data + y * plane_stride, width);
        }
        return true;
    }
    
    // Packed格式：需要从打包数据中提取
    int bytes_per_pixel = 0;
    int r_offset = -1, g_offset = -1, b_offset = -1;
    
    switch (format) {
        case AV_PIX_FMT_RGB24:
            bytes_per_pixel = 3;
            r_offset = 0; g_offset = 1; b_offset = 2;
            break;
        case AV_PIX_FMT_BGR24:
            bytes_per_pixel = 3;
            r_offset = 2; g_offset = 1; b_offset = 0;
            break;
        case AV_PIX_FMT_ARGB:
            bytes_per_pixel = 4;
            r_offset = 1; g_offset = 2; b_offset = 3;
            break;
        case AV_PIX_FMT_RGBA:
            bytes_per_pixel = 4;
            r_offset = 0; g_offset = 1; b_offset = 2;
            break;
        case AV_PIX_FMT_ABGR:
            bytes_per_pixel = 4;
            r_offset = 3; g_offset = 2; b_offset = 1;
            break;
        case AV_PIX_FMT_BGRA:
            bytes_per_pixel = 4;
            r_offset = 2; g_offset = 1; b_offset = 0;
            break;
        case AV_PIX_FMT_RGB0:
            bytes_per_pixel = 4;
            r_offset = 0; g_offset = 1; b_offset = 2;
            break;
        case AV_PIX_FMT_BGR0:
            bytes_per_pixel = 4;
            r_offset = 2; g_offset = 1; b_offset = 0;
            break;
        case AV_PIX_FMT_0RGB:
            bytes_per_pixel = 4;
            r_offset = 1; g_offset = 2; b_offset = 3;
            break;
        case AV_PIX_FMT_0BGR:
            bytes_per_pixel = 4;
            r_offset = 3; g_offset = 2; b_offset = 1;
            break;
        default:
            return false;  // 不支持的格式
    }
    
    int channel_offset = (channel == 0) ? r_offset : (channel == 1) ? g_offset : b_offset;
    if (channel_offset < 0) return false;
    
    // 提取通道数据
    for (int y = 0; y < height; y++) {
        const uint8_t* src_row = src_data + y * src_stride;
        uint8_t* dst_row = out_data + y * width;
        
        for (int x = 0; x < width; x++) {
            dst_row[x] = src_row[x * bytes_per_pixel + channel_offset];
        }
    }
    
    return true;
}

double BufferComparator::calculatePSNR_RGB_R(
    Buffer* buf1, Buffer* buf2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    if (!buf1 || !buf2) return 0.0;
    
    // 分配临时缓冲区
    std::vector<uint8_t> channel1(info1.width * info1.height);
    std::vector<uint8_t> channel2(info2.width * info2.height);
    
    if (!extractRGBChannel(buf1, info1.format, info1.width, info1.height, 0, channel1.data()) ||
        !extractRGBChannel(buf2, info2.format, info2.width, info2.height, 0, channel2.data())) {
        return 0.0;
    }
    
    // 计算R通道PSNR
    return calculatePSNR(channel1.data(), channel2.data(), 
                        info1.width, info1.height,
                        info1.width, info2.width);
}

double BufferComparator::calculatePSNR_RGB_G(
    Buffer* buf1, Buffer* buf2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    if (!buf1 || !buf2) return 0.0;
    
    std::vector<uint8_t> channel1(info1.width * info1.height);
    std::vector<uint8_t> channel2(info2.width * info2.height);
    
    if (!extractRGBChannel(buf1, info1.format, info1.width, info1.height, 1, channel1.data()) ||
        !extractRGBChannel(buf2, info2.format, info2.width, info2.height, 1, channel2.data())) {
        return 0.0;
    }
    
    return calculatePSNR(channel1.data(), channel2.data(), 
                        info1.width, info1.height,
                        info1.width, info2.width);
}

double BufferComparator::calculatePSNR_RGB_B(
    Buffer* buf1, Buffer* buf2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    if (!buf1 || !buf2) return 0.0;
    
    std::vector<uint8_t> channel1(info1.width * info1.height);
    std::vector<uint8_t> channel2(info2.width * info2.height);
    
    if (!extractRGBChannel(buf1, info1.format, info1.width, info1.height, 2, channel1.data()) ||
        !extractRGBChannel(buf2, info2.format, info2.width, info2.height, 2, channel2.data())) {
        return 0.0;
    }
    
    return calculatePSNR(channel1.data(), channel2.data(), 
                        info1.width, info1.height,
                        info1.width, info2.width);
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

/**
 * @brief 将YUV格式转换为RGB格式
 * @param buffer YUV格式的Buffer
 * @param info YUV格式信息
 * @param target_rgb_format 目标RGB格式
 * @return 转换后的AVFrame，失败返回nullptr
 */
AVFrame* BufferComparator::convertYUVToRGB(
    Buffer* buffer, const FormatInfo& info, AVPixelFormat target_rgb_format
) {
    if (!buffer || !buffer->hasImageMetadata()) {
        return nullptr;
    }
    
    AVFrame* src_frame = buffer->getAVFrame();
    if (!src_frame) {
        return nullptr;
    }
    
    // 创建目标frame
    AVFrame* dst_frame = av_frame_alloc();
    if (!dst_frame) {
        return nullptr;
    }
    
    dst_frame->format = target_rgb_format;
    dst_frame->width = info.width;
    dst_frame->height = info.height;
    
    if (av_frame_get_buffer(dst_frame, 0) < 0) {
        av_frame_free(&dst_frame);
        return nullptr;
    }
    
    // 创建转换上下文（YUV -> RGB）
    SwsContext* sws_ctx = sws_getContext(
        info.width, info.height, info.format,
        info.width, info.height, target_rgb_format,
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
    if (config_.enable_psnr && result.psnr_y > 0.0) {
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
    
    if (config_.enable_ssim && result.ssim_y > 0.0) {
        sum_ssim_y_ += result.ssim_y;
        sum_ssim_u_ += result.ssim_u;
        sum_ssim_v_ += result.ssim_v;
        
        if (result.ssim_y < min_ssim_y_) {
            min_ssim_y_ = result.ssim_y;
        }
        if (result.ssim_y > max_ssim_y_) {
            max_ssim_y_ = result.ssim_y;
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
        
        if (config_.enable_psnr) {
            fprintf(report_file_, "  PSNR: Y=%.2f U=%.2f V=%.2f dB (avg=%.2f dB)\n",
                    result.psnr_y, result.psnr_u, result.psnr_v, result.psnr_avg);
        }
        
        if (config_.enable_ssim) {
            fprintf(report_file_, "  SSIM: Y=%.4f U=%.4f V=%.4f (avg=%.4f)\n",
                    result.ssim_y, result.ssim_u, result.ssim_v, result.ssim_avg);
        }
        
        if (!result.error_message.empty()) {
            fprintf(report_file_, "  Error: %s\n", result.error_message.c_str());
        }
        
        fprintf(report_file_, "\n");
        fflush(report_file_);
    }
}

// ============================================================================
// SSIM计算 - 核心算法
// ============================================================================

/**
 * @brief SSIM 核心算法实现
 * 
 * SSIM = [亮度对比] × [对比度对比] × [结构对比]
 *      = l(x,y) × c(x,y) × s(x,y)
 * 
 * 其中：
 * - l(x,y) = (2*μx*μy + C1) / (μx² + μy² + C1)      亮度
 * - c(x,y) = (2*σx*σy + C2) / (σx² + σy² + C2)      对比度
 * - s(x,y) = (σxy + C3) / (σx*σy + C3)              结构
 * 
 * 简化公式（C3 = C2/2）：
 * SSIM = [(2*μx*μy + C1)(2*σxy + C2)] / [(μx² + μy² + C1)(σx² + σy² + C2)]
 */
double BufferComparator::calculateSSIM(
    const uint8_t* data1, const uint8_t* data2,
    int width, int height,
    int stride1, int stride2
) {
    if (!data1 || !data2 || width <= 0 || height <= 0) {
        return 0.0;
    }
    
    // SSIM 常量（ITU-T 标准）
    const double C1 = (0.01 * 255) * (0.01 * 255);  // (K1*L)^2, K1=0.01, L=255
    const double C2 = (0.03 * 255) * (0.03 * 255);  // (K2*L)^2, K2=0.03
    
    // 使用滑动窗口（8x8，简化版，标准是11x11高斯窗口）
    const int window_size = 8;
    const int step = 8;  // 步长=窗口大小，避免重叠（加速计算）
    
    double ssim_sum = 0.0;
    int window_count = 0;
    
    // 滑动窗口遍历
    for (int y = 0; y <= height - window_size; y += step) {
        for (int x = 0; x <= width - window_size; x += step) {
            // 计算窗口内的统计量
            double sum1 = 0.0, sum2 = 0.0;
            double sum1_sq = 0.0, sum2_sq = 0.0;
            double sum12 = 0.0;
            int n = window_size * window_size;
            
            for (int wy = 0; wy < window_size; wy++) {
                const uint8_t* row1 = data1 + (y + wy) * stride1 + x;
                const uint8_t* row2 = data2 + (y + wy) * stride2 + x;
                
                for (int wx = 0; wx < window_size; wx++) {
                    double p1 = row1[wx];
                    double p2 = row2[wx];
                    
                    sum1 += p1;
                    sum2 += p2;
                    sum1_sq += p1 * p1;
                    sum2_sq += p2 * p2;
                    sum12 += p1 * p2;
                }
            }
            
            // 计算均值和方差
            double mean1 = sum1 / n;
            double mean2 = sum2 / n;
            double variance1 = (sum1_sq / n) - (mean1 * mean1);
            double variance2 = (sum2_sq / n) - (mean2 * mean2);
            double covariance = (sum12 / n) - (mean1 * mean2);
            
            // 计算 SSIM
            double numerator = (2.0 * mean1 * mean2 + C1) * (2.0 * covariance + C2);
            double denominator = (mean1 * mean1 + mean2 * mean2 + C1) * 
                                (variance1 + variance2 + C2);
            
            double ssim = numerator / denominator;
            ssim_sum += ssim;
            window_count++;
        }
    }
    
    if (window_count == 0) {
        return 1.0;  // 窗口太小，认为相同
    }
    
    double mean_ssim = ssim_sum / window_count;
    
    // SSIM 范围应该在 [0, 1]，但实际计算可能略小于0（数值误差）
    if (mean_ssim < 0.0) mean_ssim = 0.0;
    if (mean_ssim > 1.0) mean_ssim = 1.0;
    
    return mean_ssim;
}

// ============================================================================
// SSIM计算 - YUV格式
// ============================================================================

double BufferComparator::calculateSSIM_YUV_Y(
    Buffer* buf1, Buffer* buf2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    uint8_t* data1 = buf1->getImagePlaneData(0);
    uint8_t* data2 = buf2->getImagePlaneData(0);
    
    if (!data1 || !data2) {
        return 0.0;
    }
    
    const int* linesize1 = buf1->getImageLinesize();
    const int* linesize2 = buf2->getImageLinesize();
    
    return calculateSSIM(data1, data2, info1.width, info1.height,
                        linesize1[0], linesize2[0]);
}

double BufferComparator::calculateSSIM_YUV_U(
    Buffer* buf1, Buffer* buf2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    uint8_t* data1 = buf1->getImagePlaneData(1);
    uint8_t* data2 = buf2->getImagePlaneData(1);
    
    if (!data1 || !data2) {
        return 1.0;  // 无U平面，认为一致
    }
    
    const int* linesize1 = buf1->getImageLinesize();
    const int* linesize2 = buf2->getImageLinesize();
    
    int uv_width = info1.width / 2;
    int uv_height = info1.height / 2;
    
    return calculateSSIM(data1, data2, uv_width, uv_height,
                        linesize1[1], linesize2[1]);
}

double BufferComparator::calculateSSIM_YUV_V(
    Buffer* buf1, Buffer* buf2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    uint8_t* data1 = buf1->getImagePlaneData(2);
    uint8_t* data2 = buf2->getImagePlaneData(2);
    
    if (!data1 || !data2) {
        return 1.0;
    }
    
    const int* linesize1 = buf1->getImageLinesize();
    const int* linesize2 = buf2->getImageLinesize();
    
    int uv_width = info1.width / 2;
    int uv_height = info1.height / 2;
    
    return calculateSSIM(data1, data2, uv_width, uv_height,
                        linesize1[2], linesize2[2]);
}

// ============================================================================
// SSIM计算 - RGB格式
// ============================================================================

double BufferComparator::calculateSSIM_RGB_R(
    Buffer* buf1, Buffer* buf2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    if (!buf1 || !buf2) return 0.0;
    
    std::vector<uint8_t> channel1(info1.width * info1.height);
    std::vector<uint8_t> channel2(info2.width * info2.height);
    
    if (!extractRGBChannel(buf1, info1.format, info1.width, info1.height, 0, channel1.data()) ||
        !extractRGBChannel(buf2, info2.format, info2.width, info2.height, 0, channel2.data())) {
        return 0.0;
    }
    
    return calculateSSIM(channel1.data(), channel2.data(), 
                       info1.width, info1.height,
                       info1.width, info2.width);
}

double BufferComparator::calculateSSIM_RGB_G(
    Buffer* buf1, Buffer* buf2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    if (!buf1 || !buf2) return 0.0;
    
    std::vector<uint8_t> channel1(info1.width * info1.height);
    std::vector<uint8_t> channel2(info2.width * info2.height);
    
    if (!extractRGBChannel(buf1, info1.format, info1.width, info1.height, 1, channel1.data()) ||
        !extractRGBChannel(buf2, info2.format, info2.width, info2.height, 1, channel2.data())) {
        return 0.0;
    }
    
    return calculateSSIM(channel1.data(), channel2.data(), 
                       info1.width, info1.height,
                       info1.width, info2.width);
}

double BufferComparator::calculateSSIM_RGB_B(
    Buffer* buf1, Buffer* buf2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    if (!buf1 || !buf2) return 0.0;
    
    std::vector<uint8_t> channel1(info1.width * info1.height);
    std::vector<uint8_t> channel2(info2.width * info2.height);
    
    if (!extractRGBChannel(buf1, info1.format, info1.width, info1.height, 2, channel1.data()) ||
        !extractRGBChannel(buf2, info2.format, info2.width, info2.height, 2, channel2.data())) {
        return 0.0;
    }
    
    return calculateSSIM(channel1.data(), channel2.data(), 
                       info1.width, info1.height,
                       info1.width, info2.width);
}

} // namespace io
} // namespace productionline
