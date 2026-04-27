#include "productionline/worker/datasource/rawdata/RawFrameSourceFromBuffer.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"
#include "bufferpool/buffer/Buffer.hpp"
#include "common/Logger.hpp"
#include "common/GlobalThreadPool.hpp"
#include <thread>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
}

// ============================================================
// RawFrameSourceFromBuffer 实现
// ============================================================

RawFrameSourceFromBuffer::RawFrameSourceFromBuffer(int width,
                                                     int height,
                                                     AVPixelFormat pix_fmt)
    : source_pool_weak_()
    , width_(width)
    , height_(height)
    , pix_fmt_(pix_fmt)
    , is_open_(false)
    , current_frame_index_(0)
    , is_direct_mode_(false)
    , direct_frame_(nullptr)
    , is_shared_mode_(false)  // 普通模式
    , total_subscribers_(0)
    , remaining_subscribers_(0)
    , current_buffer_(nullptr)
    , mutex_()
    , cv_subscribers_()
    , cv_fetch_()
    , cv_task_exit_()
    , is_running_(false)
    , fetch_task_running_(false)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.RawFrameSource.Buffer")))
{
    LOG4CPLUS_DEBUG_FMT(logger_, "构造函数（普通模式）: %dx%d, pix_fmt=%d", 
                        width_, height_, pix_fmt_);
}

RawFrameSourceFromBuffer::RawFrameSourceFromBuffer(int width,
                                                     int height,
                                                     AVPixelFormat pix_fmt,
                                                     size_t subscriber_count)
    : source_pool_weak_()
    , width_(width)
    , height_(height)
    , pix_fmt_(pix_fmt)
    , is_open_(false)
    , current_frame_index_(0)
    , is_direct_mode_(false)
    , direct_frame_(nullptr)
    , is_shared_mode_(true)  // 共享模式
    , total_subscribers_(subscriber_count)
    , remaining_subscribers_(0)
    , current_buffer_(nullptr)
    , mutex_()
    , cv_subscribers_()
    , cv_fetch_()
    , cv_task_exit_()
    , is_running_(false)
    , fetch_task_running_(false)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.RawFrameSource.Buffer")))
{
    if (subscriber_count < 2) {
        LOG4CPLUS_WARN_FMT(logger_, "共享模式 subscriber_count=%zu（应 >= 2）", subscriber_count);
    }
    LOG4CPLUS_INFO_FMT(logger_, "⭐ 共享模式构造: %dx%d, pix_fmt=%d, subscribers=%zu",
                        width_, height_, pix_fmt_, subscriber_count);
}

RawFrameSourceFromBuffer::RawFrameSourceFromBuffer(int width,
                                                     int height,
                                                     AVPixelFormat pix_fmt,
                                                     bool direct_mode)
    : source_pool_weak_()
    , width_(width)
    , height_(height)
    , pix_fmt_(pix_fmt)
    , is_open_(false)
    , current_frame_index_(0)
    , is_direct_mode_(direct_mode)
    , direct_frame_(nullptr)
    , is_shared_mode_(false)
    , total_subscribers_(0)
    , remaining_subscribers_(0)
    , current_buffer_(nullptr)
    , mutex_()
    , cv_subscribers_()
    , cv_fetch_()
    , cv_task_exit_()
    , is_running_(false)
    , fetch_task_running_(false)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.RawFrameSource.Buffer")))
{
    LOG4CPLUS_DEBUG_FMT(logger_, "构造函数（直接模式）: %dx%d, pix_fmt=%d",
                        width_, height_, pix_fmt_);
}

RawFrameSourceFromBuffer::~RawFrameSourceFromBuffer() {
    LOG4CPLUS_DEBUG(logger_, "析构函数开始");
    
    // close() 已经处理了所有清理工作
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

void RawFrameSourceFromBuffer::setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) {
    source_pool_weak_ = pool_weak;
    
    // 如果宽高未设置，从源 BufferPool 获取
    auto pool = pool_weak.lock();
    if (pool && (width_ == 0 || height_ == 0)) {
        // 尝试从 BufferPool 获取帧信息
        // 这里暂时不做处理，因为 BufferPool 可能还没有数据
        LOG4CPLUS_DEBUG(logger_, "已设置源 BufferPool");
    }
    
    LOG4CPLUS_DEBUG(logger_, "⭐ 已设置源 BufferPool");
}

bool RawFrameSourceFromBuffer::open() {
    if (is_open_.load(std::memory_order_acquire)) {
        return true;  // 已经打开
    }
    
    // 直接模式：不需要 BufferPool
    if (is_direct_mode_) {
        is_open_.store(true, std::memory_order_release);
        current_frame_index_.store(0, std::memory_order_release);
        LOG4CPLUS_DEBUG(logger_, "打开成功（直接模式）");
        return true;
    }
    
    // 验证源 BufferPool 是否有效
    auto pool = source_pool_weak_.lock();
    if (!pool) {
        LOG4CPLUS_ERROR(logger_, "无法打开：源 BufferPool 未设置或已销毁");
        return false;
    }
    
    // 共享模式初始化
    if (is_shared_mode_) {
        is_running_.store(true, std::memory_order_release);
        fetch_task_running_.store(false, std::memory_order_release);
        remaining_subscribers_.store(0, std::memory_order_release);
        current_buffer_ = nullptr;
        
        // 提交 Fetch 任务到全局线程池
        try {
            auto& thread_pool = GlobalThreadPool::getInstance().getThreadPool();
            
            fetch_task_running_.store(true, std::memory_order_release);
            
            thread_pool.detach_task([this]() {
                fetchTaskFunc();
            });
            
            LOG4CPLUS_INFO_FMT(logger_, "⭐ 共享模式已激活：Fetch 任务已提交，等待 %zu 个订阅者",
                              total_subscribers_);
        } catch (const std::exception& e) {
            LOG4CPLUS_ERROR_FMT(logger_, "提交 Fetch 任务失败: %s", e.what());
            is_running_.store(false, std::memory_order_release);
            fetch_task_running_.store(false, std::memory_order_release);
            return false;
        }
    }
    
    is_open_.store(true, std::memory_order_release);
    current_frame_index_.store(0, std::memory_order_release);
    
    LOG4CPLUS_DEBUG(logger_, "打开成功（Buffer 模式）");
    return true;
}

bool RawFrameSourceFromBuffer::open(const char* path) {
    (void)path;
    LOG4CPLUS_WARN(logger_, "Buffer 数据源不支持 open(path)，请使用 open()");
    return false;
}

void RawFrameSourceFromBuffer::close() {
    bool expected = true;
    if (!is_open_.compare_exchange_strong(expected, false,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        return;  // 已经关闭
    }
    
    // 直接模式关闭：唤醒所有等待线程
    if (is_direct_mode_) {
        {
            std::lock_guard<std::mutex> lock(direct_mutex_);
            direct_frame_consumed_ = true;
        }
        direct_cv_available_.notify_all();
        direct_cv_consumed_.notify_all();
        LOG4CPLUS_DEBUG(logger_, "直接模式：已唤醒所有等待线程");
    }

    // 共享模式关闭
    if (is_shared_mode_) {
        LOG4CPLUS_DEBUG(logger_, "========== 开始关闭流程 ==========");
        
        // 步骤1：设置停止标志
        is_running_.store(false, std::memory_order_release);
        LOG4CPLUS_DEBUG(logger_, "步骤1：设置停止标志");
        
        // 步骤2：唤醒所有等待的线程
        cv_subscribers_.notify_all();
        cv_fetch_.notify_all();
        LOG4CPLUS_DEBUG(logger_, "步骤2：唤醒所有等待的线程");
        
        // 步骤3：等待 Fetch 任务完全退出
        {
            std::unique_lock<std::mutex> lock(mutex_);
            
            if (fetch_task_running_.load(std::memory_order_acquire)) {
                LOG4CPLUS_DEBUG(logger_, "步骤3：等待 Fetch 任务退出...");
                
                bool exited = cv_task_exit_.wait_for(lock, std::chrono::seconds(5), [this]() {
                    return !fetch_task_running_.load(std::memory_order_acquire);
                });
                
                if (exited) {
                    LOG4CPLUS_DEBUG(logger_, "步骤3：✅ Fetch 任务已安全退出");
                } else {
                    LOG4CPLUS_ERROR(logger_, "步骤3：❌ 等待 Fetch 任务退出超时（5秒）");
                }
            } else {
                LOG4CPLUS_DEBUG(logger_, "步骤3：Fetch 任务未启动或已退出");
            }
        }
        
        // 步骤4：清理资源
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto pool = source_pool_weak_.lock();
            if (pool && current_buffer_) {
                pool->releaseFilled(current_buffer_);
                current_buffer_ = nullptr;
                LOG4CPLUS_DEBUG(logger_, "步骤4：已释放 current_buffer_");
            }
        }
        
        LOG4CPLUS_DEBUG(logger_, "========== 关闭流程完成 ==========");
    }
}

bool RawFrameSourceFromBuffer::isOpen() const {
    return is_open_.load(std::memory_order_acquire);
}

void RawFrameSourceFromBuffer::fetchTaskFunc() {
    LOG4CPLUS_INFO(logger_, "Fetch 任务启动（全局线程池）");
    
    // RAII 保证任务退出时通知 close()
    struct TaskExitGuard {
        RawFrameSourceFromBuffer* self;
        TaskExitGuard(RawFrameSourceFromBuffer* s) : self(s) {}
        ~TaskExitGuard() {
            self->fetch_task_running_.store(false, std::memory_order_release);
            self->cv_task_exit_.notify_all();
            LOG4CPLUS_INFO(self->logger_, "Fetch 任务退出（已通知 close()）");
        }
    } guard(this);
    
    auto pool = source_pool_weak_.lock();
    if (!pool) {
        LOG4CPLUS_ERROR(logger_, "Fetch 任务：Source BufferPool 不存在");
        return;
    }
    
    while (is_running_.load(std::memory_order_acquire)) {
        // 步骤1：等待所有订阅者完成
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_fetch_.wait(lock, [this, &pool]() {
                return remaining_subscribers_.load(std::memory_order_acquire) == 0 ||
                       !is_running_.load(std::memory_order_acquire) ||
                       (pool && !pool->isRunning());
            });
        }
        
        // 步骤2：释放当前 Buffer
        Buffer* buffer_to_release = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (current_buffer_) {
                buffer_to_release = current_buffer_;
                current_buffer_ = nullptr;
            }
        }
        if (buffer_to_release) {
            pool->releaseFilled(buffer_to_release);
        }
        
        // 检查是否应该停止
        if (!is_running_.load(std::memory_order_acquire) || !pool->isRunning()) {
            LOG4CPLUS_DEBUG(logger_, "Fetch 任务：收到停止信号");
            cv_subscribers_.notify_all();
            break;
        }
        
        // 步骤3：获取新 Buffer
        Buffer* new_buffer = pool->acquireFilled(true, 100);  // 100ms 超时
        
        if (!new_buffer) {
            if (!is_running_.load(std::memory_order_acquire) || !pool->isRunning()) {
                LOG4CPLUS_DEBUG(logger_, "Fetch 任务：停止信号");
                is_running_.store(false, std::memory_order_release);
                cv_subscribers_.notify_all();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        // 验证 Buffer 中的 AVFrame
        AVFrame* src_frame = new_buffer->getAVFrame();
        if (!src_frame || !src_frame->data[0]) {
            LOG4CPLUS_WARN(logger_, "Fetch 任务：Buffer 中的 AVFrame 无效");
            pool->releaseFilled(new_buffer);
            continue;
        }
        
        // 更新帧信息（如果之前未设置）
        if (width_ == 0) {
            width_ = src_frame->width;
        }
        if (height_ == 0) {
            height_ = src_frame->height;
        }
        if (pix_fmt_ == AV_PIX_FMT_NONE) {
            pix_fmt_ = static_cast<AVPixelFormat>(src_frame->format);
        }
        
        // 步骤4：设置新的 current_buffer_ 并唤醒订阅者
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_buffer_ = new_buffer;
            current_buffer_version_.fetch_add(1, std::memory_order_release);
            remaining_subscribers_.store(total_subscribers_, std::memory_order_release);
        }
        cv_subscribers_.notify_all();
    }
    
    LOG4CPLUS_INFO(logger_, "Fetch 任务循环结束");
}

int RawFrameSourceFromBuffer::readRawFrame(AVFrame* frame) {
    if (!is_open_.load(std::memory_order_acquire) || !frame) {
        return AVERROR(EINVAL);
    }
    
    // ========== 直接模式（条件变量同步） ==========
    if (is_direct_mode_) {
        std::unique_lock<std::mutex> lock(direct_mutex_);

        // 通知上一帧已消费完成（首次调用时 pending=false，无操作）
        if (direct_frame_pending_) {
            direct_frame_pending_ = false;
            direct_frame_consumed_ = true;
            direct_cv_consumed_.notify_one();
        }

        // 等待 setFrame 注入新帧
        direct_cv_available_.wait(lock, [this] {
            return direct_frame_ != nullptr || !is_open_.load(std::memory_order_acquire);
        });

        if (!is_open_.load(std::memory_order_acquire)) {
            direct_frame_consumed_ = true;
            direct_cv_consumed_.notify_one();
            return AVERROR_EOF;
        }

        // 直接使用消费者的 buffer，不做任何拷贝
        // 保存指针供 getDirectFrame() 返回，readAndSendFrame 直接用它编码
        last_direct_frame_ = direct_frame_;
        direct_frame_ = nullptr;
        direct_frame_pending_ = true;
        current_frame_index_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    
    auto pool = source_pool_weak_.lock();
    if (!pool) {
        LOG4CPLUS_ERROR(logger_, "Source BufferPool 已销毁");
        return AVERROR_EOF;
    }
    
    // ========== 共享模式 ==========
    if (is_shared_mode_) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // 等待新 Buffer 可用
        cv_subscribers_.wait(lock, [this]() {
            return current_buffer_ != nullptr ||
                   !is_running_.load(std::memory_order_acquire);
        });
        
        if (!is_running_.load(std::memory_order_acquire)) {
            return AVERROR_EOF;
        }
        
        if (!current_buffer_) {
            return AVERROR(EAGAIN);
        }
        
        // 复制 AVFrame 数据
        AVFrame* src_frame = current_buffer_->getAVFrame();
        int ret = copyFrame(frame, src_frame);
        
        if (ret < 0) {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG4CPLUS_ERROR_FMT(logger_, "复制帧失败: %s", err_buf);
            return ret;
        }
        
        // 标记自己已完成
        size_t remaining = remaining_subscribers_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        
        if (remaining == 0) {
            cv_fetch_.notify_one();
        }
        
        current_frame_index_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    
    // ========== 普通模式 ==========
    Buffer* filled_buffer = pool->acquireFilled(true, 100);  // 100ms 超时
    if (!filled_buffer) {
        return AVERROR(EAGAIN);
    }
    
    AVFrame* src_frame = filled_buffer->getAVFrame();
    if (!src_frame || !src_frame->data[0]) {
        LOG4CPLUS_WARN(logger_, "Buffer 中的 AVFrame 无效");
        pool->releaseFilled(filled_buffer);
        return AVERROR_EOF;
    }
    
    // 更新帧信息
    if (width_ == 0) width_ = src_frame->width;
    if (height_ == 0) height_ = src_frame->height;
    if (pix_fmt_ == AV_PIX_FMT_NONE) pix_fmt_ = static_cast<AVPixelFormat>(src_frame->format);
    
    // 复制帧数据
    int ret = copyFrame(frame, src_frame);
    
    // 释放 Buffer
    pool->releaseFilled(filled_buffer);
    
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG4CPLUS_ERROR_FMT(logger_, "复制帧失败: %s", err_buf);
        return ret;
    }
    
    current_frame_index_.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int RawFrameSourceFromBuffer::copyFrame(AVFrame* dst_frame, const AVFrame* src_frame) {
    if (!dst_frame || !src_frame) {
        return AVERROR(EINVAL);
    }
    
    // 使用 av_frame_ref 进行引用复制（零拷贝，如果可能）
    int ret = av_frame_ref(dst_frame, src_frame);
    if (ret < 0) {
        // 如果引用复制失败，尝试完整复制
        dst_frame->format = src_frame->format;
        dst_frame->width = src_frame->width;
        dst_frame->height = src_frame->height;
        
        ret = av_frame_get_buffer(dst_frame, 0);
        if (ret < 0) {
            return ret;
        }
        
        ret = av_frame_copy(dst_frame, src_frame);
        if (ret < 0) {
            return ret;
        }
        
        ret = av_frame_copy_props(dst_frame, src_frame);
        if (ret < 0) {
            return ret;
        }
    }
    
    return 0;
}

bool RawFrameSourceFromBuffer::commitRawFrame(void* worker_id) {
    if (!is_shared_mode_) {
        LOG4CPLUS_WARN(logger_, "commitRawFrame() 仅在共享模式下使用");
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = worker_states_.find(worker_id);
    if (it == worker_states_.end()) {
        LOG4CPLUS_WARN_FMT(logger_, "[Worker %p] commitRawFrame: Worker 未找到", worker_id);
        return false;
    }
    
    WorkerState& state = it->second;
    uint64_t current_version = current_buffer_version_.load(std::memory_order_acquire);
    
    if (!state.has_acquired) {
        LOG4CPLUS_WARN_FMT(logger_, "[Worker %p] commitRawFrame: 未获取", worker_id);
        return false;
    }
    
    if (state.acquired_version != current_version) {
        LOG4CPLUS_WARN_FMT(logger_, 
            "[Worker %p] commitRawFrame: 版本不匹配 (acquired=%llu, current=%llu)",
            worker_id,
            (unsigned long long)state.acquired_version,
            (unsigned long long)current_version);
        return false;
    }
    
    if (state.has_committed) {
        LOG4CPLUS_WARN_FMT(logger_,
            "[Worker %p] commitRawFrame: 已提交过版本 %llu",
            worker_id, (unsigned long long)current_version);
        return false;
    }
    
    state.has_acquired = false;
    state.has_committed = true;
    
    // 递减订阅者计数
    size_t remaining = remaining_subscribers_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    
    if (remaining == 0) {
        cv_fetch_.notify_one();
    }
    
    return true;
}

bool RawFrameSourceFromBuffer::seek(int frame_index) {
    (void)frame_index;
    LOG4CPLUS_WARN(logger_, "Buffer 数据源不支持 seek（流式数据）");
    return false;
}

bool RawFrameSourceFromBuffer::seekToBegin() {
    LOG4CPLUS_WARN(logger_, "Buffer 数据源不支持 seekToBegin（流式数据）");
    return false;
}

bool RawFrameSourceFromBuffer::seekToEnd() {
    LOG4CPLUS_WARN(logger_, "Buffer 数据源不支持 seekToEnd（流式数据）");
    return false;
}

bool RawFrameSourceFromBuffer::skip(int frame_count) {
    (void)frame_count;
    LOG4CPLUS_WARN(logger_, "Buffer 数据源不支持 skip（流式数据）");
    return false;
}

int RawFrameSourceFromBuffer::getTotalFrames() const {
    return -1;  // 流式数据，无总帧数
}

int RawFrameSourceFromBuffer::getCurrentFrameIndex() const {
    return current_frame_index_.load(std::memory_order_acquire);
}

size_t RawFrameSourceFromBuffer::getFrameSize() const {
    if (width_ > 0 && height_ > 0 && pix_fmt_ != AV_PIX_FMT_NONE) {
        int size = av_image_get_buffer_size(pix_fmt_, width_, height_, 1);
        return size > 0 ? static_cast<size_t>(size) : 0;
    }
    return 0;
}

long RawFrameSourceFromBuffer::getFileSize() const {
    return -1;  // Buffer 模式无文件大小
}

std::string RawFrameSourceFromBuffer::getPath() const {
    return "BufferPool";
}

bool RawFrameSourceFromBuffer::hasMoreFrames() const {
    return !isAtEnd();
}

bool RawFrameSourceFromBuffer::isAtEnd() const {
    if (!is_open_.load(std::memory_order_acquire)) {
        return true;
    }
    
    if (is_direct_mode_) {
        return false;  // 直接模式由调用者控制生命周期
    }
    
    auto pool = source_pool_weak_.lock();
    if (!pool) {
        return true;  // Pool 已销毁
    }
    
    return !pool->isRunning();
}
