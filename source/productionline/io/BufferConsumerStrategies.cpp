/**
 * @file BufferConsumerStrategies.cpp
 * @brief Buffer 消费策略实现
 */

#include "productionline/io/BufferConsumerStrategies.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "display/DisplayDeviceFactory.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdio>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
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

DisplayConsumer::DisplayConsumer(const DisplayType& config)
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
        display_ = DisplayDeviceFactory::create(config_);
        if (!display_->initialize(config_.device_id)) {
            LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), "DisplayConsumer: Failed to initialize display device");
            return false;
        }
        
        initialized_ = true;
        LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(), 
            "DisplayConsumer: Initialized (mode=%s)",
            config_.mode == DisplayType::TACO_VO ? "TACO_VO" :
            config_.mode == DisplayType::SHARED_FB ? "SHARED_FB" : "FRAMEBUFFER");
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
        comparator_ = std::make_unique<productionline::io::BufferComparator>();
        
        productionline::io::CompareConfig cmp_config;
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

// ============================================================
// OpencvConsumer 实现
// ============================================================

OpencvConsumer::OpencvConsumer(const OpencvType& config)
    : config_(config)
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
    productionline::io::CompareConfig cmp_cfg;
    cmp_cfg.enable_psnr = config_.enable_psnr;
    cmp_cfg.enable_ssim = config_.enable_ssim;
    cmp_cfg.min_psnr    = config_.min_psnr;
    cmp_cfg.min_ssim    = config_.min_ssim;
    cmp_cfg.verbose     = config_.verbose;

    comparator_ = std::make_unique<productionline::io::BufferComparator>();
    if (!comparator_->open(cmp_cfg)) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(),
                        "OpencvConsumer: Failed to open BufferComparator");
        return false;
    }

    initialized_ = true;
    LOG4CPLUS_INFO(log4cplus::Logger::getRoot(),
                   "OpencvConsumer: Initialized"
                   " (psnr=" << (config_.enable_psnr ? "ON" : "OFF")
                   << ", ssim=" << (config_.enable_ssim ? "ON" : "OFF") << ")");
    return true;
}

cv::Mat OpencvConsumer::applyOpencvTransform(const cv::Mat& src, int frame_index) const {
    if (src.empty()) return src;

    switch (config_.op_type) {
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
            const auto& r = config_.resize;
            if (r.dst_width <= 0 && r.dst_height <= 0 && r.fx <= 0.0 && r.fy <= 0.0) {
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
            const auto& c = config_.crop;
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
            const auto& m = config_.morph;
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
            const auto& m = config_.morph;
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
            const auto& m = config_.morph;
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
            const auto& m = config_.morph;
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
            const auto& s = config_.sobel;
            int ksize = (s.ksize % 2 == 0) ? s.ksize + 1 : s.ksize;
            cv::Mat gray, dst;
            if (src.channels() != 1) cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
            else gray = src;
            cv::Sobel(gray, dst, CV_8U, s.dx, s.dy, ksize, s.scale, s.delta);
            return dst;
        }
        case OpencvType::OpType::CANNY: {
            const auto& c = config_.canny;
            cv::Mat gray, dst;
            if (src.channels() != 1) cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
            else gray = src;
            cv::Canny(gray, dst, c.threshold1, c.threshold2, c.aperture_size);
            return dst;
        }
        case OpencvType::OpType::LAPLACIAN: {
            const auto& l = config_.laplacian;
            int ksize = (l.ksize % 2 == 0) ? l.ksize + 1 : l.ksize;
            cv::Mat gray, dst;
            if (src.channels() != 1) cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
            else gray = src;
            cv::Laplacian(gray, dst, CV_8U, ksize, l.scale, l.delta);
            return dst;
        }
        case OpencvType::OpType::TRANSLATE: {
            const auto& t = config_.translate;
            cv::Mat M = (cv::Mat_<double>(2, 3) << 1, 0, t.tx, 0, 1, t.ty);
            cv::Mat dst;
            cv::warpAffine(src, dst, M, src.size());
            return dst;
        }
        case OpencvType::OpType::ROTATE: {
            const auto& r = config_.rotate;
            cv::Point2f center(src.cols / 2.0f, src.rows / 2.0f);
            cv::Mat M = cv::getRotationMatrix2D(center, r.angle, r.scale);
            cv::Mat dst;
            cv::warpAffine(src, dst, M, src.size());
            return dst;
        }
        case OpencvType::OpType::PERSPECTIVE: {
            const auto& p = config_.perspective;
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
            const auto& l = config_.draw_line;
            cv::Mat dst = src.clone();
            cv::line(dst,
                     cv::Point(l.x1, l.y1),
                     cv::Point(l.x2, l.y2),
                     cv::Scalar(0, 255, 0),
                     l.thickness);
            return dst;
        }
        case OpencvType::OpType::DRAW_RECT: {
            const auto& r = config_.draw_rect;
            cv::Mat dst = src.clone();
            cv::rectangle(dst,
                          cv::Rect(r.x, r.y, r.width, r.height),
                          cv::Scalar(0, 0, 255),
                          r.thickness);
            return dst;
        }
        case OpencvType::OpType::PUT_TEXT: {
            const auto& t = config_.put_text;
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
            const auto& g = config_.gaussian_blur;
            int k = (g.ksize % 2 == 0) ? g.ksize + 1 : g.ksize;
            cv::Mat dst;
            cv::GaussianBlur(src, dst, cv::Size(k, k), g.sigma_x, g.sigma_x);
            return dst;
        }
        case OpencvType::OpType::THRESHOLD: {
            const auto& t = config_.threshold;
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
            const auto& c = config_.cvtcolor;
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

bool OpencvConsumer::consume(const std::vector<Buffer*>& buffers, int frame_index) {
    if (!initialized_) {
        if (!initialize(buffers)) return false;
    }

    frames_processed_++;

    if (buffers.size() >= 2 && buffers[0] && buffers[1]) {
        std::cout << "dual buffer is not supported" << std::endl;
    } else if (!buffers.empty() && buffers[0]) {
        // ── 单 Buffer 模式：直接用 frame->data 创建 Mat（零拷贝），无需深拷贝 ──
        AVFrame* orig_frame = buffers[0]->getAVFrame();
        if (!orig_frame) {
            LOG4CPLUS_WARN(log4cplus::Logger::getRoot(),
                "OpencvConsumer: no AVFrame in buffer, skip");
            return true;
        }

        // 直接用同一个 frame 创建两个 Mat，共享数据指针
        // applyOpencvTransform 会返回新的 Mat（clone 或新建），所以变换后数据独立
        cv::Mat mat_hw = cv::Mat(orig_frame);

        std::cout << avframeInfoToString(orig_frame) << std::endl;

        // 用 frame->data[0] 再创建一个 Mat（与 mat_hw 共享原始数据）
        int type = CV_8UC1;
        switch (orig_frame->format) {
            case AV_PIX_FMT_BGR24:
            case AV_PIX_FMT_RGB24:
                type = CV_8UC3;
                break;
            case AV_PIX_FMT_BGRA:
            case AV_PIX_FMT_RGBA:
                type = CV_8UC4;
                break;
        }
        cv::Mat mat_sw(orig_frame->height, orig_frame->width, type,
                       orig_frame->data[0], orig_frame->linesize[0]);

        // 对两路施加同一 OpenCV 变换
        if (config_.op_type != OpencvType::OpType::NONE) {
            mat_hw = applyOpencvTransform(mat_hw, frame_index);
            mat_sw = applyOpencvTransform(mat_sw, frame_index);

            std::cout << matInfoToString(mat_hw) << std::endl;
            std::cout << matInfoToString(mat_sw) << std::endl;
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

        if (config_.verbose) {
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
    if (config_.op_type == OpencvType::OpType::SAVE_LOAD_IMG) {
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

} // namespace consumer
