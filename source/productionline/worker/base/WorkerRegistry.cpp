#include "productionline/worker/base/WorkerRegistry.hpp"
#include "productionline/worker/base/WorkerBase.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>

// ========== 单例实例 ==========

WorkerRegistry& WorkerRegistry::getInstance() {
    static WorkerRegistry instance;
    return instance;
}

// ========== 注册管理接口实现 ==========

uint64_t WorkerRegistry::registerWorker(std::shared_ptr<WorkerBase> worker) {
    if (!worker) {
        LOG4CPLUS_WARN(logger_, "[WorkerRegistry] Cannot register null Worker");
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t id = next_id_++;

    WorkerInfo info;
    info.id = id;
    info.worker = worker;
    info.created_time = std::chrono::system_clock::now();

    workers_[id] = info;

    LOG4CPLUS_DEBUG_FMT(logger_, "[WorkerRegistry] Worker registered: '%s' (ID: %lu)",
           worker->getWorkerType(), id);

    return id;
}

void WorkerRegistry::unregisterWorker(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = workers_.find(id);
    if (it == workers_.end()) {
        LOG4CPLUS_WARN_FMT(logger_, "[WorkerRegistry] Trying to unregister non-existent Worker (ID: %lu)", id);
        return;
    }

    const char* type = it->second.worker ? it->second.worker->getWorkerType() : "Unknown";
    workers_.erase(it);

    LOG4CPLUS_DEBUG_FMT(logger_, "[WorkerRegistry] Worker unregistered: '%s' (ID: %lu)", type, id);
}

// ========== 查询接口实现 ==========

std::weak_ptr<WorkerBase> WorkerRegistry::getWorker(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = workers_.find(id);
    if (it == workers_.end()) {
        return std::weak_ptr<WorkerBase>();
    }

    return it->second.worker;
}

size_t WorkerRegistry::getWorkerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return workers_.size();
}

std::vector<uint64_t> WorkerRegistry::getAllWorkerIds() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<uint64_t> ids;
    ids.reserve(workers_.size());
    for (const auto& pair : workers_) {
        ids.push_back(pair.first);
    }
    return ids;
}

// ========== 监控接口实现 ==========

void WorkerRegistry::printAllStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    LOG4CPLUS_INFO(logger_, "========================================");
    LOG4CPLUS_INFO(logger_, "Global Worker Statistics");
    LOG4CPLUS_INFO(logger_, "========================================");
    LOG4CPLUS_INFO_FMT(logger_, "Total Workers: %zu", workers_.size());

    if (workers_.empty()) {
        LOG4CPLUS_INFO(logger_, "   (No Workers registered)");
        LOG4CPLUS_INFO(logger_, "========================================");
        return;
    }

    std::vector<uint64_t> ids;
    ids.reserve(workers_.size());
    for (const auto& pair : workers_) {
        ids.push_back(pair.first);
    }
    std::sort(ids.begin(), ids.end());

    for (uint64_t id : ids) {
        const WorkerInfo& info = workers_.at(id);
        auto worker = info.worker;

        auto time_t_val = std::chrono::system_clock::to_time_t(info.created_time);
        char time_buf[100];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",
                      std::localtime(&time_t_val));

        LOG4CPLUS_INFO_FMT(logger_, "[Worker %lu] Type: %s",
               info.id, worker ? worker->getWorkerType() : "Unknown");

        if (worker) {
            LOG4CPLUS_INFO_FMT(logger_, "   Output: %dx%d, Open: %s",
                   worker->getOutputWidth(), worker->getOutputHeight(),
                   worker->isOpen() ? "Yes" : "No");
            LOG4CPLUS_INFO_FMT(logger_, "   Path: %s",
                   worker->getPath().c_str());
        }

        LOG4CPLUS_INFO_FMT(logger_, "   Created: %s", time_buf);
    }

    LOG4CPLUS_INFO(logger_, "========================================");
}
