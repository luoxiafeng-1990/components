#pragma once

#include "buffer/bufferpool/BufferPool.hpp"
#include "productionline/worker/BufferFillingWorkerFacade.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "monitor/PerformanceMonitor.hpp"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <optional>

/**
 * @brief VideoProductionLine - 视频生产流水线
 * 
 * 架构角色：ProductionLine（生产流水线）- 从Worker获取原材料（BufferPool），进行生产
 * 
 * 职责：
 * - 从Worker获取BufferPool（原材料）
 * - 填充 BufferPool 提供的 buffer
 * - 管理多个生产者线程（循环、线程数控制）
 * - 性能监控和统计
 * 
 * 设计特点：
 * - Worker必须创建BufferPool（通过调用Allocator）
 * - 从Worker获取BufferPool（原材料）
 * - 职责单一（只负责视频读取和生产）
 * - 参数驱动（Worker配置 + 生产线控制参数）
 * - 线程安全（支持多线程生产）
 */
class VideoProductionLine {
public:
    /**
     * @brief 错误回调函数类型
     */
    using ErrorCallback = std::function<void(const std::string&)>;
    
    /**
     * @brief 构造函数
     * 
     * @param loop 是否循环播放（默认 false）
     * @param thread_count 生产者线程数（默认 1）
     * @param enable_monitor 是否启用性能监控（默认 true）
     * 
     * 注意：Worker必须在open()时自动创建BufferPool（通过调用Allocator）
     * ProductionLine从Worker获取BufferPool，不再需要外部注入
     */
    VideoProductionLine(bool loop = false, int thread_count = 1, bool enable_monitor = false);
    
    /**
     * @brief 析构函数 - 自动停止生产者
     */
    ~VideoProductionLine();
    
    /**
     * @brief 禁止拷贝构造
     */
    VideoProductionLine(const VideoProductionLine&) = delete;
    
    /**
     * @brief 禁止拷贝赋值
     */
    VideoProductionLine& operator=(const VideoProductionLine&) = delete;
    
    // ========== 核心接口 ==========
    
    /**
     * @brief 启动视频生产流水线
     * @param worker_config Worker 配置（包含文件、输出、解码器等所有配置）
     * @return true 如果启动成功
     * 
     * 注意：loop 和 thread_count 在构造函数中设置
     */
    bool start(const WorkerConfig& worker_config);
    
    /**
     * @brief 停止视频生产流水线
     */
    void stop();
    
    // ========== 查询接口 ==========
    
    /**
     * @brief 检查生产线是否正在运行
     * @return true 如果正在运行，否则返回 false
     */
    bool isRunning() const { return running_.load(); }
    
    /**
     * @brief 获取已生产的帧数
     * @return 已成功生产的帧数
     */
    int getProducedFrames() const { return produced_frames_.load(); }
    
    /**
     * @brief 获取跳过的帧数
     * @return 读取失败而跳过的帧数
     */
    int getSkippedFrames() const { return skipped_frames_.load(); }
    
    /**
     * @brief 获取平均FPS
     * @return 平均每秒生产的帧数
     */
    double getAverageFPS() const;
    
    /**
     * @brief 获取工作BufferPool ID
     * @return BufferPool的注册表ID
     * 
     * @note 消费者应使用此ID从 BufferPoolRegistry 获取 BufferPool
     */
    uint64_t getWorkingBufferPoolId() const { return working_buffer_pool_id_; }
    
    // ========== 错误处理 ==========
    
    /**
     * @brief 设置错误回调
     * @param callback 错误回调函数
     */
    void setErrorCallback(ErrorCallback callback) {
        error_callback_ = callback;
    }
    
    /**
     * @brief 获取最后一次错误信息
     */
    std::string getLastError() const;
    
    // ========== 调试接口 ==========
    
    /**
     * @brief 打印统计信息
     * 
     * @note 输出生产帧数、跳过帧数、平均FPS等统计数据
     */
    void printStats() const;
    
private:
    // ========== 内部方法 ==========
    
    /**
     * @brief 生产者线程函数
     * @param thread_id 线程ID
     */
    void producerThreadFunc(int thread_id);
    
    /**
     * @brief 获取下一个有效的帧索引
     * @return 有效的帧索引，如果无更多帧则返回 std::nullopt
     * 
     * 职责：
     * - 原子地获取下一个原始索引（使用 next_frame_index_）
     * - 使用已缓存的总帧数（total_frames_，在 start() 时从 Worker 获取）
     * - 处理循环模式和文件边界
     * - 处理溢出保护
     * 
     * 注意：这是生产者线程的进度管理逻辑，完全由 ProductionLine 负责
     */
    std::optional<int> getNextFrameIndex();
    
    /**
     * @brief 设置错误信息并触发回调
     */
    void setError(const std::string& error_msg);
    
    // ========== 成员变量 ==========
    
    /**
     * v2.0: Worker创建的BufferPool ID（Registry持有）
     * 
     * 用途：记录Worker创建的BufferPool的ID
     * 
     * 注意：Worker必须在open()时自动创建BufferPool（通过调用Allocator）
     * 如果Worker没有创建BufferPool，start()会失败
     * 
     * Registry独占持有BufferPool（shared_ptr，引用计数=1）
     * ProductionLine从Registry获取临时访问
     */
    uint64_t working_buffer_pool_id_;
    
    /**
     * v2.0: 实际工作的BufferPool weak_ptr（缓存的观察者，符合架构设计）
     * 
     * 设计：
     * - 从 Registry 获取 weak_ptr，不持有所有权（符合去中心化设计）
     * - 使用时通过 lock() 获取临时 shared_ptr
     * - 自动检测 Pool 是否已销毁（lock() 返回 nullptr）
     * - Registry 独占持有 BufferPool（shared_ptr，引用计数=1）
     */
    std::weak_ptr<BufferPool> working_buffer_pool_weak_;
    
    // Worker Facade（多线程共享）
    std::shared_ptr<BufferFillingWorkerFacade> worker_facade_sptr_;
    
    // 线程管理
    std::vector<std::thread> threads_;
    std::atomic<bool> running_;
    std::atomic<int> active_threads_;    // 活跃线程计数
    std::mutex threads_mutex_;            // 保护线程相关操作
    
    // 统计信息
    std::atomic<int> produced_frames_;
    std::atomic<int> skipped_frames_;
    std::atomic<int> next_frame_index_;  // 下一个要读取的帧索引（原子递增）
    
    // 配置（存储启动时的参数）
    bool loop_;                          // 是否循环播放
    int thread_count_;                   // 生产者线程数
    int total_frames_;                   // 总帧数
    bool enable_monitor_;                // 是否启用性能监控
    
    // 错误处理
    ErrorCallback error_callback_;
    mutable std::mutex error_mutex_;
    std::string last_error_;
    
    // 性能监控
    std::chrono::steady_clock::time_point start_time_;
    std::unique_ptr<PerformanceMonitor> monitor_;  // 填充buffer性能监控
};

