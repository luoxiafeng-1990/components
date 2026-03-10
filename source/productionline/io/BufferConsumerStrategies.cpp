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

cv::Mat OpencvConsumer::bufferToMat(Buffer* buf) const {
    if (!buf) return cv::Mat();

    // 路径 1：Buffer 已关联 cv::Mat（已由上层转换好）
    if (buf->getMat() && !buf->getMat()->empty()) {
        return buf->getMat()->clone();
    }

    // 路径 2：cv::Mat(AVFrame*) 零拷贝接口（来自 opencv2/core/tacv.hpp）
    // 注意：Mat 与 AVFrame 共享底层内存，调用方必须在 av_frame_unref 之前销毁 Mat
    AVFrame* frame = buf->getAVFrame();
    if (frame && frame->data[0] && frame->width > 0 && frame->height > 0) {
        return cv::Mat(frame);
    }

    LOG4CPLUS_WARN(log4cplus::Logger::getRoot(),
                   "OpencvConsumer::bufferToMat: no usable data source in Buffer");
    return cv::Mat();
}

cv::Mat OpencvConsumer::applyOpencvTransform(const cv::Mat& src) const {
    if (src.empty()) return src;

    switch (config_.op_type) {
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
        default:
            return src;
    }
}

bool OpencvConsumer::consume(const std::vector<Buffer*>& buffers, int frame_index) {
    if (!initialized_) {
        if (!initialize(buffers)) return false;
    }

    frames_processed_++;

    if (buffers.size() >= 2 && buffers[0] && buffers[1]) {
        // ── 比较模式：转换两个 Buffer 为 Mat，再交由 BufferComparator 比较 ──
        cv::Mat mat0 = bufferToMat(buffers[0]);
        cv::Mat mat1 = bufferToMat(buffers[1]);

        // 对 SW 参考帧（mat1）施加 opencv 变换，以便与 HW 输出（mat0）比较
        if (config_.op_type != OpencvType::OpType::NONE) {
            mat0 = applyOpencvTransform(mat0);
            mat1 = applyOpencvTransform(mat1);
        }

        // 将 Mat 临时挂载到 Buffer，以便 BufferComparator 走 is_mat 路径
        buffers[0]->setMat(&mat0);
        buffers[1]->setMat(&mat1);

        cv::imwrite("mat0_output_"+std::to_string(frame_index)+".jpg",mat0);
        cv::imwrite("mat1_output_"+std::to_string(frame_index)+".jpg",mat1);
        std::cout << "saved picture" << std::endl;

        auto result = comparator_->compare(buffers[0], buffers[1]);

        // 清除临时挂载
        buffers[0]->setMat(nullptr);
        buffers[1]->setMat(nullptr);

        frames_compared_++;
        psnr_sum_ += result.psnr_avg;
        ssim_sum_ += result.ssim_avg;

        if (!result.passed) {
            passed_ = false;
        }

        if (config_.verbose) {
            LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(),
                "OpencvConsumer [frame %d] PSNR=%.2f dB  SSIM=%.4f  %s",
                frame_index, result.psnr_avg, result.ssim_avg,
                result.passed ? "PASS" : "FAIL");
        }
    } else if (!buffers.empty() && buffers[0]) {
        // ── 单 Buffer 模式：做 Mat 转换，并按 op_type 执行 resize/crop ──
        cv::Mat mat = bufferToMat(buffers[0]);
        if (config_.op_type != OpencvType::OpType::NONE) {
            cv::Mat transformed = applyOpencvTransform(mat);
            if (config_.verbose) {
                const char* op_name = (config_.op_type == OpencvType::OpType::RESIZE)
                                      ? "RESIZE" : "CROP";
                LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(),
                    "OpencvConsumer [frame %d] %s: %dx%d -> %dx%d",
                    frame_index, op_name,
                    mat.cols, mat.rows,
                    transformed.cols, transformed.rows);
            }
        } else if (config_.verbose) {
            LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(),
                "OpencvConsumer [frame %d] Mat size=%dx%d",
                frame_index, mat.cols, mat.rows);
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
