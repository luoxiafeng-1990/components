#include "vendor/taco/display/TacoVODisplayDevice.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

// static members
std::mutex TacoVODisplayDevice::s_mutex_;
std::weak_ptr<TacoVOContext> TacoVODisplayDevice::s_weak_ctx_;

TacoVODisplayDevice::TacoVODisplayDevice(const TacoVOConfig& config)
    : config_(config)
{
}

TacoVODisplayDevice::~TacoVODisplayDevice() {
    cleanup();
}

const char* TacoVODisplayDevice::findDeviceNode(int /*device_index*/) {
    return "taco-vo";
}

bool TacoVODisplayDevice::initialize(int /*device_index*/) {
    if (initialized_) {
        return true;
    }

    std::lock_guard<std::mutex> lock(s_mutex_);

    ctx_ = s_weak_ctx_.lock();
    if (!ctx_) {
        ctx_ = std::make_shared<TacoVOContext>(config_);
        s_weak_ctx_ = ctx_;
    }

    channel_id_ = ctx_->allocateChannel();
    if (channel_id_ < 0) {
        ctx_.reset();
        return false;
    }

    initialized_ = true;
    return true;
}

void TacoVODisplayDevice::cleanup() {
    if (!initialized_) {
        return;
    }

    if (ctx_ && channel_id_ >= 0) {
        ctx_->releaseChannel(channel_id_);
        channel_id_ = -1;
    }

    ctx_.reset();
    // When this was the last holder, ~TacoVOContext() runs automatically
    initialized_ = false;
}

bool TacoVODisplayDevice::displayBuffer(Buffer* buffer) {
    if (!initialized_ || !ctx_ || channel_id_ < 0) {
        return false;
    }
    return ctx_->sendFrame(channel_id_, buffer);
}

bool TacoVODisplayDevice::displayBuffer(BufferPool* /*pool*/, int /*buffer_index*/) {
    return false;  // not applicable for taco-vo
}

bool TacoVODisplayDevice::waitVerticalSync() {
    // taco-vo handles vsync internally via its display engine
    return true;
}

int TacoVODisplayDevice::getWidth() const {
    return ctx_ ? ctx_->getScreenWidth() : config_.screen_width;
}

int TacoVODisplayDevice::getHeight() const {
    return ctx_ ? ctx_->getScreenHeight() : config_.screen_height;
}

int TacoVODisplayDevice::getBytesPerPixel() const {
    // NV12 = 1.5 bytes/pixel → round up to 2; ARGB = 4
    if (config_.frame_format == 0) {
        return 2;
    }
    return 4;
}

int TacoVODisplayDevice::getBitsPerPixel() const {
    if (config_.frame_format == 0) {
        return 12;  // NV12
    }
    return 32;  // ARGB
}

int TacoVODisplayDevice::getBufferCount() const {
    return config_.frame_pool_size;
}

size_t TacoVODisplayDevice::getBufferSize() const {
    if (config_.frame_format == 0) {
        return static_cast<size_t>(config_.frame_width) * config_.frame_height * 3 / 2;
    }
    return static_cast<size_t>(config_.frame_width) * config_.frame_height * 4;
}

int TacoVODisplayDevice::getCurrentDisplayBuffer() const {
    return 0;
}
