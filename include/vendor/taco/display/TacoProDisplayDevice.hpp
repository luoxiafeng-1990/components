#ifndef TACO_PRO_DISPLAY_DEVICE_HPP
#define TACO_PRO_DISPLAY_DEVICE_HPP

#include "vendor/contracts/IDisplayDevice.hpp"
#include "vendor/taco/display/TacoProDisplayExtension.hpp"
#include "vendor/taco/display/TacoProDisplayContext.hpp"

#include <memory>

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

/**
 * TacoProDisplayDevice - 基于 TacoProDisplayContext 的多通道显示设备（vendor=tacopro）
 *
 * 每个 DisplayConsumer 持有一个此类实例，但所有实例共享同一个
 * TacoProDisplayContext（通过 shared_ptr 单例管理）。
 */
class TacoProDisplayDevice : public IDisplayDevice {
public:
    explicit TacoProDisplayDevice(const TacoProDisplayExtension& config);
    ~TacoProDisplayDevice() override;

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

    bool lastDisplayFailed() const { return last_display_failed_; }

private:
    TacoProDisplayExtension config_;
    int channel_id_ = -1;
    bool initialized_ = false;
    bool last_display_failed_ = false;

    std::shared_ptr<TacoProDisplayContext> context_;

    log4cplus::Logger logger_;
};

#endif // TACO_PRO_DISPLAY_DEVICE_HPP
