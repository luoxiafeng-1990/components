#include "productionline/VideoProductionLine.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "common/Logger.hpp"
#include "common/GlobalThreadPool.hpp"
#include <stdio.h>
#include <chrono>
#include <string>

// ============================================================
// 构造函数和析构函数
// ============================================================

VideoProductionLine::VideoProductionLine(bool loop, int thread_count, bool enable_monitor)
    : working_buffer_pool_id_(0)
    , working_buffer_pool_weak_()
    , worker_facade_sptr_(nullptr)
    , threads_()
    , running_(false)
    , active_threads_(0)
    , threads_mutex_()
    , produced_frames_(0)
    , skipped_frames_(0)
    , next_frame_index_(0)
    , loop_(loop)
    , thread_count_(thread_count)
    , total_frames_(0)
    , enable_monitor_(enable_monitor)
    , error_callback_(nullptr)
    , error_mutex_()
    , last_error_()
    , start_time_()
    , monitor_(nullptr)
    , log_prefix_("")
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.VideoLine")))
{
    // 打印生命周期开始
    LOG4CPLUS_INFO(logger_, "创建: loop=" << (loop_ ? "true" : "false") 
                   << ", threads=" << thread_count_);
    
    if (thread_count < 1) {
        LOG4CPLUS_WARN(logger_, "Invalid thread_count, using 1");
        thread_count_ = 1;
    }
}

VideoProductionLine::~VideoProductionLine() {
    // 获取logger
    // logger_ 已在构造函数初始化
    
    // 打印生命周期结束
    LOG4CPLUS_INFO(logger_, "");
    LOG4CPLUS_INFO(logger_, "" << std::string(69, '='));
    LOG4CPLUS_INFO(logger_, "析构: 已生产 " << produced_frames_.load() << " 帧, 跳过 " << skipped_frames_.load() << " 帧");
    LOG4CPLUS_INFO(logger_, "" << std::string(69, '='));
    
    // 🔧 修复：无论 running_ 的状态如何，都必须确保所有线程被正确 join
    // 避免 std::thread 在 joinable 状态下被析构导致 std::terminate()
    if (running_.load()) {
        stop();
    } else {
        // 即使 running_ 是 false，也要确保所有线程被 join
        std::lock_guard<std::mutex> lock(threads_mutex_);
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        threads_.clear();
    }
}

// ============================================================
// 核心接口实现
// ============================================================

bool VideoProductionLine::start(const WorkerConfig& worker_config) {
    // logger_ 已在构造函数初始化
    
    // 检查是否已经在运行
    if (running_.load()) {
        LOG4CPLUS_WARN(logger_, "Already running");
        return false;
    }
    
    // ⭐ 初始化全局线程池（从配置读取）
    initializeGlobalThreadPool(worker_config.thread_pool_size);
    
    LOG4CPLUS_INFO(logger_, "BufferFillingWorkerFacade: " << worker_config.data_source.path);
    
    // 创建共享的 BufferFillingWorkerFacade 对象（v2.2：只传入完整配置）
    worker_facade_sptr_ = std::make_shared<BufferFillingWorkerFacade>(worker_config);
    LOG4CPLUS_INFO(logger_, "启动Worker...");
    
    // v2.2：简化的 open 接口（所有参数从 config 获取）
    if (!worker_facade_sptr_->open()) {
        setError(std::string("Failed to open data source: ") + worker_config.data_source.path);
        worker_facade_sptr_.reset();
        return false;
    }
    
    // v2.0: Worker必须在open()时自动创建BufferPool（通过调用Allocator）
    // v2.3: 从 Worker 获取它的主要 BufferPool 类型
    BufferPoolType primary_type = worker_facade_sptr_->getPrimaryBufferPoolType();
    
    // 使用主要类型获取 BufferPool ID
    uint64_t worker_pool_id = worker_facade_sptr_->getOutputBufferPoolId(primary_type);
    if (worker_pool_id == 0) {
        setError("Worker failed to create primary BufferPool");
        worker_facade_sptr_.reset();
        return false;
    }
    
    // v2.0: 记录 pool_id 并从 Registry 获取 weak_ptr（符合架构设计）
    working_buffer_pool_id_ = worker_pool_id;
    working_buffer_pool_weak_ = BufferPoolRegistry::getInstance().getPool(worker_pool_id);
    
    // 验证 Pool 是否存在
    auto pool_sptr = working_buffer_pool_weak_.lock();
    if (!pool_sptr) {
        setError("Failed to get BufferPool from Registry");
        worker_facade_sptr_.reset();
        return false;
    }
    
    LOG4CPLUS_INFO_FMT(logger_, "Using %s pool (ID: %lu)", 
                 bufferPoolTypeToString(primary_type), working_buffer_pool_id_);
    
    total_frames_ = worker_facade_sptr_->getTotalFrames();
    size_t frame_size = worker_facade_sptr_->getFrameSize();
    
    LOG4CPLUS_INFO(logger_, "Worker已就绪: " << worker_facade_sptr_->getWorkerType());
    LOG4CPLUS_INFO(logger_, "  - 分辨率: " << worker_facade_sptr_->getWidth() << "x" << worker_facade_sptr_->getHeight());
    LOG4CPLUS_INFO(logger_, "  - 总帧数: " << total_frames_);
    LOG4CPLUS_INFO(logger_, "  - 帧大小: " << (frame_size / (1024.0 * 1024.0)) << " MB");
    
    // 重置状态
    running_.store(true);
    produced_frames_.store(0);
    skipped_frames_.store(0);
    next_frame_index_.store(0);
    start_time_ = std::chrono::steady_clock::now();
    
    // 初始化性能监控（仅在启用时）
    if (enable_monitor_) {
        monitor_ = std::make_unique<PerformanceMonitor>();
        monitor_->setReportInterval(1000);
        LOG4CPLUS_INFO(logger_, "  - 性能监控: 已启用");
    }
    
    // 启动生产者线程
    threads_.reserve(thread_count_);
    active_threads_.store(thread_count_);
    
    LOG4CPLUS_INFO(logger_, "启动生产线: " << thread_count_ << " threads");
    
    for (int i = 0; i < thread_count_; i++) {
        try {
            threads_.emplace_back(&VideoProductionLine::producerThreadFunc, this, i);
            LOG4CPLUS_INFO(logger_, "  - Thread #" << i << " started");
        } catch (const std::exception& e) {
            LOG4CPLUS_ERROR(logger_, "Failed to start thread #" << i << ": " << e.what());
            // 停止已启动的线程
            running_.store(false);
            active_threads_.store(0);  // 重置活跃线程计数
            for (auto& thread : threads_) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            threads_.clear();
            worker_facade_sptr_.reset();
            setError(std::string("Failed to start producer thread: ") + e.what());
            return false;
        }
    }
    
    return true;
}

void VideoProductionLine::stop() {
    // 加锁保护线程相关操作
    std::lock_guard<std::mutex> lock(threads_mutex_);
    
    if (!running_.load()) {
        return;
    }
    
    LOG4CPLUS_INFO(logger_, "Stopping VideoProductionLine...");
    
    // 设置停止标志
    running_.store(false);
    
    // 等待所有线程退出
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
    
    // 重置活跃线程计数
    active_threads_.store(0);
    // 停止性能监控
    if (monitor_) {
        monitor_->stop();
        monitor_.reset();
    }
    
    LOG4CPLUS_INFO(logger_, "VideoProductionLine stopped");
    LOG4CPLUS_INFO_FMT(logger_, "Total produced: %d frames", produced_frames_.load());
    LOG4CPLUS_INFO_FMT(logger_, "Total skipped: %d frames", skipped_frames_.load());
    LOG4CPLUS_INFO_FMT(logger_, "Average FPS: %.2f", getAverageFPS());
}

// ============================================================
// 查询接口实现
// ============================================================

double VideoProductionLine::getAverageFPS() const {
    if (!running_.load() && threads_.empty()) {
        // 已停止，计算总体平均
        auto duration = std::chrono::steady_clock::now() - start_time_;
        double seconds = std::chrono::duration<double>(duration).count();
        if (seconds > 0) {
            return produced_frames_.load() / seconds;
        }
    } else if (running_.load()) {
        // 正在运行，计算当前平均
        auto duration = std::chrono::steady_clock::now() - start_time_;
        double seconds = std::chrono::duration<double>(duration).count();
        if (seconds > 0) {
            return produced_frames_.load() / seconds;
        }
    }
    return 0.0;
}

std::string VideoProductionLine::getLastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void VideoProductionLine::printStats() const {
    LOG4CPLUS_DEBUG_FMT(logger_, "VideoProductionLine Statistics: Running: %s, Produced: %d, Skipped: %d, Total: %d, FPS: %.2f, Threads: %zu",
                  running_.load() ? "Yes" : "No", produced_frames_.load(), skipped_frames_.load(), 
                  total_frames_, getAverageFPS(), threads_.size());
}

// ============================================================
// 内部方法实现
// ============================================================

std::optional<int> VideoProductionLine::getNextFrameIndex() {
    // 1. 原子地获取下一个原始索引
    int raw_index = next_frame_index_.fetch_add(1);
    
    // 2. 使用已缓存的总帧数（在 start() 时从 Worker 获取）
    if (total_frames_ <= 0) {
        return std::nullopt;
    }
    
    // 3. 处理循环模式和文件边界
    if (raw_index >= total_frames_) {
        if (loop_) {
            // 循环模式：归一化到有效范围
            int normalized = raw_index % total_frames_;
            
            // 4. 溢出保护：定期重置计数器（避免整数溢出）
            if (raw_index > 0 && raw_index % (total_frames_ * 2) == 0) {
                next_frame_index_.store(normalized + 1);
            }
            
            return normalized;
        } else {
            // 非循环模式：无更多帧
            return std::nullopt;
        }
    }
    
    // 有效索引，直接返回
    return raw_index;
}

void VideoProductionLine::producerThreadFunc(int thread_id) {
    // 从缓存的 weak_ptr 获取临时 shared_ptr（符合架构设计）
    auto pool_sptr = working_buffer_pool_weak_.lock();
    if (!pool_sptr) {
        LOG4CPLUS_ERROR_FMT(logger_, "Thread #%d: BufferPool not found or destroyed", thread_id);
        return;
    }
    
    LOG4CPLUS_INFO_FMT(logger_, "[VideoProductionLine] Thread #%d: Starting unified producer loop", thread_id);
    LOG4CPLUS_INFO_FMT(logger_, "[VideoProductionLine] Working BufferPool: '%s'", pool_sptr->getName().c_str());
    
    int thread_produced = 0;
    int thread_skipped = 0;
    int consecutive_failures = 0;
    int buffer_wait_count = 0;  // 缓冲区等待计数器，避免频繁日志
    if (monitor_) {
        monitor_->start();  // 启动后Timer会自动触发周期性报告
    }
    
    while (running_.load()) {
        // 🎯 统一的流程：从工作 BufferPool 获取 buffer（使用临时 shared_ptr）
        Buffer* buffer = nullptr;
        while (running_.load() && buffer == nullptr) {
            buffer = pool_sptr->acquireFree(true, 100);  // 100ms 超时
            if (buffer == nullptr && running_.load()) {
                // 超时但仍在运行，继续等待
                buffer_wait_count++;
                // 每100次等待才打印一次日志，避免过于频繁
                if (buffer_wait_count % 100 == 1) {
                    LOG4CPLUS_DEBUG_FMT(logger_, "[VideoProductionLine][Thread #%d] Waiting for free buffer from pool '%s' (thread_produced=%d, wait_count=%d)...",
                                  thread_id, pool_sptr->getName().c_str(), thread_produced, buffer_wait_count);
                }
            }
        }

        // 检查是否因为停止信号退出循环
        if (!running_.load()) {
            break;
        }
        
        if (monitor_) {
            monitor_->beginTiming("fill_buffer");
        }
        bool fill_success = worker_facade_sptr_->fillBuffer(thread_produced, buffer);
        
        // 5. 🎯 统一的处理：提交或归还
        if (fill_success) {
            // ✅ 填充成功：提交到 filled 队列（供消费者使用）
            pool_sptr->submitFilled(buffer);
            produced_frames_.fetch_add(1);
            thread_produced++;
            consecutive_failures = 0;  // 重置失败计数
            if (monitor_) {
                monitor_->endTiming("fill_buffer");
            }
        } else {
            // ⚠️ 填充失败：检查 Worker 是否到达 EOF
            if (worker_facade_sptr_->isAtEnd()) {
                // Worker 到达 EOF
                if (loop_) {
                    // 🔧 修复：循环模式下，当 Worker 到达 EOF 时，重置 Worker
                    // 这确保循环播放时 Worker 能够从文件开头重新开始读取
                    LOG4CPLUS_DEBUG_FMT(logger_, "[Thread #%d] Worker reached EOF in loop mode, resetting to begin (frame_index=%d)", 
                                  thread_id, thread_produced);
                    if (worker_facade_sptr_->seekToBegin()) {
                        // 重置成功：归还 buffer，重置失败计数，继续下一次循环
                        // 注意：不增加 skipped_frames，因为这是正常的循环重置操作
                        pool_sptr->releaseFree(buffer);
                        consecutive_failures = 0;
                    } else {
                        LOG4CPLUS_ERROR_FMT(logger_, "[Thread #%d] Failed to reset Worker to begin", thread_id);
                        // 重置失败，按正常失败处理
                        pool_sptr->releaseFree(buffer);
                        skipped_frames_.fetch_add(1);
                        thread_skipped++;
                        consecutive_failures++;
                    }
                } else {
                    // 🔧 修复：非循环模式下，Worker 到达 EOF 时应该停止循环
                    LOG4CPLUS_DEBUG_FMT(logger_, "[Thread #%d] Worker reached EOF in non-loop mode, stopping producer thread", 
                                  thread_id);
                    pool_sptr->releaseFree(buffer);
                    // 停止循环，退出生产者线程
                    break;
                }
            } else {
                // 非 EOF 情况：正常处理失败（可能是损坏帧等其他错误）
                pool_sptr->releaseFree(buffer);
                skipped_frames_.fetch_add(1);
                thread_skipped++;
                // 🎯 累加连续失败次数（PerformanceMonitor的Timer会每2秒自动打印统计）
                consecutive_failures++;
                if (consecutive_failures > kMaxConsecutiveFailures) {
                    LOG4CPLUS_ERROR_FMT(logger_, "[Thread #%d] Failed to fill buffer %d times in a row, stopping producer thread", 
                                  thread_id, kMaxConsecutiveFailures);
                    break;
                }
            }
            if (monitor_) {
                monitor_->endTiming("fill_buffer");
            }
        }
    }
    
    // 🔧 修复：线程退出前停止 PerformanceMonitor
    // 注意：monitor_ 是共享的，所有线程都使用同一个 monitor
    // stop() 方法是线程安全的，可以多次调用（内部会检查是否已停止）
    // 虽然多个线程退出时都会调用 stop()，但这是安全的，因为 stop() 内部有锁保护
    // 实际上，只要有一个线程调用了 stop()，定时器就会停止，其他线程的调用会被忽略
    if (monitor_) {
        monitor_->stop();
    }
    pool_sptr->shutdown();
    // 线程结束
    LOG4CPLUS_INFO_FMT(logger_, "Thread #%d finished: produced=%d, skipped=%d, final_consecutive_failures=%d",
                 thread_id, thread_produced, thread_skipped, consecutive_failures);
    
    // 减少活跃线程计数
    int remaining = active_threads_.fetch_sub(1) - 1;
    if (remaining == 0) {
        // 最后一个线程退出，设置 running_ 为 false
        running_.store(false);
        LOG4CPLUS_INFO(logger_, "All producer threads finished naturally, production line stopped");
    }
}

void VideoProductionLine::setError(const std::string& error_msg) {
    // 保存错误消息
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = error_msg;
    }
    
    // 调用用户回调
    if (error_callback_) {
        try {
            error_callback_(error_msg);
        } catch (...) {
            LOG4CPLUS_WARN(logger_, "Exception in error callback");
        }
    }
    
    // 打印到控制台
    LOG4CPLUS_ERROR_FMT(logger_, "VideoProductionLine Error: %s", error_msg.c_str());
}

// ============================================================
// 全局资源初始化
// ============================================================

void VideoProductionLine::initializeGlobalThreadPool(int thread_pool_size) {
    // 如果为 0，使用默认值 64
    if (thread_pool_size == 0) {
        thread_pool_size = 64;
    }
    
    // 验证范围：必须 > 0 且 <= 128
    if (thread_pool_size <= 0) {
        LOG4CPLUS_WARN(logger_, "线程池大小必须 > 0，使用默认值 64");
        thread_pool_size = 64;
    } else if (thread_pool_size > 128) {
        LOG4CPLUS_WARN(logger_, "线程池大小超过最大值 128，使用最大值 128");
        thread_pool_size = 128;
    }
    
    auto& global_pool = GlobalThreadPool::getInstance();
    
    // 检查线程池是否已初始化
    if (global_pool.isInitialized()) {
        int current_size = global_pool.getSize();
        if (current_size != thread_pool_size) {
            LOG4CPLUS_WARN(logger_, "全局线程池已初始化，使用现有大小: " << current_size 
                           << "（请求大小: " << thread_pool_size << "）");
        } else {
            LOG4CPLUS_DEBUG(logger_, "全局线程池已初始化，大小匹配: " << current_size);
        }
    } else {
        // 初始化全局线程池
        global_pool.setSize(thread_pool_size);
        LOG4CPLUS_INFO(logger_, "全局线程池已初始化 (size=" << thread_pool_size << ")");
    }
}