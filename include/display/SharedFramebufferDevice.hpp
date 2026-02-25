#ifndef SHARED_FRAMEBUFFER_DEVICE_HPP
#define SHARED_FRAMEBUFFER_DEVICE_HPP

#include "display/IDisplayDevice.hpp"
#include "display/SharedDisplayContext.hpp"
#include "productionline/worker/WorkerConfig.hpp"

#include <memory>

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

/**
 * SharedFramebufferDevice - 基于 SharedDisplayContext 的多通道显示设备
 *
 * 每个 DisplayConsumer 持有一个此类实例，但所有实例共享同一个
 * SharedDisplayContext（通过 shared_ptr 单例管理）。
 *
 * 生命周期：
 *   - initialize() 时 acquire SharedDisplayContext 并注册通道
 *   - displayBuffer() 时委托给 SharedDisplayContext::channelWrite()
 *   - cleanup() 时注销通道并释放 shared_ptr
 *   - 最后一个实例释放时 SharedDisplayContext 自动销毁
 *
 * 兼容性：
 *   - 实现完整的 IDisplayDevice 接口
 *   - DisplayConsumer 无需修改调用方式
 */
class SharedFramebufferDevice : public IDisplayDevice {
public:
    using TacoVOConfig = WorkerConfig::ConsumerTypeConfig::DisplayType::TacoVOConfig;

    explicit SharedFramebufferDevice(const TacoVOConfig& config);
    ~SharedFramebufferDevice() override;

    // ============ IDisplayDevice 接口 ============

    const char* findDeviceNode(int device_index) override;
    bool initialize(int device_index) override;
    void cleanup() override;

    int getWidth() const override;
    int getHeight() const override;
    int getBytesPerPixel() const override;
    int getBitsPerPixel() const override;
    int getBufferCount() const override;
    size_t getBufferSize() const override;

    bool displayBuffer(Buffer* buffer) override;
    bool displayBuffer(BufferPool* pool, int buffer_index) override;
    bool waitVerticalSync() override;
    int getCurrentDisplayBuffer() const override;

    /**
     * 查询上次 displayBuffer 是否失败（用于帧缓存机制）
     */
    bool lastDisplayFailed() const { return last_display_failed_; }

private:
    TacoVOConfig config_;
    int channel_id_ = -1;
    bool initialized_ = false;
    bool last_display_failed_ = false;

    std::shared_ptr<SharedDisplayContext> context_;

    log4cplus::Logger logger_;
};

#endif // SHARED_FRAMEBUFFER_DEVICE_HPP
