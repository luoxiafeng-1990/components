/**
 * @file BufferConsumerStrategies.cpp
 * @brief Buffer 消费策略实现
 */

#include "productionline/io/BufferConsumerStrategies.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <sstream>
#include <iomanip>
#include <cstring>

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
        
        // 获取 Display BufferPool ID（用于 memcpy 模式）
        display_pool_id_ = display_->getBufferPoolId();
        if (display_pool_id_ == 0) {
            LOG4CPLUS_WARN(log4cplus::Logger::getRoot(), 
                "DisplayConsumer: Display BufferPool not available, memcpy mode disabled");
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
    bool success = false;
    
    // 智能选择显示方式：
    // 1. 如果 Buffer 有物理地址，优先使用 DMA 零拷贝
    // 2. 否则使用 memcpy 方式拷贝到 framebuffer（软件解码支持）
    
    if (buffer->getPhysicalAddress() != 0) {
        // DMA 零拷贝方式（性能最优）
        success = display_->displayBufferByDMA(buffer);
    }
    
    if (!success && display_pool_id_ != 0) {
        // memcpy 方式（软件解码支持）
        // 获取 Display BufferPool
        auto pool_weak = BufferPoolRegistry::getInstance().getPool(display_pool_id_);
        auto pool = pool_weak.lock();
        if (!pool) {
            LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), 
                "DisplayConsumer: Display BufferPool not found or already destroyed");
            failed_count_++;
            return true;
        }
        
        // 从 Display BufferPool 获取空闲的 framebuffer
        Buffer* display_buffer = pool->acquireFree(true, 100);
        if (display_buffer == nullptr) {
            LOG4CPLUS_WARN(log4cplus::Logger::getRoot(), 
                "DisplayConsumer: Failed to acquire free display buffer, skipping frame");
            failed_count_++;
            return true;
        }
        
        // 检查源 buffer 和目标 buffer 的虚拟地址
        void* src_addr = buffer->getVirtualAddress();
        void* dst_addr = display_buffer->getVirtualAddress();
        
        if (!src_addr) {
            LOG4CPLUS_ERROR_FMT(log4cplus::Logger::getRoot(), 
                "DisplayConsumer: Source buffer virtual address is nullptr (buffer #%u)", 
                buffer->id());
            pool->releaseFree(display_buffer);
            failed_count_++;
            return true;
        }
        
        if (!dst_addr) {
            LOG4CPLUS_ERROR_FMT(log4cplus::Logger::getRoot(), 
                "DisplayConsumer: Display buffer virtual address is nullptr (buffer #%u)", 
                display_buffer->id());
            pool->releaseFree(display_buffer);
            failed_count_++;
            return true;
        }
        
        // 计算安全的拷贝大小
        size_t copy_size = std::min(buffer->size(), display_buffer->size());
        if (copy_size == 0) {
            LOG4CPLUS_WARN(log4cplus::Logger::getRoot(), 
                "DisplayConsumer: Copy size is 0, skipping frame");
            pool->releaseFree(display_buffer);
            failed_count_++;
            return true;
        }
        
        // 拷贝数据（软件解码的关键步骤）
        std::memcpy(dst_addr, src_addr, copy_size);
        
        // 等待垂直同步并显示
        display_->waitVerticalSync();
        if (display_->displayFilledFramebuffer(display_buffer)) {
            success = true;
        } else {
            LOG4CPLUS_WARN(log4cplus::Logger::getRoot(), "DisplayConsumer: Display failed");
        }
        
        // 归还 display buffer
        pool->releaseFree(display_buffer);
    }
    
    if (success) {
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
// SaveRawConsumer 实现（支持多通道）
// ============================================================

SaveRawConsumer::SaveRawConsumer(const std::string& output_path, int max_frames)
    : output_paths_({output_path})
    , max_frames_per_channel_({max_frames})
    , initialized_(false)
{
}

SaveRawConsumer::SaveRawConsumer(const std::vector<std::string>& output_paths, 
                                 const std::vector<int>& max_frames_per_channel)
    : output_paths_(output_paths)
    , max_frames_per_channel_(max_frames_per_channel)
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
    
    if (output_paths_.empty() || output_paths_[0].empty()) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), "SaveRawConsumer: Output path is empty");
        return false;
    }
    
    // 不在这里创建 Writer，而是延迟到 consume 时根据通道创建
    initialized_ = true;
    LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(), 
        "SaveRawConsumer: Initialized with %zu output path(s)", output_paths_.size());
    return true;
}

std::string SaveRawConsumer::getOutputPath(int channel) const {
    if (output_paths_.empty()) return "";
    if (channel < 0 || static_cast<size_t>(channel) >= output_paths_.size()) {
        // 如果通道超出范围，使用第一个路径并添加通道后缀
        if (output_paths_.size() == 1 && channel > 0) {
            const std::string& base = output_paths_[0];
            size_t dot = base.rfind('.');
            if (dot != std::string::npos) {
                return base.substr(0, dot) + "_ch" + std::to_string(channel) + base.substr(dot);
            }
            return base + "_ch" + std::to_string(channel);
        }
        return output_paths_[0];
    }
    return output_paths_[channel];
}

int SaveRawConsumer::getMaxFrames(int channel) const {
    if (max_frames_per_channel_.empty()) return -1;
    if (channel < 0 || static_cast<size_t>(channel) >= max_frames_per_channel_.size()) {
        // 如果通道超出范围，使用第一个值
        return max_frames_per_channel_[0];
    }
    return max_frames_per_channel_[channel];
}

productionline::io::BufferWriter* SaveRawConsumer::getOrCreateWriter(int channel, Buffer* sample_buffer) {
    // 如果已存在，直接返回
    auto it = writers_.find(channel);
    if (it != writers_.end()) {
        return it->second.get();
    }
    
    // 创建新的 Writer
    std::string path = getOutputPath(channel);
    AVPixelFormat format = sample_buffer->getImageFormat();
    int width = sample_buffer->getImageWidth();
    int height = sample_buffer->getImageHeight();
    
    auto writer = std::make_unique<productionline::io::BufferWriter>();
    if (!writer->openRaw(path.c_str(), format, width, height)) {
        LOG4CPLUS_ERROR_FMT(log4cplus::Logger::getRoot(), 
            "SaveRawConsumer: Failed to open output file for channel %d: %s", 
            channel, path.c_str());
        return nullptr;
    }
    
    int max_frames = getMaxFrames(channel);
    LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(), 
        "SaveRawConsumer: Created writer for channel %d: %s (max_frames=%d)", 
        channel, path.c_str(), max_frames);
    
    auto* ptr = writer.get();
    writers_[channel] = std::move(writer);
    saved_counts_[channel] = 0;
    return ptr;
}

bool SaveRawConsumer::consume(const std::vector<Buffer*>& buffers, int frame_index) {
    (void)frame_index;
    
    if (!initialized_ || buffers.empty() || !buffers[0]) {
        return true;
    }
    
    Buffer* buffer = buffers[0];
    
    // 获取 Buffer 的输出通道
    int channel = buffer->getOutputChannel();
    if (channel < 0) {
        channel = 0;  // 默认通道 0
    }
    
    // 检查该通道是否达到最大保存帧数
    int channel_max_frames = getMaxFrames(channel);
    if (channel_max_frames > 0) {
        auto it = saved_counts_.find(channel);
        int channel_saved = (it != saved_counts_.end()) ? it->second : 0;
        if (channel_saved >= channel_max_frames) {
            return true;  // 该通道已达到限制，跳过
        }
    }
    
    // 获取或创建对应通道的 Writer
    auto* writer = getOrCreateWriter(channel, buffer);
    if (!writer) {
        return true;  // 创建失败，跳过
    }
    
    if (writer->write(buffer)) {
        saved_counts_[channel]++;
    }
    
    return true;
}

void SaveRawConsumer::finalize() {
    for (auto& pair : writers_) {
        if (pair.second) {
            pair.second->close();
            LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(), 
                "SaveRawConsumer: Channel %d finalized, saved %d frames to %s",
                pair.first, saved_counts_[pair.first], getOutputPath(pair.first).c_str());
        }
    }
    writers_.clear();
    saved_counts_.clear();
    initialized_ = false;
}

int SaveRawConsumer::getSavedCount() const {
    int total = 0;
    for (const auto& pair : saved_counts_) {
        total += pair.second;
    }
    return total;
}

int SaveRawConsumer::getSavedCount(int channel) const {
    auto it = saved_counts_.find(channel);
    return it != saved_counts_.end() ? it->second : 0;
}

std::string SaveRawConsumer::getStats() const {
    std::ostringstream oss;
    oss << "SaveRawConsumer: " << getSavedCount() << " frames saved";
    if (writers_.size() > 1) {
        oss << " (";
        bool first = true;
        for (const auto& pair : saved_counts_) {
            if (!first) oss << ", ";
            oss << "ch" << pair.first << ":" << pair.second;
            first = false;
        }
        oss << ")";
    }
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

CompareConsumer::CompareConsumer(const WorkerConfig::ConsumerTypeConfig::CompareType& config)
    : config_(config)
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
        
        // 直接传递配置给 BufferComparator（配置已统一）
        if (!comparator_->open(config_)) {
            LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), 
                "CompareConsumer: Failed to open comparator");
            return false;
        }
        
        initialized_ = true;
        LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(), 
            "CompareConsumer: Initialized (PSNR: %s, SSIM: %s, min_psnr: %.1f, min_ssim: %.2f)",
            config_.enable_psnr ? "enabled" : "disabled",
            config_.enable_ssim ? "enabled" : "disabled",
            config_.min_psnr, config_.min_ssim);
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
    if (config_.enable_psnr) {
        psnr_sum_ += psnr;
        
        if (psnr < config_.min_psnr) {
            passed_ = false;
            LOG4CPLUS_WARN_FMT(log4cplus::Logger::getRoot(), 
                "CompareConsumer: Frame %d PSNR %.2f < %.2f (threshold)",
                frame_index, psnr, config_.min_psnr);
        }
    }
    
    // 累加 SSIM
    if (config_.enable_ssim) {
        ssim_sum_ += ssim;
        
        if (ssim < config_.min_ssim) {
            passed_ = false;
            LOG4CPLUS_WARN_FMT(log4cplus::Logger::getRoot(), 
                "CompareConsumer: Frame %d SSIM %.4f < %.4f (threshold)",
                frame_index, ssim, config_.min_ssim);
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
