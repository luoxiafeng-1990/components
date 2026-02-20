#include "productionline/WorkerSyncCoordinator.hpp"
#include "productionline/io/BufferComparator.hpp"
#include "buffer/bufferpool/Buffer.hpp"
#include <log4cplus/loggingmacros.h>

// ============================================================
// CompareCallbackContext 成员方法实现
// ============================================================

void CompareCallbackContext::initFromCompareType(
    const WorkerConfig::ConsumerTypeConfig::CompareType& compare_config
) {
    enable_psnr = compare_config.enable_psnr;
    enable_ssim = compare_config.enable_ssim;
    min_psnr = compare_config.min_psnr;
    min_ssim = compare_config.min_ssim;
    verbose = compare_config.verbose;
}

double CompareCallbackContext::getAveragePsnr() const {
    int count = total_frames.load();
    return count > 0 ? psnr_sum.load() / count : 0.0;
}

double CompareCallbackContext::getAverageSsim() const {
    int count = total_frames.load();
    return count > 0 ? ssim_sum.load() / count : 0.0;
}

double CompareCallbackContext::getPassRate() const {
    int count = total_frames.load();
    return count > 0 ? 100.0 * passed_frames.load() / count : 0.0;
}

bool CompareCallbackContext::isPassed() const {
    return failed_frames.load() == 0;
}

CompareCallbackContext::~CompareCallbackContext() {
    closeComparator();
}

bool CompareCallbackContext::openComparator() {
    // 如果已经打开，直接返回
    if (comparator_opened_ && comparator_) {
        return true;
    }
    
    // 创建 comparator
    comparator_ = std::make_unique<productionline::io::BufferComparator>();
    
    // 配置
    productionline::io::CompareConfig config;
    config.enable_psnr = enable_psnr;
    config.enable_ssim = enable_ssim;
    config.min_psnr = min_psnr;
    config.min_ssim = min_ssim;
    config.verbose = verbose;
    config.save_report = false;
    
    // 打开
    comparator_opened_ = comparator_->open(config);
    return comparator_opened_;
}

void CompareCallbackContext::closeComparator() {
    if (comparator_ && comparator_opened_) {
        comparator_->close();
        comparator_opened_ = false;
    }
}

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
    Buffer* buffer,
    FillStatus status
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
    
    // 记录 Worker 的 Buffer 和结果状态
    sync.worker_buffers[worker_name] = buffer;
    sync.worker_results[worker_name] = status;
    sync.arrived_count++;
    
    LOG4CPLUS_DEBUG_FMT(logger_, 
        "[Frame %llu] Worker '%s' 到达同步点 (%zu/%zu), 状态: %s", 
        (unsigned long long)frame_version, 
        worker_name.c_str(),
        sync.arrived_count, 
        total_workers_,
        fillStatusToString(status));
    
    // 检查是否所有 Worker 都到达
    if (sync.arrived_count == total_workers_) {
        // ✅ 所有 Worker 都到达，分析状态并决定处理方式
        LOG4CPLUS_DEBUG_FMT(logger_, 
            "[Frame %llu] 所有 Worker 就绪，分析状态...", 
            (unsigned long long)frame_version);
        
        // 统计各状态的数量
        // v2.33 变更：使用 FillStatus 判断
        int success_count = 0;
        int eagain_count = 0;
        int error_count = 0;
        
        for (const auto& [name, s] : sync.worker_results) {
            if (s == FillStatus::Success) {
                success_count++;
            } else if (static_cast<int>(s) > 0) {
                // 正值都是可重试状态（NonVideoPacket, CodecEagain, PacketAlreadyProcessed）
                eagain_count++;
            } else {
                // 负值都是终止/错误状态（EndOfStream, 各种 Error）
                error_count++;
            }
        }
        
        // 根据状态组合决定处理方式
        if (success_count == static_cast<int>(total_workers_)) {
            // 场景 1：都成功 → 执行回调对比
            LOG4CPLUS_DEBUG_FMT(logger_, 
                "[Frame %llu] 所有 Worker 成功，执行回调链 (%zu 个回调)", 
                (unsigned long long)frame_version,
                callback_chain_.size());
            
            bool callback_result = executeCallbackChain(frame_version, sync.worker_buffers);
            sync.should_submit = callback_result;
            
            if (callback_result) {
                LOG4CPLUS_DEBUG_FMT(logger_, 
                    "[Frame %llu] 回调链执行成功，允许提交", 
                    (unsigned long long)frame_version);
            } else {
                LOG4CPLUS_WARN_FMT(logger_, 
                    "[Frame %llu] 回调链执行失败，拒绝提交", 
                    (unsigned long long)frame_version);
            }
        }
        else if (eagain_count == static_cast<int>(total_workers_)) {
            // 场景 2a：都 EAGAIN → 跳过回调，继续下一帧
            LOG4CPLUS_DEBUG_FMT(logger_, 
                "[Frame %llu] 所有 Worker 返回 EAGAIN，跳过当前帧", 
                (unsigned long long)frame_version);
            sync.should_submit = false;
        }
        else if (success_count > 0 && eagain_count > 0) {
            // 场景 3a：一个成功 + 一个 EAGAIN → ❌ 报错！不应该发生
            LOG4CPLUS_ERROR_FMT(logger_, 
                "[Frame %llu] ❌ EAGAIN 不一致！%d 个成功，%d 个 EAGAIN。"
                "无法进行帧同步对比，请检查解码器配置！", 
                (unsigned long long)frame_version,
                success_count, eagain_count);
            sync.should_submit = false;
            // TODO: 可以设置标志通知外部切换比较模式
        }
        else {
            // 场景 2b/3b：有 ERROR（无论是否有成功）→ 跳过回调，继续下一帧
            LOG4CPLUS_DEBUG_FMT(logger_, 
                "[Frame %llu] 有 Worker 返回错误 (成功:%d, EAGAIN:%d, ERROR:%d)，跳过当前帧", 
                (unsigned long long)frame_version,
                success_count, eagain_count, error_count);
            sync.should_submit = false;
        }
        
        sync.callback_executed = true;
        
        // 唤醒所有等待的 Worker
        cv_.notify_all();
        
        // 清理旧版本数据（保留最近 10 帧）
        cleanupOldFrames(frame_version);
        
        return sync.should_submit;
        
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

// ============================================================
// 静态工厂方法：创建默认比较回调
// ============================================================

CallbackChainItem WorkerSyncCoordinator::createDefaultCompareCallback(
    CompareCallbackContext* context
) {
    FrameSyncCallback callback = [](uint64_t frame_version, 
                                    const std::map<std::string, Buffer*>& worker_buffers,
                                    void* ctx) -> bool {
        auto* compare_ctx = static_cast<CompareCallbackContext*>(ctx);
        if (!compare_ctx) {
            return true;  // 无上下文，跳过比较
        }
        
        // 需要至少 2 个 Worker 的 Buffer 才能比较
        if (worker_buffers.size() < 2) {
            return true;
        }
        
        // 获取前两个 Buffer（参考 vs 测试）
        auto it = worker_buffers.begin();
        Buffer* reference_buffer = it->second;
        ++it;
        Buffer* test_buffer = it->second;
        
        if (!reference_buffer || !test_buffer) {
            return true;
        }
        
        // ⭐ v2.28 优化：使用已打开的 comparator，避免每帧重复创建
        // 如果 comparator 未打开，尝试打开（懒初始化）
        if (!compare_ctx->comparator_opened_) {
            if (!compare_ctx->openComparator()) {
                return true;  // 打开失败，不阻塞流程
            }
        }
        
        // 执行比较（直接使用已打开的 comparator）
        auto result = compare_ctx->comparator_->compare(reference_buffer, test_buffer);
        
        double psnr = result.psnr_avg;
        double ssim = result.ssim_avg;
        
        // 判断是否通过
        bool psnr_ok = !compare_ctx->enable_psnr || (psnr >= compare_ctx->min_psnr);
        bool ssim_ok = !compare_ctx->enable_ssim || (ssim >= compare_ctx->min_ssim);
        bool passed = psnr_ok && ssim_ok;
        
        // 更新统计（原子操作）
        compare_ctx->total_frames.fetch_add(1);
        if (passed) {
            compare_ctx->passed_frames.fetch_add(1);
        } else {
            compare_ctx->failed_frames.fetch_add(1);
        }
        
        // 累加 PSNR/SSIM（原子 CAS 操作）
        double old_psnr = compare_ctx->psnr_sum.load();
        while (!compare_ctx->psnr_sum.compare_exchange_weak(old_psnr, old_psnr + psnr)) {}
        
        double old_ssim = compare_ctx->ssim_sum.load();
        while (!compare_ctx->ssim_sum.compare_exchange_weak(old_ssim, old_ssim + ssim)) {}
        
        // 调用外部回调（如果有）
        if (compare_ctx->result_callback) {
            compare_ctx->result_callback(
                static_cast<int>(frame_version), psnr, ssim, passed);
        }
        
        // 日志（仅失败时或 verbose 模式）
        if (!passed || compare_ctx->verbose) {
            auto logger = log4cplus::Logger::getInstance(
                LOG4CPLUS_TEXT("components.WorkerSyncCoordinator"));
            if (!passed) {
                LOG4CPLUS_WARN_FMT(logger, 
                    "[Frame %llu] Compare FAILED: PSNR=%.2f (min=%.2f), SSIM=%.4f (min=%.4f)",
                    (unsigned long long)frame_version, 
                    psnr, compare_ctx->min_psnr,
                    ssim, compare_ctx->min_ssim);
            } else {
                LOG4CPLUS_DEBUG_FMT(logger, 
                    "[Frame %llu] Compare PASSED: PSNR=%.2f, SSIM=%.4f",
                    (unsigned long long)frame_version, psnr, ssim);
            }
        }
        
        return true;  // 比较结果不影响 Buffer 提交
    };
    
    return CallbackChainItem(callback, context, "default_compare_callback");
}
