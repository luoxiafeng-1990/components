#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "common/Logger.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
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

uint64_t BufferPoolRegistry::registerPool(std::shared_ptr<BufferPool> pool, uint64_t allocator_id) {
    if (!pool) {
        LOG_WARN("[Registry]  Error: Cannot register null BufferPool\n");
        return 0;
    }
    
    if (allocator_id == 0) {
        LOG_WARN("[Registry]  Error: Invalid allocator_id (0)\n");
        return 0;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 从 pool 对象获取 name 和 category
    const std::string& name = pool->getName();
    const std::string& category = pool->getCategory();
    
    // 检查名称是否已存在
    if (name_to_id_.find(name) != name_to_id_.end()) {
        LOG_WARN("[Registry]  Warning: BufferPool name '%s' already exists, appending ID suffix\n", 
               name.c_str());
    }
    
    // 分配 ID
    uint64_t id = next_id_++;
    
    // 创建 PoolInfo（v2.0: 使用 shared_ptr，Registry 独占持有）
    PoolInfo info;
    info.pool = pool;  // ✅ Registry 持有所有权（引用计数=1）
    info.id = id;
    info.name = name;
    info.category = category;
    info.created_time = std::chrono::system_clock::now();
    info.allocator_id = allocator_id;  // 🆕 记录创建者 Allocator ID
    
    // 注册
    pools_[id] = info;
    name_to_id_[name] = id;
    
    LOG_DEBUG("[Registry] [Registry] BufferPool registered: '%s' (ID: %lu, Allocator ID: %lu, Category: %s, ref_count=1)\n",
           name.c_str(), id, allocator_id, category.empty() ? "None" : category.c_str());
    
    return id;
}

void BufferPoolRegistry::unregisterPool(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = pools_.find(id);
    if (it == pools_.end()) {
        LOG_WARN("[Registry]  Warning: Trying to unregister non-existent BufferPool (ID: %lu)\n", id);
        return;
    }
    
    const std::string& name = it->second.name;
    
    // 移除名称索引
    name_to_id_.erase(name);
    
    // 移除 Pool（v2.0: 释放 shared_ptr，引用计数 -1 → 0 → 触发 Pool 析构）
    pools_.erase(it);
    
    LOG_DEBUG("[Registry] [Registry] BufferPool unregistered and destroyed: '%s' (ID: %lu)\n", name.c_str(), id);
}

// ========== 公开接口实现 ==========

std::weak_ptr<BufferPool> BufferPoolRegistry::getPool(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = pools_.find(id);
    if (it == pools_.end()) {
        return std::weak_ptr<BufferPool>();  // 返回空的 weak_ptr
    }
    
    // v2.0: 返回 weak_ptr（观察者模式，不持有所有权）
    return it->second.pool;
}

size_t BufferPoolRegistry::getPoolCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pools_.size();
}

// ========== v2.0 新增：Allocator 友元方法 ==========

std::shared_ptr<BufferPool> BufferPoolRegistry::getPoolSpecialForAllocator(uint64_t id) {
    // 🔑 私有方法，只有友元 BufferAllocatorBase 可以调用
    // 用于 Allocator 析构时获取 Pool 并清理 Buffer
    // 返回 shared_ptr（不是 weak_ptr），保证清理期间 Pool 不被销毁
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = pools_.find(id);
    if (it == pools_.end()) {
        return nullptr;
    }
    
    // 返回 shared_ptr（临时持有，用于清理）
    return it->second.pool;
}

std::vector<uint64_t> BufferPoolRegistry::getPoolsByAllocator(uint64_t allocator_id) const {
    // 🔑 私有方法，只有友元 BufferAllocatorBase 可以调用
    // 用于 Allocator 析构时查询所有属于它的 Pool
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<uint64_t> pool_ids;
    pool_ids.reserve(pools_.size());  // 预分配空间
    
    // 遍历所有 Pool，查找匹配的 allocator_id
    for (const auto& pair : pools_) {
        if (pair.second.allocator_id == allocator_id) {
            pool_ids.push_back(pair.first);
        }
    }
    
    return pool_ids;
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
        std::shared_ptr<BufferPool> pool = info.pool;  // v2.0: 直接使用 shared_ptr
        
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
        // v2.0: 直接使用 shared_ptr
        std::shared_ptr<BufferPool> pool = pair.second.pool;
        total += pool->getTotalCount() * pool->getBufferSize();
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
        // v2.0: 直接使用 shared_ptr
        std::shared_ptr<BufferPool> pool = pair.second.pool;
        stats.total_buffers += pool->getTotalCount();
        stats.total_free += pool->getFreeCount();
        stats.total_filled += pool->getFilledCount();
        stats.total_memory += pool->getTotalCount() * pool->getBufferSize();
    }
    
    return stats;
}
