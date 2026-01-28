#ifndef WORKER_SYNC_COORDINATOR_HPP
#define WORKER_SYNC_COORDINATOR_HPP

#include <functional>
#include <map>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <log4cplus/logger.h>

// 前向声明
class Buffer;

// ⭐ 注意：FrameSyncCallback 和 CallbackChainItem 的定义在 WorkerConfig.hpp 中
// 这里只需要包含该头文件即可
#include "productionline/worker/WorkerConfig.hpp"

/**
 * @brief Worker 同步协调器
 * 
 * 职责：
 * - 协调同一 Connector 内的多个 Worker
 * - 在 Worker 解码完成后、提交 Buffer 前插入同步点
 * - 按顺序执行回调链（Pipeline 模式）
 * 
 * 设计原则：
 * - 单一职责：只负责 Worker 间的同步协调
 * - 与 BufferPacketSource 解耦：通过 frame_version 关联
 * - 可选配置：无回调时零开销
 * 
 * 使用场景：
 * - PSNR 对比：硬件解码 vs 软件解码
 * - 质量检测：多路解码结果一致性检查
 * - 数据聚合：多 Worker 协同处理
 * 
 * @note 每个 Connector 创建一个实例
 */
class WorkerSyncCoordinator {
public:
    /**
     * @brief 构造函数
     * 
     * @param worker_names 参与同步的 Worker 名称列表
     * @param callback_chain 回调链（可选，默认为空）
     */
    WorkerSyncCoordinator(
        const std::vector<std::string>& worker_names,
        const CallbackChain& callback_chain = {}
    );
    
    /**
     * @brief 析构函数
     */
    ~WorkerSyncCoordinator();
    
    /**
     * @brief Worker 到达同步点
     * 
     * @param worker_name Worker 名称
     * @param frame_version 帧版本号（来自 BufferPacketSource）
     * @param buffer 解码后的 Buffer
     * @return true=允许提交, false=拒绝提交
     * 
     * 行为：
     * - 如果回调链为空，直接返回 true（零开销）
     * - 阻塞等待，直到所有 Worker 都到达同一版本
     * - 最后一个到达的 Worker 触发回调链执行
     * - 按顺序执行回调链，任何回调返回 false 则终止链
     * - 回调执行完毕后，所有 Worker 被唤醒
     * 
     * 线程安全：是
     */
    bool arrive(const std::string& worker_name, uint64_t frame_version, Buffer* buffer);
    
    /**
     * @brief 获取参与同步的 Worker 数量
     */
    size_t getWorkerCount() const { return total_workers_; }
    
    /**
     * @brief 获取回调链长度
     */
    size_t getCallbackCount() const { return callback_chain_.size(); }
    
    /**
     * @brief 检查是否启用了同步（是否有回调）
     */
    bool isEnabled() const { return !callback_chain_.empty(); }
    
private:
    /**
     * @brief 单帧同步数据
     */
    struct FrameSync {
        std::map<std::string, Buffer*> worker_buffers;  // worker_name -> Buffer*
        size_t arrived_count = 0;                       // 已到达的 Worker 数量
        bool callback_executed = false;                 // 回调是否已执行
        bool should_submit = true;                      // 是否允许提交
    };
    
    /**
     * @brief 执行回调链
     * 
     * @param frame_version 帧版本号
     * @param worker_buffers 所有 Worker 的 Buffer
     * @return true=所有回调通过, false=有回调失败
     */
    bool executeCallbackChain(
        uint64_t frame_version,
        const std::map<std::string, Buffer*>& worker_buffers
    );
    
    /**
     * @brief 清理旧版本的同步数据
     * 
     * @param current_version 当前版本号
     */
    void cleanupOldFrames(uint64_t current_version);
    
    // 配置
    std::vector<std::string> worker_names_;  // 参与同步的 Worker 名称列表
    size_t total_workers_;                   // Worker 总数
    CallbackChain callback_chain_;           // 回调链
    
    // 同步状态
    std::mutex mutex_;                       // 互斥锁
    std::condition_variable cv_;             // 条件变量
    std::map<uint64_t, FrameSync> frame_syncs_;  // frame_version -> FrameSync
    
    // 日志
    log4cplus::Logger logger_;
};

#endif // WORKER_SYNC_COORDINATOR_HPP
