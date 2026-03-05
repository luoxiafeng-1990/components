#pragma once

#include "productionline/VideoProductionLine.hpp"
#include "productionline/worker/BufferFillingWorkerFacade.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/WorkerSyncCoordinator.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "common/Logger.hpp"
#include "common/GlobalThreadPool.hpp"
#include "common/Timer.hpp"
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
#include <set>
#include <unordered_map>

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
    // ⭐ v2.20：配置结构已移动到 WorkerConfig.hpp
    // 使用：ProducerConfig, ConsumerConfig, ConnectorConfig, WorkerGroup, MultiWorkerConfig
    
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
     * @brief 获取指定 Group 的消费者 BufferPool ID（通过索引）
     * @param group_index Group 索引
     * @param consumer_index 消费者索引（在该 Group 内）
     * @return BufferPool ID，如果索引无效则返回 0
     */
    uint64_t getGroupConsumerBufferPoolId(size_t group_index, size_t consumer_index) const;
    
    /**
     * @brief 获取所有生产线的总成功帧数（实时计算）
     */
    int64_t getAllLineFramesProduced() const;
    
    /**
     * @brief 获取所有生产线的总失败帧数（实时计算）
     */
    int64_t getAllLineFramesFailed() const;
    
    /**
     * @brief 获取指定 Group 的活跃 Worker 数量（实时计算）
     * @param group_index Group 索引
     * @return 活跃 Worker 数量，如果索引无效则返回 0
     */
    int getActiveWorkerCount(size_t group_index) const;
    
    /**
     * @brief 打印详细统计信息
     */
    void printDetailedStats() const;

private:
    // ========== 内部类型定义 ==========
    
    /**
     * @brief WorkerGroupRuntime - Group 运行时数据
     * 
     * ⭐ 设计说明：配置与运行时分离模式（Configuration vs Runtime State）
     * 
     * 职责：
     * - 存储"实际创建的对象"和"运行时状态"
     * - 在 start() 时根据 WorkerGroupConfig 创建
     * - 在 stop() 时销毁
     * - 包含不可序列化的对象（线程、智能指针等）
     */
    struct WorkerGroupRuntime {
        std::string group_id;
        
        // 生产者信息
        struct ProducerInfo {
            std::string producer_name;
            std::unique_ptr<VideoProductionLine> producer_line;
            uint64_t buffer_pool_id{0};
            std::weak_ptr<BufferPool> buffer_pool_weak;
        };
        std::vector<std::unique_ptr<ProducerInfo>> producer_infos;
        std::unordered_map<std::string, ProducerInfo*> producer_info_mapped_by_name;
        
        // 消费者信息
        struct ConsumerInfo {
            std::string consumer_name;
            std::shared_ptr<BufferFillingWorkerFacade> worker;
            uint64_t buffer_pool_id{0};
            std::weak_ptr<BufferPool> buffer_pool_weak;
        };
        std::vector<std::unique_ptr<ConsumerInfo>> consumer_infos;
        std::unordered_map<std::string, ConsumerInfo*> consumer_info_mapped_by_name;
        
        // 连接器列表
        std::vector<std::unique_ptr<Connector>> connectors;
        
        // ⭐ v2.23 新增：每个 Connector 的同步协调器
        std::map<size_t, std::unique_ptr<WorkerSyncCoordinator>> connector_coordinators;
        
        // Group 独立线程
        std::thread group_thread;
        std::atomic<bool> is_running{false};
        
        // ⭐ Group 级别统计
        struct GroupStats {
            std::atomic<int64_t> frames_produced{0};  // Group 成功帧数
            std::atomic<int64_t> frames_failed{0};    // Group 失败帧数
        };
        GroupStats stats;  // Group 统计信息
        
        // ⭐ Worker 生产统计
        struct WorkerProductionStats {
            std::atomic<int64_t> worker_frames_produced{0};  // Worker 累计生产的帧数
            std::atomic<int64_t> worker_frames_failed{0};    // Worker 累计失败的帧数
            std::atomic<int64_t> consecutive_failures{0};    // 连续失败次数（用于熔断）
            std::atomic<bool> is_active{true};               // 是否活跃
        };
        
        // Worker 统计映射：consumer_name -> WorkerProductionStats
        std::unordered_map<std::string, std::unique_ptr<WorkerProductionStats>> worker_stats;
        
        // ========== 查询方法 ==========
        
        /**
         * @brief 根据生产者名称获取 ProducerInfo 指针
         * @param producer_name 生产者名称
         * @return ProducerInfo 指针，如果不存在则返回 nullptr
         */
        ProducerInfo* getProducerInfo(const std::string& producer_name) const;
        
        /**
         * @brief 根据消费者名称获取 ConsumerInfo 指针
         * @param consumer_name 消费者名称
         * @return ConsumerInfo 指针，如果不存在则返回 nullptr
         */
        ConsumerInfo* getConsumerInfo(const std::string& consumer_name) const;
        
        /**
         * @brief 根据生产者名称获取 BufferPool ID
         * @param producer_name 生产者名称
         * @return BufferPool ID，如果不存在则返回 0
         */
        uint64_t getProducerBufferPoolId(const std::string& producer_name) const;
        
        /**
         * @brief 根据消费者名称获取 BufferPool ID
         * @param consumer_name 消费者名称
         * @return BufferPool ID，如果不存在则返回 0
         */
        uint64_t getConsumerBufferPoolId(const std::string& consumer_name) const;
        
        /**
         * @brief 根据消费者名称查找所属的 Connector
         * @param consumer_name 消费者名称
         * @return Connector 指针，如果不存在则返回 nullptr
         */
        Connector* getConnectorForConsumer(const std::string& consumer_name) const;
        
        /**
         * @brief 根据 Connector 指针获取其索引
         * @param connector Connector 指针
         * @return Connector 索引，如果不存在则返回 SIZE_MAX
         */
        size_t getConnectorIndex(const Connector* connector) const;
    };
    
    // ========== 内部方法 ==========
    
    /**
     * @brief 校验配置（私有函数）
     * 
     * 校验内容：
     * - 每个 Group 必须至少有一个生产者和一个消费者
     * - 每个 Group 必须至少有一个连接器
     * - 连接器的 producer_ids 和 consumer_ids 必须存在
     * - 连接器模式校验（ONE_TO_ONE、ONE_TO_MANY、MANY_TO_ONE 的规则）
     * - 检查是否有未连接的 Producer/Consumer
     * 
     * @return true 如果配置有效，false 如果配置无效
     */
    bool validateConfig() const;
    
    /**
     * @brief WorkerGroup 调度线程函数
     * 
     * 核心逻辑：
     * 1. 为该 Group 内所有 Worker 提交常驻任务到线程池
     * 2. 任务提交后，调度线程结束
     */
    void groupThreadFunc(const std::shared_ptr<WorkerGroupRuntime>& group);
    
    /**
     * @brief Worker 常驻线程函数
     * 
     * 核心逻辑：
     * 1. 循环执行：acquireFree -> fillBuffer -> submitFilled
     * 2. fillBuffer 内部会调用 readAndSendPacket，可能等待其他订阅者
     * 3. 永不退出（除非 stop 或 fatal 错误）
     * 
     * @param group Group 运行时对象指针
     * @param consumer_info Consumer 信息指针
     * @param consumer_name Consumer 名称
     */
    void workerThreadFunc(const std::shared_ptr<WorkerGroupRuntime>& group, 
                         WorkerGroupRuntime::ConsumerInfo* consumer_info,
                         const std::string& consumer_name);
    
    /**
     * @brief 执行帧同步（如果启用）
     * 
     * ⭐ v2.27 新增：将帧同步逻辑封装到独立函数，避免代码重复
     * ⭐ v2.29 修改：添加 FillBufferResult 参数，支持区分 SUCCESS/EAGAIN/ERROR
     * 
     * 注意：帧同步模式下，deferred_commit 已在 createConsumersForGroup 中被设置为 true
     * 因此 Worker 内部不会调用 commitEncodedPacket，由此函数负责调用
     * 
     * @param group WorkerGroup 运行时
     * @param consumer_name 消费者名称
     * @param consumer_info 消费者信息
     * @param buffer 解码后的 Buffer（失败时传 nullptr）
     * @param status fillBuffer 的返回状态
     * @return true=允许提交, false=拒绝提交（未启用帧同步时始终返回 true）
     * 
     * v2.33 变更：参数类型从 FillBufferResult 改为 FillStatus
     * v2.34 变更：参数类型从 FillStatus 改为 FillResult
     */
    bool performFrameSync(const std::shared_ptr<WorkerGroupRuntime>& group,
                         const std::string& consumer_name,
                         WorkerGroupRuntime::ConsumerInfo* consumer_info,
                         Buffer* buffer,
                         const FillResult& result);
    
    /**
     * @brief 设置错误信息并触发回调
     */
    void setError(const std::string& error_msg) const;
    
    /**
     * @brief 为单个 Group 创建所有生产者
     * @param group Group 运行时对象指针
     * @param group_config Group 配置
     * @return true 如果成功，false 如果失败（失败时已调用 setError 和 groups_.clear()）
     */
    bool createProducersForGroup(WorkerGroupRuntime* group, const WorkerGroupConfig& group_config);
    
    /**
     * @brief 为单个 Group 创建所有 Connector 并设置 shared_source
     * @param group Group 运行时对象指针
     * @param group_config Group 配置
     * @return true 如果成功，false 如果失败（失败时已调用 setError 和 groups_.clear()）
     */
    bool createConnectorsForGroup(WorkerGroupRuntime* group, const WorkerGroupConfig& group_config);
    
    /**
     * @brief 为单个 Group 创建所有消费者
     * @param group Group 运行时对象指针
     * @param group_config Group 配置
     * @return true 如果成功，false 如果失败（失败时已调用 setError 和 groups_.clear()）
     */
    bool createConsumersForGroup(WorkerGroupRuntime* group, const WorkerGroupConfig& group_config);
    
    /**
     * @brief 启动所有 Group 线程
     * @return true 如果成功，false 如果失败（失败时已调用 setError 和 groups_.clear()）
     */
    bool startGroupThreads();
    
    /**
     * @brief 启动统计报告定时器
     */
    void startStatsReportTimer();
    
    /**
     * @brief 停止统计报告定时器
     */
    void stopStatsReportTimer();
    
    // ========== 成员变量 ==========
    
    // 配置
    MultiWorkerConfig config_;
    
    // ⭐ 核心：WorkerGroup 列表
    std::vector<std::shared_ptr<WorkerGroupRuntime>> groups_;
    
    // 错误处理（mutable 允许在 const 函数中修改）
    mutable std::mutex error_mutex_;
    mutable std::queue<std::string> error_queue_;  // 错误队列（用于诊断）
    
    // ⭐ Logger（每个类持有自己的 Logger 实例）
    log4cplus::Logger logger_;
    
    // 日志前缀
    std::string log_prefix_;
    
    // 统计报告定时器
    Timer stats_report_timer_;
    std::atomic<Timer::TimerId> stats_report_timer_id_{0};
};
