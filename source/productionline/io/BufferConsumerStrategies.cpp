/**
 * @file BufferConsumerStrategies.cpp
 * @brief Buffer 消费策略实现
 */

#include "productionline/io/BufferConsumerStrategies.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <sstream>
#include <iomanip>

namespace consumer {

// ============================================================
// CountConsumer 实现
// ============================================================

bool CountConsumer::consume(const std::vector<Buffer*>& buffers, int frame_index) {
    (void)buffers;
    (void)frame_index;
    frame_count_.fetch_add(1);
    return true;
}

std::string CountConsumer::getStats() const {
    std::ostringstream oss;
    oss << "CountConsumer: " << frame_count_.load() << " frames";
    return oss.str();
}

// ============================================================
// DisplayConsumer 实现
// ============================================================

DisplayConsumer::DisplayConsumer(int device_id)
    : device_id_(device_id)
    , display_(nullptr)
    , success_count_(0)
    , failed_count_(0)
    , initialized_(false)
{
}

DisplayConsumer::~DisplayConsumer() {
    finalize();
}

bool DisplayConsumer::initialize(const std::vector<Buffer*>& first_buffers) {
    if (initialized_) {
        return true;
    }
    
    if (first_buffers.empty() || !first_buffers[0]) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), "DisplayConsumer: No buffer for initialization");
        return false;
    }
    
    try {
        display_ = std::make_unique<LinuxFramebufferDevice>();
        if (!display_->initialize(device_id_)) {
            LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), "DisplayConsumer: Failed to open framebuffer device");
            return false;
        }
        initialized_ = true;
        LOG4CPLUS_INFO(log4cplus::Logger::getRoot(), "DisplayConsumer: Initialized successfully");
        return true;
    } catch (const std::exception& e) {
        LOG4CPLUS_ERROR_FMT(log4cplus::Logger::getRoot(), 
            "DisplayConsumer: Exception during initialization: %s", e.what());
        return false;
    }
}

bool DisplayConsumer::consume(const std::vector<Buffer*>& buffers, int frame_index) {
    (void)frame_index;
    
    if (!initialized_ || !display_ || buffers.empty() || !buffers[0]) {
        failed_count_++;
        return true;
    }
    
    Buffer* buffer = buffers[0];
    
    // 显示 Buffer 内容
    if (display_->displayBuffer(buffer)) {
        success_count_++;
    } else {
        failed_count_++;
    }
    
    return true;
}

void DisplayConsumer::finalize() {
    if (display_) {
        display_->cleanup();
        display_.reset();
    }
    initialized_ = false;
}

std::string DisplayConsumer::getStats() const {
    std::ostringstream oss;
    oss << "DisplayConsumer: " << success_count_ << " displayed, "
        << failed_count_ << " failed";
    return oss.str();
}

// ============================================================
// SaveRawConsumer 实现
// ============================================================

SaveRawConsumer::SaveRawConsumer(const std::string& output_path, int max_frames)
    : output_path_(output_path)
    , max_frames_(max_frames)
    , writer_(nullptr)
    , saved_count_(0)
    , initialized_(false)
{
}

SaveRawConsumer::~SaveRawConsumer() {
    finalize();
}

bool SaveRawConsumer::initialize(const std::vector<Buffer*>& first_buffers) {
    if (initialized_) {
        return true;
    }
    
    if (first_buffers.empty() || !first_buffers[0]) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), "SaveRawConsumer: No buffer for initialization");
        return false;
    }
    
    if (output_path_.empty()) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), "SaveRawConsumer: Output path is empty");
        return false;
    }
    
    try {
        // 从首帧 Buffer 获取图像元数据
        Buffer* first_buffer = first_buffers[0];
        AVPixelFormat format = first_buffer->getImageFormat();
        int width = first_buffer->getImageWidth();
        int height = first_buffer->getImageHeight();
        
        writer_ = std::make_unique<productionline::io::BufferWriter>();
        if (!writer_->openRaw(output_path_.c_str(), format, width, height)) {
            LOG4CPLUS_ERROR_FMT(log4cplus::Logger::getRoot(), 
                "SaveRawConsumer: Failed to open output file: %s", output_path_.c_str());
            return false;
        }
        initialized_ = true;
        LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(), 
            "SaveRawConsumer: Initialized, output: %s", output_path_.c_str());
        return true;
    } catch (const std::exception& e) {
        LOG4CPLUS_ERROR_FMT(log4cplus::Logger::getRoot(), 
            "SaveRawConsumer: Exception during initialization: %s", e.what());
        return false;
    }
}

bool SaveRawConsumer::consume(const std::vector<Buffer*>& buffers, int frame_index) {
    (void)frame_index;
    
    if (!initialized_ || !writer_ || buffers.empty() || !buffers[0]) {
        return true;
    }
    
    // 检查是否达到最大保存帧数
    if (max_frames_ > 0 && saved_count_ >= max_frames_) {
        return true;
    }
    
    Buffer* buffer = buffers[0];
    
    if (writer_->write(buffer)) {
        saved_count_++;
    }
    
    return true;
}

void SaveRawConsumer::finalize() {
    if (writer_) {
        writer_->close();
        writer_.reset();
    }
    initialized_ = false;
    LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(), 
        "SaveRawConsumer: Finalized, saved %d frames to %s", 
        saved_count_, output_path_.c_str());
}

std::string SaveRawConsumer::getStats() const {
    std::ostringstream oss;
    oss << "SaveRawConsumer: " << saved_count_ << " frames saved to " << output_path_;
    return oss.str();
}

// ============================================================
// SaveEncodedConsumer 实现
// ============================================================

SaveEncodedConsumer::SaveEncodedConsumer(
    const std::string& output_path,
    const struct AVCodecParameters* codec_params,
    AVRational time_base
)
    : output_path_(output_path)
    , codec_params_(codec_params)
    , time_base_(time_base)
    , writer_(nullptr)
    , packet_count_(0)
    , initialized_(false)
{
}

SaveEncodedConsumer::~SaveEncodedConsumer() {
    finalize();
}

bool SaveEncodedConsumer::initialize(const std::vector<Buffer*>& first_buffers) {
    if (initialized_) {
        return true;
    }
    
    (void)first_buffers;
    
    if (output_path_.empty()) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), "SaveEncodedConsumer: Output path is empty");
        return false;
    }
    
    try {
        writer_ = std::make_unique<productionline::io::BufferWriter>();
        if (!writer_->openEncoded(output_path_.c_str(), codec_params_, time_base_)) {
            LOG4CPLUS_ERROR_FMT(log4cplus::Logger::getRoot(), 
                "SaveEncodedConsumer: Failed to open output file: %s", output_path_.c_str());
            return false;
        }
        initialized_ = true;
        LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(), 
            "SaveEncodedConsumer: Initialized, output: %s", output_path_.c_str());
        return true;
    } catch (const std::exception& e) {
        LOG4CPLUS_ERROR_FMT(log4cplus::Logger::getRoot(), 
            "SaveEncodedConsumer: Exception during initialization: %s", e.what());
        return false;
    }
}

bool SaveEncodedConsumer::consume(const std::vector<Buffer*>& buffers, int frame_index) {
    (void)frame_index;
    
    if (!initialized_ || !writer_ || buffers.empty() || !buffers[0]) {
        return true;
    }
    
    Buffer* buffer = buffers[0];
    
    if (writer_->write(buffer)) {
        packet_count_++;
    }
    
    return true;
}

void SaveEncodedConsumer::finalize() {
    if (writer_) {
        writer_->close();
        writer_.reset();
    }
    initialized_ = false;
    LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(), 
        "SaveEncodedConsumer: Finalized, saved %d packets to %s", 
        packet_count_, output_path_.c_str());
}

std::string SaveEncodedConsumer::getStats() const {
    std::ostringstream oss;
    oss << "SaveEncodedConsumer: " << packet_count_ << " packets saved to " << output_path_;
    return oss.str();
}

// ============================================================
// CompareConsumer 实现
// ============================================================

CompareConsumer::CompareConsumer(
    double min_psnr,
    double min_ssim,
    bool enable_psnr,
    bool enable_ssim
)
    : min_psnr_(min_psnr)
    , min_ssim_(min_ssim)
    , enable_psnr_(enable_psnr)
    , enable_ssim_(enable_ssim)
    , comparator_(nullptr)
    , compared_count_(0)
    , psnr_sum_(0.0)
    , ssim_sum_(0.0)
    , passed_(true)
    , initialized_(false)
{
}

CompareConsumer::~CompareConsumer() {
    finalize();
}

bool CompareConsumer::initialize(const std::vector<Buffer*>& first_buffers) {
    if (initialized_) {
        return true;
    }
    
    if (first_buffers.size() < 2) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), 
            "CompareConsumer: Need at least 2 buffers for comparison");
        return false;
    }
    
    try {
        comparator_ = std::make_unique<productionline::io::BufferComparator>();
        
        // 配置 BufferComparator
        productionline::io::CompareConfig config;
        config.enable_psnr = enable_psnr_;
        config.enable_ssim = enable_ssim_;
        config.quick_psnr_threshold = min_psnr_;
        config.ssim_threshold = min_ssim_;
        
        if (!comparator_->open(config)) {
            LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), 
                "CompareConsumer: Failed to open comparator");
            return false;
        }
        
        initialized_ = true;
        LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(), 
            "CompareConsumer: Initialized (PSNR: %s, SSIM: %s, min_psnr: %.1f, min_ssim: %.2f)",
            enable_psnr_ ? "enabled" : "disabled",
            enable_ssim_ ? "enabled" : "disabled",
            min_psnr_, min_ssim_);
        return true;
    } catch (const std::exception& e) {
        LOG4CPLUS_ERROR_FMT(log4cplus::Logger::getRoot(), 
            "CompareConsumer: Exception during initialization: %s", e.what());
        return false;
    }
}

bool CompareConsumer::consume(const std::vector<Buffer*>& buffers, int frame_index) {
    if (!initialized_ || !comparator_ || buffers.size() < 2) {
        return true;
    }
    
    Buffer* buffer1 = buffers[0];
    Buffer* buffer2 = buffers[1];
    
    if (!buffer1 || !buffer2) {
        return true;
    }
    
    // 使用 compare() 方法获取完整对比结果
    auto result = comparator_->compare(buffer1, buffer2);
    
    double psnr = result.psnr_avg;
    double ssim = result.ssim_avg;
    
    // 累加 PSNR
    if (enable_psnr_) {
        psnr_sum_ += psnr;
        
        if (psnr < min_psnr_) {
            passed_ = false;
            LOG4CPLUS_WARN_FMT(log4cplus::Logger::getRoot(), 
                "CompareConsumer: Frame %d PSNR %.2f < %.2f (threshold)",
                frame_index, psnr, min_psnr_);
        }
    }
    
    // 累加 SSIM
    if (enable_ssim_) {
        ssim_sum_ += ssim;
        
        if (ssim < min_ssim_) {
            passed_ = false;
            LOG4CPLUS_WARN_FMT(log4cplus::Logger::getRoot(), 
                "CompareConsumer: Frame %d SSIM %.4f < %.4f (threshold)",
                frame_index, ssim, min_ssim_);
        }
    }
    
    compared_count_++;
    
    return true;
}

void CompareConsumer::finalize() {
    if (comparator_) {
        comparator_->close();
        comparator_.reset();
    }
    initialized_ = false;
    
    LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(), 
        "CompareConsumer: Finalized, compared %d frames, avg PSNR: %.2f, avg SSIM: %.4f, passed: %s",
        compared_count_, getAveragePsnr(), getAverageSsim(), passed_ ? "YES" : "NO");
}

std::string CompareConsumer::getStats() const {
    std::ostringstream oss;
    oss << "CompareConsumer: " << compared_count_ << " frames compared"
        << ", avg PSNR: " << std::fixed << std::setprecision(2) << getAveragePsnr()
        << ", avg SSIM: " << std::fixed << std::setprecision(4) << getAverageSsim()
        << ", passed: " << (passed_ ? "YES" : "NO");
    return oss.str();
}

double CompareConsumer::getAveragePsnr() const {
    return compared_count_ > 0 ? psnr_sum_ / compared_count_ : 0.0;
}

double CompareConsumer::getAverageSsim() const {
    return compared_count_ > 0 ? ssim_sum_ / compared_count_ : 0.0;
}

bool CompareConsumer::isPassed() const {
    return passed_;
}

// ============================================================
// MultiConsumer 实现
// ============================================================

void MultiConsumer::addStrategy(std::shared_ptr<IBufferConsumer> strategy) {
    if (strategy) {
        strategies_.push_back(strategy);
    }
}

bool MultiConsumer::initialize(const std::vector<Buffer*>& first_buffers) {
    bool all_success = true;
    for (auto& strategy : strategies_) {
        if (!strategy->initialize(first_buffers)) {
            all_success = false;
        }
    }
    return all_success;
}

bool MultiConsumer::consume(const std::vector<Buffer*>& buffers, int frame_index) {
    bool continue_consume = true;
    for (auto& strategy : strategies_) {
        if (!strategy->consume(buffers, frame_index)) {
            continue_consume = false;
        }
    }
    return continue_consume;
}

void MultiConsumer::finalize() {
    for (auto& strategy : strategies_) {
        strategy->finalize();
    }
}

std::string MultiConsumer::getStats() const {
    std::ostringstream oss;
    oss << "MultiConsumer (" << strategies_.size() << " strategies):";
    for (size_t i = 0; i < strategies_.size(); i++) {
        oss << "\n  [" << i << "] " << strategies_[i]->getStats();
    }
    return oss.str();
}

} // namespace consumer
