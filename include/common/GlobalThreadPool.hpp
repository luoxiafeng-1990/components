#pragma once

#include <memory>
#include <mutex>

// 严格模式：依赖必须存在，由构建系统保证
#include "third_party/include/BS_thread_pool.hpp"

/**
 * @brief GlobalThreadPool - 全局线程池单例
 * 
 * 设计原则（严格版）：
 * - 依赖必须存在：构建系统保证 third_party/include/BS_thread_pool.hpp 可用
 * - 资源统一管理：整个项目只有一个线程池实例，避免资源浪费
 * - 线程安全：使用单例模式 + 互斥锁保护
 * 
 * 构建要求：
 * - 确保 third_party/include/BS_thread_pool.hpp 已被拉取/下载
 * 
 * 使用方式：
 * ```cpp
 * // 初始化（可选，如果未初始化会自动使用默认大小）
 * GlobalThreadPool::getInstance().setSize(8);
 * 
 * // 获取线程池引用
 * auto& thread_pool = GlobalThreadPool::getInstance().getThreadPool();
 * thread_pool.detach_task([...]() { ... });
 * ```
 */
class GlobalThreadPool {
public:
    /**
     * @brief 获取全局线程池单例
     */
    static GlobalThreadPool& getInstance() {
        static GlobalThreadPool instance;
        return instance;
    }
    
    /**
     * @brief 获取线程池引用
     * 
     * 如果线程池未初始化，会自动使用默认大小（4）初始化
     */
    BS::thread_pool<>& getThreadPool() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!thread_pool_) {
            thread_pool_ = std::make_unique<BS::thread_pool<>>(default_size_);
        }
        return *thread_pool_;
    }
    
    /**
     * @brief 设置线程池大小
     * 
     * 注意：只在第一次调用时生效，如果线程池已初始化则忽略
     * 
     * @param size 线程池大小
     */
    void setSize(int size) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!thread_pool_ && size > 0) {
            default_size_ = size;
        }
    }
    
    /**
     * @brief 等待所有任务完成
     *
     * 注意：不可在持有 mutex_ 期间调用 thread_pool_->wait()。
     * PARALLEL COMPARE 等多条 MultiWorkerProductionLine 会并发 stop()→wait()；
     * 若 wait() 持锁，其它 stop() 无法置 running_/is_running=false，
     * 而其 worker 任务仍在池中运行，持锁方的 wait() 将永远等不到，形成死锁。
     */
    void wait() {
        BS::thread_pool<>* pool = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pool = thread_pool_.get();
        }
        if (pool) {
            pool->wait();
        }
    }
    
    /**
     * @brief 检查线程池是否已初始化
     * @return true 如果已初始化，false 否则
     */
    bool isInitialized() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return thread_pool_ != nullptr;
    }
    
    /**
     * @brief 获取当前线程池大小
     * @return 线程池大小（如果未初始化，返回 default_size_）
     */
    int getSize() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (thread_pool_) {
            return thread_pool_->get_thread_count();
        }
        return default_size_;
    }
    
    // 禁止拷贝
    GlobalThreadPool(const GlobalThreadPool&) = delete;
    GlobalThreadPool& operator=(const GlobalThreadPool&) = delete;

private:
    GlobalThreadPool() = default;
    ~GlobalThreadPool() = default;
    
    mutable std::mutex mutex_;  // mutable 以支持 const 方法
    std::unique_ptr<BS::thread_pool<>> thread_pool_;
    int default_size_ = 64;  // 默认线程池大小（与配置默认值一致）
};
