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
     */
    void wait() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (thread_pool_) {
            thread_pool_->wait();
        }
    }
    
    // 禁止拷贝
    GlobalThreadPool(const GlobalThreadPool&) = delete;
    GlobalThreadPool& operator=(const GlobalThreadPool&) = delete;

private:
    GlobalThreadPool() = default;
    ~GlobalThreadPool() = default;
    
    std::mutex mutex_;
    std::unique_ptr<BS::thread_pool<>> thread_pool_;
    int default_size_ = 4;  // 默认线程池大小
};
