/**
 * @file BufferConsumerStrategies.cpp
 * @brief Buffer 消费策略实现
 */

#include "consumptionline/BufferConsumerStrategies.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "buffer/BufferAllocatorFacade.hpp"
#include "buffer/BufferAllocatorFactory.hpp"
#include "vendor/taco/display/DisplayDeviceFactory.hpp"
#include "productionline/worker/FFmpegEncodeWorker.hpp"
#include "productionline/worker/RawFrameSourceFromBuffer.hpp"
#include "productionline/VideoProductionLine.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdio>
#include <cerrno>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
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

DisplayConsumer::DisplayConsumer(const DisplayConsumerConfig& config)
    : config_(config)
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
        if (!config_.vendor) {
            LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), "DisplayConsumer: vendor is null");
            return false;
        }
        display_ = DisplayDeviceFactory::create(*config_.vendor);
        if (!display_->initialize(config_.device_id)) {
            LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), "DisplayConsumer: Failed to initialize display device");
            return false;
        }
        
        initialized_ = true;
        LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(), 
            "DisplayConsumer: Initialized (vendor=%s)",
            config_.vendor->kind());
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
        last_consume_failed_ = true;
        return true;
    }
    
    bool success = display_->displayBuffer(buffers[0]);
    last_consume_failed_ = !success;
    
    if (success) {
        success_count_++;
    } else {
        failed_count_++;
    }
    
    return true;
}

bool DisplayConsumer::shouldRetainBuffer() const {
    return last_consume_failed_;
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

consumptionline::io::BufferWriter* SaveRawConsumer::getOrCreateWriter(int channel, Buffer* sample_buffer) {
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
    
    auto writer = std::make_unique<consumptionline::io::BufferWriter>();
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
        writer_ = std::make_unique<consumptionline::io::BufferWriter>();
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
// ChannelCompareConsumer 实现（v2.27）
// ============================================================

ChannelCompareConsumer::ChannelCompareConsumer(
    std::shared_ptr<BufferPool> pool,
    const WorkerConfig::ConsumerTypeConfig::CompareType& config)
    : pool_(pool)
    , config_(config)
    , comparator_(nullptr)
    , running_(false)
    , compared_count_(0)
    , mismatch_count_(0)
    , psnr_sum_(0.0)
    , ssim_sum_(0.0)
    , passed_(true)
    , initialized_(false)
{
}

ChannelCompareConsumer::~ChannelCompareConsumer() {
    stop();
    finalize();
}

bool ChannelCompareConsumer::initialize(const std::vector<Buffer*>& first_buffers) {
    (void)first_buffers;
    
    if (initialized_) return true;
    
    try {
        comparator_ = std::make_unique<consumptionline::io::BufferComparator>();
        
        consumptionline::io::CompareConfig cmp_config;
        cmp_config.enable_psnr = config_.enable_psnr;
        cmp_config.enable_ssim = config_.enable_ssim;
        cmp_config.min_psnr = config_.min_psnr;
        cmp_config.min_ssim = config_.min_ssim;
        cmp_config.verbose = config_.verbose;
        
        if (!comparator_->open(cmp_config)) {
            LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), 
                "ChannelCompareConsumer: Failed to open comparator");
            return false;
        }
        
        initialized_ = true;
        LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(), 
            "ChannelCompareConsumer: Initialized (ref_ch=%d, cmp_ch=%d, PSNR=%s, SSIM=%s)",
            config_.reference_channel, config_.compare_channel,
            config_.enable_psnr ? "enabled" : "disabled",
            config_.enable_ssim ? "enabled" : "disabled");
        return true;
    } catch (const std::exception& e) {
        LOG4CPLUS_ERROR_FMT(log4cplus::Logger::getRoot(), 
            "ChannelCompareConsumer: Exception: %s", e.what());
        return false;
    }
}

bool ChannelCompareConsumer::consume(const std::vector<Buffer*>& buffers, int frame_index) {
    (void)buffers;
    (void)frame_index;
    return true;  // 被动模式不使用
}

void ChannelCompareConsumer::run(int max_frames) {
    if (!initialized_ && !initialize({})) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), 
            "ChannelCompareConsumer: Failed to initialize");
        return;
    }
    
    running_ = true;
    
    Buffer* cached_buffer = nullptr;
    int64_t cached_pts = AV_NOPTS_VALUE;
    
    while (running_) {
        Buffer* buffer = pool_->acquireFilled();
        if (!buffer) {
            break;  // pool 关闭或无数据
        }
        
        int64_t pts = buffer->getPts();
        int channel = buffer->getOutputChannel();
        
        // 只关心指定的两个通道
        if (channel != config_.reference_channel && channel != config_.compare_channel) {
            pool_->releaseFilled(buffer);
            continue;
        }
        
        if (cached_buffer == nullptr) {
            // 第一个 buffer，缓存（不释放）
            cached_buffer = buffer;
            cached_pts = pts;
        } else {
            // 第二个 buffer
            if (pts == cached_pts) {
                // PTS 匹配，执行比较
                auto result = comparator_->compare(cached_buffer, buffer);
                
                double psnr = result.psnr_avg;
                double ssim = result.ssim_avg;
                
                compared_count_++;
                psnr_sum_ += psnr;
                ssim_sum_ += ssim;
                
                bool psnr_ok = !config_.enable_psnr || (psnr >= config_.min_psnr);
                bool ssim_ok = !config_.enable_ssim || (ssim >= config_.min_ssim);
                
                if (!psnr_ok || !ssim_ok) {
                    passed_ = false;
                    LOG4CPLUS_WARN_FMT(log4cplus::Logger::getRoot(), 
                        "ChannelCompare[%d]: PTS=%lld FAILED - PSNR=%.2f, SSIM=%.4f",
                        compared_count_, (long long)pts, psnr, ssim);
                } else if (config_.verbose) {
                    LOG4CPLUS_DEBUG_FMT(log4cplus::Logger::getRoot(), 
                        "ChannelCompare[%d]: PTS=%lld PASSED - PSNR=%.2f, SSIM=%.4f",
                        compared_count_, (long long)pts, psnr, ssim);
                }
                
                // 释放两个 buffer
                pool_->releaseFilled(cached_buffer);
                pool_->releaseFilled(buffer);
                cached_buffer = nullptr;
                
            } else {
                // PTS 不匹配，记录异常
                mismatch_count_++;
                LOG4CPLUS_WARN_FMT(log4cplus::Logger::getRoot(), 
                    "ChannelCompare: PTS mismatch! cached=%lld (ch=%d), new=%lld (ch=%d)",
                    (long long)cached_pts, cached_buffer->getOutputChannel(),
                    (long long)pts, channel);
                
                // 释放旧的，缓存新的
                pool_->releaseFilled(cached_buffer);
                cached_buffer = buffer;
                cached_pts = pts;
            }
        }
        
        // 检查是否达到最大帧数
        if (max_frames > 0 && compared_count_ >= max_frames) {
            break;
        }
    }
    
    // 释放可能剩余的缓存 buffer
    if (cached_buffer) {
        pool_->releaseFilled(cached_buffer);
        LOG4CPLUS_WARN(log4cplus::Logger::getRoot(), 
            "ChannelCompare: Unpaired buffer at exit");
    }
    
    running_ = false;
}

void ChannelCompareConsumer::stop() {
    running_ = false;
}

void ChannelCompareConsumer::finalize() {
    if (comparator_) {
        comparator_->close();
        comparator_.reset();
    }
    initialized_ = false;
    
    LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(), 
        "ChannelCompareConsumer: compared=%d, mismatch=%d, avg_psnr=%.2f, avg_ssim=%.4f, %s",
        compared_count_, mismatch_count_, 
        getAveragePsnr(), getAverageSsim(), 
        passed_ ? "PASSED" : "FAILED");
}

std::string ChannelCompareConsumer::getStats() const {
    std::ostringstream oss;
    oss << "ChannelCompare: " << compared_count_ << " compared"
        << ", " << mismatch_count_ << " mismatch"
        << ", PSNR=" << std::fixed << std::setprecision(2) << getAveragePsnr()
        << ", SSIM=" << std::fixed << std::setprecision(4) << getAverageSsim()
        << ", " << (passed_ ? "PASSED" : "FAILED");
    return oss.str();
}

double ChannelCompareConsumer::getAveragePsnr() const {
    return compared_count_ > 0 ? psnr_sum_ / compared_count_ : 0.0;
}

double ChannelCompareConsumer::getAverageSsim() const {
    return compared_count_ > 0 ? ssim_sum_ / compared_count_ : 0.0;
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
    auto it = strategies_.begin();
    while (it != strategies_.end()) {
        if (!(*it)->initialize(first_buffers)) {
            LOG4CPLUS_WARN_FMT(
                log4cplus::Logger::getRoot(),
                "MultiConsumer: sub-consumer initialize failed, removing (remaining: %zu)",
                strategies_.size() - 1);
            fprintf(stderr, "[WARN] MultiConsumer: sub-consumer initialize failed, removing\n");
            it = strategies_.erase(it);
        } else {
            ++it;
        }
    }
    return !strategies_.empty();
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

// ============================================================
// OpencvConsumer 实现
// ============================================================

OpencvConsumer::OpencvConsumer(const OpencvType& opencv_config, const CompareType& compare_config)
    : opencv_config_(opencv_config)
    , compare_config_(compare_config)
    , comparator_(nullptr)
    , frames_processed_(0)
    , frames_compared_(0)
    , psnr_sum_(0.0)
    , ssim_sum_(0.0)
    , passed_(true)
    , initialized_(false)
{
}

OpencvConsumer::~OpencvConsumer() {
    finalize();
}

bool OpencvConsumer::initialize(const std::vector<Buffer*>& first_buffers) {
    if (initialized_) return true;
    (void)first_buffers;

    // 构建 BufferComparator 配置
    consumptionline::io::CompareConfig cmp_cfg;
    cmp_cfg.enable_psnr = compare_config_.enable_psnr;
    cmp_cfg.enable_ssim = compare_config_.enable_ssim;
    cmp_cfg.min_psnr    = compare_config_.min_psnr;
    cmp_cfg.min_ssim    = compare_config_.min_ssim;
    cmp_cfg.verbose     = compare_config_.verbose;

    comparator_ = std::make_unique<consumptionline::io::BufferComparator>();
    if (!comparator_->open(cmp_cfg)) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(),
                        "OpencvConsumer: Failed to open BufferComparator");
        return false;
    }

    initialized_ = true;
    LOG4CPLUS_INFO(log4cplus::Logger::getRoot(),
                   "OpencvConsumer: Initialized"
                   " (psnr=" << (compare_config_.enable_psnr ? "ON" : "OFF")
                   << ", ssim=" << (compare_config_.enable_ssim ? "ON" : "OFF") << ")");
    return true;
}

cv::Mat OpencvConsumer::applyOpencvTransform(const cv::Mat& src, int frame_index) const {
    if (src.empty()) return src;

    switch (opencv_config_.op_type) {
        case OpencvType::OpType::SAVE_LOAD_IMG: {
            // 图片保存/读取 I/O 测试（使用固定文件名，反复删除创建）
            const std::string temp_filename = "/tmp/opencv_test_frame.jpg";

            try {
                // 保存原始 Mat 为 JPEG 图片
                LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(),
                    "OpencvConsumer::applyOpencvTransform [frame %d] Saving to: %s",
                    frame_index, temp_filename.c_str());

                bool save_ok = cv::imwrite(temp_filename, src);
                if (!save_ok) {
                    LOG4CPLUS_WARN_FMT(log4cplus::Logger::getRoot(),
                        "OpencvConsumer::applyOpencvTransform [frame %d] Failed to save image to %s",
                        frame_index, temp_filename.c_str());
                    return src;
                }

                // 读取刚才保存的图片
                cv::Mat mat_loaded = cv::imread(temp_filename, cv::IMREAD_COLOR);
                if (mat_loaded.empty()) {
                    LOG4CPLUS_WARN_FMT(log4cplus::Logger::getRoot(),
                        "OpencvConsumer::applyOpencvTransform [frame %d] Failed to load image from %s",
                        frame_index, temp_filename.c_str());
                    return src;
                }

                LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(),
                    "OpencvConsumer::applyOpencvTransform [frame %d] Loaded from: %s (size=%dx%d channels=%d)",
                    frame_index, temp_filename.c_str(),
                    mat_loaded.cols, mat_loaded.rows, mat_loaded.channels());

                return mat_loaded;
            } catch (const std::exception& e) {
                LOG4CPLUS_ERROR_FMT(log4cplus::Logger::getRoot(),
                    "OpencvConsumer::applyOpencvTransform [frame %d] Exception in SAVE_LOAD_IMG: %s",
                    frame_index, e.what());
                return src;
            }
        }
        case OpencvType::OpType::RESIZE: {
            const auto& r = opencv_config_.resize;
            if (r.dst_width <= 0 || r.dst_height <= 0 || r.fx <= 0.0 || r.fy <= 0.0) {
                LOG4CPLUS_WARN(log4cplus::Logger::getRoot(),
                    "OpencvConsumer::applyOpencvTransform: RESIZE params invalid, skip");
                return src;
            }
            
            cv::Mat dst;
            cv::resize(src, dst,
                       cv::Size(r.dst_width, r.dst_height),
                       r.fx, r.fy,
                       r.interpolation);
            return dst;
        }
        case OpencvType::OpType::CROP: {
            const auto& c = opencv_config_.crop;
            if (c.width <= 0 || c.height <= 0 ||
                c.x < 0 || c.y < 0 ||
                c.x + c.width > src.cols || c.y + c.height > src.rows) {
                LOG4CPLUS_WARN_FMT(log4cplus::Logger::getRoot(),
                    "OpencvConsumer::applyOpencvTransform: CROP ROI(%d,%d,%d,%d) "
                    "out of bounds for Mat(%dx%d), skip",
                    c.x, c.y, c.width, c.height, src.cols, src.rows);
                return src;
            }
            return src(cv::Rect(c.x, c.y, c.width, c.height)).clone();
        }
        case OpencvType::OpType::ERODE: {
            const auto& m = opencv_config_.morph;
            cv::Mat kernel = cv::getStructuringElement(
                m.kernel_shape,
                cv::Size(m.kernel_size, m.kernel_size));
            cv::Mat dst;
            cv::erode(src, dst, kernel,
                      cv::Point(m.anchor_x, m.anchor_y),
                      m.iterations);
            return dst;
        }
        case OpencvType::OpType::DILATE: {
            const auto& m = opencv_config_.morph;
            cv::Mat kernel = cv::getStructuringElement(
                m.kernel_shape,
                cv::Size(m.kernel_size, m.kernel_size));
            cv::Mat dst;
            cv::dilate(src, dst, kernel,
                       cv::Point(m.anchor_x, m.anchor_y),
                       m.iterations);
            return dst;
        }
        case OpencvType::OpType::MORPH_OPEN: {
            // 开运算：先腐蚀后膨胀
            const auto& m = opencv_config_.morph;
            cv::Mat kernel = cv::getStructuringElement(
                m.kernel_shape,
                cv::Size(m.kernel_size, m.kernel_size));
            cv::Mat tmp, dst;
            cv::erode(src, tmp, kernel,
                      cv::Point(m.anchor_x, m.anchor_y),
                      m.iterations);
            cv::dilate(tmp, dst, kernel,
                       cv::Point(m.anchor_x, m.anchor_y),
                       m.iterations);
            return dst;
        }
        case OpencvType::OpType::MORPH_CLOSE: {
            // 闭运算：先膨胀后腐蚀
            const auto& m = opencv_config_.morph;
            cv::Mat kernel = cv::getStructuringElement(
                m.kernel_shape,
                cv::Size(m.kernel_size, m.kernel_size));
            cv::Mat tmp, dst;
            cv::dilate(src, tmp, kernel,
                       cv::Point(m.anchor_x, m.anchor_y),
                       m.iterations);
            cv::erode(tmp, dst, kernel,
                      cv::Point(m.anchor_x, m.anchor_y),
                      m.iterations);
            return dst;
        }
        case OpencvType::OpType::SOBEL: {
            const auto& s = opencv_config_.sobel;
            int ksize = (s.ksize % 2 == 0) ? s.ksize + 1 : s.ksize;
            cv::Mat gray, dst;
            if (src.channels() != 1) cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
            else gray = src;
            cv::Sobel(gray, dst, CV_8U, s.dx, s.dy, ksize, s.scale, s.delta);
            return dst;
        }
        case OpencvType::OpType::CANNY: {
            const auto& c = opencv_config_.canny;
            cv::Mat gray, dst;
            if (src.channels() != 1) cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
            else gray = src;
            cv::Canny(gray, dst, c.threshold1, c.threshold2, c.aperture_size);
            return dst;
        }
        case OpencvType::OpType::LAPLACIAN: {
            const auto& l = opencv_config_.laplacian;
            int ksize = (l.ksize % 2 == 0) ? l.ksize + 1 : l.ksize;
            cv::Mat gray, dst;
            if (src.channels() != 1) cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
            else gray = src;
            cv::Laplacian(gray, dst, CV_8U, ksize, l.scale, l.delta);
            return dst;
        }
        case OpencvType::OpType::TRANSLATE: {
            const auto& t = opencv_config_.translate;
            cv::Mat M = (cv::Mat_<double>(2, 3) << 1, 0, t.tx, 0, 1, t.ty);
            cv::Mat dst;
            cv::warpAffine(src, dst, M, src.size());
            return dst;
        }
        case OpencvType::OpType::ROTATE: {
            const auto& r = opencv_config_.rotate;
            cv::Point2f center(src.cols / 2.0f, src.rows / 2.0f);
            cv::Mat M = cv::getRotationMatrix2D(center, r.angle, r.scale);
            cv::Mat dst;
            cv::warpAffine(src, dst, M, src.size());
            return dst;
        }
        case OpencvType::OpType::PERSPECTIVE: {
            const auto& p = opencv_config_.perspective;
            int off = p.offset;
            cv::Point2f src_pts[4] = {
                {0.f, 0.f},
                {(float)src.cols, 0.f},
                {(float)src.cols, (float)src.rows},
                {0.f, (float)src.rows}
            };
            cv::Point2f dst_pts[4] = {
                {(float)off,              (float)off},
                {(float)(src.cols - off), 0.f},
                {(float)src.cols,         (float)src.rows},
                {0.f,                     (float)src.rows}
            };
            cv::Mat M = cv::getPerspectiveTransform(src_pts, dst_pts);
            cv::Mat dst;
            cv::warpPerspective(src, dst, M, src.size());
            return dst;
        }
        case OpencvType::OpType::DRAW_LINE: {
            const auto& l = opencv_config_.draw_line;
            cv::Mat dst = src.clone();
            cv::line(dst,
                     cv::Point(l.x1, l.y1),
                     cv::Point(l.x2, l.y2),
                     cv::Scalar(0, 255, 0),
                     l.thickness);
            return dst;
        }
        case OpencvType::OpType::DRAW_RECT: {
            const auto& r = opencv_config_.draw_rect;
            cv::Mat dst = src.clone();
            cv::rectangle(dst,
                          cv::Rect(r.x, r.y, r.width, r.height),
                          cv::Scalar(0, 0, 255),
                          r.thickness);
            return dst;
        }
        case OpencvType::OpType::PUT_TEXT: {
            const auto& t = opencv_config_.put_text;
            cv::Mat dst = src.clone();
            cv::putText(dst, "Hello OpenCV",
                        cv::Point(t.x, t.y),
                        cv::FONT_HERSHEY_SIMPLEX,
                        t.font_scale,
                        cv::Scalar(255, 255, 255),
                        t.thickness);
            return dst;
        }
        case OpencvType::OpType::GAUSSIAN_BLUR: {
            const auto& g = opencv_config_.gaussian_blur;
            int k = (g.ksize % 2 == 0) ? g.ksize + 1 : g.ksize;
            cv::Mat dst;
            cv::GaussianBlur(src, dst, cv::Size(k, k), g.sigma_x, g.sigma_x);
            return dst;
        }
        case OpencvType::OpType::THRESHOLD: {
            const auto& t = opencv_config_.threshold;
            cv::Mat gray, dst;
            if (src.channels() != 1) cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
            else gray = src;
            cv::threshold(gray, dst, t.thresh, t.maxval, t.type);
            return dst;
        }
        case OpencvType::OpType::SPLIT:
        case OpencvType::OpType::MERGE: {
            // split/merge 测试：将多通道图像分离后重新合并，验证通道处理正确性
            std::vector<cv::Mat> channels;
            cv::split(src, channels);
            cv::Mat merged;
            cv::merge(channels, merged);
            return merged;
        }
        case OpencvType::OpType::CVTCOLOR: {
            // cvtColor 测试：颜色空间转换
            const auto& c = opencv_config_.cvtcolor;
            cv::Mat dst;

            // 对于 NV12 等 YUV 格式，需要特殊处理
            // cv::Mat(AVFrame*) 创建的 Mat 高度是原图的 3/2（Y+UV 平面）
            // cvtColor 的 code 如 COLOR_YUV2BGR_NV12 需要完整高度的输入
            if (c.dstCn > 0) {
                cv::cvtColor(src, dst, c.code, c.dstCn);
            } else {
                cv::cvtColor(src, dst, c.code);
            }
            return dst;
        }
        default:
            return src;
    }
}

std::string matInfoToString(const cv::Mat& mat) {
    int matType = mat.type();
    int depth = matType & CV_MAT_DEPTH_MASK;
    int channels = (matType >> CV_CN_SHIFT) + 1;
    
    std::string depthStr;
    switch(depth) {
        case CV_8U:  depthStr = "CV_8U"; break;
        case CV_8S:  depthStr = "CV_8S"; break;
        case CV_16U: depthStr = "CV_16U"; break;
        case CV_16S: depthStr = "CV_16S"; break;
        case CV_32S: depthStr = "CV_32S"; break;
        case CV_32F: depthStr = "CV_32F"; break;
        case CV_64F: depthStr = "CV_64F"; break;
        case CV_16F: depthStr = "CV_16F"; break;
        default:     depthStr = "CV_UNKNOWN";
    }
    
    std::stringstream ss;
    ss << "Mat Info: ";
    
    // 添加尺寸信息
    if (mat.dims == 2) {
        // 二维矩阵：显示rows和cols
        ss << "Size(" << mat.cols << "x" << mat.rows << ") ";
    } else if (mat.dims > 2) {
        // 多维矩阵：显示各个维度
        ss << "Dims[" << mat.dims << "] Size[";
        for (int i = 0; i < mat.dims; ++i) {
            ss << mat.size[i];
            if (i < mat.dims - 1) ss << "x";
        }
        ss << "] ";
    } else {
        // 空矩阵或无维度
        ss << "Empty ";
    }
    
    // 添加类型信息
    ss << depthStr << "C" << channels;
    
    // 添加更多详细信息
    ss << " | Depth:" << depth 
       << " | Channels:" << channels
       << " | Total:" << mat.total()
       << " | Continuous:" << (mat.isContinuous() ? "Yes" : "No");
    
    return ss.str();
}

std::string avframeInfoToString(const AVFrame* frame) {
    // 安全检查：检查输入指针是否有效
    if (frame == NULL) {
        fprintf(stderr, "错误：AVFrame指针为空\n");
        return std::string();
    }
    
    // 获取像素格式的枚举值
    enum AVPixelFormat pix_fmt = static_cast<AVPixelFormat>(frame->format);
    
    // 打印像素格式的数值（枚举值）
    printf("AVFrame像素格式（数值）: %d\n", pix_fmt);
    
    // 获取像素格式的字符串名称
    const char* pix_fmt_name = av_get_pix_fmt_name(pix_fmt);

    return std::string(pix_fmt_name);
}

/**
 * @brief 将 AVFrame 转换为 BGR cv::Mat（CV_8UC3）
 *
 * 支持 FFmpeg AVPixelFormat 定义的全部像素格式：
 *  - YUV 420 半平面（NV12/NV21）、全平面（YUV420P/YV12）、10-bit 半平面（P010）
 *  - YUV 422 半平面（NV16/NV61）、全平面（YUV422P）
 *  - YUV 444 半平面（NV24）、全平面（YUV444P）
 *  - Packed RGB 8-bit（ARGB/ABGR/RGBA/BGRA/RGB/BGR 及 X 填充变体）
 *  - Planar RGB 8-bit（RGB888_PLANAR / BGR888_PLANAR / GBRP）
 *  - 16-bit per channel Planar RGB（R16G16B16 / B16G16R16），输出降至 8-bit
 *  - 10-bit Packed RGB（2101010 / 10102），输出降至 8-bit
 *
 * 多平面处理原则：
 *  1. 逐行拷贝各平面（尊重 linesize stride）至独立的 cv::Mat
 *  2. 通过 cv::merge() 合并通道，或拼装连续缓冲区后调用 cv::cvtColor
 *
 * @param frame  源 AVFrame（不可为空，data[] 指针必须有效）
 * @return CV_8UC3 BGR 格式的 cv::Mat；出错时返回空 Mat
 */
static cv::Mat avframeToMat(const AVFrame* frame)
{
    if (!frame || !frame->data[0]) {
        LOG4CPLUS_WARN(log4cplus::Logger::getRoot(),
            "avframeToMat: null frame or data pointer");
        return cv::Mat();
    }

    const int w = frame->width;
    const int h = frame->height;

    if (w <= 0 || h <= 0) {
        LOG4CPLUS_WARN_FMT(log4cplus::Logger::getRoot(),
            "avframeToMat: invalid frame dimensions %dx%d", w, h);
        return cv::Mat();
    }

    // 使用 frame->format（AVPixelFormat 枚举）
    const AVPixelFormat fmt = static_cast<AVPixelFormat>(frame->format);

    // ─── 辅助 lambda：按行拷贝单通道 8-bit 平面（处理 linesize > cols 的 stride）
    // rows × cols 个 uint8_t，cols = 平面有效宽度（字节数）
    auto copyPlane8 = [](const uint8_t* src, int linesize,
                         int rows, int cols) -> cv::Mat {
        cv::Mat plane(rows, cols, CV_8UC1);
        for (int r = 0; r < rows; ++r)
            std::memcpy(plane.ptr(r),
                        src + static_cast<ptrdiff_t>(r) * linesize,
                        cols);
        return plane;
    };

    // ─── 辅助 lambda：拷贝交错双通道（UV / VU）8-bit 平面 → CV_8UC2
    // pairs_per_row = 水平方向像素对数（YUV420 为 w/2，YUV444 为 w）
    auto copyPlaneUV8 = [](const uint8_t* src, int linesize,
                           int rows, int pairs_per_row) -> cv::Mat {
        cv::Mat plane(rows, pairs_per_row, CV_8UC2);
        for (int r = 0; r < rows; ++r)
            std::memcpy(plane.ptr(r),
                        src + static_cast<ptrdiff_t>(r) * linesize,
                        pairs_per_row * 2);
        return plane;
    };

    cv::Mat bgr;

    switch (fmt) {
    // ══════════════════════════════════════════════════════════════════
    // YUV 420 半平面：NV12（UV 交错）/ NV21（VU 交错）
    // AV_PIX_FMT_NV12 / AV_PIX_FMT_NV21
    // data[0] = Y  (h × w 字节)
    // data[1] = UV or VU  (h/2 × w 字节)
    // ══════════════════════════════════════════════════════════════════
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_NV21: {
        // 组装 OpenCV 需要的连续布局：前 h 行 = Y，后 h/2 行 = UV
        cv::Mat yuv(h * 3 / 2, w, CV_8UC1);
        for (int r = 0; r < h; ++r)
            std::memcpy(yuv.ptr(r),
                        frame->data[0] + static_cast<ptrdiff_t>(r) * frame->linesize[0],
                        w);
        for (int r = 0; r < h / 2; ++r)
            std::memcpy(yuv.ptr(h + r),
                        frame->data[1] + static_cast<ptrdiff_t>(r) * frame->linesize[1],
                        w);
        cv::cvtColor(yuv, bgr,
            fmt == AV_PIX_FMT_NV12 ? cv::COLOR_YUV2BGR_NV12 : cv::COLOR_YUV2BGR_NV21);
        break;
    }

    // ══════════════════════════════════════════════════════════════════
    // YUV 420 全平面：I420（Y/U/V）/ YV12（Y/V/U，V 与 U 互换）
    // AV_PIX_FMT_YUV420P / AV_PIX_FMT_YUVJ420P
    // data[0] = Y  (h × w)
    // data[1] = U(I420) 或 V(YV12)  (h/2 × w/2)
    // data[2] = V(I420) 或 U(YV12)  (h/2 × w/2)
    // OpenCV I420 布局：(h*3/2) × w — 前 h 行 Y，再 h/4 行 U，再 h/4 行 V
    // ══════════════════════════════════════════════════════════════════
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P: {
        const bool is_yuv420p = (fmt == AV_PIX_FMT_YUV420P);
        const uint8_t* u_src = frame->data[1];
        const uint8_t* v_src = frame->data[2];
        const int u_ls = frame->linesize[1];
        const int v_ls = frame->linesize[2];

        cv::Mat yuv(h * 3 / 2, w, CV_8UC1);
        // Y 平面
        for (int r = 0; r < h; ++r)
            std::memcpy(yuv.ptr(r),
                        frame->data[0] + static_cast<ptrdiff_t>(r) * frame->linesize[0],
                        w);
        // U、V 平面：各 h/2 行 × w/2 列，连续写入 yuv[h] 之后
        uint8_t* u_dst = yuv.ptr(h);
        uint8_t* v_dst = u_dst + (h / 2) * (w / 2);
        for (int r = 0; r < h / 2; ++r) {
            std::memcpy(u_dst + r * (w / 2),
                        u_src + static_cast<ptrdiff_t>(r) * u_ls, w / 2);
            std::memcpy(v_dst + r * (w / 2),
                        v_src + static_cast<ptrdiff_t>(r) * v_ls, w / 2);
        }
        cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);
        break;
    }

    // ══════════════════════════════════════════════════════════════════
    // P010：10-bit YUV420 半平面（uint16 MSB 对齐，高 8-bit 有效）
    // AV_PIX_FMT_P010LE / AV_PIX_FMT_P010BE
    // 提取高 8 位后，按 NV12 路径转换
    // ══════════════════════════════════════════════════════════════════
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_P010BE: {
        cv::Mat yuv(h * 3 / 2, w, CV_8UC1);
        // Y 平面（uint16 → uint8，取高 8 位）
        for (int r = 0; r < h; ++r) {
            const uint16_t* src16 = reinterpret_cast<const uint16_t*>(
                frame->data[0] + static_cast<ptrdiff_t>(r) * frame->linesize[0]);
            uint8_t* dst = yuv.ptr(r);
            for (int c = 0; c < w; ++c)
                dst[c] = static_cast<uint8_t>(src16[c] >> 8);
        }
        // UV 平面（uint16 → uint8）
        for (int r = 0; r < h / 2; ++r) {
            const uint16_t* src16 = reinterpret_cast<const uint16_t*>(
                frame->data[1] + static_cast<ptrdiff_t>(r) * frame->linesize[1]);
            uint8_t* dst = yuv.ptr(h + r);
            for (int c = 0; c < w; ++c)
                dst[c] = static_cast<uint8_t>(src16[c] >> 8);
        }
        cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_NV12);
        break;
    }

    // ══════════════════════════════════════════════════════════════════
    // YUV 422 全平面：YUV422P
    // AV_PIX_FMT_YUV422P / AV_PIX_FMT_YUVJ422P
    // data[0] = Y  (h × w)
    // data[1] = U (Cb)  (h × w/2)
    // data[2] = V (Cr)  (h × w/2)
    // ══════════════════════════════════════════════════════════════════
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P: {
        cv::Mat y_plane = copyPlane8(frame->data[0], frame->linesize[0], h, w);
        cv::Mat u_half  = copyPlane8(frame->data[1], frame->linesize[1], h, w / 2);
        cv::Mat v_half  = copyPlane8(frame->data[2], frame->linesize[2], h, w / 2);

        cv::Mat u_full, v_full;
        cv::resize(u_half, u_full, cv::Size(w, h), 0, 0, cv::INTER_LINEAR);
        cv::resize(v_half, v_full, cv::Size(w, h), 0, 0, cv::INTER_LINEAR);

        cv::Mat ycrcb;
        cv::merge(std::vector<cv::Mat>{y_plane, v_full, u_full}, ycrcb);
        cv::cvtColor(ycrcb, bgr, cv::COLOR_YCrCb2BGR);
        break;
    }

    // ══════════════════════════════════════════════════════════════════
    // YUV 444 半平面：YUV444P
    // AV_PIX_FMT_YUV444P
    // data[0] = Y, data[1] = U, data[2] = V
    // ══════════════════════════════════════════════════════════════════
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ444P: {
        cv::Mat y_plane = copyPlane8(frame->data[0], frame->linesize[0], h, w);
        cv::Mat u_plane = copyPlane8(frame->data[1], frame->linesize[1], h, w);
        cv::Mat v_plane = copyPlane8(frame->data[2], frame->linesize[2], h, w);

        cv::Mat ycrcb;
        cv::merge(std::vector<cv::Mat>{y_plane, v_plane, u_plane}, ycrcb);
        cv::cvtColor(ycrcb, bgr, cv::COLOR_YCrCb2BGR);
        break;
    }

    // ══════════════════════════════════════════════════════════════════
    // Packed RGB 8-bit — 4 字节/像素（含 Alpha 或 X 填充通道）
    // ══════════════════════════════════════════════════════════════════
    case AV_PIX_FMT_BGRA:  // BGRA8888
    case AV_PIX_FMT_BGR0:  // BGRX8888
    {
        // 内存布局 [B][G][R][A/X]：OpenCV CV_8UC4 原生，直接去 Alpha
        cv::Mat raw = copyPlane8(frame->data[0], frame->linesize[0], h, w * 4);
        cv::cvtColor(cv::Mat(h, w, CV_8UC4, raw.data), bgr, cv::COLOR_BGRA2BGR);
        break;
    }
    case AV_PIX_FMT_RGBA:  // RGBA8888
    case AV_PIX_FMT_RGB0:  // RGBX8888
    {
        // 内存布局 [R][G][B][A/X]
        cv::Mat raw = copyPlane8(frame->data[0], frame->linesize[0], h, w * 4);
        cv::cvtColor(cv::Mat(h, w, CV_8UC4, raw.data), bgr, cv::COLOR_RGBA2BGR);
        break;
    }
    case AV_PIX_FMT_ARGB:  // ARGB8888
    case AV_PIX_FMT_0RGB:  // XRGB8888
    {
        // 内存布局 [A/X][R][G][B]
        // mixChannels 重排：ARGB(ch0=A,1=R,2=G,3=B) → BGRA(ch0=B,1=G,2=R,3=A)
        cv::Mat raw = copyPlane8(frame->data[0], frame->linesize[0], h, w * 4);
        cv::Mat src4(h, w, CV_8UC4, raw.data);
        cv::Mat bgra(h, w, CV_8UC4);
        const int mix[] = {3, 0,  2, 1,  1, 2,  0, 3};  // src_ch → dst_ch
        cv::mixChannels(&src4, 1, &bgra, 1, mix, 4);
        cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
        break;
    }
    case AV_PIX_FMT_ABGR:  // ABGR8888
    case AV_PIX_FMT_0BGR:  // XBGR8888
    {
        // 内存布局 [A/X][B][G][R]
        // mixChannels 重排：ABGR(ch0=A,1=B,2=G,3=R) → BGRA(ch0=B,1=G,2=R,3=A)
        cv::Mat raw = copyPlane8(frame->data[0], frame->linesize[0], h, w * 4);
        cv::Mat src4(h, w, CV_8UC4, raw.data);
        cv::Mat bgra(h, w, CV_8UC4);
        const int mix[] = {1, 0,  2, 1,  3, 2,  0, 3};
        cv::mixChannels(&src4, 1, &bgra, 1, mix, 4);
        cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
        break;
    }

    // ══════════════════════════════════════════════════════════════════
    // Packed RGB 8-bit — 3 字节/像素
    // ══════════════════════════════════════════════════════════════════
    case AV_PIX_FMT_BGR24:  // BGR888
    {
        cv::Mat raw = copyPlane8(frame->data[0], frame->linesize[0], h, w * 3);
        bgr = cv::Mat(h, w, CV_8UC3, raw.data).clone();
        break;
    }
    case AV_PIX_FMT_RGB24:  // RGB888
    {
        cv::Mat raw = copyPlane8(frame->data[0], frame->linesize[0], h, w * 3);
        cv::cvtColor(cv::Mat(h, w, CV_8UC3, raw.data), bgr, cv::COLOR_RGB2BGR);
        break;
    }

    // ══════════════════════════════════════════════════════════════════
    // Planar RGB 8-bit：逐平面拷贝后 cv::merge() 合并为 BGR
    // ══════════════════════════════════════════════════════════════════
    case AV_PIX_FMT_GBRP:  // GBR Planar
    {
        // AV_PIX_FMT_GBRP：data[0]=G  data[1]=B  data[2]=R
        cv::Mat g = copyPlane8(frame->data[0], frame->linesize[0], h, w);
        cv::Mat b = copyPlane8(frame->data[1], frame->linesize[1], h, w);
        cv::Mat r = copyPlane8(frame->data[2], frame->linesize[2], h, w);
        cv::merge(std::vector<cv::Mat>{b, g, r}, bgr);
        break;
    }

    // ══════════════════════════════════════════════════════════════════
    // 10-bit Packed RGB（2101010 / 10102）
    // 每像素一个 uint32，各通道占 10 bit，取各通道高 8 位输出
    // ══════════════════════════════════════════════════════════════════
    case AV_PIX_FMT_RGBA64LE:
    case AV_PIX_FMT_RGBA64BE:
    case AV_PIX_FMT_BGRA64LE:
    case AV_PIX_FMT_BGRA64BE:
    {
        // 处理 16-bit 每通道的 RGB
        bgr.create(h, w, CV_8UC3);
        for (int r = 0; r < h; ++r) {
            const uint16_t* src16 = reinterpret_cast<const uint16_t*>(
                frame->data[0] + static_cast<ptrdiff_t>(r) * frame->linesize[0]);
            uint8_t* dst = bgr.ptr(r);
            
            for (int c = 0; c < w; ++c) {
                uint8_t b, g, rb;
                
                if (fmt == AV_PIX_FMT_RGBA64LE || fmt == AV_PIX_FMT_RGBA64BE) {
                    // R[0:15], G[16:31], B[32:47], A[48:63]
                    rb = static_cast<uint8_t>(src16[c * 4 + 0] >> 8);  // R
                    g  = static_cast<uint8_t>(src16[c * 4 + 1] >> 8);  // G
                    b  = static_cast<uint8_t>(src16[c * 4 + 2] >> 8);  // B
                } else { // AV_PIX_FMT_BGRA64LE or AV_PIX_FMT_BGRA64BE
                    // B[0:15], G[16:31], R[32:47], A[48:63]
                    b  = static_cast<uint8_t>(src16[c * 4 + 0] >> 8);  // B
                    g  = static_cast<uint8_t>(src16[c * 4 + 1] >> 8);  // G
                    rb = static_cast<uint8_t>(src16[c * 4 + 2] >> 8);  // R
                }
                
                dst[c * 3 + 0] = b;
                dst[c * 3 + 1] = g;
                dst[c * 3 + 2] = rb;
            }
        }
        break;
    }

    default:
        LOG4CPLUS_WARN_FMT(log4cplus::Logger::getRoot(),
            "avframeToMat: unsupported AVPixelFormat %d, returning empty Mat",
            static_cast<int>(fmt));
        break;
    }

    return bgr;
}

bool OpencvConsumer::consume(const std::vector<Buffer*>& buffers, int frame_index) {
    if (!initialized_) {
        if (!initialize(buffers)) return false;
    }

    frames_processed_++;

    if (buffers.size() >= 2 && buffers[0] && buffers[1]) {
        std::cout << "dual buffer is not supported" << std::endl;
    } else if (!buffers.empty() && buffers[0]) {
        // ── 单 Buffer 模式：直接用 frame->data 创建 Mat（零拷贝），无需深拷贝 ──
        AVFrame* avframe_hw = buffers[0]->getAVFrame();
        if (!avframe_hw) {
            LOG4CPLUS_WARN(log4cplus::Logger::getRoot(),
                "OpencvConsumer: no AVFrame in buffer, skip");
            return true;
        }

        // 直接用同一个 frame 创建两个 Mat，共享数据指针
        // applyOpencvTransform 会返回新的 Mat（clone 或新建），所以变换后数据独立
        AVFrame* avframe_sw = av_frame_clone(avframe_hw);
        cv::Mat mat_hw = cv::Mat(avframe_hw);
        cv::Mat mat_sw = avframeToMat(avframe_sw);

        cv::Mat mat_hw_bgr;
        cv::cvtColor(mat_hw,mat_hw_bgr,cv::COLOR_YUV2BGR_NV12);
        // 对两路施加同一 OpenCV 变换
        if (opencv_config_.op_type != OpencvType::OpType::NONE) {
            mat_hw = applyOpencvTransform(mat_hw_bgr, frame_index);
            mat_sw = applyOpencvTransform(mat_sw, frame_index);
        }

        // 临时 Buffer 包装 Mat；comparator 检测到 setMat 后走 is_mat 路径
        Buffer ref_buf(0, nullptr, 0, 0, Buffer::Ownership::EXTERNAL);
        ref_buf.setMat(&mat_sw);
        buffers[0]->setMat(&mat_hw);

        // 比较
        auto result = comparator_->compare(buffers[0], &ref_buf);

        // 清理
        buffers[0]->setMat(nullptr);

        frames_compared_++;
        psnr_sum_ += result.psnr_avg;
        ssim_sum_ += result.ssim_avg;
        if (!result.passed) passed_ = false;

        if (compare_config_.verbose) {
            LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(),
                "OpencvConsumer [frame %d] PSNR=%.2f dB  SSIM=%.4f  %s",
                frame_index, result.psnr_avg, result.ssim_avg,
                result.passed ? "PASS" : "FAIL");
        }
    }

    return true;
}

void OpencvConsumer::finalize() {
    if (!initialized_) return;

    if (comparator_) {
        comparator_->close();
        comparator_->printSummary();
    }

    // 清理 SAVE_LOAD_IMG 操作的临时文件
    if (opencv_config_.op_type == OpencvType::OpType::SAVE_LOAD_IMG) {
        const std::string temp_filename = "/tmp/opencv_test_frame.jpg";
        ::remove(temp_filename.c_str());
        LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(),
            "OpencvConsumer: Cleaned up temporary file: %s", temp_filename.c_str());
    }

    LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(),
        "OpencvConsumer: processed=%d  compared=%d  avgPSNR=%.2f dB  avgSSIM=%.4f  %s",
        frames_processed_, frames_compared_,
        getAveragePsnr(), getAverageSsim(),
        passed_ ? "PASS" : "FAIL");

    initialized_ = false;
}

std::string OpencvConsumer::getStats() const {
    std::ostringstream oss;
    oss << "OpencvConsumer: processed=" << frames_processed_
        << " compared=" << frames_compared_
        << std::fixed << std::setprecision(2)
        << " avgPSNR=" << getAveragePsnr() << "dB"
        << std::setprecision(4)
        << " avgSSIM=" << getAverageSsim()
        << " " << (passed_ ? "PASS" : "FAIL");
    return oss.str();
}

double OpencvConsumer::getAveragePsnr() const {
    return frames_compared_ > 0 ? psnr_sum_ / frames_compared_ : 0.0;
}

double OpencvConsumer::getAverageSsim() const {
    return frames_compared_ > 0 ? ssim_sum_ / frames_compared_ : 0.0;
}

bool OpencvConsumer::isPassed() const {
    return passed_;
}

// ============================================================
// JpegEncodeConsumer 实现（v3.3）
// ============================================================

JpegEncodeConsumer::JpegEncodeConsumer(const Config& config)
    : logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Consumer.JpegEncode")))
    , config_(config)
    , on_frame_(config.on_frame)
{
    LOG4CPLUS_DEBUG_FMT(logger_, "构造: encoder=%s quality=%d fps=%d pipe='%s' callback=%s",
                        config_.encoder_name.c_str(), config_.quality,
                        config_.target_fps, config_.output_pipe.c_str(),
                        on_frame_ ? "YES" : "NO");
}

JpegEncodeConsumer::~JpegEncodeConsumer() {
    finalize();
}

bool JpegEncodeConsumer::initialize(const std::vector<Buffer*>& first_buffers) {
    if (first_buffers.empty() || !first_buffers[0]) {
        fprintf(stderr, "[JpegEncodeConsumer] initialize: first_buffers 为空\n");
        return false;
    }

    AVFrame* first_frame = first_buffers[0]->getAVFrame();
    if (!first_frame || !first_frame->data[0]) {
        fprintf(stderr, "[JpegEncodeConsumer] initialize: 首帧 AVFrame 无效\n");
        return false;
    }

    int src_width  = first_frame->width;
    int src_height = first_frame->height;
    AVPixelFormat src_pix_fmt = static_cast<AVPixelFormat>(first_frame->format);

    fprintf(stderr, "[JpegEncodeConsumer] 初始化: 源 %dx%d pix_fmt=%d(%s) encoder=%s\n",
            src_width, src_height, src_pix_fmt,
            av_get_pix_fmt_name(src_pix_fmt) ? av_get_pix_fmt_name(src_pix_fmt) : "?",
            config_.encoder_name.c_str());

    // 1. 创建编码输入 BufferPool（AVFrame 分配器）
    {
        size_t frame_size = static_cast<size_t>(src_width) * src_height * 3 / 2;
        BufferAllocatorFacade allocator(
            BufferAllocatorFactory::AllocatorType::AVFRAME);
        input_pool_id_ = allocator.allocatePoolWithBuffers(
            4, frame_size, "JpegEncodeInput", "ENCODE_INPUT");
        if (input_pool_id_ == 0) {
            LOG4CPLUS_ERROR(logger_, "创建编码输入 BufferPool 失败");
            return false;
        }
        input_pool_ = BufferPoolRegistry::getInstance().getPool(input_pool_id_).lock();
        if (!input_pool_) {
            LOG4CPLUS_ERROR(logger_, "获取编码输入 BufferPool 失败");
            return false;
        }
    }

    // 2. 创建编码 Worker + VideoProductionLine（buffer 模式）
    std::vector<std::string> encoder_candidates = { config_.encoder_name };
    if (config_.encoder_name != "mjpeg") {
        encoder_candidates.push_back("mjpeg");
    }

    bool started = false;
    for (const auto& enc_name : encoder_candidates) {
        WorkerConfig enc_config;
        enc_config.global.worker_type = WorkerType::FFMPEG_ENCODE;
        enc_config.display.width  = src_width;
        enc_config.display.height = src_height;
        enc_config.encoder.name = enc_name;
        enc_config.encoder.enable_hardware =
            (enc_name.find("taco") != std::string::npos);
        enc_config.encoder.input_pix_fmt = static_cast<int>(src_pix_fmt);
        enc_config.encoder.jpeg.quality = config_.quality;
        enc_config.encoder.framerate_num = config_.target_fps > 0 ? config_.target_fps : 15;
        enc_config.encoder.framerate_den = 1;
        enc_config.encoder.gop_size = 1;
        enc_config.encoder.max_b_frames = 0;
        enc_config.data_source.buffer_count = 4;
        enc_config.data_source.buffer_mode = true;

        encode_pipeline_ = std::make_unique<::VideoProductionLine>(
            false, 1, false);

        if (encode_pipeline_->start(enc_config)) {
            // 关联编码输入 pool
            auto facade = encode_pipeline_->getWorkerFacade();
            if (facade) {
                facade->setSourceBufferPool(
                    BufferPoolRegistry::getInstance().getPool(input_pool_id_));
            }

            encode_pool_id_ = encode_pipeline_->getWorkingBufferPoolId();
            fprintf(stderr, "[JpegEncodeConsumer] 编码器 '%s' 启动成功 (BufferPool模式), "
                    "input_pool=%lu, output_pool=%lu\n",
                    enc_name.c_str(), input_pool_id_, encode_pool_id_);
            started = true;
            break;
        }

        fprintf(stderr, "[JpegEncodeConsumer] 编码器 '%s' 启动失败\n", enc_name.c_str());
        encode_pipeline_.reset();
    }

    if (!started) {
        fprintf(stderr, "[JpegEncodeConsumer] 所有编码器均失败\n");
        return false;
    }

    // 3. 启动读取线程
    reader_running_ = true;
    reader_thread_ = std::thread(&JpegEncodeConsumer::readerThreadFunc, this);

    if (config_.target_fps > 0 && config_.target_fps < 60) {
        frame_interval_ = std::max(1, 30 / config_.target_fps);
    }

    if (!config_.output_pipe.empty()) {
        if (!openPipe()) {
            LOG4CPLUS_WARN_FMT(logger_, "打开 FIFO '%s' 失败", config_.output_pipe.c_str());
        }
    }

    LOG4CPLUS_INFO_FMT(logger_, "初始化完成: input_pool=%lu output_pool=%lu interval=%d",
                       input_pool_id_, encode_pool_id_, frame_interval_);
    return true;
}

bool JpegEncodeConsumer::consume(const std::vector<Buffer*>& buffers, int frame_index) {
    if (buffers.empty() || !buffers[0] || !input_pool_ || !encode_pipeline_) {
        return true;
    }

    if (frame_interval_ > 1 && (frame_index % frame_interval_ != 0)) {
        skipped_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    AVFrame* src_frame = buffers[0]->getAVFrame();
    if (!src_frame || !src_frame->data[0]) {
        return true;
    }

    // acquireFree: 取一个空闲 buffer（非阻塞，取不到就跳过）
    Buffer* dst_buf = input_pool_->acquireFree(false, 0);
    if (!dst_buf) {
        skipped_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // av_frame_ref: 引用计数共享解码 AVFrame（微秒级，零拷贝，保留完整 DMA 元数据）
    AVFrame* dst_frame = dst_buf->getAVFrame();
    if (dst_frame) {
        av_frame_unref(dst_frame);
        if (av_frame_ref(dst_frame, src_frame) < 0) {
            input_pool_->releaseFree(dst_buf);
            error_count_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }

    // submitFilled: 编码 Worker 的 readRawFrame 会 acquireFilled 取到这个 buffer
    input_pool_->submitFilled(dst_buf);
    return true;
}

void JpegEncodeConsumer::finalize() {
    reader_running_.store(false, std::memory_order_release);

    if (encode_pipeline_) {
        encode_pipeline_->stop();
        encode_pipeline_.reset();
    }

    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }

    // 清理 input pool 中残留帧的引用
    if (input_pool_) {
        for (Buffer* buf : input_pool_->getAllManagedBuffers()) {
            AVFrame* f = buf->getAVFrame();
            if (f) av_frame_unref(f);
        }
        input_pool_.reset();
    }

    if (pipe_fd_ >= 0) {
        ::close(pipe_fd_);
        pipe_fd_ = -1;
    }

    LOG4CPLUS_INFO_FMT(logger_, "finalize: encoded=%d skipped=%d errors=%d",
                       encoded_count_.load(), skipped_count_.load(),
                       error_count_.load());
}

std::string JpegEncodeConsumer::getStats() const {
    return "JpegEncode: encoded=" + std::to_string(encoded_count_.load()) +
           " skipped=" + std::to_string(skipped_count_.load()) +
           " errors=" + std::to_string(error_count_.load());
}

void JpegEncodeConsumer::readerThreadFunc() {
    LOG4CPLUS_INFO_FMT(logger_, "编码输出读取线程启动, pool_id=%lu", encode_pool_id_);

    auto pool_weak = BufferPoolRegistry::getInstance().getPool(encode_pool_id_);
    auto pool = pool_weak.lock();
    if (!pool) {
        LOG4CPLUS_ERROR_FMT(logger_, "编码 BufferPool (id=%lu) 无效，读取线程退出", encode_pool_id_);
        return;
    }

    LOG4CPLUS_INFO_FMT(logger_, "读取线程获得 pool '%s' (free=%d, filled=%d)",
                       pool->getName().c_str(),
                       pool->getFreeCount(), pool->getFilledCount());

    int poll_count = 0;
    while (reader_running_.load(std::memory_order_acquire)) {
        Buffer* buf = pool->acquireFilled(true, 200);
        poll_count++;
        if (poll_count % 50 == 1) {
            LOG4CPLUS_DEBUG_FMT(logger_, "读取线程 poll #%d: buf=%p free=%d filled=%d",
                               poll_count, (void*)buf,
                               pool->getFreeCount(), pool->getFilledCount());
        }
        if (!buf) continue;

        AVPacket* pkt = buf->getAVPacket();
        if (pkt && pkt->data && pkt->size > 0) {
            encoded_count_.fetch_add(1, std::memory_order_relaxed);

            if (on_frame_) {
                on_frame_(pkt->data, static_cast<size_t>(pkt->size));
            }
            if (pipe_fd_ >= 0) {
                writeToPipe(pkt->data, pkt->size);
            }
        }

        pool->releaseFilled(buf);
    }

    LOG4CPLUS_INFO(logger_, "编码输出读取线程退出");
}

bool JpegEncodeConsumer::openPipe() {
    const char* path = config_.output_pipe.c_str();

    struct stat st;
    if (stat(path, &st) != 0) {
        if (mkfifo(path, 0666) != 0) {
            LOG4CPLUS_ERROR_FMT(logger_, "mkfifo('%s') 失败: %s",
                               path, strerror(errno));
            return false;
        }
    } else if (!S_ISFIFO(st.st_mode)) {
        LOG4CPLUS_ERROR_FMT(logger_, "'%s' 存在但不是 FIFO", path);
        return false;
    }

    pipe_fd_ = ::open(path, O_WRONLY | O_NONBLOCK);
    if (pipe_fd_ < 0) {
        if (errno == ENXIO) {
            LOG4CPLUS_WARN_FMT(logger_, "FIFO '%s' 暂无读取端（ENXIO），稍后重试", path);
        } else {
            LOG4CPLUS_ERROR_FMT(logger_, "open('%s') 失败: %s", path, strerror(errno));
        }
        return false;
    }

    LOG4CPLUS_INFO_FMT(logger_, "FIFO '%s' 已打开 (fd=%d)", path, pipe_fd_);
    return true;
}

void JpegEncodeConsumer::writeToPipe(const uint8_t* data, int size) {
    if (pipe_fd_ < 0 || !data || size <= 0) return;

    uint32_t net_size = htonl(static_cast<uint32_t>(size));
    ssize_t written = ::write(pipe_fd_, &net_size, sizeof(net_size));
    if (written != sizeof(net_size)) {
        if (errno == EPIPE || errno == EAGAIN) {
            return;
        }
        LOG4CPLUS_WARN_FMT(logger_, "写 FIFO 长度头失败: %s", strerror(errno));
        return;
    }

    ssize_t total = 0;
    while (total < size) {
        written = ::write(pipe_fd_, data + total, size - total);
        if (written < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            if (errno == EPIPE) return;
            LOG4CPLUS_WARN_FMT(logger_, "写 FIFO 数据失败: %s", strerror(errno));
            return;
        }
        total += written;
    }
}

} // namespace consumer
