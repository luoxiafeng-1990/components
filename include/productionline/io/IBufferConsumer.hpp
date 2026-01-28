/**
 * @file IBufferConsumer.hpp
 * @brief Buffer 消费者策略接口
 * 
 * 设计模式：策略模式（Strategy Pattern）
 * 
 * 职责：
 * - 定义 Buffer 消费的统一接口
 * - 支持单 Buffer 消费（SINGLE 模式）
 * - 支持多 Buffer 消费（COMPARE 模式）
 * 
 * 使用场景：
 * - CountConsumer: 仅统计帧数
 * - DisplayConsumer: 显示输出
 * - SaveRawConsumer: 保存原始 YUV/RGB
 * - SaveEncodedConsumer: 保存编码流
 * - CompareConsumer: PSNR/SSIM 对比
 * - MultiConsumer: 多策略组合（消费类型叠加）
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
    CONSUME_NONE         = 0,         ///< 无消费（仅生产）
    CONSUME_COUNT        = 1 << 0,    ///< 仅统计帧数（0x01）
    CONSUME_DISPLAY      = 1 << 1,    ///< 显示输出（0x02）
    CONSUME_SAVE_RAW     = 1 << 2,    ///< 保存原始 YUV/RGB（0x04）
    CONSUME_SAVE_ENCODED = 1 << 3,    ///< 保存编码流（0x08）
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
};

} // namespace consumer

#endif // IBUFFER_CONSUMER_HPP
