#ifndef TACO_DISPLAY_DEVICE_HPP
#define TACO_DISPLAY_DEVICE_HPP

#include "vendor/contracts/IDisplayDevice.hpp"
#include "vendor/taco/display/TacoDisplayExtension.hpp"
#include "vendor/taco/display/TacoDisplayContext.hpp"

#include <memory>
#include <mutex>

/**
 * TacoDisplayDevice - taco VO per-channel 显示设备（vendor=taco）
 *
 * 内部通过 static weak_ptr + shared_ptr 模式自动管理共享的 TacoDisplayContext。
 * 每个实例拥有一个独立的 taco-vo channel。
 */
class TacoDisplayDevice : public IDisplayDevice {
public:
    explicit TacoDisplayDevice(const TacoDisplayExtension& config);
    ~TacoDisplayDevice() override;

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
    int getChannelId() const override { return channel_id_; }

private:
    TacoDisplayExtension config_;
    int channel_id_ = -1;
    bool initialized_ = false;

    std::shared_ptr<TacoDisplayContext> ctx_;
    static std::mutex s_mutex_;
    static std::weak_ptr<TacoDisplayContext> s_weak_ctx_;
};

#endif // TACO_DISPLAY_DEVICE_HPP
