#include "productionline/worker/BufferPacketSource.hpp"
#include "common/Logger.hpp"
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

BufferPacketSource::BufferPacketSource(const AVCodecParameters* codec_params)
    : codec_params_(codec_params)
    , current_buffer_(nullptr)
    , is_open_(false)  // 🎯 原子变量初始化
{
    if (!codec_params_) {
        LOG_WARN("[BufferPacketSource] codec_params is nullptr");
    }
    LOG_DEBUG("[BufferPacketSource] 构造函数");
}

BufferPacketSource::~BufferPacketSource() {
    LOG_DEBUG("[BufferPacketSource] 析构函数开始");
    close();
    LOG_DEBUG("[BufferPacketSource] 析构函数体结束");
}

bool BufferPacketSource::open() {
    if (is_open_.load(std::memory_order_acquire)) {
        return true;  // 已经打开
    }
    
    // Buffer 模式下，只需要验证 codec_params 是否有效
    if (!codec_params_) {
        LOG_ERROR("[BufferPacketSource] Cannot open: codec_params is nullptr");
        return false;
    }
    
    is_open_.store(true, std::memory_order_release);  // 🎯 原子操作设置状态
    LOG_DEBUG("[BufferPacketSource] Opened (Buffer mode)");
    
    return true;
}

void BufferPacketSource::close() {
    current_buffer_ = nullptr;
    // 🎯 原子检查并设置：如果 is_open_ 是 true，则设置为 false
    bool expected = true;
    if (!is_open_.compare_exchange_strong(expected, false,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        // is_open_ 已经是 false，说明已经关闭过了，直接返回
        return;
    }
}

bool BufferPacketSource::isOpen() const {
    return is_open_.load(std::memory_order_acquire);  // 🎯 原子操作读取状态
}

int BufferPacketSource::readPacket(AVPacket* packet) {
    if (!is_open_.load(std::memory_order_acquire) || !packet) {
        return AVERROR(EINVAL);
    }
    
    if (!current_buffer_) {
        LOG_DEBUG("[BufferPacketSource] No current buffer set, returning EOF");
        return AVERROR_EOF;
    }
    
    // 从 Buffer 获取 packet
    AVPacket* src_packet = current_buffer_->getAVPacket();
    if (!src_packet) {
        LOG_WARN("[BufferPacketSource] Buffer has no AVPacket");
        return AVERROR_EOF;
    }
    
    // 检查 packet 是否有效
    if (src_packet->data == nullptr || src_packet->size == 0) {
        LOG_DEBUG("[BufferPacketSource] Packet is empty, returning EOF");
        return AVERROR_EOF;
    }
    
    // 复制 packet 数据（使用 av_packet_ref）
    int ret = copyPacket(packet, src_packet);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR_FMT("[BufferPacketSource] Failed to copy packet: %s", err_buf);
        return ret;
    }
    
    return 0;  // 成功
}

const AVCodecParameters* BufferPacketSource::getCodecParameters() const {
    return codec_params_;
}

int BufferPacketSource::getVideoStreamIndex() const {
    // Buffer 模式下，没有流索引的概念，返回 0（表示第一个/唯一的流）
    return 0;
}

int BufferPacketSource::getTotalFrames() const {
    // Buffer 模式下，无法知道总帧数（流式数据）
    return -1;
}

long BufferPacketSource::getFileSize() const {
    // Buffer 模式下，没有文件大小概念
    return -1;
}

std::string BufferPacketSource::getFilePath() const {
    // Buffer 模式下，没有文件路径概念
    return std::string();
}

bool BufferPacketSource::seek(int frame_index) {
    // Buffer 模式：流式数据，不支持 seek
    LOG_WARN("[BufferPacketSource] Seek not supported in Buffer mode (streaming data)");
    return false;
}

bool BufferPacketSource::isEof() const {
    // Buffer 模式的 EOF 状态：当前 buffer 为空或无效
    if (!is_open_.load(std::memory_order_acquire)) {
        return true;
    }
    
    if (!current_buffer_) {
        return true;  // 没有当前 buffer，视为 EOF
    }
    
    // 检查 buffer 中的 packet 是否有效
    AVPacket* src_packet = current_buffer_->getAVPacket();
    if (!src_packet || src_packet->data == nullptr || src_packet->size == 0) {
        return true;  // packet 无效，视为 EOF
    }
    
    return false;  // 有有效的 packet，不是 EOF
}

void BufferPacketSource::setCurrentBuffer(Buffer* buffer) {
    current_buffer_ = buffer;
}

void BufferPacketSource::clearCurrentBuffer() {
    current_buffer_ = nullptr;
}

int BufferPacketSource::copyPacket(AVPacket* dst_packet, const AVPacket* src_packet) {
    if (!dst_packet || !src_packet) {
        return AVERROR(EINVAL);
    }
    
    // 使用 av_packet_ref 复制 packet（包括所有数据和元数据）
    return av_packet_ref(dst_packet, src_packet);
}

