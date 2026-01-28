#include "productionline/WorkerSyncCoordinator.hpp"
#include "buffer/bufferpool/Buffer.hpp"
#include <log4cplus/loggingmacros.h>

WorkerSyncCoordinator::WorkerSyncCoordinator(
    const std::vector<std::string>& worker_names,
    const CallbackChain& callback_chain
)
    : worker_names_(worker_names)
    , total_workers_(worker_names.size())
    , callback_chain_(callback_chain)
    , mutex_()
    , cv_()
    , frame_syncs_()
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.WorkerSyncCoordinator")))
{
    if (total_workers_ == 0) {
        LOG4CPLUS_WARN(logger_, "创建 WorkerSyncCoordinator 但 Worker 数量为 0");
    }
    
    if (callback_chain_.empty()) {
        LOG4CPLUS_DEBUG_FMT(logger_, 
            "创建 WorkerSyncCoordinator (%zu 个 Worker, 无回调 - 快速路径)", 
            total_workers_);
    } else {
        LOG4CPLUS_INFO_FMT(logger_, 
            "创建 WorkerSyncCoordinator (%zu 个 Worker, %zu 个回调)", 
            total_workers_, callback_chain_.size());
        
        // 打印回调链信息
        for (size_t i = 0; i < callback_chain_.size(); i++) {
            LOG4CPLUS_INFO_FMT(logger_, 
                "  回调 [%zu]: %s", 
                i, callback_chain_[i].name.c_str());
        }
    }
}

WorkerSyncCoordinator::~WorkerSyncCoordinator() {
    LOG4CPLUS_DEBUG(logger_, "析构 WorkerSyncCoordinator");
}

bool WorkerSyncCoordinator::arrive(
    const std::string& worker_name, 
    uint64_t frame_version, 
    Buffer* buffer
) {
    // ⭐ 快速路径：如果没有配置回调，直接返回（零开销）
    if (callback_chain_.empty()) {
        return true;
    }
    
    std::unique_lock<std::mutex> lock(mutex_);
    
    // 获取或创建当前版本的同步点
    FrameSync& sync = frame_syncs_[frame_version];
    
    // 防御性检查：防止重复到达
    if (sync.worker_buffers.find(worker_name) != sync.worker_buffers.end()) {
        LOG4CPLUS_ERROR_FMT(logger_, 
            "[Frame %llu] Worker '%s' 重复到达同步点", 
            (unsigned long long)frame_version, 
            worker_name.c_str());
        return false;
    }
    
    // 记录 Worker 的 Buffer
    sync.worker_buffers[worker_name] = buffer;
    sync.arrived_count++;
    
    LOG4CPLUS_DEBUG_FMT(logger_, 
        "[Frame %llu] Worker '%s' 到达同步点 (%zu/%zu)", 
        (unsigned long long)frame_version, 
        worker_name.c_str(),
        sync.arrived_count, 
        total_workers_);
    
    // 检查是否所有 Worker 都到达
    if (sync.arrived_count == total_workers_) {
        // ✅ 所有 Worker 都到达，执行回调链
        LOG4CPLUS_DEBUG_FMT(logger_, 
            "[Frame %llu] 所有 Worker 就绪，执行回调链 (%zu 个回调)", 
            (unsigned long long)frame_version,
            callback_chain_.size());
        
        bool result = executeCallbackChain(frame_version, sync.worker_buffers);
        
        sync.should_submit = result;
        sync.callback_executed = true;
        
        if (result) {
            LOG4CPLUS_DEBUG_FMT(logger_, 
                "[Frame %llu] 回调链执行成功，允许提交", 
                (unsigned long long)frame_version);
        } else {
            LOG4CPLUS_WARN_FMT(logger_, 
                "[Frame %llu] 回调链执行失败，拒绝提交", 
                (unsigned long long)frame_version);
        }
        
        // 唤醒所有等待的 Worker
        cv_.notify_all();
        
        // 清理旧版本数据（保留最近 10 帧）
        cleanupOldFrames(frame_version);
        
        return result;
        
    } else {
        // ⏳ 等待其他 Worker 到达
        LOG4CPLUS_DEBUG_FMT(logger_, 
            "[Frame %llu] Worker '%s' 等待其他 Worker...", 
            (unsigned long long)frame_version,
            worker_name.c_str());
        
        cv_.wait(lock, [&sync]() {
            return sync.callback_executed;
        });
        
        LOG4CPLUS_DEBUG_FMT(logger_, 
            "[Frame %llu] Worker '%s' 被唤醒，结果: %s", 
            (unsigned long long)frame_version,
            worker_name.c_str(),
            sync.should_submit ? "允许提交" : "拒绝提交");
        
        return sync.should_submit;
    }
}

bool WorkerSyncCoordinator::executeCallbackChain(
    uint64_t frame_version,
    const std::map<std::string, Buffer*>& worker_buffers
) {
    // 按顺序执行回调链
    for (size_t i = 0; i < callback_chain_.size(); i++) {
        const auto& item = callback_chain_[i];
        
        LOG4CPLUS_DEBUG_FMT(logger_, 
            "[Frame %llu] 执行回调 [%zu/%zu]: %s", 
            (unsigned long long)frame_version,
            i + 1,
            callback_chain_.size(),
            item.name.c_str());
        
        try {
            bool result = item.callback(frame_version, worker_buffers, item.context);
            
            if (!result) {
                // 回调返回 false，终止链
                LOG4CPLUS_WARN_FMT(logger_, 
                    "[Frame %llu] 回调 [%zu/%zu]: %s 返回 false，终止链", 
                    (unsigned long long)frame_version,
                    i + 1,
                    callback_chain_.size(),
                    item.name.c_str());
                return false;
            }
            
            LOG4CPLUS_DEBUG_FMT(logger_, 
                "[Frame %llu] 回调 [%zu/%zu]: %s 返回 true，继续", 
                (unsigned long long)frame_version,
                i + 1,
                callback_chain_.size(),
                item.name.c_str());
            
        } catch (const std::exception& e) {
            // 回调异常，视为失败
            LOG4CPLUS_ERROR_FMT(logger_, 
                "[Frame %llu] 回调 [%zu/%zu]: %s 异常: %s", 
                (unsigned long long)frame_version,
                i + 1,
                callback_chain_.size(),
                item.name.c_str(),
                e.what());
            return false;
        } catch (...) {
            // 未知异常
            LOG4CPLUS_ERROR_FMT(logger_, 
                "[Frame %llu] 回调 [%zu/%zu]: %s 未知异常", 
                (unsigned long long)frame_version,
                i + 1,
                callback_chain_.size(),
                item.name.c_str());
            return false;
        }
    }
    
    // 所有回调都通过
    LOG4CPLUS_DEBUG_FMT(logger_, 
        "[Frame %llu] 所有回调通过 (%zu 个)", 
        (unsigned long long)frame_version,
        callback_chain_.size());
    
    return true;
}

void WorkerSyncCoordinator::cleanupOldFrames(uint64_t current_version) {
    // 保留最近 10 帧，删除更早的帧数据
    const uint64_t KEEP_FRAMES = 10;
    
    if (current_version < KEEP_FRAMES) {
        return;  // 还没有足够的帧需要清理
    }
    
    uint64_t threshold = current_version - KEEP_FRAMES;
    
    auto it = frame_syncs_.begin();
    while (it != frame_syncs_.end()) {
        if (it->first < threshold) {
            LOG4CPLUS_DEBUG_FMT(logger_, 
                "清理旧帧数据: Frame %llu", 
                (unsigned long long)it->first);
            it = frame_syncs_.erase(it);
        } else {
            ++it;
        }
    }
}
