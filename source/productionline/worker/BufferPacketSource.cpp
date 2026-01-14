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
    , is_shared_mode_(false)  // ⭐ v2.18：普通模式
    , total_subscribers_(0)
    , current_request_count_(0)
    , current_buffer_(nullptr)
    , mutex_()
    , cv_()
    , is_running_(false)
{
    if (!codec_params_) {
        LOG_WARN("[BufferPacketSource] codec_params is nullptr");
    }
    LOG_DEBUG("[BufferPacketSource] 构造函数（v2.13：Pool 模式）");
}

BufferPacketSource::BufferPacketSource(const AVCodecParameters* codec_params, size_t subscriber_count)
    : codec_params_(codec_params)
    , source_pool_()
    , is_open_(false)
    , is_shared_mode_(true)  // ⭐ v2.18：共享模式
    , total_subscribers_(subscriber_count)
    , current_request_count_(0)
    , current_buffer_(nullptr)
    , mutex_()
    , cv_()
    , is_running_(false)
{
    if (!codec_params_) {
        LOG_WARN("[BufferPacketSource] codec_params is nullptr");
    }
    if (subscriber_count < 2) {
        LOG_WARN_FMT("[BufferPacketSource] Shared mode with subscriber_count=%zu (should be >= 2)", subscriber_count);
    }
    LOG_INFO_FMT("[BufferPacketSource] ⭐ v2.18 共享模式：创建发布者，订阅者数量=%zu", subscriber_count);
}

BufferPacketSource::~BufferPacketSource() {
    LOG_DEBUG("[BufferPacketSource] 析构函数开始");
    close();
    
    // ⭐ v2.18：共享模式清理
    if (is_shared_mode_) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_buffer_) {
            // 释放当前持有的 Buffer
            auto pool = source_pool_.lock();
            if (pool) {
                pool->releaseFilled(current_buffer_);
            }
            current_buffer_ = nullptr;
        }
    }
    
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
    
    // ⭐ v2.18：共享模式初始化
    if (is_shared_mode_) {
        is_running_.store(true, std::memory_order_release);
        current_request_count_.store(0, std::memory_order_release);
        current_buffer_ = nullptr;
        LOG_INFO_FMT("[BufferPacketSource] ⭐ 共享模式已激活：等待 %zu 个订阅者", total_subscribers_);
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
    
    // ⭐ v2.18：共享模式关闭
    if (is_shared_mode_) {
        is_running_.store(false, std::memory_order_release);
        cv_.notify_all();  // 唤醒所有等待的线程
        
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_buffer_) {
            auto pool = source_pool_.lock();
            if (pool) {
                pool->releaseFilled(current_buffer_);
            }
            current_buffer_ = nullptr;
        }
    }
}

bool BufferPacketSource::isOpen() const {
    return is_open_.load(std::memory_order_acquire);  // 🎯 原子操作读取状态
}

int BufferPacketSource::readPacket(AVPacket* packet) {
    if (!is_open_.load(std::memory_order_acquire) || !packet) {
        return AVERROR(EINVAL);
    }
    
    auto pool = source_pool_.lock();
    if (!pool) {
        LOG_ERROR("[BufferPacketSource] Source BufferPool已销毁");
        return AVERROR_EOF;
    }
    
    // ========== v2.18：共享模式（发布-订阅）==========
    if (is_shared_mode_) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // 1. 增加请求计数器
        size_t my_request = current_request_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
        
        // 2. 第一个到达的订阅者：负责获取新的 Buffer
        if (my_request == 1) {
            // ⭐ Publisher 职责：获取新 Buffer
            // 释放旧 Buffer（如果有）
            if (current_buffer_) {
                pool->releaseFilled(current_buffer_);
                current_buffer_ = nullptr;
            }
            
            // 获取新 Buffer
            lock.unlock();  // 释放锁，避免阻塞其他订阅者
            Buffer* new_buffer = pool->acquireFilled(true, 100);  // 100ms 超时
            lock.lock();    // 重新获取锁
            
            if (!new_buffer) {
                // 获取失败，重置计数器
                current_request_count_.store(0, std::memory_order_release);
                cv_.notify_all();
                return AVERROR(EAGAIN);
            }
            
            // 验证 Buffer
            AVPacket* src_packet = new_buffer->getAVPacket();
            if (!src_packet || src_packet->data == nullptr || src_packet->size == 0) {
                LOG_WARN("[BufferPacketSource] Buffer 中的 AVPacket 无效");
                pool->releaseFilled(new_buffer);
                current_request_count_.store(0, std::memory_order_release);
                cv_.notify_all();
                return AVERROR_EOF;
            }
            
            // ✅ 设置为当前共享 Buffer
            current_buffer_ = new_buffer;
            
            LOG_DEBUG_FMT("[BufferPacketSource] ⭐ Publisher 获取新 Buffer：等待 %zu 个订阅者", 
                         total_subscribers_);
            
            // 唤醒其他等待的订阅者
            cv_.notify_all();
        } else {
            // ⭐ 其他订阅者：等待 Publisher 获取 Buffer
            cv_.wait(lock, [this]() {
                return current_buffer_ != nullptr || 
                       current_request_count_.load(std::memory_order_acquire) == 0 ||
                       !is_running_.load(std::memory_order_acquire);
            });
            
            // 检查是否被关闭
            if (!is_running_.load(std::memory_order_acquire)) {
                return AVERROR_EOF;
            }
            
            // 检查 Publisher 是否获取成功
            if (!current_buffer_) {
                return AVERROR(EAGAIN);
            }
        }
        
        // 3. 所有订阅者复制 packet 数据
        AVPacket* src_packet = current_buffer_->getAVPacket();
        int ret = copyPacket(packet, src_packet);
        
        if (ret < 0) {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_ERROR_FMT("[BufferPacketSource] Failed to copy packet: %s", err_buf);
        }
        
        // 4. 最后一个订阅者：重置计数器
        size_t completed = current_request_count_.fetch_sub(1, std::memory_order_acq_rel);
        if (completed == 1) {
            // ⭐ 所有订阅者都完成了，准备下一轮
            current_request_count_.store(0, std::memory_order_release);
            LOG_DEBUG("[BufferPacketSource] ⭐ 所有订阅者已完成，准备下一轮");
        }
        
        return ret;
    }
    
    // ========== v2.13：普通模式 - 直接从 BufferPool 获取数据 ==========
    
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

