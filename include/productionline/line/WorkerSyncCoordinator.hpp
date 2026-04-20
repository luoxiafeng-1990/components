#ifndef PRODUCTIONLINE_LINE_WORKER_SYNC_COORDINATOR_HPP
#define PRODUCTIONLINE_LINE_WORKER_SYNC_COORDINATOR_HPP

#include <functional>
#include <map>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <log4cplus/logger.h>

// 前向声明
class Buffer;

// ⭐ 注意：FrameSyncCallback 和 CallbackChainItem 的定义在 WorkerConfig.hpp 中
// 这里只需要包含该头文件即可
#include "productionline/worker/config/Connector.hpp"
#include "consumptionline/config/ConsumerTypeConfig.hpp"
#include "productionline/worker/config/FrameSyncTypes.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"
#include "productionline/worker/base/WorkerBase.hpp"  // for FillStatus, FillResult
#include "consumptionline/types/compare/BufferComparator.hpp"

// ============================================================
// 比较回调上下文（与 WorkerSyncCoordinator 配套使用）
// ============================================================

/**
 * @brief 比较回调上下文
 * 
 * 用于 WorkerSyncCoordinator 的 callback_chain，
 * 在 MultiWorkerProductionLine 内部执行帧同步比较。
 * 
 * 使用方式：
 * @code
 * auto compare_ctx = std::make_shared<CompareCallbackContext>();
 * compare_ctx->initFromCompareType(worker_config.consumer_type.compare);
 * compare_ctx->worker1_name = "hw_decoder";
 * compare_ctx->worker2_name = "sw_decoder";
 * 
 * connector_config.enable_frame_sync = true;
 * connector_config.callback_chain.push_back(
 *     WorkerSyncCoordinator::createDefaultCompareCallback(compare_ctx.get()));
 * @endcode
 */
struct CompareCallbackContext {
    // Worker 名称（用于日志和结果区分）
    std::string worker1_name;
    std::string worker2_name;
    
    // 比较配置（从 ConsumerTypeConfig.compare 初始化）
    bool enable_psnr = true;
    bool enable_ssim = true;
    double min_psnr = 38.0;
    double min_ssim = 0.95;
    bool verbose = false;
    /// 完整 compare 配置快照（strategy / enable_parallel / warn_* 等），供 BufferComparator::open 使用
    WorkerConfig::ConsumerTypeConfig::CompareType compare_config_snapshot_{};
    
    // 统计计数器（原子，线程安全）
    std::atomic<int> total_frames{0};
    std::atomic<int> passed_frames{0};
    std::atomic<int> failed_frames{0};
    
    // 累计值（用于计算平均值）
    std::atomic<double> psnr_sum{0.0};
    std::atomic<double> ssim_sum{0.0};
    
    // 外部回调（可选，每帧比较完成后调用）
    std::function<void(int frame_index, double psnr, double ssim, bool passed)> 
        result_callback;
    
    // ⭐ v2.28 优化：BufferComparator 作为成员，避免每帧重复创建
    std::unique_ptr<consumptionline::io::BufferComparator> comparator_;
    bool comparator_opened_ = false;
    
    CompareCallbackContext() = default;
    
    // 析构函数：关闭 comparator
    ~CompareCallbackContext();
    
    // 禁止拷贝（因为有 atomic 成员和 unique_ptr）
    CompareCallbackContext(const CompareCallbackContext&) = delete;
    CompareCallbackContext& operator=(const CompareCallbackContext&) = delete;
    
    /**
     * @brief 从 ConsumerTypeConfig::CompareType 初始化配置
     */
    void initFromCompareType(const WorkerConfig::ConsumerTypeConfig::CompareType& compare_config);
    
    /**
     * @brief 打开 BufferComparator
     * 
     * 应在 initFromCompareType() 之后调用。
     * 只会在首次调用时创建和打开，后续调用直接返回 true。
     * 
     * @return true 打开成功
     * @return false 打开失败
     */
    bool openComparator();
    
    /**
     * @brief 关闭 BufferComparator
     * 
     * 通常不需要手动调用，析构函数会自动调用。
     */
    void closeComparator();
    
    // 获取平均 PSNR
    double getAveragePsnr() const;
    
    // 获取平均 SSIM
    double getAverageSsim() const;
    
    // 获取通过率
    double getPassRate() const;
    
    // 判断是否通过
    bool isPassed() const;
};

// ============================================================
// OpenCV 回调上下文（用于多生产者 Buffer 算术运算）
// ============================================================

/**
 * @brief OpenCV 回调上下文
 *
 * 用于 WorkerSyncCoordinator 的 callback_chain，
 * 在多个 Worker 的 Buffer 上执行 OpenCV 算术运算（如 add、absdiff）。
 *
 * 使用方式：
 * @code
 * auto opencv_ctx = std::make_shared<OpenCVCallbackContext>();
 * opencv_ctx->config = worker_config.consumer_type.opencv;
 *
 * connector_config.enable_frame_sync = true;
 * connector_config.callback_chain.push_back(
 *     WorkerSyncCoordinator::createOpenCVCallback(opencv_ctx.get()));
 * @endcode
 */
struct OpenCVCallbackContext {
    // OpenCV 配置（从 ConsumerTypeConfig.opencv 初始化）
    WorkerConfig::ConsumerTypeConfig::OpencvType config;

    // Compare 配置快照（用于 PSNR/SSIM/verbose）
    WorkerConfig::ConsumerTypeConfig::CompareType compare_config_snapshot_{};

    // 统计计数器（原子，线程安全）
    std::atomic<int> total_frames{0};
    std::atomic<int> passed_frames{0};
    std::atomic<int> failed_frames{0};

    // 累计值（用于计算平均值）
    std::atomic<double> psnr_sum{0.0};
    std::atomic<double> ssim_sum{0.0};

    // Worker 名称（用于日志和结果区分）
    std::string worker1_name;
    std::string worker2_name;

    // ⭐ v2.28 优化：BufferComparator 作为成员，用于计算质量指标
    std::unique_ptr<consumptionline::io::BufferComparator> comparator_;
    bool comparator_opened_ = false;

    OpenCVCallbackContext() = default;

    // 析构函数：关闭 comparator
    ~OpenCVCallbackContext();

    // 禁止拷贝（因为有 atomic 成员和 unique_ptr）
    OpenCVCallbackContext(const OpenCVCallbackContext&) = delete;
    OpenCVCallbackContext& operator=(const OpenCVCallbackContext&) = delete;

    /**
     * @brief 打开 BufferComparator（用于计算结果的 PSNR/SSIM）
     *
     * @return true 打开成功
     * @return false 打开失败
     */
    bool openComparator();

    /**
     * @brief 关闭 BufferComparator
     */
    void closeComparator();

    // 获取平均 PSNR
    double getAveragePsnr() const;

    // 获取平均 SSIM
    double getAverageSsim() const;

    // 获取通过率
    double getPassRate() const;

    // 判断是否通过
    bool isPassed() const;
};

// ============================================================
// WorkerSyncCoordinator 类
// ============================================================

/**
 * @brief Worker 同步协调器
 * 
 * 职责：
 * - 协调同一 Connector 内的多个 Worker
 * - 在 Worker 解码完成后、提交 Buffer 前插入同步点
 * - 按顺序执行回调链（Pipeline 模式）
 * 
 * 设计原则：
 * - 单一职责：只负责 Worker 间的同步协调
 * - 与 EncodedPacketSourceFromBuffer 解耦：通过 frame_version 关联
 * - 可选配置：无回调时零开销
 * 
 * 使用场景：
 * - PSNR 对比：硬件解码 vs 软件解码
 * - 质量检测：多路解码结果一致性检查
 * - 数据聚合：多 Worker 协同处理
 * 
 * @note 每个 Connector 创建一个实例
 */
class WorkerSyncCoordinator {
public:
    /**
     * @brief 构造函数
     * 
     * @param worker_names 参与同步的 Worker 名称列表
     * @param callback_chain 回调链（可选，默认为空）
     */
    WorkerSyncCoordinator(
        const std::vector<std::string>& worker_names,
        const CallbackChain& callback_chain = {}
    );
    
    /**
     * @brief 析构函数
     */
    ~WorkerSyncCoordinator();
    
    /**
     * @brief Worker 到达同步点
     * 
     * @param worker_name Worker 名称
     * @param frame_version 帧版本号（来自 EncodedPacketSourceFromBuffer）
     * @param buffer 解码后的 Buffer（失败时为 nullptr）
     * @param result fillBuffer 的返回结果（SUCCESS/EAGAIN/ERROR）
     * @return true=允许提交, false=拒绝提交
     * 
     * 行为：
     * - 如果回调链为空，直接返回 true（零开销）
     * - 阻塞等待，直到所有 Worker 都到达同一版本
     * - 最后一个到达的 Worker 触发回调链执行
     * - 按顺序执行回调链，任何回调返回 false 则终止链
     * - 回调执行完毕后，所有 Worker 被唤醒
     * 
     * v2.29 新增场景处理：
     * - 都成功：执行回调对比
     * - 都 EAGAIN：跳过回调，继续下一帧
     * - 都 ERROR：跳过回调，继续下一帧
     * - 一个成功 + 一个 EAGAIN：报错！不应该发生
     * - 一个成功 + 一个 ERROR：跳过回调，继续下一帧
     * 
     * 线程安全：是
     * 
     * v2.33 变更：参数类型从 FillBufferResult 改为 FillStatus
     * v2.34 变更：参数类型从 FillStatus 改为 FillResult
     */
    bool arrive(const std::string& worker_name, uint64_t frame_version, 
                Buffer* buffer, const FillResult& result);
    
    /**
     * @brief Worker 退出时减少 total_workers_（v2.38 新增）
     *
     * 使得存活的 Worker 到达同步点后不必等待已退出的 Worker。
     */
    void removeWorker(const std::string& worker_name);
    
    // ============================================================
    // 静态工厂方法：创建默认比较回调
    // ============================================================
    
    /**
     * @brief 创建默认的比较回调
     *
     * @param context 比较上下文（用于累积统计结果）
     * @return CallbackChainItem 可添加到 ConnectorConfig.callback_chain
     *
     * 功能：
     * - 使用 BufferComparator 计算 PSNR/SSIM
     * - 将结果累积到 CompareCallbackContext 中
     * - 根据阈值判断是否通过
     *
     * 使用示例：
     * @code
     * auto compare_ctx = std::make_shared<CompareCallbackContext>();
     * compare_ctx->initFromCompareType(worker_config.consumer_type.compare);
     *
     * connector_config.enable_frame_sync = true;
     * connector_config.callback_chain.push_back(
     *     WorkerSyncCoordinator::createDefaultCompareCallback(compare_ctx.get()));
     * @endcode
     *
     * @note context 的生命周期需由调用者管理（推荐使用 shared_ptr）
     */
    static CallbackChainItem createDefaultCompareCallback(CompareCallbackContext* context);

    /**
     * @brief 创建 OpenCV 算术运算回调
     *
     * @param context OpenCV 上下文（用于累积统计结果）
     * @return CallbackChainItem 可添加到 ConnectorConfig.callback_chain
     *
     * 功能：
     * - 获取两个 Worker 的 Buffer 并转换为 cv::Mat
     * - 执行指定的 OpenCV 算术运算（如 add、absdiff）
     * - 计算结果的 PSNR/SSIM（如果启用）
     * - 将统计结果累积到 OpenCVCallbackContext 中
     *
     * 支持的操作（OpencvType::OpType）：
     * - ADD：两个 Mat 相加
     * - ABSDIFF：两个 Mat 的绝对差
     *
     * 使用示例：
     * @code
     * auto opencv_ctx = std::make_shared<OpenCVCallbackContext>();
     * opencv_ctx->config.op_type = OpencvType::OpType::ADD;
     * opencv_ctx->config.enable_psnr = true;
     *
     * connector_config.enable_frame_sync = true;
     * connector_config.callback_chain.push_back(
     *     WorkerSyncCoordinator::createOpenCVCallback(opencv_ctx.get()));
     * @endcode
     *
     * @note context 的生命周期需由调用者管理（推荐使用 shared_ptr）
     */
    static CallbackChainItem createOpenCVCallback(OpenCVCallbackContext* context);
    
private:
    /**
     * @brief 单帧同步数据
     * 
     * v2.33 变更：worker_results 类型从 FillBufferResult 改为 FillStatus
     * v2.34 变更：worker_results 类型从 FillStatus 改为 FillResult
     */
    struct FrameSync {
        std::map<std::string, Buffer*> worker_buffers;      // worker_name -> Buffer*
        std::map<std::string, FillResult> worker_results;   // worker_name -> FillResult
        size_t arrived_count = 0;                           // 已到达的 Worker 数量
        bool callback_executed = false;                     // 回调是否已执行
        bool should_submit = true;                          // 是否允许提交
    };
    
    /**
     * @brief 执行回调链
     * 
     * @param frame_version 帧版本号
     * @param worker_buffers 所有 Worker 的 Buffer
     * @return true=所有回调通过, false=有回调失败
     */
    bool executeCallbackChain(
        uint64_t frame_version,
        const std::map<std::string, Buffer*>& worker_buffers
    );
    
    /**
     * @brief 清理旧版本的同步数据
     * 
     * @param current_version 当前版本号
     */
    void cleanupOldFrames(uint64_t current_version);
    
    // 配置
    std::vector<std::string> worker_names_;  // 参与同步的 Worker 名称列表
    size_t total_workers_;                   // Worker 总数
    CallbackChain callback_chain_;           // 回调链
    
    // 同步状态
    std::mutex mutex_;                       // 互斥锁
    std::condition_variable cv_;             // 条件变量
    std::map<uint64_t, FrameSync> frame_syncs_;  // frame_version -> FrameSync
    
    // B 帧 reorder 容错：基于 PTS 的深拷贝帧缓存
    // 当 EAGAIN 不一致时，对成功 worker 的帧做 av_frame_clone() 深拷贝，
    // 后续 PTS 匹配后再执行比较。避免持有 Buffer* 导致 use-after-free。
    struct PendingFrame {
        std::string worker_name;
        int64_t pts;
        AVFrame* frame;    // av_frame_clone() 产出，我们拥有所有权
    };
    std::vector<PendingFrame> pending_frames_;

    void clearPendingFrames();
    void tryMatchPending(const std::map<std::string, Buffer*>& current_buffers);

    // 日志
    log4cplus::Logger logger_;
};

#endif // WORKER_SYNC_COORDINATOR_HPP
