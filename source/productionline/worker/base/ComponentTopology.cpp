#include "productionline/worker/base/ComponentTopology.hpp"
#include "productionline/worker/base/WorkerBase.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"
#include <algorithm>

// ========== 单例 ==========

ComponentTopology& ComponentTopology::getInstance() {
    static ComponentTopology instance;
    return instance;
}

ComponentTopology::ComponentTopology()
    : next_line_id_(1)
    , next_group_id_(1)
    , next_worker_id_(1)
    , next_pool_id_(1)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Topology")))
{
}

// ==================== Worker 注册 ====================

uint64_t ComponentTopology::registerWorker(std::shared_ptr<WorkerBase> worker) {
    if (!worker) {
        LOG4CPLUS_WARN(logger_, "[Topology] Cannot register null Worker");
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = next_worker_id_++;

    WorkerInfo info;
    info.id = id;
    info.worker = worker;
    info.created_time = std::chrono::system_clock::now();
    workers_[id] = std::move(info);

    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Worker registered: '%s' (ID: %lu)",
                        worker->getWorkerType(), id);
    return id;
}

// ==================== Pool 注册 ====================

uint64_t ComponentTopology::registerPool(std::shared_ptr<BufferPool> pool, uint64_t allocator_id) {
    if (!pool) {
        LOG4CPLUS_WARN(logger_, "[Topology] Cannot register null BufferPool");
        return 0;
    }
    if (allocator_id == 0) {
        LOG4CPLUS_WARN(logger_, "[Topology] Invalid allocator_id (0)");
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const std::string& name = pool->getName();
    const std::string& category = pool->getCategory();

    if (pool_name_to_id_.find(name) != pool_name_to_id_.end()) {
        LOG4CPLUS_WARN_FMT(logger_, "[Topology] BufferPool name '%s' already exists, appending ID suffix",
                           name.c_str());
    }

    uint64_t id = next_pool_id_++;

    PoolInfo info;
    info.id = id;
    info.pool = pool;
    info.name = name;
    info.category = category;
    info.allocator_id = allocator_id;
    info.created_time = std::chrono::system_clock::now();

    pools_[id] = std::move(info);
    pool_name_to_id_[name] = id;

    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Pool registered: '%s' (ID: %lu, Allocator: %lu, Category: %s)",
                        name.c_str(), id, allocator_id, category.empty() ? "None" : category.c_str());
    return id;
}

std::weak_ptr<BufferPool> ComponentTopology::getPool(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pools_.find(id);
    if (it == pools_.end()) {
        return std::weak_ptr<BufferPool>();
    }
    return it->second.pool;
}

// ==================== Line / Group 注册 ====================

uint64_t ComponentTopology::registerLine(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = next_line_id_++;
    LineInfo info;
    info.id = id;
    info.name = name.empty() ? ("Line-" + std::to_string(id)) : name;
    lines_[id] = std::move(info);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Registered Line: id=%lu, name='%s'",
                        id, lines_[id].name.c_str());
    return id;
}

uint64_t ComponentTopology::registerGroup(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = next_group_id_++;
    GroupInfo info;
    info.id = id;
    info.name = name.empty() ? ("Group-" + std::to_string(id)) : name;
    groups_[id] = std::move(info);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Registered Group: id=%lu, name='%s'",
                        id, groups_[id].name.c_str());
    return id;
}

// ==================== 关联 ====================

void ComponentTopology::linkWorkerToLine(uint64_t line_id, uint64_t worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lines_.find(line_id);
    if (it == lines_.end()) {
        LOG4CPLUS_WARN_FMT(logger_, "[Topology] linkWorkerToLine: Line %lu not found", line_id);
        return;
    }
    it->second.worker_ids.insert(worker_id);
    worker_to_line_[worker_id] = line_id;
    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Linked Worker %lu -> Line %lu", worker_id, line_id);
}

void ComponentTopology::linkGroupToLine(uint64_t line_id, uint64_t group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto line_it = lines_.find(line_id);
    if (line_it == lines_.end()) {
        LOG4CPLUS_WARN_FMT(logger_, "[Topology] linkGroupToLine: Line %lu not found", line_id);
        return;
    }
    auto group_it = groups_.find(group_id);
    if (group_it == groups_.end()) {
        LOG4CPLUS_WARN_FMT(logger_, "[Topology] linkGroupToLine: Group %lu not found", group_id);
        return;
    }
    line_it->second.group_ids.push_back(group_id);
    group_it->second.parent_line_id = line_id;
    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Linked Group %lu -> Line %lu", group_id, line_id);
}

void ComponentTopology::linkWorkerToGroup(uint64_t group_id, uint64_t worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) {
        LOG4CPLUS_WARN_FMT(logger_, "[Topology] linkWorkerToGroup: Group %lu not found", group_id);
        return;
    }
    it->second.worker_ids.insert(worker_id);
    worker_to_group_[worker_id] = group_id;
    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Linked Worker %lu -> Group %lu", worker_id, group_id);
}

void ComponentTopology::linkProducerLineToGroup(uint64_t group_id, uint64_t producer_line_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) {
        LOG4CPLUS_WARN_FMT(logger_, "[Topology] linkProducerLineToGroup: Group %lu not found", group_id);
        return;
    }
    it->second.producer_line_ids.push_back(producer_line_id);
    producer_line_to_group_[producer_line_id] = group_id;
    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Linked ProducerLine %lu -> Group %lu",
                        producer_line_id, group_id);
}

void ComponentTopology::linkPoolToWorker(uint64_t worker_id, uint64_t pool_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    worker_pools_[worker_id].insert(pool_id);
    pool_to_worker_[pool_id] = worker_id;
    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Linked Pool %lu -> Worker %lu", pool_id, worker_id);
}

void ComponentTopology::unlinkPool(uint64_t pool_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pool_to_worker_.find(pool_id);
    if (it != pool_to_worker_.end()) {
        auto wp_it = worker_pools_.find(it->second);
        if (wp_it != worker_pools_.end()) {
            wp_it->second.erase(pool_id);
        }
        pool_to_worker_.erase(it);
    }
}

// ==================== 注销 ====================

void ComponentTopology::unregisterLine(uint64_t line_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lines_.find(line_id);
    if (it == lines_.end()) return;

    for (uint64_t wid : it->second.worker_ids) {
        worker_to_line_.erase(wid);
        // 同步清除 Worker 关联的 Pool 拓扑
        auto wp_it = worker_pools_.find(wid);
        if (wp_it != worker_pools_.end()) {
            for (uint64_t pid : wp_it->second) {
                pool_to_worker_.erase(pid);
            }
            worker_pools_.erase(wp_it);
        }
    }
    lines_.erase(it);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Unregistered Line %lu", line_id);
}

void ComponentTopology::unregisterGroup(uint64_t group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return;

    for (uint64_t wid : it->second.worker_ids) {
        worker_to_group_.erase(wid);
        auto wp_it = worker_pools_.find(wid);
        if (wp_it != worker_pools_.end()) {
            for (uint64_t pid : wp_it->second) {
                pool_to_worker_.erase(pid);
            }
            worker_pools_.erase(wp_it);
        }
    }
    for (uint64_t plid : it->second.producer_line_ids) {
        producer_line_to_group_.erase(plid);
    }
    uint64_t parent = it->second.parent_line_id;
    if (parent != 0) {
        auto line_it = lines_.find(parent);
        if (line_it != lines_.end()) {
            auto& gids = line_it->second.group_ids;
            gids.erase(std::remove(gids.begin(), gids.end(), group_id), gids.end());
        }
    }
    groups_.erase(it);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Unregistered Group %lu", group_id);
}

// ==================== Pool 管理（友元方法）====================

std::shared_ptr<BufferPool> ComponentTopology::getPoolSpecialForAllocator(uint64_t pool_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pools_.find(pool_id);
    if (it == pools_.end()) {
        return nullptr;
    }
    return it->second.pool;
}

std::vector<uint64_t> ComponentTopology::getPoolsByAllocator(uint64_t allocator_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint64_t> pool_ids;
    pool_ids.reserve(pools_.size());
    for (const auto& pair : pools_) {
        if (pair.second.allocator_id == allocator_id) {
            pool_ids.push_back(pair.first);
        }
    }
    return pool_ids;
}

void ComponentTopology::unregisterPool(uint64_t pool_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pools_.find(pool_id);
    if (it == pools_.end()) {
        LOG4CPLUS_WARN_FMT(logger_, "[Topology] Trying to unregister non-existent Pool (ID: %lu)", pool_id);
        return;
    }

    const std::string name = it->second.name;

    // 清除拓扑关联
    auto pw_it = pool_to_worker_.find(pool_id);
    if (pw_it != pool_to_worker_.end()) {
        auto wp_it = worker_pools_.find(pw_it->second);
        if (wp_it != worker_pools_.end()) {
            wp_it->second.erase(pool_id);
        }
        pool_to_worker_.erase(pw_it);
    }

    pool_name_to_id_.erase(name);
    pools_.erase(it);

    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Pool unregistered: '%s' (ID: %lu)", name.c_str(), pool_id);
}

// ==================== 诊断 ====================

void ComponentTopology::printTopology() const {
    std::lock_guard<std::mutex> lock(mutex_);
    LOG4CPLUS_INFO(logger_, "========== Component Topology ==========");
    LOG4CPLUS_INFO_FMT(logger_, "Lines: %zu, Groups: %zu, Workers: %zu, Pools: %zu",
                       lines_.size(), groups_.size(), workers_.size(), pools_.size());

    for (const auto& [lid, line] : lines_) {
        LOG4CPLUS_INFO_FMT(logger_, "  Line[%lu] '%s'", lid, line.name.c_str());

        for (uint64_t wid : line.worker_ids) {
            auto w_it = workers_.find(wid);
            const char* wtype = (w_it != workers_.end() && w_it->second.worker)
                                    ? w_it->second.worker->getWorkerType() : "?";
            LOG4CPLUS_INFO_FMT(logger_, "    Worker[%lu] type='%s'", wid, wtype);
            auto pw = worker_pools_.find(wid);
            if (pw != worker_pools_.end()) {
                for (uint64_t pid : pw->second) {
                    auto p_it = pools_.find(pid);
                    const char* pname = (p_it != pools_.end()) ? p_it->second.name.c_str() : "?";
                    LOG4CPLUS_INFO_FMT(logger_, "      Pool[%lu] '%s'", pid, pname);
                }
            }
        }

        for (uint64_t gid : line.group_ids) {
            auto git = groups_.find(gid);
            if (git == groups_.end()) continue;
            const auto& grp = git->second;
            LOG4CPLUS_INFO_FMT(logger_, "    Group[%lu] '%s'", gid, grp.name.c_str());

            for (uint64_t plid : grp.producer_line_ids) {
                LOG4CPLUS_INFO_FMT(logger_, "      ProducerLine[%lu]", plid);
            }
            for (uint64_t cwid : grp.worker_ids) {
                auto w_it = workers_.find(cwid);
                const char* wtype = (w_it != workers_.end() && w_it->second.worker)
                                        ? w_it->second.worker->getWorkerType() : "?";
                LOG4CPLUS_INFO_FMT(logger_, "      ConsumerWorker[%lu] type='%s'", cwid, wtype);
                auto pw = worker_pools_.find(cwid);
                if (pw != worker_pools_.end()) {
                    for (uint64_t pid : pw->second) {
                        auto p_it = pools_.find(pid);
                        const char* pname = (p_it != pools_.end()) ? p_it->second.name.c_str() : "?";
                        LOG4CPLUS_INFO_FMT(logger_, "        Pool[%lu] '%s'", pid, pname);
                    }
                }
            }
        }
    }
    LOG4CPLUS_INFO(logger_, "=========================================");
}
