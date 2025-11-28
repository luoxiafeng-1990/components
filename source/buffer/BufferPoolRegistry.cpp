#include "buffer/BufferPoolRegistry.hpp"
#include "buffer/BufferPool.hpp"
#include <stdio.h>
#include <algorithm>
#include <iomanip>
#include <sstream>

// ========== 单例实例 ==========

BufferPoolRegistry& BufferPoolRegistry::getInstance() {
    static BufferPoolRegistry instance;
    return instance;
}

// ========== 注册管理接口实现 ==========

uint64_t BufferPoolRegistry::registerPoolWeak(std::shared_ptr<BufferPool> temp_shared) {
    if (!temp_shared) {
        printf("⚠️  Error: Cannot register null BufferPool\n");
        return 0;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 从 pool 对象获取 name 和 category
    const std::string& name = temp_shared->getName();
    const std::string& category = temp_shared->getCategory();
    
    // 检查名称是否已存在
    if (name_to_id_.find(name) != name_to_id_.end()) {
        printf("⚠️  Warning: BufferPool name '%s' already exists, appending ID suffix\n", 
               name.c_str());
    }
    
    // 分配 ID
    uint64_t id = next_id_++;
    
    // 创建 PoolInfo（使用 weak_ptr，不持有所有权）
    PoolInfo info;
    info.pool = temp_shared;  // 转为 weak_ptr（不增加引用计数）
    info.id = id;
    info.name = name;
    info.category = category;
    info.created_time = std::chrono::system_clock::now();
    
    // 注册
    pools_[id] = info;
    name_to_id_[name] = id;
    
    printf("📦 [Registry] BufferPool registered (weak_ptr): '%s' (ID: %lu, Category: %s)\n",
           name.c_str(), id, category.empty() ? "None" : category.c_str());
    
    return id;
}

void BufferPoolRegistry::unregisterPool(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = pools_.find(id);
    if (it == pools_.end()) {
        printf("⚠️  Warning: Trying to unregister non-existent BufferPool (ID: %lu)\n", id);
        return;
    }
    
    const std::string& name = it->second.name;
    
    // 移除名称索引
    name_to_id_.erase(name);
    
    // 移除 Pool
    pools_.erase(it);
    
    printf("📦 [Registry] BufferPool unregistered: '%s' (ID: %lu)\n", name.c_str(), id);
}

// ========== 只读接口实现 ==========

std::shared_ptr<const BufferPool> BufferPoolRegistry::getPoolReadOnly(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = pools_.find(id);
    if (it == pools_.end()) {
        return nullptr;
    }
    
    // 尝试提升 weak_ptr 为 shared_ptr（如果 Pool 已销毁，返回 nullptr）
    auto pool = it->second.pool.lock();
    if (!pool) {
        return nullptr;  // Pool 已销毁
    }
    
    // 返回只读版本（const shared_ptr）
    return std::const_pointer_cast<const BufferPool>(pool);
}

std::shared_ptr<const BufferPool> BufferPoolRegistry::getPoolReadOnlyByName(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = name_to_id_.find(name);
    if (it == name_to_id_.end()) {
        return nullptr;
    }
    
    uint64_t id = it->second;
    auto pool_it = pools_.find(id);
    if (pool_it == pools_.end()) {
        return nullptr;
    }
    
    // 尝试提升 weak_ptr 为 shared_ptr（如果 Pool 已销毁，返回 nullptr）
    auto pool = pool_it->second.pool.lock();
    if (!pool) {
        return nullptr;  // Pool 已销毁
    }
    
    // 返回只读版本（const shared_ptr）
    return std::const_pointer_cast<const BufferPool>(pool);
}

std::vector<std::shared_ptr<const BufferPool>> BufferPoolRegistry::getAllPoolsReadOnly() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::shared_ptr<const BufferPool>> result;
    result.reserve(pools_.size());
    
    for (const auto& pair : pools_) {
        // 尝试提升 weak_ptr 为 shared_ptr（跳过已销毁的 Pool）
        auto pool = pair.second.pool.lock();
        if (pool) {
            result.push_back(std::const_pointer_cast<const BufferPool>(pool));
        }
    }
    
    return result;
}

std::vector<std::shared_ptr<const BufferPool>> BufferPoolRegistry::getPoolsByCategoryReadOnly(const std::string& category) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::shared_ptr<const BufferPool>> result;
    
    for (const auto& pair : pools_) {
        if (pair.second.category == category) {
            // 尝试提升 weak_ptr 为 shared_ptr（跳过已销毁的 Pool）
            auto pool = pair.second.pool.lock();
            if (pool) {
                result.push_back(std::const_pointer_cast<const BufferPool>(pool));
            }
        }
    }
    
    return result;
}

std::vector<std::shared_ptr<const BufferPool>> BufferPoolRegistry::getWorkerPoolsReadOnly() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::shared_ptr<const BufferPool>> result;
    
    for (const auto& pair : pools_) {
        if (pair.second.category == "Worker") {
            // 尝试提升 weak_ptr 为 shared_ptr（跳过已销毁的 Pool）
            auto pool = pair.second.pool.lock();
            if (pool) {
                result.push_back(std::const_pointer_cast<const BufferPool>(pool));
            }
        }
    }
    
    return result;
}

std::shared_ptr<const BufferPool> BufferPoolRegistry::getWorkerPoolReadOnly(const std::string& worker_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Worker Pool 名称格式：WorkerPool_<WorkerName>
    std::string pool_name = "WorkerPool_" + worker_name;
    
    auto it = name_to_id_.find(pool_name);
    if (it == name_to_id_.end()) {
        return nullptr;
    }
    
    uint64_t id = it->second;
    auto pool_it = pools_.find(id);
    if (pool_it == pools_.end()) {
        return nullptr;
    }
    
    // 尝试提升 weak_ptr 为 shared_ptr（如果 Pool 已销毁，返回 nullptr）
    auto pool = pool_it->second.pool.lock();
    if (!pool) {
        return nullptr;  // Pool 已销毁
    }
    
    // 返回只读版本（const shared_ptr）
    return std::const_pointer_cast<const BufferPool>(pool);
}

size_t BufferPoolRegistry::getPoolCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pools_.size();
}

// ========== 读写接口实现（仅 ProductionLine 可以调用）==========

std::shared_ptr<BufferPool> BufferPoolRegistry::getPoolForProductionLine(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = pools_.find(id);
    if (it == pools_.end()) {
        return nullptr;
    }
    
    // 尝试提升 weak_ptr 为 shared_ptr（如果 Pool 已销毁，返回 nullptr）
    return it->second.pool.lock();
}

std::shared_ptr<BufferPool> BufferPoolRegistry::getPoolByNameForProductionLine(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = name_to_id_.find(name);
    if (it == name_to_id_.end()) {
        return nullptr;
    }
    
    uint64_t id = it->second;
    auto pool_it = pools_.find(id);
    if (pool_it == pools_.end()) {
        return nullptr;
    }
    
    // 尝试提升 weak_ptr 为 shared_ptr（如果 Pool 已销毁，返回 nullptr）
    return pool_it->second.pool.lock();
}

void BufferPoolRegistry::cleanupExpiredPools() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto it = pools_.begin(); it != pools_.end(); ) {
        if (it->second.pool.expired()) {
            // Pool 已销毁，清理条目
            name_to_id_.erase(it->second.name);
            it = pools_.erase(it);
        } else {
            ++it;
        }
    }
}

// ========== 全局监控接口实现 ==========

void BufferPoolRegistry::printAllStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    printf("\n");
    printf("========================================\n");
    printf("📊 Global BufferPool Statistics\n");
    printf("========================================\n");
    printf("Total Pools: %zu\n\n", pools_.size());
    
    if (pools_.empty()) {
        printf("   (No BufferPools registered)\n");
        printf("========================================\n\n");
        return;
    }
    
    // 按 ID 排序输出
    std::vector<uint64_t> ids;
    ids.reserve(pools_.size());
    for (const auto& pair : pools_) {
        ids.push_back(pair.first);
    }
    std::sort(ids.begin(), ids.end());
    
    size_t total_memory = 0;
    
    for (uint64_t id : ids) {
        const PoolInfo& info = pools_.at(id);
        std::shared_ptr<BufferPool> pool = info.pool.lock();  // 提升 weak_ptr
        
        if (!pool) {
            // Pool 已销毁，跳过
            continue;
        }
        
        // 格式化时间
        auto time_t_val = std::chrono::system_clock::to_time_t(info.created_time);
        char time_buf[100];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", 
                      std::localtime(&time_t_val));
        
        // 打印 Pool 信息
        printf("[%s] %s (ID: %lu)\n",
               info.category.empty() ? "Uncategorized" : info.category.c_str(),
               info.name.c_str(),
               info.id);
        
        printf("   Buffers: %d total, %d free, %d filled\n",
               pool->getTotalCount(),
               pool->getFreeCount(),
               pool->getFilledCount());
        
        size_t pool_memory = pool->getTotalCount() * pool->getBufferSize();
        total_memory += pool_memory;
        
        printf("   Memory: %.2f MB\n", pool_memory / (1024.0 * 1024.0));
        printf("   Created: %s\n\n", time_buf);
    }
    
    printf("========================================\n");
    printf("TOTAL MEMORY: %.2f MB\n", total_memory / (1024.0 * 1024.0));
    printf("========================================\n\n");
}

size_t BufferPoolRegistry::getTotalMemoryUsage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    size_t total = 0;
    
    for (const auto& pair : pools_) {
        std::shared_ptr<BufferPool> pool = pair.second.pool.lock();  // 提升 weak_ptr
        if (pool) {
            total += pool->getTotalCount() * pool->getBufferSize();
        }
    }
    
    return total;
}

BufferPoolRegistry::GlobalStats BufferPoolRegistry::getGlobalStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    GlobalStats stats;
    stats.total_pools = static_cast<int>(pools_.size());
    stats.total_buffers = 0;
    stats.total_free = 0;
    stats.total_filled = 0;
    stats.total_memory = 0;
    
    for (const auto& pair : pools_) {
        std::shared_ptr<BufferPool> pool = pair.second.pool.lock();  // 提升 weak_ptr
        if (pool) {
            stats.total_buffers += pool->getTotalCount();
            stats.total_free += pool->getFreeCount();
            stats.total_filled += pool->getFilledCount();
            stats.total_memory += pool->getTotalCount() * pool->getBufferSize();
        }
    }
    
    return stats;
}





