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
 * - 支持多个生产者 Worker（如多路 RTSP 录制）
 * - 使用线程池（BS::thread_pool）管理多个消费者 worker
 * - 支持同步等待所有消费者完成后再归还 buffer
 * - 完善的错误处理和统计监控
 * 
 * 工作流程：
 * 1. 创建多个生产者 Worker（如 Record Worker）
 * 2. 创建多个消费者 Worker（如解码器）
 * 3. 从生产者 BufferPool 获取 filled buffer（包含 AVPacket）
 * 4. 将 buffer 分发给多个消费者 worker 并行处理
 * 5. 等待所有消费者完成
 * 6. 归还生产者 buffer
 * 7. 重复步骤 3-6
 */
class MultiWorkerProductionLine : public VideoProductionLine {
public:
    /**
     * @brief WorkerGroup - Worker 工作组
     * 
     * ⭐ 核心概念：一个 Group = 1个生产者 + N个消费者
     * - Group 内强同步：生产者的每个 buffer 必须同时分发给所有消费者
     * - Group 间独立：多个 Group 并行运行，互不干扰
     * - 数据源模式：消费者自动配置为 Buffer 模式，关联到生产者的 BufferPool
     */
    struct WorkerGroup {
        // 组标识
        std::string group_id;                        // 组ID（用于日志和监控）
        
        // 生产者配置（1个）
        WorkerConfig producer_config;
        
        // 消费者配置列表（N个）
        std::vector<WorkerConfig> consumer_configs;
        
        // 组级别配置（可选，覆盖全局配置）
        int sync_timeout_ms = -1;                    // 同步超时（-1 表示使用全局配置）
        int max_consecutive_errors = -1;             // 最大连续错误数（-1 表示使用全局配置）
        bool continue_on_error = false;              // 部分消费者失败是否继续
        
        WorkerGroup() = default;
        explicit WorkerGroup(const std::string& id) : group_id(id) {}
    };
    
    /**
     * @brief 多Worker配置结构（重构版 - 基于 WorkerGroup）
     */
    struct MultiWorkerConfig {
        // ⭐ 核心：Worker Group 列表
        std::vector<WorkerGroup> groups;
        
        // 全局线程池配置
        int thread_pool_size = 4;                    // 共享线程池大小
        int max_pending_tasks = 100;                 // 全局背压阈值（保留，未来可用）
        
        // 全局容错配置（Group 可覆盖）
        int default_sync_timeout_ms = 5000;          // 默认同步超时时间（毫秒）
        int default_max_consecutive_errors = 10;     // 默认最大连续错误数
        bool default_continue_on_error = false;      // 默认部分失败是否继续
        
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
     * 注意：此方法会启动所有生产者 Worker 和所有消费者 Worker
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
     * @brief 获取 WorkerGroup 数量
     */
    size_t getGroupCount() const { return groups_.size(); }
    
    /**
     * @brief 获取指定 Group 的生产者 BufferPool ID
     * @param group_index Group 索引
     * @return BufferPool ID，如果索引无效则返回 0
     */
    uint64_t getGroupProducerBufferPoolId(size_t group_index) const;
    
    /**
     * @brief 获取指定 Group 的消费者数量
     * @param group_index Group 索引
     * @return 消费者数量
     */
    size_t getGroupConsumerCount(size_t group_index) const;
    
    /**
     * @brief 获取指定 Group 的消费者 BufferPool ID
     * @param group_index Group 索引
     * @param consumer_index 消费者索引（在该 Group 内）
     * @return BufferPool ID，如果索引无效则返回 0
     */
    uint64_t getGroupConsumerBufferPoolId(size_t group_index, size_t consumer_index) const;
    
    /**
     * @brief 获取统计信息
     */
    struct Statistics {
        std::atomic<int64_t> total_packets_processed{0};
        std::atomic<int64_t> total_packets_succeeded{0};
        std::atomic<int64_t> total_packets_failed{0};
        std::atomic<int64_t> total_decode_time_us{0};
    };
    
    const Statistics& getStatistics() const { return stats_; }
    
    /**
     * @brief 打印详细统计信息
     */
    void printDetailedStats() const;

private:
    // ========== 内部类型定义 ==========
    
    /**
     * @brief WorkerGroup 运行时信息
     * 
     * 必须在使用它的方法声明之前定义
     */
    struct WorkerGroupRuntime {
        std::string group_id;
        
        // 生产者信息
        std::unique_ptr<VideoProductionLine> producer_line;
        uint64_t producer_buffer_pool_id;
        std::weak_ptr<BufferPool> producer_buffer_pool_weak;
        
        // 消费者信息列表
        struct ConsumerInfo {
            std::shared_ptr<BufferFillingWorkerFacade> worker;
            uint64_t buffer_pool_id;
            std::weak_ptr<BufferPool> buffer_pool_weak;
            std::atomic<int64_t> success_count{0};
            std::atomic<int64_t> error_count{0};
            std::atomic<bool> is_active{true};
        };
        std::vector<std::unique_ptr<ConsumerInfo>> consumers;
        
        // Group 独立线程
        std::thread group_thread;
        std::atomic<bool> is_running{false};
        
        // Group 级别统计
        std::atomic<int64_t> processed_count{0};
        std::atomic<int64_t> success_count{0};
        std::atomic<int64_t> error_count{0};
        std::atomic<int> consecutive_errors{0};
        
        // Group 配置
        int sync_timeout_ms;
        int max_consecutive_errors;
        bool continue_on_error;
        
        WorkerGroupRuntime() = default;
    };
    
    // ========== 内部方法 ==========
    
    /**
     * @brief WorkerGroup 独立线程函数
     * 
     * 每个 Group 有自己的线程，独立运行生产-消费循环
     * 
     * 核心逻辑：
     * 1. 触发该 Group 内所有消费者处理（消费者自动从生产者 Pool 获取数据）
     * 2. 等待所有消费者完成
     * 3. 循环执行
     */
    void groupThreadFunc(WorkerGroupRuntime* group);
    
    /**
     * @brief 创建生产者 Worker
     */
    bool createProducer(WorkerGroupRuntime* group, const WorkerConfig& producer_config);
    
    /**
     * @brief 创建消费者 Workers
     */
    bool createConsumers(WorkerGroupRuntime* group, 
                        const std::vector<WorkerConfig>& consumer_configs,
                        const AVCodecParameters* producer_codec_params);
    
    /**
     * @brief 设置错误信息并触发回调
     */
    void setError(const std::string& error_msg);
    
    // ========== 成员变量 ==========
    
    // 配置
    MultiWorkerConfig config_;
    
    // ⭐ 核心：WorkerGroup 列表
    std::vector<std::unique_ptr<WorkerGroupRuntime>> groups_;
    
    // 共享线程池（所有 Group 共享）
    std::unique_ptr<BS::thread_pool<>> thread_pool_;
    
    // 错误处理
    std::mutex error_mutex_;
    std::queue<std::string> error_queue_;  // 错误队列（用于诊断）
    
    // 统计信息
    Statistics stats_;
    
    // 日志前缀
    std::string log_prefix_;
};

