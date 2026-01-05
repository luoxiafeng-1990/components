#pragma once

#include "productionline/VideoProductionLine.hpp"
#include "productionline/worker/BufferFillingWorkerFacade.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "common/Logger.hpp"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <map>
#include <queue>
#include <chrono>

// 第三方线程池库（需要下载 BS::thread_pool.hpp）
// GitHub: https://github.com/bshoshany/thread-pool
// 下载地址: https://raw.githubusercontent.com/bshoshany/thread-pool/master/include/BS_thread_pool.hpp
// 放置位置: packages/components/include/third_party/BS_thread_pool.hpp
// 
// 如果文件不存在，请运行以下命令下载：
// cd packages/components/include/third_party
// wget https://raw.githubusercontent.com/bshoshany/thread-pool/master/include/BS_thread_pool.hpp

// 检查文件是否存在
#ifndef MULTI_WORKER_THREAD_POOL_INCLUDED
#define MULTI_WORKER_THREAD_POOL_INCLUDED

#ifdef __has_include
    #if __has_include("third_party/include/BS_thread_pool.hpp")
        #include "third_party/include/BS_thread_pool.hpp"
        #define HAS_BS_THREAD_POOL 1
    #else
        #define HAS_BS_THREAD_POOL 0
    #endif
#else
    #include "third_party/include/BS_thread_pool.hpp"
    #define HAS_BS_THREAD_POOL 1
#endif

#if !HAS_BS_THREAD_POOL
    // 如果文件不存在，先提供占位符类型定义（确保类型完整）
    namespace BS {
        class thread_pool {
        public:
            thread_pool(int) {}
            template<typename F>
            void push_task(F&&) {}
            void wait() {}
        };
    }
    // 然后报错（但类型已经定义，所以不会出现类型错误）
    #error "BS_thread_pool.hpp not found! Please run: cd packages/components/include/third_party && ./download_bs_thread_pool.sh"
#endif

#endif // MULTI_WORKER_THREAD_POOL_INCLUDED

// FFmpeg 头文件（用于 AVPacket 操作）
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

/**
 * @brief CountDownLatch - 倒计时门闩（同步机制）
 * 
 * 用于等待多个线程完成任务
 * 类似 Java 的 CountDownLatch
 */
class CountDownLatch {
public:
    /**
     * @brief 构造函数
     * @param count 初始计数（需要等待的线程数）
     */
    explicit CountDownLatch(int count) : count_(count) {}
    
    /**
     * @brief 计数减1
     * 当计数减到0时，唤醒所有等待的线程
     */
    void countDown() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ > 0) {
            count_--;
            if (count_ == 0) {
                cv_.notify_all();
            }
        }
    }
    
    /**
     * @brief 等待计数减到0
     * @param timeout_ms 超时时间（毫秒），-1 表示无限等待
     * @return true 如果计数减到0，false 如果超时
     */
    bool wait(int timeout_ms = -1) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (timeout_ms < 0) {
            cv_.wait(lock, [this] { return count_ == 0; });
            return true;
        } else {
            return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), 
                               [this] { return count_ == 0; });
        }
    }
    
    /**
     * @brief 获取当前计数
     */
    int getCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    int count_;
};

/**
 * @brief MultiWorkerProductionLine - 多Worker生产流水线
 * 
 * 架构角色：扩展的生产流水线 - 支持一个生产者（record worker）和多个消费者（解码器 worker）
 * 
 * 设计特点：
 * - 派生自 VideoProductionLine，复用基础功能
 * - 内部使用 VideoProductionLine 作为 record worker 的生产者
 * - 使用线程池（BS::thread_pool）管理多个消费者 worker
 * - 支持同步等待所有消费者完成后再归还 buffer
 * - 完善的错误处理和统计监控
 * 
 * 工作流程：
 * 1. 创建内部的 VideoProductionLine（record worker）作为生产者
 * 2. 创建多个解码器 worker 作为消费者
 * 3. 从 record bufferpool 获取 filled buffer（包含 AVPacket）
 * 4. 将 buffer 分发给多个消费者 worker 并行解码
 * 5. 等待所有消费者完成
 * 6. 归还 record buffer
 * 7. 重复步骤 3-6
 */
class MultiWorkerProductionLine : public VideoProductionLine {
public:
    /**
     * @brief 多Worker配置结构
     */
    struct MultiWorkerConfig {
        // Record worker 配置（生产者）
        WorkerConfig record_worker_config;
        
        // 消费者 worker 配置列表
        std::vector<WorkerConfig> consumer_configs;
        
        // 线程池配置
        int thread_pool_size = 4;           // 线程池大小
        int max_pending_tasks = 100;        // 最大待处理任务数（背压）
        
        // 同步配置
        int sync_timeout_ms = 5000;         // 同步超时时间（毫秒）
        
        // 容错配置
        bool continue_on_error = false;     // 部分失败是否继续
        int max_consecutive_errors = 10;   // 最大连续错误数
        
        MultiWorkerConfig() = default;
    };
    
    /**
     * @brief 构造函数
     * 
     * @param config 多Worker配置
     * @param loop 是否循环播放（默认 false）
     * @param thread_count 生产者线程数（默认 1）
     * @param enable_monitor 是否启用性能监控（默认 false）
     */
    MultiWorkerProductionLine(
        const MultiWorkerConfig& config,
        bool loop = false,
        int thread_count = 1,
        bool enable_monitor = false
    );
    
    /**
     * @brief 析构函数
     */
    ~MultiWorkerProductionLine();
    
    // 禁止拷贝
    MultiWorkerProductionLine(const MultiWorkerProductionLine&) = delete;
    MultiWorkerProductionLine& operator=(const MultiWorkerProductionLine&) = delete;
    
    // ========== 核心接口（重写父类）==========
    
    /**
     * @brief 启动多Worker生产流水线
     * 
     * 注意：此方法会启动内部的 record worker 和所有消费者 worker
     * 
     * @return true 如果启动成功
     */
    bool start();
    
    /**
     * @brief 停止多Worker生产流水线
     */
    void stop();
    
    // ========== 查询接口 ==========
    
    /**
     * @brief 获取Record Worker的BufferPool ID
     */
    uint64_t getRecordBufferPoolId() const { return record_buffer_pool_id_; }
    
    /**
     * @brief 获取消费者Worker数量
     */
    size_t getConsumerCount() const { return consumer_workers_.size(); }
    
    /**
     * @brief 获取消费者Worker的BufferPool ID
     * @param index 消费者索引
     * @return BufferPool ID，如果索引无效则返回 0
     */
    uint64_t getConsumerBufferPoolId(size_t index) const;
    
    /**
     * @brief 获取统计信息
     */
    struct Statistics {
        std::atomic<int64_t> total_packets_processed{0};
        std::atomic<int64_t> total_packets_succeeded{0};
        std::atomic<int64_t> total_packets_failed{0};
        std::atomic<int64_t> total_decode_time_us{0};
        std::map<size_t, std::atomic<int64_t>> consumer_success_count;
        std::map<size_t, std::atomic<int64_t>> consumer_error_count;
    };
    
    const Statistics& getStatistics() const { return stats_; }
    
    /**
     * @brief 打印详细统计信息
     */
    void printDetailedStats() const;

private:
    // ========== 内部方法 ==========
    
    /**
     * @brief 重写生产者线程函数
     * 
     * 核心逻辑：
     * 1. 从 record bufferpool 获取 filled buffer
     * 2. 分发给多个消费者 worker 并行解码
     * 3. 等待所有消费者完成
     * 4. 归还 record buffer
     */
    void producerThreadFunc(int thread_id) override;
    
    /**
     * @brief 设置错误信息并触发回调
     */
    void setError(const std::string& error_msg);
    
    // ========== 成员变量 ==========
    
    // 配置
    MultiWorkerConfig config_;
    
    // 内部生产者（record worker）
    std::unique_ptr<VideoProductionLine> record_production_line_;
    uint64_t record_buffer_pool_id_;
    std::weak_ptr<BufferPool> record_buffer_pool_weak_;
    
    // 消费者 worker 管理
    struct ConsumerWorkerInfo {
        std::shared_ptr<BufferFillingWorkerFacade> worker;
        uint64_t buffer_pool_id;
        std::weak_ptr<BufferPool> buffer_pool_weak;
        std::atomic<int64_t> success_count{0};
        std::atomic<int64_t> error_count{0};
        std::atomic<bool> is_active{true};
        
        ConsumerWorkerInfo() = default;
    };
    // 使用指针存储，避免 vector 重新分配时的移动问题（atomic 成员无法移动）
    std::vector<std::unique_ptr<ConsumerWorkerInfo>> consumer_workers_;
    
    // 线程池（使用 BS::thread_pool，使用默认模板参数 tp::none）
    std::unique_ptr<BS::thread_pool<>> thread_pool_;
    
    // 背压控制
    std::atomic<int> pending_tasks_{0};
    std::mutex backpressure_mutex_;
    std::condition_variable backpressure_cv_;
    
    // 错误处理
    std::atomic<int> consecutive_errors_{0};
    std::mutex error_mutex_;
    std::queue<std::string> error_queue_;  // 错误队列（用于诊断）
    
    // 统计信息
    Statistics stats_;
    
    // 日志前缀
    std::string log_prefix_;
};

