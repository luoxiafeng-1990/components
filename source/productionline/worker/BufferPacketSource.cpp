#include "productionline/worker/BufferPacketSource.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "common/Logger.hpp"
#include "common/GlobalThreadPool.hpp"
#include <cstring>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

// ============================================================
// PacketGuard 实现
// ============================================================

PacketGuard::PacketGuard(BufferPacketSource* source) 
    : packet_(nullptr), source_(source) 
{
    if (source_) {
        packet_ = source_->acquirePacket();
    }
}

PacketGuard::~PacketGuard() {
    if (packet_ && source_) {
        source_->releasePacket();
    }
}

PacketGuard::PacketGuard(PacketGuard&& other) noexcept 
    : packet_(other.packet_), source_(other.source_) 
{
    other.packet_ = nullptr;
    other.source_ = nullptr;
}

PacketGuard& PacketGuard::operator=(PacketGuard&& other) noexcept {
    if (this != &other) {
        // 先释放当前持有的资源
        if (packet_ && source_) {
            source_->releasePacket();
        }
        
        // 转移所有权
        packet_ = other.packet_;
        source_ = other.source_;
        other.packet_ = nullptr;
        other.source_ = nullptr;
    }
    return *this;
}

AVPacket* PacketGuard::get() const {
    return packet_;
}

PacketGuard::operator bool() const {
    return packet_ != nullptr;
}

// ============================================================
// BufferPacketSource 实现
// ============================================================

BufferPacketSource::BufferPacketSource(const AVCodecParameters* codec_params)
    : codec_params_(codec_params)
    , source_pool_()
    , is_open_(false)  // 🎯 原子变量初始化
    , is_shared_mode_(false)  // ⭐ v2.18：普通模式
    , total_subscribers_(0)
    , remaining_subscribers_(0)
    , current_buffer_(nullptr)
    , mutex_()
    , cv_subscribers_()
    , cv_fetch_()
    , cv_task_exit_()
    , is_running_(false)
    , fetch_task_running_(false)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.BufferPacketSource")))
{
    if (!codec_params_) {
        LOG4CPLUS_WARN(logger_, "codec_params is nullptr");
    }
    LOG4CPLUS_DEBUG(logger_, "构造函数（v2.13：Pool 模式）");
}

BufferPacketSource::BufferPacketSource(const AVCodecParameters* codec_params, size_t subscriber_count)
    : codec_params_(codec_params)
    , source_pool_()
    , is_open_(false)
    , is_shared_mode_(true)  // ⭐ v2.18：共享模式
    , total_subscribers_(subscriber_count)
    , remaining_subscribers_(0)  // ✅ 初始值为 0，表示没有订阅者在等待
    , current_buffer_(nullptr)
    , mutex_()
    , cv_subscribers_()
    , cv_fetch_()
    , cv_task_exit_()
    , is_running_(false)
    , fetch_task_running_(false)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.BufferPacketSource")))
{
    if (!codec_params_) {
        LOG4CPLUS_WARN(logger_, "codec_params is nullptr");
    }
    if (subscriber_count < 2) {
        LOG4CPLUS_WARN_FMT(logger_, "Shared mode with subscriber_count=%zu (should be >= 2)", subscriber_count);
    }
    LOG4CPLUS_INFO_FMT(logger_, "⭐ v2.18 共享模式（RAII）：创建发布者，订阅者数量=%zu", subscriber_count);
}

BufferPacketSource::~BufferPacketSource() {
    LOG4CPLUS_DEBUG(logger_, "析构函数开始");
    
    // close() 已经处理了所有清理工作（包括等待 Fetch 任务退出）
    close();
    
    // 双重检查：确保 Fetch 任务已退出
    if (fetch_task_running_.load(std::memory_order_acquire)) {
        LOG4CPLUS_WARN(logger_, "析构时 Fetch 任务仍在运行，等待...");
        std::unique_lock<std::mutex> lock(mutex_);
        cv_task_exit_.wait_for(lock, std::chrono::seconds(2), [this]() {
            return !fetch_task_running_.load(std::memory_order_acquire);
        });
    }
    
    LOG4CPLUS_DEBUG(logger_, "析构函数结束");
}

bool BufferPacketSource::open() {
    if (is_open_.load(std::memory_order_acquire)) {
        return true;  // 已经打开
    }
    
    // Buffer 模式下，只需要验证 codec_params 是否有效
    if (!codec_params_) {
        LOG4CPLUS_ERROR(logger_, "Cannot open: codec_params is nullptr");
        return false;
    }
    
    // ⭐ v2.18：共享模式初始化
    if (is_shared_mode_) {
        is_running_.store(true, std::memory_order_release);
        fetch_task_running_.store(false, std::memory_order_release);
        remaining_subscribers_.store(0, std::memory_order_release);  // ✅ 初始值为 0
        current_buffer_ = nullptr;
        
        // 提交 Fetch 任务到全局线程池
        try {
            auto& thread_pool = GlobalThreadPool::getInstance().getThreadPool();
            
            // 标记任务即将启动
            fetch_task_running_.store(true, std::memory_order_release);
            
            // 提交长期运行的任务
            thread_pool.detach_task([this]() {
                fetchTaskFunc();  // 执行 Fetch 逻辑
            });
            
            LOG4CPLUS_INFO_FMT(logger_, "⭐ 共享模式已激活：Fetch 任务已提交到全局线程池，等待 %zu 个订阅者", 
                        total_subscribers_);
        } catch (const std::exception& e) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to submit fetch task: %s", e.what());
            is_running_.store(false, std::memory_order_release);
            fetch_task_running_.store(false, std::memory_order_release);
            return false;
        }
    }
    
    is_open_.store(true, std::memory_order_release);  // 🎯 原子操作设置状态
    LOG4CPLUS_DEBUG(logger_, "Opened (Buffer mode)");
    
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
        LOG4CPLUS_DEBUG(logger_, "========== 开始关闭流程 ==========");
        
        // ========== 步骤1：设置停止标志 ==========
        is_running_.store(false, std::memory_order_release);
        LOG4CPLUS_DEBUG(logger_, "步骤1：设置停止标志 (is_running_ = false)");
        
        // ========== 步骤2：唤醒所有等待的线程 ==========
        cv_subscribers_.notify_all();  // 唤醒订阅者
        cv_fetch_.notify_all();        // 唤醒 Fetch 任务
        LOG4CPLUS_DEBUG(logger_, "步骤2：唤醒所有等待的线程");
        
        // ========== 步骤3：等待 Fetch 任务完全退出 ==========
        {
            std::unique_lock<std::mutex> lock(mutex_);
            
            // 检查 Fetch 任务是否还在运行
            if (fetch_task_running_.load(std::memory_order_acquire)) {
                LOG4CPLUS_DEBUG(logger_, "步骤3：等待 Fetch 任务退出...");
                
                // 等待 Fetch 任务设置 fetch_task_running_ = false
                // 使用超时避免死锁（最多等待 5 秒）
                bool exited = cv_task_exit_.wait_for(lock, std::chrono::seconds(5), [this]() {
                    return !fetch_task_running_.load(std::memory_order_acquire);
                });
                
                if (exited) {
                    LOG4CPLUS_DEBUG(logger_, "步骤3：✅ Fetch 任务已安全退出");
                } else {
                    LOG4CPLUS_ERROR(logger_, "步骤3：❌ 等待 Fetch 任务退出超时（5秒）");
                    // 即使超时，也继续清理（避免死锁）
                }
            } else {
                LOG4CPLUS_DEBUG(logger_, "步骤3：Fetch 任务未启动或已退出");
            }
        }
        
        // ========== 步骤4：现在可以安全清理资源了 ==========
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto pool = source_pool_.lock();
            if (pool) {
                if (current_buffer_) {
                    pool->releaseFilled(current_buffer_);
                    current_buffer_ = nullptr;
                    LOG4CPLUS_DEBUG(logger_, "步骤4：已释放 current_buffer_");
                }
            }
        }
        
        LOG4CPLUS_DEBUG(logger_, "========== 关闭流程完成 ==========");
    }
}

bool BufferPacketSource::isOpen() const {
    return is_open_.load(std::memory_order_acquire);  // 🎯 原子操作读取状态
}

void BufferPacketSource::fetchTaskFunc() {
    LOG4CPLUS_INFO(logger_, "Fetch 任务启动（全局线程池，RAII 模式）");
    
    // RAII 保证任务退出时通知 close()
    struct TaskExitGuard {
        BufferPacketSource* self;
        TaskExitGuard(BufferPacketSource* s) : self(s) {}
        ~TaskExitGuard() {
            // 任务即将退出，设置标志并通知
            self->fetch_task_running_.store(false, std::memory_order_release);
            self->cv_task_exit_.notify_all();
            LOG4CPLUS_INFO(self->logger_, "Fetch 任务退出（已通知 close()）");
        }
    } guard(this);
    
    auto pool = source_pool_.lock();
    if (!pool) {
        LOG4CPLUS_ERROR(logger_, "Fetch 任务：Source BufferPool 不存在");
        return;
    }
    
    // ⏱️ 连续超时检测：记录最后一次成功获取 Buffer 的时间
    auto last_success_time = std::chrono::steady_clock::now();
    const std::chrono::seconds timeout_threshold(5);  // 5秒超时阈值
    
    while (is_running_.load(std::memory_order_acquire)) {
        // ========== 步骤1：等待所有订阅者完成（releasePacket 调用）==========
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_fetch_.wait(lock, [this]() {
                // ✅ 等待所有订阅者完成（remaining_subscribers_ == 0）
                return remaining_subscribers_.load(std::memory_order_acquire) == 0 ||
                       !is_running_.load(std::memory_order_acquire);
            });
            
            // 检查是否被停止
            if (!is_running_.load(std::memory_order_acquire)) {
                LOG4CPLUS_DEBUG(logger_, "Fetch 任务：收到停止信号");
                break;
            }
        }
        
        // ========== 步骤2：释放当前 Buffer（单缓冲）==========
        if (current_buffer_) {
            pool->releaseFilled(current_buffer_);
            current_buffer_ = nullptr;
        }
        
        // ========== 步骤3：获取新 Buffer ==========
        Buffer* new_buffer = pool->acquireFilled(true, 100);  // 100ms 超时
        
        if (!new_buffer) {
            // 超时或没有数据
            if (!is_running_.load(std::memory_order_acquire)) {
                LOG4CPLUS_DEBUG(logger_, "Fetch 任务：收到停止信号");
                break;  // 停止信号
            }
            
            // ⏱️ 检查连续超时时长
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_success_time);
            
            if (elapsed >= timeout_threshold) {
                // 连续 5 秒无法获取 Buffer，检查 Pool 状态
                if (!pool->isRunning()) {
                    LOG4CPLUS_WARN(logger_, "Fetch 任务：连续 5 秒无法获取 Buffer，且 Pool 已停止，退出任务");
                    is_running_.store(false, std::memory_order_release);
                    
                    // 清空 current_buffer_，避免订阅者继续使用旧数据
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        current_buffer_ = nullptr;
                    }
                    
                    // 唤醒所有等待的订阅者，让它们检测到 EOF
                    cv_subscribers_.notify_all();
                    break;  // Pool 已停止，退出任务
                } else {
                    // Pool 还在运行，但长时间无数据，打印警告
                    LOG4CPLUS_WARN_FMT(logger_, "Fetch 任务：连续 %ld 秒无法获取 Buffer，但 Pool 仍在运行",
                                      elapsed.count());
                    // 重置计时器，继续等待
                    last_success_time = now;
                }
            }
            
            // 继续等待
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        // 验证 Buffer
        AVPacket* src_packet = new_buffer->getAVPacket();
        if (!src_packet || src_packet->data == nullptr || src_packet->size == 0) {
            LOG4CPLUS_WARN(logger_, "Fetch 任务：Buffer 中的 AVPacket 无效");
            pool->releaseFilled(new_buffer);
            continue;
        }
        
        // ✅ 成功获取有效 Buffer，重置超时计时器
        last_success_time = std::chrono::steady_clock::now();
        
        // ========== 步骤4：设置新的 current_buffer_ 并唤醒订阅者 ==========
        {
            std::lock_guard<std::mutex> lock(mutex_);
            
            // ✅ 单缓冲：直接设置
            current_buffer_ = new_buffer;
            
            // 重置订阅者计数器
            remaining_subscribers_.store(total_subscribers_, std::memory_order_release);
        }
        
        // 唤醒所有等待的订阅者
        cv_subscribers_.notify_all();
    }
    
    LOG4CPLUS_INFO(logger_, "Fetch 任务循环结束");
}

int BufferPacketSource::readPacket(AVPacket* packet) {
    if (!is_open_.load(std::memory_order_acquire) || !packet) {
        return AVERROR(EINVAL);
    }
    
    auto pool = source_pool_.lock();
    if (!pool) {
        LOG4CPLUS_ERROR(logger_, "Source BufferPool已销毁");
        return AVERROR_EOF;
    }
    
    // ========== v2.18：共享模式（订阅者）==========
    if (is_shared_mode_) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // 1. 等待新 Buffer 可用
        cv_subscribers_.wait(lock, [this]() {
            return current_buffer_ != nullptr || 
                   !is_running_.load(std::memory_order_acquire);
        });
        
        // 检查是否被关闭
        if (!is_running_.load(std::memory_order_acquire)) {
            return AVERROR_EOF;
        }
        if (source_pool_.lock()->isRunning() == false) {
            return AVERROR_EOF;  // Pool 已停止，数据源结束
        }
        // 检查 Buffer 是否有效
        if (!current_buffer_) {
            return AVERROR(EAGAIN);
        }
        
        // 2. 复制 packet 数据
        AVPacket* src_packet = current_buffer_->getAVPacket();
        int ret = copyPacket(packet, src_packet);
        
        if (ret < 0) {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to copy packet: %s", err_buf);
        }
        // 3. 标记自己已完成
        size_t remaining = remaining_subscribers_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        
        // LOG4CPLUS_DEBUG_FMT(logger_, "订阅者完成，剩余: %zu", remaining);
        
        // 4. 如果是最后一个完成的订阅者，唤醒 Fetch 任务
        if (remaining == 0) {
            // LOG4CPLUS_DEBUG(logger_, "⭐ 所有订阅者已完成，通知 Fetch 任务");
            cv_fetch_.notify_one();
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
        LOG4CPLUS_WARN(logger_, "Buffer 中的 AVPacket 无效");
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
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to copy packet: %s", err_buf);
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
    LOG4CPLUS_WARN(logger_, "Seek not supported in Buffer mode (streaming data)");
    return false;
}

bool BufferPacketSource::isAtEnd() const {
    // Buffer 模式的 EOF 状态：检查 Pool 是否还有数据
    if (!is_open_.load(std::memory_order_acquire)) {
        return true;  // 未打开，视为 EOF
    }
    
    // 检查 BufferPool 是否可用
    auto pool = source_pool_.lock();
    if (!pool) {
        return true;  // Pool 已销毁，视为 EOF
    }
    
    if (pool->isRunning()) 
        return false;  // Pool 仍在运行，未到 EOF
    else 
        return true; // Pool 已停止，数据源结束
    
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
    LOG4CPLUS_DEBUG(logger_, "⭐ v2.13：已设置源 BufferPool");
}

AVPacket* BufferPacketSource::acquirePacket() {
    if (!is_shared_mode_) {
        LOG4CPLUS_ERROR(logger_, "acquirePacket() only supported in shared mode");
        return nullptr;
    }
    
    std::unique_lock<std::mutex> lock(mutex_);
    
    // 等待新 Buffer 可用
    cv_subscribers_.wait(lock, [this]() {
        return current_buffer_ != nullptr || 
               !is_running_.load(std::memory_order_acquire);
    });
    
    // 检查是否被关闭
    if (!is_running_.load(std::memory_order_acquire)) {
        return nullptr;  // EOF
    }
    
    auto pool = source_pool_.lock();
    if (!pool || !pool->isRunning()) {
        return nullptr;  // Pool 已停止
    }
    
    // 检查 Buffer 是否有效
    if (!current_buffer_) {
        return nullptr;
    }
    
    // ✅ 返回 AVPacket 指针，不递减 remaining_subscribers_
    return current_buffer_->getAVPacket();
}

void BufferPacketSource::releasePacket() {
    if (!is_shared_mode_) {
        return;
    }
    
    // ✅ 递减订阅者计数
    size_t remaining = remaining_subscribers_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    
    // 如果是最后一个完成的订阅者，唤醒 Fetch 任务
    if (remaining == 0) {
        cv_fetch_.notify_one();
    }
}

int BufferPacketSource::copyPacket(AVPacket* dst_packet, const AVPacket* src_packet) {
    if (!dst_packet || !src_packet) {
        return AVERROR(EINVAL);
    }
    
    // ✅ 方案：使用裸指针方案（零拷贝视图）
    // 
    // 设计原则：
    //   1. dst_packet 只是一个"视图"，指向 src_packet 的数据
    //   2. 不增加引用计数，不拥有数据
    //   3. avcodec_send_packet() 会在内部处理引用计数（如果需要）
    //   4. dst_packet 的生命周期必须短于 src_packet
    //   5. 调用者**不能**调用 av_packet_unref(dst_packet)
    //
    // 为什么不用 av_packet_ref()：
    //   - av_packet_ref() 会增加引用计数，需要对应的 unref
    //   - 但 dst_packet 来自消费者 Buffer，其生命周期由 BufferPool 管理
    //   - Buffer::freeBuffer() 会调用 av_packet_unref()，导致双重释放或引用计数混乱
    //   - 共享模式下，Fetch任务会等待所有订阅者完成后才释放 src_packet
    //   - 所以裸指针方案是安全的，且避免了引用计数管理的复杂性
    //
    // 为什么保留 side_data：
    //   - H.264 解码需要 PPS/SPS 等关键信息
    //   - 这些信息存储在 side_data 中
    //   - 必须复制 side_data 指针（不是深拷贝，只是指针）
    
    // 方法：让 dst_packet 的所有字段指向 src_packet
    // 等价于：dst_packet 就是 src_packet 的别名
    dst_packet->buf = nullptr;                  // 不使用引用计数
    dst_packet->data = src_packet->data;        // 直接指向原始数据
    dst_packet->size = src_packet->size;
    dst_packet->pts = src_packet->pts;
    dst_packet->dts = src_packet->dts;
    dst_packet->stream_index = src_packet->stream_index;
    dst_packet->flags = src_packet->flags;
    dst_packet->duration = src_packet->duration;
    dst_packet->pos = src_packet->pos;
    
    // ⭐ 关键修复：保留 side_data（不能设为 nullptr）
    // side_data 包含 H.264 的 PPS/SPS/SEI 等关键解码信息
    // 这里只是复制指针，不是深拷贝，所以是安全的
    dst_packet->side_data = src_packet->side_data;
    dst_packet->side_data_elems = src_packet->side_data_elems;
    
    return 0;
}

BufferPacketSource::SourceType BufferPacketSource::getDataSourceType() const {
    return SourceType::BUFFER_SOURCE;
}