#include "productionline/worker/EncodedPacketSourceFromBuffer.hpp"
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
// PacketGuard 实现（v3.0 已移除）
// ============================================================
// ⭐ v2.22 修改：移除 PacketGuard RAII 包装器
// 新的三状态 API（acquire/commit/cancel）提供了更精确的控制

// ============================================================
// EncodedPacketSourceFromBuffer 实现
// ============================================================

EncodedPacketSourceFromBuffer::EncodedPacketSourceFromBuffer(const AVCodecParameters* codec_params)
    : codec_params_(codec_params)
    , source_pool_()
    , is_open_(false)  // 原子变量初始化
    , current_frame_index_(0)  // 当前帧索引初始化
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
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.EncodedPacketSourceFromBuffer")))
{
    if (!codec_params_) {
        LOG4CPLUS_WARN(logger_, "codec_params is nullptr");
    }
    LOG4CPLUS_DEBUG(logger_, "构造函数（v2.13：Pool 模式）");
}

EncodedPacketSourceFromBuffer::EncodedPacketSourceFromBuffer(const AVCodecParameters* codec_params, size_t subscriber_count)
    : codec_params_(codec_params)
    , source_pool_()
    , is_open_(false)
    , current_frame_index_(0)  // 当前帧索引初始化
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
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.EncodedPacketSourceFromBuffer")))
{
    if (!codec_params_) {
        LOG4CPLUS_WARN(logger_, "codec_params is nullptr");
    }
    if (subscriber_count < 2) {
        LOG4CPLUS_WARN_FMT(logger_, "Shared mode with subscriber_count=%zu (should be >= 2)", subscriber_count);
    }
    LOG4CPLUS_INFO_FMT(logger_, "⭐ v2.18 共享模式（RAII）：创建发布者，订阅者数量=%zu", subscriber_count);
}

EncodedPacketSourceFromBuffer::~EncodedPacketSourceFromBuffer() {
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

bool EncodedPacketSourceFromBuffer::open() {
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
    
    is_open_.store(true, std::memory_order_release);  // 原子操作设置状态
    LOG4CPLUS_DEBUG(logger_, "Opened (Buffer mode)");
    
    return true;
}

void EncodedPacketSourceFromBuffer::close() {
    // 原子检查并设置：如果 is_open_ 是 true，则设置为 false
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

bool EncodedPacketSourceFromBuffer::isOpen() const {
    return is_open_.load(std::memory_order_acquire);  // 原子操作读取状态
}

void EncodedPacketSourceFromBuffer::fetchTaskFunc() {
    LOG4CPLUS_INFO(logger_, "Fetch 任务启动（全局线程池，RAII 模式）");
    
    // RAII 保证任务退出时通知 close()
    struct TaskExitGuard {
        EncodedPacketSourceFromBuffer* self;
        TaskExitGuard(EncodedPacketSourceFromBuffer* s) : self(s) {}
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
                // ⭐ v2.32 修复：始终等待 remaining_subscribers == 0
                // 无论是正常 EOF 还是外部停止，都必须等待 Worker 处理完当前 buffer
                // 这样才能安全地释放 current_buffer_
                return remaining_subscribers_.load(std::memory_order_acquire) == 0;
            });
        }
        
        // ========== 步骤2：释放当前 Buffer（单缓冲）==========
        // 🔒 修复：在锁内读取和清空 current_buffer_，在锁外释放 Buffer
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
        
        // ⭐ v2.32 新增：步骤2 完成后检查是否需要退出
        // 此时 current_buffer_ 已经安全释放，可以退出
        if (!is_running_.load(std::memory_order_acquire)) {
            LOG4CPLUS_DEBUG(logger_, "Fetch 任务：收到外部停止信号，安全退出");
            cv_subscribers_.notify_all();
            break;
        }
        
        // ========== 步骤3：获取新 Buffer ==========
        // ⭐ v2.32 修复：Pool shutdown 后继续获取剩余 buffer
        // - Pool 运行中：使用阻塞模式 + 100ms 超时
        // - Pool 已 shutdown：使用非阻塞模式，持续获取直到队列为空
        bool pool_running = pool->isRunning();
        Buffer* new_buffer = pool->acquireFilled(pool_running, pool_running ? 100 : 0);
        
        if (!new_buffer) {
            // 超时或没有数据
            if (!is_running_.load(std::memory_order_acquire)) {
                LOG4CPLUS_DEBUG(logger_, "Fetch 任务：收到外部停止信号");
                cv_subscribers_.notify_all();
                break;
            }
            
            if (!pool_running) {
                // ⭐ Pool 已 shutdown 且非阻塞获取返回 nullptr，说明队列真的为空了
                LOG4CPLUS_DEBUG(logger_, "Fetch 任务：数据源 EOF（BufferPool 已排空）");
                is_running_.store(false, std::memory_order_release);
                cv_subscribers_.notify_all();
                break;
            }
            
            // Pool 还在运行，超时，继续等待
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
            
            // ⭐ v2.22 新增：递增版本号
            current_buffer_version_.fetch_add(1, std::memory_order_release);
            
            // ✅ 不需要清空 worker_states_！
            // Worker 状态会根据版本号自动判断
            
            // 重置订阅者计数器
            remaining_subscribers_.store(total_subscribers_, std::memory_order_release);
        }
        // 唤醒所有等待的订阅者
        cv_subscribers_.notify_all();
    }
    
    LOG4CPLUS_INFO(logger_, "Fetch 任务循环结束");
}

// v2.32 删除：readEncodedPacket 已被 acquireEncodedPacket 统一接口替代

const AVCodecParameters* EncodedPacketSourceFromBuffer::getCodecParameters() const {
    return codec_params_;
}

int EncodedPacketSourceFromBuffer::getVideoStreamIndex() const {
    // Buffer 模式下，没有流索引的概念，返回 0（表示第一个/唯一的流）
    return 0;
}

int EncodedPacketSourceFromBuffer::getTotalFrames() const {
    // Buffer 模式下，无法知道总帧数（流式数据）
    return -1;
}

long EncodedPacketSourceFromBuffer::getFileSize() const {
    // Buffer 模式下，没有文件大小概念
    return -1;
}

std::string EncodedPacketSourceFromBuffer::getPath() const {
    // Buffer 模式下，没有文件路径概念
    return "BufferPool";
}

bool EncodedPacketSourceFromBuffer::seek(int frame_index) {
    // Buffer 模式：流式数据，不支持 seek
    LOG4CPLUS_WARN(logger_, "Seek not supported in Buffer mode (streaming data)");
    return false;
}

bool EncodedPacketSourceFromBuffer::isAtEnd() const {
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

int EncodedPacketSourceFromBuffer::getSourceWidth() const {
    const AVCodecParameters* params = getCodecParameters();
    return params ? params->width : 0;
}

int EncodedPacketSourceFromBuffer::getSourceHeight() const {
    const AVCodecParameters* params = getCodecParameters();
    return params ? params->height : 0;
}

AVPixelFormat EncodedPacketSourceFromBuffer::getSourcePixelFormat() const {
    const AVCodecParameters* params = getCodecParameters();
    return params ? static_cast<AVPixelFormat>(params->format) : AV_PIX_FMT_NONE;
}

void EncodedPacketSourceFromBuffer::setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) {
    source_pool_ = pool_weak;
    LOG4CPLUS_DEBUG(logger_, "⭐ v2.13：已设置源 BufferPool");
}

PacketAcquireResult EncodedPacketSourceFromBuffer::acquireEncodedPacket(AVPacket* out_packet, void* worker_id) {
    (void)out_packet;  // Buffer 共享模式忽略 out_packet，返回借用指针
    
    using Result = PacketAcquireResult;
    
    if (!is_shared_mode_) {
        LOG4CPLUS_ERROR(logger_, "acquireEncodedPacket() only supported in shared mode");
        return Result::invalidMode();
    }
    
    std::unique_lock<std::mutex> lock(mutex_);
    // ⭐ v2.22 修改：阻塞等待新 buffer 或 EOF
    cv_subscribers_.wait(lock, [this]() {
        return current_buffer_ != nullptr || 
               !is_running_.load(std::memory_order_acquire);
    });
    
    // 检查 EOF
    if (!is_running_.load(std::memory_order_acquire) && !current_buffer_) {
        LOG4CPLUS_DEBUG_FMT(logger_, "[Worker %p] acquireEncodedPacket: EOF", worker_id);
        return Result::eof();  // EOF：已停止且无可用数据
    }
    
    if (!current_buffer_) {
        LOG4CPLUS_ERROR_FMT(logger_, "[Worker %p] acquireEncodedPacket: Internal error - "
            "cv woke up with is_running=true but current_buffer is null", worker_id);
        return Result::internalError();
    }
    
    uint64_t current_version = current_buffer_version_.load(std::memory_order_acquire);
    
    // ⭐ v2.22 新增：获取或创建 Worker 状态
    WorkerState& state = worker_states_[worker_id];
    
    // ⭐ v2.22 新增：检查是否已处理过当前版本
    // 注意：检查 acquired_version，不管 has_acquired 状态
    // 因为 commit 后 has_acquired 会被重置，但 acquired_version 保持
    if (state.acquired_version == current_version) {
        // ❌ 已处理过当前版本（无论是否已 commit），不能重复获取
        LOG4CPLUS_DEBUG_FMT(logger_, 
            "[Worker %p] acquireEncodedPacket: Already processed version %llu (has_acquired=%d, has_committed=%d)", 
            worker_id, (unsigned long long)current_version, 
            state.has_acquired, state.has_committed);
        return Result::packetAlreadyProcessed();  // ⭐ v2.31：当前 packet 已处理过
    }
    
    // ✅ 新版本或首次获取，可以获取
    state.acquired_version = current_version;
    state.has_acquired = true;
    state.has_committed = false;  // 重置 commit 标志
    return Result::success(current_buffer_->getAVPacket());
}

bool EncodedPacketSourceFromBuffer::commitEncodedPacket(void* worker_id) {
    if (!is_shared_mode_) {
        LOG4CPLUS_WARN(logger_, "commitEncodedPacket() only supported in shared mode");
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 1. 检查 Worker 是否存在
    auto it = worker_states_.find(worker_id);
    if (it == worker_states_.end()) {
        LOG4CPLUS_WARN_FMT(logger_, "[Worker %p] commitEncodedPacket: Worker not found", worker_id);
        return false;
    }
    
    WorkerState& state = it->second;
    uint64_t current_version = current_buffer_version_.load(std::memory_order_acquire);
    
    // 2. 检查状态
    if (!state.has_acquired) {
        LOG4CPLUS_WARN_FMT(logger_, "[Worker %p] commitEncodedPacket: Not acquired", worker_id);
        return false;
    }
    
    if (state.acquired_version != current_version) {
        LOG4CPLUS_WARN_FMT(logger_, 
            "[Worker %p] commitEncodedPacket: Version mismatch (acquired=%llu, current=%llu)", 
            worker_id, 
            (unsigned long long)state.acquired_version,
            (unsigned long long)current_version);
        return false;
    }
    
    // ⭐ v2.22 新增：检查是否已 commit（防止重复 commit）
    if (state.has_committed) {
        LOG4CPLUS_WARN_FMT(logger_, 
            "[Worker %p] commitEncodedPacket: Already committed version %llu", 
            worker_id, (unsigned long long)current_version);
        return false;
    }
    
    // ⭐ v2.22 修复：重置 has_acquired 并标记已 commit
    state.has_acquired = false;
    state.has_committed = true;
    
    // 3. 递减订阅者计数
    size_t remaining = remaining_subscribers_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    LOG4CPLUS_DEBUG_FMT(logger_, "commitEncodedPacket: remaining=%zu", remaining);
    // 4. 如果所有订阅者都完成，唤醒 Fetch 任务
    if (remaining == 0) {
        // ⭐ v2.22 修复：不在这里清空 current_buffer_！
        // current_buffer_ 的清空和释放由 fetchTaskFunc() 负责
        LOG4CPLUS_DEBUG(logger_, "commitEncodedPacket: notify_one");
        cv_fetch_.notify_one();
    }
    
    return true;
}

void EncodedPacketSourceFromBuffer::cancelEncodedPacket(void* worker_id) {
    if (!is_shared_mode_) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = worker_states_.find(worker_id);
    if (it == worker_states_.end()) {
        return;
    }
    
    WorkerState& state = it->second;
    uint64_t current_version = current_buffer_version_.load(std::memory_order_acquire);
    
    LOG4CPLUS_DEBUG_FMT(logger_, 
        "[Worker %p] cancelEncodedPacket: version=%llu", 
        worker_id, (unsigned long long)current_version);
    
    // 重置获取状态（允许重新 acquire）
    // 注意：不递减 remaining_subscribers_！
    state.has_acquired = false;
}

int EncodedPacketSourceFromBuffer::copyPacket(AVPacket* dst_packet, const AVPacket* src_packet) {
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

IDataSourceNavigator::SourceType EncodedPacketSourceFromBuffer::getDataSourceType() const {
    return SourceType::BUFFER_SOURCE;
}

bool EncodedPacketSourceFromBuffer::open(const char* path) {
    (void)path;
    LOG4CPLUS_WARN(logger_, "Buffer source does not support open(path), use open()");
    return false;
}

bool EncodedPacketSourceFromBuffer::seekToBegin() {
    LOG4CPLUS_WARN(logger_, "Buffer source does not support seekToBegin (streaming data)");
    return false;
}

bool EncodedPacketSourceFromBuffer::seekToEnd() {
    LOG4CPLUS_WARN(logger_, "Buffer source does not support seekToEnd (streaming data)");
    return false;
}

bool EncodedPacketSourceFromBuffer::skip(int frame_count) {
    (void)frame_count;
    LOG4CPLUS_WARN(logger_, "Buffer source does not support skip (streaming data)");
    return false;
}

int EncodedPacketSourceFromBuffer::getCurrentFrameIndex() const {
    return current_frame_index_.load(std::memory_order_acquire);
}

size_t EncodedPacketSourceFromBuffer::getFrameSize() const {
    // Buffer 模式无法估算帧大小
    return 0;
}

bool EncodedPacketSourceFromBuffer::hasMoreFrames() const {
    return !isAtEnd();
}
