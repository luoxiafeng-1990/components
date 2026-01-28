/**
 * @file BufferConsumerStrategies.cpp
 * @brief 消费者策略实现库（第三部分：策略实现部分）
 * 
 * 此文件包含所有消费者策略的具体实现。
 */

#include "productionline/consumer/BufferConsumerStrategies.hpp"
#include "productionline/io/BufferWriter.hpp"
#include "display/LinuxFramebufferDevice.hpp"
#include "common/Logger.hpp"
#include <sstream>
#include <iomanip>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
}

namespace productionline {
namespace consumer {

// ============================================================================
// DisplayConsumer 实现
// ============================================================================

DisplayConsumer::DisplayConsumer(LinuxFramebufferDevice* display,
                                 bool ch0_enable,
                                 bool ch1_enable)
    : display_(display)
    , ch0_enable_(ch0_enable)
    , ch1_enable_(ch1_enable)
    , success_count_(0)
    , failed_count_(0)
    , total_count_(0)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Consumer.Display")))
{
    if (!display_) {
        LOG4CPLUS_ERROR(logger_, "Display device is nullptr");
    }
}

bool DisplayConsumer::initialize(Buffer* first_buffer) {
    (void)first_buffer;
    // 显示消费者不需要特殊初始化
    return true;
}

bool DisplayConsumer::consume(Buffer* buffer, int channel_id) {
    if (!display_ || !buffer) {
        return false;
    }
    
    total_count_++;
    
    // 等待垂直同步
    display_->waitVerticalSync();
    
    // DMA 显示
    if (display_->displayBufferByDMA(buffer)) {
        success_count_++;
        return true;
    } else {
        failed_count_++;
        LOG4CPLUS_WARN_FMT(logger_, "DMA display failed for ch%d buffer (phys_addr=0x%llx)",
                          channel_id, 
                          (unsigned long long)buffer->getPhysicalAddress());
        return false;
    }
}

std::string DisplayConsumer::getStats() const {
    std::ostringstream oss;
    oss << "Display: total=" << total_count_
        << ", success=" << success_count_
        << ", failed=" << failed_count_;
    if (total_count_ > 0) {
        oss << ", success_rate=" << std::fixed << std::setprecision(1)
            << (100.0 * success_count_ / total_count_) << "%";
    }
    return oss.str();
}

bool DisplayConsumer::shouldConsumeChannel(int channel_id) const {
    if (channel_id == 0) {
        return ch0_enable_;
    } else if (channel_id == 1) {
        return ch1_enable_;
    }
    return false;
}

// ============================================================================
// FileWriterConsumer 实现
// ============================================================================

FileWriterConsumer::FileWriterConsumer(const std::string& output_path,
                                       bool enable_ch0,
                                       bool enable_ch1)
    : output_path_(output_path)
    , initialized_(false)
    , enable_ch0_(enable_ch0)
    , enable_ch1_(enable_ch1)
    , write_count_(0)
    , failed_count_(0)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Consumer.FileWriter")))
{
}

FileWriterConsumer::~FileWriterConsumer() {
    cleanup();
}

bool FileWriterConsumer::initialize(Buffer* first_buffer) {
    if (!first_buffer || !first_buffer->hasImageMetadata()) {
        LOG4CPLUS_ERROR(logger_, "First buffer has no image metadata");
        return false;
    }
    
    AVPixelFormat format = first_buffer->getImageFormat();
    int width = first_buffer->getImageWidth();
    int height = first_buffer->getImageHeight();
    
    writer_ = std::make_unique<io::BufferWriter>();
    if (!writer_->openRaw(output_path_.c_str(), format, width, height)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to open output file: %s", output_path_.c_str());
        return false;
    }
    
    initialized_ = true;
    LOG4CPLUS_INFO_FMT(logger_, "Opened output file: %s (format: %s, %dx%d)",
                      output_path_.c_str(),
                      av_get_pix_fmt_name(format),
                      width, height);
    return true;
}

bool FileWriterConsumer::consume(Buffer* buffer, int channel_id) {
    (void)channel_id;
    
    if (!initialized_ || !writer_) {
        return false;
    }
    
    if (writer_->write(buffer)) {
        write_count_++;
        return true;
    } else {
        failed_count_++;
        return false;
    }
}

void FileWriterConsumer::cleanup() {
    if (writer_) {
        writer_->close();
        writer_.reset();
    }
    initialized_ = false;
}

std::string FileWriterConsumer::getStats() const {
    std::ostringstream oss;
    oss << "FileWriter: written=" << write_count_
        << ", failed=" << failed_count_
        << ", path=" << output_path_;
    return oss.str();
}

bool FileWriterConsumer::shouldConsumeChannel(int channel_id) const {
    if (channel_id == 0) {
        return enable_ch0_;
    } else if (channel_id == 1) {
        return enable_ch1_;
    }
    return false;
}

// ============================================================================
// MultiChannelFileWriterConsumer 实现
// ============================================================================

MultiChannelFileWriterConsumer::MultiChannelFileWriterConsumer(
    const std::vector<std::string>& output_paths,
    bool enable_ch0, bool enable_ch1)
    : output_paths_(output_paths)
    , enable_ch0_(enable_ch0)
    , enable_ch1_(enable_ch1)
    , write_count_(0)
    , failed_count_(0)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Consumer.MultiChannelFileWriter")))
{
    writers_.resize(output_paths.size());
    initialized_.resize(output_paths.size(), false);
}

MultiChannelFileWriterConsumer::~MultiChannelFileWriterConsumer() {
    cleanup();
}

bool MultiChannelFileWriterConsumer::initialize(Buffer* first_buffer) {
    if (!first_buffer || !first_buffer->hasImageMetadata()) {
        LOG4CPLUS_ERROR(logger_, "First buffer has no image metadata");
        return false;
    }
    
    int channel_id = first_buffer->getOutputChannel();
    AVPixelFormat format = first_buffer->getImageFormat();
    int width = first_buffer->getImageWidth();
    int height = first_buffer->getImageHeight();
    
    // 确定 writer 索引
    int writer_index = -1;
    if (channel_id == 0 && enable_ch0_) {
        writer_index = 0;
    } else if (channel_id == 1 && enable_ch1_) {
        writer_index = enable_ch0_ ? 1 : 0;
    }
    
    if (writer_index < 0 || writer_index >= (int)writers_.size()) {
        LOG4CPLUS_ERROR_FMT(logger_, "Invalid writer index: %d (channel: %d)", 
                          writer_index, channel_id);
        return false;
    }
    
    // 创建并初始化 writer
    writers_[writer_index] = std::make_unique<io::BufferWriter>();
    if (!writers_[writer_index]->openRaw(output_paths_[writer_index].c_str(), 
                                        format, width, height)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to open output file: %s", 
                          output_paths_[writer_index].c_str());
        return false;
    }
    
    initialized_[writer_index] = true;
    LOG4CPLUS_INFO_FMT(logger_, "Opened writer[%d] for channel %d: %s (format: %s, %dx%d)",
                      writer_index, channel_id,
                      output_paths_[writer_index].c_str(),
                      av_get_pix_fmt_name(format),
                      width, height);
    
    // 写入第一个 buffer
    writers_[writer_index]->write(first_buffer);
    return true;
}

bool MultiChannelFileWriterConsumer::consume(Buffer* buffer, int channel_id) {
    if (!buffer) {
        return false;
    }
    
    // 确定 writer 索引
    int writer_index = -1;
    if (channel_id == 0 && enable_ch0_) {
        writer_index = 0;
    } else if (channel_id == 1 && enable_ch1_) {
        writer_index = enable_ch0_ ? 1 : 0;
    } else {
        // 通道未启用
        return true;  // 不算失败，只是跳过
    }
    
    if (writer_index < 0 || writer_index >= (int)writers_.size()) {
        return false;
    }
    
    // 如果 writer 还未初始化，现在初始化（对于第二个通道）
    if (!initialized_[writer_index]) {
        if (!buffer->hasImageMetadata()) {
            return false;
        }
        
        AVPixelFormat format = buffer->getImageFormat();
        int width = buffer->getImageWidth();
        int height = buffer->getImageHeight();
        
        writers_[writer_index] = std::make_unique<io::BufferWriter>();
        if (!writers_[writer_index]->openRaw(output_paths_[writer_index].c_str(), 
                                            format, width, height)) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to open writer[%d]: %s", 
                              writer_index, output_paths_[writer_index].c_str());
            return false;
        }
        
        initialized_[writer_index] = true;
        LOG4CPLUS_INFO_FMT(logger_, "Opened writer[%d] for channel %d: %s",
                          writer_index, channel_id, output_paths_[writer_index].c_str());
    }
    
    // 写入 buffer
    if (writers_[writer_index]->write(buffer)) {
        write_count_++;
        return true;
    } else {
        failed_count_++;
        return false;
    }
}

void MultiChannelFileWriterConsumer::cleanup() {
    for (size_t i = 0; i < writers_.size(); i++) {
        if (initialized_[i] && writers_[i]) {
            LOG4CPLUS_INFO_FMT(logger_, "Closing writer[%zu]: %lld frames saved to %s",
                              i, (long long)writers_[i]->getWriteCount(),
                              output_paths_[i].c_str());
            writers_[i]->close();
            writers_[i].reset();
        }
    }
    initialized_.clear();
    initialized_.resize(output_paths_.size(), false);
}

std::string MultiChannelFileWriterConsumer::getStats() const {
    std::ostringstream oss;
    oss << "MultiChannelFileWriter: written=" << write_count_
        << ", failed=" << failed_count_;
    for (size_t i = 0; i < writers_.size(); i++) {
        if (initialized_[i] && writers_[i]) {
            oss << ", writer[" << i << "]=" << writers_[i]->getWriteCount() << " frames";
        }
    }
    return oss.str();
}

bool MultiChannelFileWriterConsumer::shouldConsumeChannel(int channel_id) const {
    if (channel_id == 0) {
        return enable_ch0_;
    } else if (channel_id == 1) {
        return enable_ch1_;
    }
    return false;
}

// ============================================================================
// EncodedStreamWriterConsumer 实现
// ============================================================================

EncodedStreamWriterConsumer::EncodedStreamWriterConsumer(
    const std::string& output_path,
    const AVCodecParameters* codec_params,
    AVRational time_base)
    : output_path_(output_path)
    , codec_params_(codec_params)
    , time_base_(time_base)
    , initialized_(false)
    , packet_count_(0)
    , total_bytes_(0)
    , failed_count_(0)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Consumer.EncodedStreamWriter")))
{
}

EncodedStreamWriterConsumer::~EncodedStreamWriterConsumer() {
    cleanup();
}

bool EncodedStreamWriterConsumer::initialize(Buffer* first_buffer) {
    (void)first_buffer;
    
    if (!codec_params_) {
        LOG4CPLUS_ERROR(logger_, "Codec parameters is nullptr");
        return false;
    }
    
    writer_ = std::make_unique<io::BufferWriter>();
    if (!writer_->openEncoded(output_path_.c_str(), codec_params_, time_base_)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to open encoded output file: %s", 
                          output_path_.c_str());
        return false;
    }
    
    initialized_ = true;
    LOG4CPLUS_INFO_FMT(logger_, "Opened encoded output file: %s", output_path_.c_str());
    return true;
}

bool EncodedStreamWriterConsumer::consume(Buffer* buffer, int channel_id) {
    (void)channel_id;
    
    if (!initialized_ || !writer_) {
        return false;
    }
    
    size_t used_size = buffer->getUsedSize();
    if (used_size > 0) {
        if (writer_->write(buffer)) {
            packet_count_++;
            total_bytes_ += used_size;
            return true;
        } else {
            failed_count_++;
            return false;
        }
    }
    
    return true;  // 空 buffer 不算失败
}

void EncodedStreamWriterConsumer::cleanup() {
    if (writer_) {
        writer_->close();
        writer_.reset();
    }
    initialized_ = false;
}

std::string EncodedStreamWriterConsumer::getStats() const {
    std::ostringstream oss;
    oss << "EncodedStreamWriter: packets=" << packet_count_
        << ", bytes=" << (total_bytes_ / (1024 * 1024)) << " MB"
        << ", failed=" << failed_count_
        << ", path=" << output_path_;
    return oss.str();
}

} // namespace consumer
} // namespace productionline
