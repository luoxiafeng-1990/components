#pragma once

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <vector>
#include <string>
#include <chrono>

class WorkerBase;

/**
 * @brief Worker 全局注册表（单例）
 * 
 * 设计参照 BufferPoolRegistry，与其保持一致的架构风格。
 * 
 * 职责：
 * - 持有所有已注册 Worker 的 shared_ptr（共享所有权）
 * - 提供全局查询和生命周期追踪接口
 * - 通过 weak_ptr 供外部观察 Worker 是否仍存活
 * 
 * 与 BufferPoolRegistry 的关键差异：
 * - BufferPoolRegistry 独占持有 BufferPool（引用计数=1）
 * - WorkerRegistry 共享持有 Worker（ProductionLine 也持有 shared_ptr）
 * - 因此 WorkerRegistry 的 unregister 只释放 Registry 侧的引用，
 *   实际 Worker 在 ProductionLine 释放后才析构
 * 
 * ID 编号：从 1 开始，与 BufferPoolRegistry 一致
 * 
 * 线程安全：所有接口内部使用 mutex 保护
 */
class WorkerRegistry {
public:
    static WorkerRegistry& getInstance();

    WorkerRegistry(const WorkerRegistry&) = delete;
    WorkerRegistry& operator=(const WorkerRegistry&) = delete;
    WorkerRegistry(WorkerRegistry&&) = delete;
    WorkerRegistry& operator=(WorkerRegistry&&) = delete;

    // ========== 注册管理接口 ==========

    /**
     * @brief 注册 Worker（由 Factory::create() 自动调用）
     * 
     * @param worker Worker 的 shared_ptr
     * @return 唯一 ID（从 1 开始递增），失败返回 0
     */
    uint64_t registerWorker(std::shared_ptr<WorkerBase> worker);

    /**
     * @brief 注销 Worker
     * 
     * 释放 Registry 侧对 Worker 的 shared_ptr 引用。
     * 如果 ProductionLine 仍持有 shared_ptr，Worker 不会析构。
     * 
     * @param id Worker ID
     */
    void unregisterWorker(uint64_t id);

    // ========== 查询接口 ==========

    /**
     * @brief 获取 Worker（返回 weak_ptr，观察者模式）
     * 
     * @param id Worker ID
     * @return weak_ptr<WorkerBase>，不存在时返回空 weak_ptr
     */
    std::weak_ptr<WorkerBase> getWorker(uint64_t id) const;

    /**
     * @brief 获取已注册的 Worker 总数
     */
    size_t getWorkerCount() const;

    /**
     * @brief 获取所有已注册的 Worker ID 列表
     */
    std::vector<uint64_t> getAllWorkerIds() const;

    // ========== 监控接口 ==========

    struct WorkerInfo {
        uint64_t id;
        std::shared_ptr<WorkerBase> worker;
        std::chrono::system_clock::time_point created_time;
    };

    /**
     * @brief 打印所有 Worker 的摘要信息
     */
    void printAllStats() const;

private:
    WorkerRegistry()
        : next_id_(1)
        , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Registry")))
    {}
    ~WorkerRegistry() = default;

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, WorkerInfo> workers_;
    uint64_t next_id_;
    log4cplus::Logger logger_;
};
