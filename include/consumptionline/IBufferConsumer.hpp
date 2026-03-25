/**
 * @file IBufferConsumer.hpp
 * @brief Buffer 消费者策略接口
 * 
 * 设计模式：策略模式（Strategy Pattern）
 * 
 * 职责：
 * - 定义 Buffer 消费的统一接口
 * - 支持单 Buffer 消费（SINGLE 模式）
 * - 支持多 Buffer 消费（COMPARE/PARALLEL 模式）
 * 
 * 使用场景：
 * - CountConsumer: 仅统计帧数
 * - DisplayConsumer: 显示输出
 * - SaveRawConsumer: 保存原始 YUV/RGB
 * - SaveEncodedConsumer: 保存编码流
 * - MultiConsumer: 多策略组合（消费类型叠加）
 * - NpuInferenceConsumer: NPU 推理输出
 * 
 * 注：PSNR/SSIM 比较功能已迁移至 WorkerSyncCoordinator::createDefaultCompareCallback
 */

#ifndef IBUFFER_CONSUMER_HPP
#define IBUFFER_CONSUMER_HPP

#include "buffer/bufferpool/Buffer.hpp"
#include <vector>
#include <string>
#include <cstdint>

namespace consumer {

/**
 * @brief 消费类型标志（可叠加）
 * 
 * 使用位标志可以组合多种消费类型：
 * - CONSUME_DISPLAY | CONSUME_SAVE_RAW = 同时显示和保存
 */
enum ConsumeTypeFlags : uint32_t {
    CONSUME_NONE            = 0,         ///< 无消费（仅生产）
    CONSUME_COUNT           = 1 << 0,    ///< 仅统计帧数（0x01）
    CONSUME_DISPLAY         = 1 << 1,    ///< 显示输出（0x02）
    CONSUME_SAVE_RAW        = 1 << 2,    ///< 保存原始 YUV/RGB（0x04）
    CONSUME_SAVE_ENCODED    = 1 << 3,    ///< 保存编码流（0x08）
    CONSUME_CHANNEL_COMPARE = 1 << 4,    ///< ⭐ v2.27：通道比较（0x10）
    CONSUME_OPENCV          = 1 << 5,    ///< OpenCV消费：Buffer→Mat转换并计算PSNR/SSIM（0x20）
    CONSUME_NPU_INFERENCE   = 1 << 6, ///< ⭐ v2.28：NPU 推理（0x20）
};

/**
 * @brief 执行模式
 * 
 * 描述如何组织生产线和消费循环
 */
enum class ExecuteMode {
    SINGLE,     ///< 单路消费：1 个 Worker，1 个消费循环
    COMPARE,    ///< 对比消费：N 个 Worker，同步获取 N 个 BufferPool，进行 PSNR/SSIM
    PARALLEL,   ///< 并行消费：N 个 Worker，N 个独立线程，每个线程独立消费
};

/**
 * @brief Buffer 消费者接口（策略模式）
 * 
 * 接口设计支持：
 * - 单 Buffer 消费（vector 中只有 1 个元素）
 * - 多 Buffer 消费（COMPARE 场景，vector 中有多个元素）
 */
class IBufferConsumer {
public:
    virtual ~IBufferConsumer() = default;
    
    /**
     * @brief 消费 Buffer（支持单个或多个）
     * 
     * @param buffers Buffer 列表
     *        - 单路消费：buffers.size() == 1
     *        - COMPARE：buffers.size() >= 2（如 HW buffer + SW buffer）
     * @param frame_index 帧索引
     * @return true 继续消费，false 停止
     */
    virtual bool consume(const std::vector<Buffer*>& buffers, int frame_index) = 0;
    
    /**
     * @brief 初始化（首帧到达时调用）
     * @param first_buffers 第一批 Buffer（用于检测格式等）
     * @return true 初始化成功，false 初始化失败
     */
    virtual bool initialize(const std::vector<Buffer*>& first_buffers) {
        (void)first_buffers;
        return true;
    }
    
    /**
     * @brief 清理资源
     */
    virtual void finalize() {}
    
    /**
     * @brief 获取统计信息
     * @return 统计信息字符串
     */
    virtual std::string getStats() const { return ""; }

    /**
     * @brief 询问消费者是否需要保留当前 buffer（而非归还到 pool）
     *
     * 用于显示消费者在 channelWrite 失败时（如定时器正在切换 buffer），
     * 通知 consumeLoop 保留该帧以便下一周期重试，避免帧丢失。
     *
     * @return true 需要保留（不要 releaseFilled），false 正常归还
     */
    virtual bool shouldRetainBuffer() const { return false; }
};

} // namespace consumer

#endif // IBUFFER_CONSUMER_HPP
