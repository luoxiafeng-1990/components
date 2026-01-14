#include "productionline/worker/BufferPacketSource.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "common/Logger.hpp"
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

BufferPacketSource::BufferPacketSource(const AVCodecParameters* codec_params)
    : codec_params_(codec_params)
    , source_pool_()
    , is_open_(false)  // 🎯 原子变量初始化
{
    if (!codec_params_) {
        LOG_WARN("[BufferPacketSource] codec_params is nullptr");
    }
    LOG_DEBUG("[BufferPacketSource] 构造函数（v2.13：Pool 模式）");
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
    
    // ⭐ v2.13：Pool 模式 - 直接从 BufferPool 获取数据
    auto pool = source_pool_.lock();
    if (!pool) {
        LOG_ERROR("[BufferPacketSource] Source BufferPool已销毁");
        return AVERROR_EOF;
    }
    
    // 1. 从 filled queue 获取 Buffer（带超时，避免死锁）
    Buffer* filled_buffer = pool->acquireFilled(true, 100);  // 100ms 超时
    if (!filled_buffer) {
        // 超时或 Pool 关闭
        return AVERROR(EAGAIN);  // 返回 EAGAIN 表示暂时无数据，可以重试
    }
    
    // 2. 从 Buffer 获取 AVPacket
    AVPacket* src_packet = filled_buffer->getAVPacket();
    if (!src_packet || src_packet->data == nullptr || src_packet->size == 0) {
        LOG_WARN("[BufferPacketSource] Buffer 中的 AVPacket 无效");
        pool->releaseFilled(filled_buffer);  // 释放 Buffer
        return AVERROR_EOF;
    }
    
    // 3. 复制 packet 数据（使用 av_packet_ref，零拷贝）
    int ret = copyPacket(packet, src_packet);
    
    // 4. ⭐ 立即释放 Buffer 回 filled queue（已完成复制）
    pool->releaseFilled(filled_buffer);
    
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
    // Buffer 模式的 EOF 状态：检查 Pool 是否还有数据
    if (!is_open_.load(std::memory_order_acquire)) {
        return true;  // 未打开，视为 EOF
    }
    
    // 检查 BufferPool 是否可用
    auto pool = source_pool_.lock();
    if (!pool) {
        return true;  // Pool 已销毁，视为 EOF
    }
    
    // ⭐ 注意：无法在不阻塞的情况下准确判断 Pool 是否有数据
    // 因此返回 false，让 readPacket() 尝试获取（会超时返回）
    return false;
}

int BufferPacketSource::getSourceWidth() const {
    const AVCodecParameters* params = getCodecParameters();
    return params ? params->width : 0;
}

int BufferPacketSource::getSourceHeight() const {
    const AVCodecParameters* params = getCodecParameters();
    return params ? params->height : 0;
}

AVPixelFormat BufferPacketSource::getSourcePixelFormat() const {
    const AVCodecParameters* params = getCodecParameters();
    return params ? static_cast<AVPixelFormat>(params->format) : AV_PIX_FMT_NONE;
}

void BufferPacketSource::setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) {
    source_pool_ = pool_weak;
    LOG_DEBUG("[BufferPacketSource] ⭐ v2.13：已设置源 BufferPool");
}

int BufferPacketSource::copyPacket(AVPacket* dst_packet, const AVPacket* src_packet) {
    if (!dst_packet || !src_packet) {
        return AVERROR(EINVAL);
    }
    
    // 使用 av_packet_ref 复制 packet（包括所有数据和元数据）
    return av_packet_ref(dst_packet, src_packet);
}

