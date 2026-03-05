#ifndef TACO_VO_DISPLAY_DEVICE_HPP
#define TACO_VO_DISPLAY_DEVICE_HPP

#include "display/IDisplayDevice.hpp"
#include "display/TacoVOContext.hpp"
#include "productionline/worker/WorkerConfig.hpp"

#include <memory>
#include <mutex>

/**
 * TacoVODisplayDevice - taco-vo per-channel 显示设备
 *
 * 实现 IDisplayDevice 接口，作为 LinuxFramebufferDevice 的替代品。
 * 内部通过 static weak_ptr + shared_ptr 模式自动管理共享的 TacoVOContext：
 * - 第一个实例创建 Context（layer/view/engine 初始化）
 * - 后续实例复用已有 Context
 * - 最后一个实例析构时 Context 自动清理
 *
 * 每个实例拥有一个独立的 taco-vo channel，对应九宫格中的一个位置。
 */
class TacoVODisplayDevice : public IDisplayDevice {
public:
    using TacoVOConfig = WorkerConfig::ConsumerTypeConfig::DisplayType::TacoVOConfig;

    explicit TacoVODisplayDevice(const TacoVOConfig& config);
    ~TacoVODisplayDevice() override;

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

private:
    TacoVOConfig config_;
    int channel_id_ = -1;
    bool initialized_ = false;

    // shared context (weak_ptr for lazy creation, shared_ptr to keep alive)
    std::shared_ptr<TacoVOContext> ctx_;
    static std::mutex s_mutex_;
    static std::weak_ptr<TacoVOContext> s_weak_ctx_;
};

#endif // TACO_VO_DISPLAY_DEVICE_HPP
