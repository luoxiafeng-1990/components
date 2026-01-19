#pragma once

#include "productionline/VideoProductionLine.hpp"
#include "productionline/worker/BufferFillingWorkerFacade.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "common/Logger.hpp"
#include "common/GlobalThreadPool.hpp"
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
        
        // Group 独立线程
        std::thread group_thread;
        std::atomic<bool> is_running{false};
        
        // Group 级别统计
        std::atomic<int64_t> processed_count{0};
        std::atomic<int64_t> success_count{0};
        std::atomic<int64_t> error_count{0};
        
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
    };
    
    // 向后兼容：保留 GroupData 作为 WorkerGroupRuntime 的别名
    using GroupData = WorkerGroupRuntime;
    
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
    
    // ========== 成员变量 ==========
    
    // 配置
    MultiWorkerConfig config_;
    
    // ⭐ 核心：WorkerGroup 列表
    std::vector<std::unique_ptr<GroupData>> groups_;
    
    // 错误处理（mutable 允许在 const 函数中修改）
    mutable std::mutex error_mutex_;
    mutable std::queue<std::string> error_queue_;  // 错误队列（用于诊断）
    
    // 统计信息
    Statistics stats_;
    
    // ⭐ Logger（每个类持有自己的 Logger 实例）
    log4cplus::Logger logger_;
    
    // 日志前缀
    std::string log_prefix_;
};

