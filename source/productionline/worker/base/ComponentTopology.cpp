#include "productionline/worker/base/ComponentTopology.hpp"

// ========== 单例 ==========

ComponentTopology& ComponentTopology::getInstance() {
    static ComponentTopology instance;
    return instance;
}

ComponentTopology::ComponentTopology()
    : next_line_id_(1)
    , next_group_id_(1)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Topology")))
{
}

// ==================== 注册 ====================

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

void ComponentTopology::linkWorkerToLine(uint64_t line_id, uint64_t worker_registry_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lines_.find(line_id);
    if (it == lines_.end()) {
        LOG4CPLUS_WARN_FMT(logger_, "[Topology] linkWorkerToLine: Line %lu not found", line_id);
        return;
    }
    it->second.worker_ids.insert(worker_registry_id);
    worker_to_line_[worker_registry_id] = line_id;
    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Linked Worker %lu -> Line %lu",
                        worker_registry_id, line_id);
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

void ComponentTopology::linkWorkerToGroup(uint64_t group_id, uint64_t worker_registry_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) {
        LOG4CPLUS_WARN_FMT(logger_, "[Topology] linkWorkerToGroup: Group %lu not found", group_id);
        return;
    }
    it->second.worker_ids.insert(worker_registry_id);
    worker_to_group_[worker_registry_id] = group_id;
    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Linked Worker %lu -> Group %lu",
                        worker_registry_id, group_id);
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

void ComponentTopology::linkPoolToWorker(uint64_t worker_registry_id, uint64_t pool_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    worker_pools_[worker_registry_id].insert(pool_id);
    pool_to_worker_[pool_id] = worker_registry_id;
    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Linked Pool %lu -> Worker %lu",
                        pool_id, worker_registry_id);
}

// ==================== 正向查询 ====================

std::vector<uint64_t> ComponentTopology::getWorkersOfLine(uint64_t line_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lines_.find(line_id);
    if (it == lines_.end()) return {};
    return {it->second.worker_ids.begin(), it->second.worker_ids.end()};
}

std::vector<uint64_t> ComponentTopology::getGroupsOfLine(uint64_t line_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lines_.find(line_id);
    if (it == lines_.end()) return {};
    return it->second.group_ids;
}

std::vector<uint64_t> ComponentTopology::getWorkersOfGroup(uint64_t group_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return {};
    return {it->second.worker_ids.begin(), it->second.worker_ids.end()};
}

std::vector<uint64_t> ComponentTopology::getProducerLinesOfGroup(uint64_t group_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return {};
    return it->second.producer_line_ids;
}

std::vector<uint64_t> ComponentTopology::getPoolsOfWorker(uint64_t worker_registry_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = worker_pools_.find(worker_registry_id);
    if (it == worker_pools_.end()) return {};
    return {it->second.begin(), it->second.end()};
}

// ==================== 反向查询 ====================

uint64_t ComponentTopology::getLineOfWorker(uint64_t worker_registry_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = worker_to_line_.find(worker_registry_id);
    return (it != worker_to_line_.end()) ? it->second : 0;
}

uint64_t ComponentTopology::getGroupOfWorker(uint64_t worker_registry_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = worker_to_group_.find(worker_registry_id);
    return (it != worker_to_group_.end()) ? it->second : 0;
}

uint64_t ComponentTopology::getLineOfGroup(uint64_t group_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = groups_.find(group_id);
    return (it != groups_.end()) ? it->second.parent_line_id : 0;
}

uint64_t ComponentTopology::getGroupOfProducerLine(uint64_t producer_line_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = producer_line_to_group_.find(producer_line_id);
    return (it != producer_line_to_group_.end()) ? it->second : 0;
}

// ==================== 注销 ====================

void ComponentTopology::unregisterLine(uint64_t line_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lines_.find(line_id);
    if (it == lines_.end()) return;

    // 清除直属 Worker 的反向索引
    for (uint64_t wid : it->second.worker_ids) {
        worker_to_line_.erase(wid);
        worker_pools_.erase(wid);
    }
    // 注意：不递归清除 Group，由调用者在 stop() 中自行 unregisterGroup
    lines_.erase(it);
    LOG4CPLUS_DEBUG_FMT(logger_, "[Topology] Unregistered Line %lu", line_id);
}

void ComponentTopology::unregisterGroup(uint64_t group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return;

    for (uint64_t wid : it->second.worker_ids) {
        worker_to_group_.erase(wid);
        worker_pools_.erase(wid);
    }
    for (uint64_t plid : it->second.producer_line_ids) {
        producer_line_to_group_.erase(plid);
    }
    // 从父 Line 的 group_ids 中移除
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

void ComponentTopology::unlinkWorker(uint64_t worker_registry_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 从 Line 直属中移除
    auto wl_it = worker_to_line_.find(worker_registry_id);
    if (wl_it != worker_to_line_.end()) {
        auto line_it = lines_.find(wl_it->second);
        if (line_it != lines_.end()) {
            line_it->second.worker_ids.erase(worker_registry_id);
        }
        worker_to_line_.erase(wl_it);
    }
    // 从 Group 中移除
    auto wg_it = worker_to_group_.find(worker_registry_id);
    if (wg_it != worker_to_group_.end()) {
        auto group_it = groups_.find(wg_it->second);
        if (group_it != groups_.end()) {
            group_it->second.worker_ids.erase(worker_registry_id);
        }
        worker_to_group_.erase(wg_it);
    }
    // 清除 Pool 关联
    auto wp_it = worker_pools_.find(worker_registry_id);
    if (wp_it != worker_pools_.end()) {
        for (uint64_t pid : wp_it->second) {
            pool_to_worker_.erase(pid);
        }
        worker_pools_.erase(wp_it);
    }
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

// ==================== 诊断 ====================

size_t ComponentTopology::getLineCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_.size();
}

size_t ComponentTopology::getGroupCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return groups_.size();
}

void ComponentTopology::printTopology() const {
    std::lock_guard<std::mutex> lock(mutex_);
    LOG4CPLUS_INFO(logger_, "========== Component Topology ==========");
    LOG4CPLUS_INFO_FMT(logger_, "Lines: %zu, Groups: %zu", lines_.size(), groups_.size());

    for (auto& [lid, line] : lines_) {
        LOG4CPLUS_INFO_FMT(logger_, "  Line[%lu] '%s'", lid, line.name.c_str());

        // 直属 Worker（简单模式）
        for (uint64_t wid : line.worker_ids) {
            LOG4CPLUS_INFO_FMT(logger_, "    Worker[%lu]", wid);
            auto pw = worker_pools_.find(wid);
            if (pw != worker_pools_.end()) {
                for (uint64_t pid : pw->second) {
                    LOG4CPLUS_INFO_FMT(logger_, "      Pool[%lu]", pid);
                }
            }
        }

        // Group（MultiWorker 模式）
        for (uint64_t gid : line.group_ids) {
            auto git = groups_.find(gid);
            if (git == groups_.end()) continue;
            auto& grp = git->second;
            LOG4CPLUS_INFO_FMT(logger_, "    Group[%lu] '%s'", gid, grp.name.c_str());

            for (uint64_t plid : grp.producer_line_ids) {
                LOG4CPLUS_INFO_FMT(logger_, "      ProducerLine[%lu]", plid);
            }
            for (uint64_t cwid : grp.worker_ids) {
                LOG4CPLUS_INFO_FMT(logger_, "      ConsumerWorker[%lu]", cwid);
                auto pw = worker_pools_.find(cwid);
                if (pw != worker_pools_.end()) {
                    for (uint64_t pid : pw->second) {
                        LOG4CPLUS_INFO_FMT(logger_, "        Pool[%lu]", pid);
                    }
                }
            }
        }
    }
    LOG4CPLUS_INFO(logger_, "=========================================");
}
