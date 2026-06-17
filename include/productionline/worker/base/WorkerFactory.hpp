#ifndef WORKER_FACTORY_HPP
#define WORKER_FACTORY_HPP

#include "productionline/worker/base/WorkerBase.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include <memory>

/**
 * @brief WorkerFactory - Worker 工厂
 * 
 * 架构角色：Worker Factory（工人工厂）
 * 
 * 设计模式：工厂模式（Factory Pattern）
 * 
 * 职责：
 * - 根据环境和配置创建合适的Worker实现
 * - 封装Worker创建逻辑
 * - 支持自动检测和手动指定两种模式
 * - 创建后自动注册到 ComponentTopology（共享所有权）
 * 
 * 创建的Worker类型：
 * - FFmpegDecodeWorker: FFmpeg解码Worker（统一处理文件和RTSP流）
 * - FfmpegPacketRecorderWorker: FFmpeg录制Worker
 * - FFmpegEncodeWorker: FFmpeg编码Worker
 * 
 * 返回值语义：
 * - 返回 shared_ptr<WorkerBase>（替代 unique_ptr）
 * - Factory 内部自动注册到 ComponentTopology
 * - ProductionLine 和 Topology 共享 Worker 所有权
 * 
 * 注意：WorkerType 枚举定义在 WorkerConfig.hpp 中，避免循环依赖
 */
class WorkerFactory {
public:
    using WorkerType = ::WorkerType;
    
    /**
     * 创建Worker（工厂方法）
     * 
     * 创建策略（优先级从高到低）：
     * 1. 用户显式指定 (type != AUTO)
     * 2. 环境变量 (VIDEO_READER_TYPE)
     * 3. 配置文件 (/etc/video_reader.conf)
     * 4. 自动检测系统能力
     * 
     * 创建后自动注册到 ComponentTopology。
     * 
     * @param config Worker配置（默认空配置）
     * @param owner_type 拓扑归属类型（NONE=不注册拓扑, LINE=归属Line, GROUP=归属Group）
     * @param owner_id 归属的 Line/Group 拓扑 ID（owner_type 为 NONE 时忽略）
     * @return Worker实例（shared_ptr，与 Topology 共享所有权）
     */
    static std::shared_ptr<WorkerBase> create(
        const WorkerConfig& config = WorkerConfig(),
        TopologyOwnerType owner_type = TopologyOwnerType::NONE,
        uint64_t owner_id = 0);
    
    /**
     * 将类型转换为字符串
     * @param type 类型
     * @return 类型名称
     */
    static const char* typeToString(WorkerType type);

private:
    static log4cplus::Logger logger_;
    static std::unique_ptr<WorkerBase> autoDetect(const WorkerConfig& config = WorkerConfig());
    static std::unique_ptr<WorkerBase> createByType(WorkerType type, const WorkerConfig& config = WorkerConfig());
    static WorkerType getTypeFromEnvironment();
    static WorkerType getTypeFromConfig();
};

#endif // WORKER_FACTORY_HPP
