#include "productionline/line/WorkerSyncCoordinator.hpp"
#include "consumptionline/BufferComparator.hpp"
#include "buffer/bufferpool/Buffer.hpp"
#include <log4cplus/loggingmacros.h>
#include <set>
#include <chrono>

// ============================================================
// CompareCallbackContext 成员方法实现
// ============================================================

void CompareCallbackContext::initFromCompareType(
    const WorkerConfig::ConsumerTypeConfig::CompareType& compare_config
) {
    compare_config_snapshot_ = compare_config;
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
    comparator_ = std::make_unique<consumptionline::io::BufferComparator>();
    
    // 配置：使用完整快照，避免 strategy/enable_parallel 等回落到错误组合导致 avg 未写入
    consumptionline::io::CompareConfig config = compare_config_snapshot_;
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

// ============================================================
// OpenCVCallbackContext 成员方法实现
// ============================================================

OpenCVCallbackContext::~OpenCVCallbackContext() {
    closeComparator();
}

bool OpenCVCallbackContext::openComparator() {
    // 如果已经打开，直接返回
    if (comparator_opened_ && comparator_) {
        return true;
    }

    // 创建 comparator
    comparator_ = std::make_unique<consumptionline::io::BufferComparator>();

    // 配置：使用 compare_config_snapshot_
    consumptionline::io::CompareConfig compare_config = compare_config_snapshot_;

    // 打开
    comparator_opened_ = comparator_->open(compare_config);
    return comparator_opened_;
}

void OpenCVCallbackContext::closeComparator() {
    if (comparator_ && comparator_opened_) {
        comparator_->close();
        comparator_opened_ = false;
    }
}

double OpenCVCallbackContext::getAveragePsnr() const {
    int count = total_frames.load();
    return count > 0 ? psnr_sum.load() / count : 0.0;
}

double OpenCVCallbackContext::getAverageSsim() const {
    int count = total_frames.load();
    return count > 0 ? ssim_sum.load() / count : 0.0;
}

double OpenCVCallbackContext::getPassRate() const {
    int count = total_frames.load();
    return count > 0 ? 100.0 * passed_frames.load() / count : 0.0;
}

bool OpenCVCallbackContext::isPassed() const {
    return failed_frames.load() == 0;
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

void WorkerSyncCoordinator::removeWorker(const std::string& worker_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (total_workers_ == 0) return;

    LOG4CPLUS_INFO_FMT(logger_,
        "removeWorker '%s': total_workers %zu -> %zu",
        worker_name.c_str(), total_workers_, total_workers_ - 1);

    total_workers_--;
    // 唤醒可能正在 cv_.wait_for 等待该 worker 的其他 worker
    cv_.notify_all();
}

bool WorkerSyncCoordinator::arrive(
    const std::string& worker_name, 
    uint64_t frame_version, 
    Buffer* buffer,
    const FillResult& result
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
    sync.worker_results.insert_or_assign(worker_name, result);
    sync.arrived_count++;
    
    LOG4CPLUS_DEBUG_FMT(logger_, 
        "[Frame %llu] Worker '%s' 到达同步点 (%zu/%zu), 状态: %s", 
        (unsigned long long)frame_version, 
        worker_name.c_str(),
        sync.arrived_count, 
        total_workers_,
        result.statusString());
    
    // 检查是否所有 Worker 都到达
    if (sync.arrived_count == total_workers_) {
        // ✅ 所有 Worker 都到达，分析状态并决定处理方式
        LOG4CPLUS_DEBUG_FMT(logger_, 
            "[Frame %llu] 所有 Worker 就绪，分析状态...", 
            (unsigned long long)frame_version);
        
        // 统计各状态的数量
        // v2.34 变更：使用 FillResult 判断
        // v2.36 变更：分离 eof_count / error_count，EOF 是正常结束而非错误
        int success_count = 0;
        int eagain_count = 0;
        int eof_count   = 0;
        int error_count = 0;
        
        for (const auto& [name, r] : sync.worker_results) {
            if (r.ok()) {
                success_count++;
            } else if (r.shouldContinue() || r.shouldRetry()) {
                eagain_count++;
            } else if (r.isEoFlush() || r.isAcquireEof()) {
                // 干净退出信号，不计入 error_count：
                // - isEoFlush()：codec flush pipeline 清空
                // - isAcquireEof()：数据获取层报告数据源耗尽（消费者此时 isAtEnd() 同步为 true）
                eof_count++;
            } else {
                error_count++;
            }
        }
        
        // 根据状态组合决定处理方式
        if (success_count == static_cast<int>(total_workers_)) {
            // 场景 1：都成功
            // 先尝试消化 pending 缓存：把当前各 worker 的帧也加入匹配池
            tryMatchPending(sync.worker_buffers);

            // 检查当前帧是否所有 worker 的 PTS 一致（无 B 帧偏差）
            std::set<int64_t> pts_set;
            for (const auto& [name, buf] : sync.worker_buffers) {
                if (buf) pts_set.insert(buf->getPts());
            }

            if (pts_set.size() == 1) {
                // PTS 一致 → 直接执行回调对比
                LOG4CPLUS_DEBUG_FMT(logger_,
                    "[Frame %llu] 所有 Worker 成功且 PTS 一致 (PTS=%lld)，执行回调链",
                    (unsigned long long)frame_version, (long long)*pts_set.begin());

                bool callback_result = executeCallbackChain(frame_version, sync.worker_buffers);
                sync.should_submit = callback_result;
            } else {
                // PTS 不一致（B 帧 reorder 导致）→ 各 worker 的帧分别缓存
                for (const auto& [name, buf] : sync.worker_buffers) {
                    if (buf && buf->getAVFrame()) {
                        AVFrame* cloned = av_frame_clone(buf->getAVFrame());
                        if (cloned) {
                            pending_frames_.push_back({name, buf->getPts(), cloned});
                            LOG4CPLUS_DEBUG_FMT(logger_,
                                "[Frame %llu] PTS 不一致，缓存 Worker '%s' (PTS=%lld) 的深拷贝帧",
                                (unsigned long long)frame_version, name.c_str(), (long long)buf->getPts());
                        }
                    }
                }
                // 再次尝试匹配
                tryMatchPending(sync.worker_buffers);
                sync.should_submit = false;
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
            // 场景 3a：部分成功 + 部分 EAGAIN（B 帧 reorder）
            // 将成功 worker 的帧做深拷贝缓存到 pending
            for (const auto& [name, r] : sync.worker_results) {
                if (r.ok() && sync.worker_buffers.count(name)) {
                    Buffer* buf = sync.worker_buffers.at(name);
                    if (buf && buf->getAVFrame()) {
                        AVFrame* cloned = av_frame_clone(buf->getAVFrame());
                        if (cloned) {
                            pending_frames_.push_back({name, buf->getPts(), cloned});
                            LOG4CPLUS_DEBUG_FMT(logger_,
                                "[Frame %llu] EAGAIN 不一致，缓存 Worker '%s' (PTS=%lld) 的深拷贝帧",
                                (unsigned long long)frame_version, name.c_str(), (long long)buf->getPts());
                        }
                    }
                }
            }
            sync.should_submit = false;
        }
        else if (eof_count > 0) {
            // 场景 4：有 Worker 正常 EOF → 数据源结束
            LOG4CPLUS_DEBUG_FMT(logger_,
                "[Frame %llu] 有 Worker EOF (成功:%d, EAGAIN:%d, EOF:%d, 错误:%d)，跳过当前帧",
                (unsigned long long)frame_version,
                success_count, eagain_count, eof_count, error_count);
            clearPendingFrames();
            sync.should_submit = false;
        }
        else {
            // 场景 2b/3b：有不可恢复 ERROR → 跳过回调，清空 pending
            LOG4CPLUS_WARN_FMT(logger_, 
                "[Frame %llu] 有 Worker 返回错误 (成功:%d, EAGAIN:%d, EOF:%d, 错误:%d)，跳过当前帧", 
                (unsigned long long)frame_version,
                success_count, eagain_count, eof_count, error_count);
            clearPendingFrames();
            sync.should_submit = false;
        }
        
        sync.callback_executed = true;
        
        // 唤醒所有等待的 Worker
        cv_.notify_all();
        
        // 清理旧版本数据（保留最近 10 帧）
        cleanupOldFrames(frame_version);
        
        return sync.should_submit;
        
    } else {
        // ⏳ 等待其他 Worker 到达（带超时防死锁）
        LOG4CPLUS_DEBUG_FMT(logger_, 
            "[Frame %llu] Worker '%s' 等待其他 Worker...", 
            (unsigned long long)frame_version,
            worker_name.c_str());
        
        // v2.38: 增加检查 arrived_count >= total_workers_，
        // 以便 removeWorker() 减少 total_workers_ 后能立即唤醒
        bool waited_ok = cv_.wait_for(lock, std::chrono::seconds(5), [&sync, this]() {
            return sync.callback_executed || sync.arrived_count >= total_workers_;
        });

        // 如果因 total_workers_ 减少而满足条件但 callback 未执行，
        // 则由当前 worker 负责执行回调逻辑
        if (waited_ok && !sync.callback_executed && sync.arrived_count >= total_workers_) {
            LOG4CPLUS_INFO_FMT(logger_,
                "[Frame %llu] Worker '%s' 因其他 Worker 退出而成为最后到达者，执行回调",
                (unsigned long long)frame_version,
                worker_name.c_str());
            // 仅此 worker 存活，跳过比较
            sync.should_submit = true;
            sync.callback_executed = true;
            cv_.notify_all();
            cleanupOldFrames(frame_version);
            return sync.should_submit;
        }

        if (!waited_ok) {
            LOG4CPLUS_WARN_FMT(logger_,
                "[Frame %llu] Worker '%s' 等待超时 (5s)，其他 Worker 可能已终止，跳过此帧",
                (unsigned long long)frame_version,
                worker_name.c_str());
            clearPendingFrames();
            return false;
        }
        
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
// B 帧 reorder 容错：深拷贝 PTS 匹配
// ============================================================

void WorkerSyncCoordinator::clearPendingFrames() {
    for (auto& pf : pending_frames_) {
        if (pf.frame) {
            av_frame_free(&pf.frame);
        }
    }
    pending_frames_.clear();
}

void WorkerSyncCoordinator::tryMatchPending(
    const std::map<std::string, Buffer*>& /* current_buffers */
) {
    if (pending_frames_.empty() || total_workers_ < 2) return;

    // 按 PTS 分组：pts → { worker_name → index_in_pending }
    std::map<int64_t, std::map<std::string, size_t>> pts_groups;
    for (size_t i = 0; i < pending_frames_.size(); i++) {
        pts_groups[pending_frames_[i].pts][pending_frames_[i].worker_name] = i;
    }

    std::set<size_t> consumed_indices;
    for (auto& [pts, wmap] : pts_groups) {
        if (wmap.size() < total_workers_) continue;

        auto it = wmap.begin();
        size_t idx_a = it->second; ++it;
        size_t idx_b = it->second;

        AVFrame* frame_a = pending_frames_[idx_a].frame;
        AVFrame* frame_b = pending_frames_[idx_b].frame;

        if (frame_a && frame_b && !callback_chain_.empty()) {
            for (const auto& item : callback_chain_) {
                auto* cmp_ctx = static_cast<CompareCallbackContext*>(item.context);
                if (!cmp_ctx) continue;

                if (!cmp_ctx->comparator_opened_) {
                    cmp_ctx->openComparator();
                }
                if (!cmp_ctx->comparator_opened_ || !cmp_ctx->comparator_) continue;

                auto result = cmp_ctx->comparator_->compareAVFrames(frame_a, frame_b);
                double psnr = result.psnr_avg;
                double ssim = result.ssim_avg;

                bool psnr_ok = !cmp_ctx->enable_psnr || (psnr >= cmp_ctx->min_psnr);
                bool ssim_ok = !cmp_ctx->enable_ssim || (ssim >= cmp_ctx->min_ssim);
                bool passed = psnr_ok && ssim_ok;

                cmp_ctx->total_frames.fetch_add(1);
                if (passed) cmp_ctx->passed_frames.fetch_add(1);
                else cmp_ctx->failed_frames.fetch_add(1);

                double old_psnr = cmp_ctx->psnr_sum.load();
                while (!cmp_ctx->psnr_sum.compare_exchange_weak(old_psnr, old_psnr + psnr)) {}
                double old_ssim = cmp_ctx->ssim_sum.load();
                while (!cmp_ctx->ssim_sum.compare_exchange_weak(old_ssim, old_ssim + ssim)) {}

                if (cmp_ctx->result_callback) {
                    cmp_ctx->result_callback(static_cast<int>(pts), psnr, ssim, passed);
                }

                if (!passed) {
                    LOG4CPLUS_WARN_FMT(logger_,
                        "[PTS %lld] PTS-match FAILED: PSNR=%.2f SSIM=%.4f",
                        (long long)pts, psnr, ssim);
                } else {
                    LOG4CPLUS_DEBUG_FMT(logger_,
                        "[PTS %lld] PTS-match PASSED: PSNR=%.2f SSIM=%.4f",
                        (long long)pts, psnr, ssim);
                }
            }
        }

        for (auto& [name, idx] : wmap) {
            consumed_indices.insert(idx);
        }
    }

    if (!consumed_indices.empty()) {
        std::vector<PendingFrame> remaining;
        for (size_t i = 0; i < pending_frames_.size(); i++) {
            if (consumed_indices.count(i)) {
                av_frame_free(&pending_frames_[i].frame);
            } else {
                remaining.push_back(pending_frames_[i]);
            }
        }
        pending_frames_ = std::move(remaining);

        if (pending_frames_.size() > 64) {
            LOG4CPLUS_WARN_FMT(logger_,
                "pending_frames_ 超过 64 帧未匹配，清空");
            clearPendingFrames();
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

// ============================================================
// 静态工厂方法：创建 OpenCV 算术运算回调
// ============================================================

CallbackChainItem WorkerSyncCoordinator::createOpenCVCallback(
    OpenCVCallbackContext* context
) {
    FrameSyncCallback callback = [](uint64_t frame_version,
                                    const std::map<std::string, Buffer*>& worker_buffers,
                                    void* ctx) -> bool {
        auto* opencv_ctx = static_cast<OpenCVCallbackContext*>(ctx);
        if (!opencv_ctx) {
            return true;  // 无上下文，跳过处理
        }

        // 需要至少 2 个 Worker 的 Buffer 才能进行算术运算
        if (worker_buffers.size() < 2) {
            return true;
        }

        // 获取前两个 Buffer
        auto it = worker_buffers.begin();
        Buffer* buffer1 = it->second;
        ++it;
        Buffer* buffer2 = it->second;

        if (!buffer1 || !buffer2) {
            return true;
        }

        auto logger = log4cplus::Logger::getInstance(
            LOG4CPLUS_TEXT("components.WorkerSyncCoordinator"));

        try {
            // 将 Buffer 转换为 cv::Mat
            // 优先使用 getMat()，否则使用 getAVFrame()
            cv::Mat mat1, mat2;

            // 转换 buffer1
            if (buffer1->getMat()) {
                mat1 = *buffer1->getMat();
            } else if (buffer1->getAVFrame()) {
                mat1 = cv::Mat(buffer1->getAVFrame());
            } else {
                LOG4CPLUS_WARN_FMT(logger,
                    "[Frame %llu] Buffer1 无法转换为 Mat",
                    (unsigned long long)frame_version);
                return true;
            }

            // 转换 buffer2
            if (buffer2->getMat()) {
                mat2 = *buffer2->getMat();
            } else if (buffer2->getAVFrame()) {
                mat2 = cv::Mat(buffer2->getAVFrame());
            } else {
                LOG4CPLUS_WARN_FMT(logger,
                    "[Frame %llu] Buffer2 无法转换为 Mat",
                    (unsigned long long)frame_version);
                return true;
            }

            // 检查尺寸是否匹配
            if (mat1.size() != mat2.size()) {
                LOG4CPLUS_WARN_FMT(logger,
                    "[Frame %llu] Mat 尺寸不匹配: (%d,%d) vs (%d,%d)",
                    (unsigned long long)frame_version,
                    mat1.cols, mat1.rows, mat2.cols, mat2.rows);
                return true;
            }

            // 执行 OpenCV 算术运算
            cv::Mat result;
            std::string op_name;

            switch (opencv_ctx->config.op_type) {
                case WorkerConfig::ConsumerTypeConfig::OpencvType::OpType::ADD: {
                    cv::add(mat1, mat2, result);
                    op_name = "ADD";
                    break;
                }
                case WorkerConfig::ConsumerTypeConfig::OpencvType::OpType::ABSDIFF: {
                    cv::absdiff(mat1, mat2, result);
                    op_name = "ABSDIFF";
                    break;
                }
                case WorkerConfig::ConsumerTypeConfig::OpencvType::OpType::ADD_WEIGHTED: {
                    // addWeighted(src1, alpha, src2, beta, gamma, dst)
                    // 默认参数：alpha=0.5, beta=0.5, gamma=0
                    double alpha = 0.5, beta = 0.5, gamma = 0.0;
                    cv::addWeighted(mat1, alpha, mat2, beta, gamma, result);
                    op_name = "ADD_WEIGHTED";
                    break;
                }
                case WorkerConfig::ConsumerTypeConfig::OpencvType::OpType::BITWISE_AND: {
                    cv::bitwise_and(mat1, mat2, result);
                    op_name = "BITWISE_AND";
                    break;
                }
                case WorkerConfig::ConsumerTypeConfig::OpencvType::OpType::BITWISE_OR: {
                    cv::bitwise_or(mat1, mat2, result);
                    op_name = "BITWISE_OR";
                    break;
                }
                case WorkerConfig::ConsumerTypeConfig::OpencvType::OpType::BITWISE_XOR: {
                    cv::bitwise_xor(mat1, mat2, result);
                    op_name = "BITWISE_XOR";
                    break;
                }
                case WorkerConfig::ConsumerTypeConfig::OpencvType::OpType::BITWISE_NOT: {
                    // bitwise_not 是单操作数，只使用 mat1
                    cv::bitwise_not(mat1, result);
                    op_name = "BITWISE_NOT";
                    break;
                }
                default: {
                    LOG4CPLUS_WARN_FMT(logger,
                        "[Frame %llu] OpenCV 操作类型 %d 未支持",
                        (unsigned long long)frame_version,
                        static_cast<int>(opencv_ctx->config.op_type));
                    return true;
                }
            }

            // 统计帧数
            opencv_ctx->total_frames.fetch_add(1);

            // 如果启用了 PSNR/SSIM 计算，使用原始 buffer 进行对比
            if (opencv_ctx->compare_config_snapshot_.enable_psnr || opencv_ctx->compare_config_snapshot_.enable_ssim) {
                // 打开 comparator（懒初始化）
                if (!opencv_ctx->comparator_opened_) {
                    if (!opencv_ctx->openComparator()) {
                        LOG4CPLUS_WARN_FMT(logger,
                            "[Frame %llu] 打开 BufferComparator 失败",
                            (unsigned long long)frame_version);
                        return true;
                    }
                }

                // 对两个原始 Buffer 进行质量对比
                auto cmp_result = opencv_ctx->comparator_->compare(buffer1, buffer2);

                double psnr = cmp_result.psnr_avg;
                double ssim = cmp_result.ssim_avg;

                // 判断是否通过
                bool psnr_ok = !opencv_ctx->compare_config_snapshot_.enable_psnr ||
                              (psnr >= opencv_ctx->compare_config_snapshot_.min_psnr);
                bool ssim_ok = !opencv_ctx->compare_config_snapshot_.enable_ssim ||
                              (ssim >= opencv_ctx->compare_config_snapshot_.min_ssim);
                bool passed = psnr_ok && ssim_ok;

                // 更新统计
                if (passed) {
                    opencv_ctx->passed_frames.fetch_add(1);
                } else {
                    opencv_ctx->failed_frames.fetch_add(1);
                }

                // 累加 PSNR/SSIM
                double old_psnr = opencv_ctx->psnr_sum.load();
                while (!opencv_ctx->psnr_sum.compare_exchange_weak(old_psnr, old_psnr + psnr)) {}

                double old_ssim = opencv_ctx->ssim_sum.load();
                while (!opencv_ctx->ssim_sum.compare_exchange_weak(old_ssim, old_ssim + ssim)) {}

                // 日志
                if (!passed || opencv_ctx->compare_config_snapshot_.verbose) {
                    if (!passed) {
                        LOG4CPLUS_WARN_FMT(logger,
                            "[Frame %llu] OpenCV %s FAILED: PSNR=%.2f (min=%.2f), SSIM=%.4f (min=%.4f)",
                            (unsigned long long)frame_version,
                            op_name.c_str(),
                            psnr, opencv_ctx->compare_config_snapshot_.min_psnr,
                            ssim, opencv_ctx->compare_config_snapshot_.min_ssim);
                    } else {
                        LOG4CPLUS_DEBUG_FMT(logger,
                            "[Frame %llu] OpenCV %s PASSED: PSNR=%.2f, SSIM=%.4f",
                            (unsigned long long)frame_version,
                            op_name.c_str(),
                            psnr, ssim);
                    }
                }
            } else {
                // 仅统计成功，不进行质量评估
                opencv_ctx->passed_frames.fetch_add(1);
                if (opencv_ctx->compare_config_snapshot_.verbose) {
                    LOG4CPLUS_INFO_FMT(logger,
                        "[Frame %llu] OpenCV %s 执行成功",
                        (unsigned long long)frame_version,
                        op_name.c_str());
                }
            }

        } catch (const std::exception& e) {
            auto logger = log4cplus::Logger::getInstance(
                LOG4CPLUS_TEXT("components.WorkerSyncCoordinator"));
            LOG4CPLUS_ERROR_FMT(logger,
                "[Frame %llu] OpenCV 操作异常: %s",
                (unsigned long long)frame_version,
                e.what());
            opencv_ctx->failed_frames.fetch_add(1);
        } catch (...) {
            auto logger = log4cplus::Logger::getInstance(
                LOG4CPLUS_TEXT("components.WorkerSyncCoordinator"));
            LOG4CPLUS_ERROR_FMT(logger,
                "[Frame %llu] OpenCV 操作未知异常",
                (unsigned long long)frame_version);
            opencv_ctx->failed_frames.fetch_add(1);
        }

        return true;  // 操作结果不影响 Buffer 提交
    };

    return CallbackChainItem(callback, context, "opencv_arithmetic_callback");
}
