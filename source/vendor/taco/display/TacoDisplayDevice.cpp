#include "vendor/taco/display/TacoDisplayDevice.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

std::mutex TacoDisplayDevice::s_mutex_;
std::weak_ptr<TacoDisplayContext> TacoDisplayDevice::s_weak_ctx_;

TacoDisplayDevice::TacoDisplayDevice(const TacoDisplayExtension& config)
    : config_(config)
{
}

TacoDisplayDevice::~TacoDisplayDevice() {
    cleanup();
}

const char* TacoDisplayDevice::findDeviceNode(int /*device_index*/) {
    return "taco-display";
}

bool TacoDisplayDevice::initialize(int /*device_index*/) {
    if (initialized_) {
        return true;
    }

    std::lock_guard<std::mutex> lock(s_mutex_);

    ctx_ = s_weak_ctx_.lock();
    if (!ctx_) {
        ctx_ = std::make_shared<TacoDisplayContext>(config_);
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

void TacoDisplayDevice::cleanup() {
    if (!initialized_) {
        return;
    }

    if (ctx_ && channel_id_ >= 0) {
        ctx_->releaseChannel(channel_id_);
        channel_id_ = -1;
    }

    ctx_.reset();
    initialized_ = false;
}

bool TacoDisplayDevice::displayBuffer(Buffer* buffer) {
    if (!initialized_ || !ctx_ || channel_id_ < 0) {
        return false;
    }
    return ctx_->sendFrame(channel_id_, buffer);
}

bool TacoDisplayDevice::displayBuffer(BufferPool* /*pool*/, int /*buffer_index*/) {
    return false;
}

bool TacoDisplayDevice::waitVerticalSync() {
    return true;
}

int TacoDisplayDevice::getWidth() const {
    return ctx_ ? ctx_->getScreenWidth() : config_.screen_width;
}

int TacoDisplayDevice::getHeight() const {
    return ctx_ ? ctx_->getScreenHeight() : config_.screen_height;
}

int TacoDisplayDevice::getBytesPerPixel() const {
    if (config_.frame_format == 0) {
        return 2;
    }
    return 4;
}

int TacoDisplayDevice::getBitsPerPixel() const {
    if (config_.frame_format == 0) {
        return 12;
    }
    return 32;
}

int TacoDisplayDevice::getBufferCount() const {
    return config_.frame_pool_size;
}

size_t TacoDisplayDevice::getBufferSize() const {
    // Must match TacoDisplayContext::allocateFramePool: VO layer uses screen dimensions
    if (config_.frame_format == 0) {
        return static_cast<size_t>(config_.screen_width) * config_.screen_height * 3 / 2;
    }
    return static_cast<size_t>(config_.screen_width) * config_.screen_height * 4;
}

int TacoDisplayDevice::getCurrentDisplayBuffer() const {
    return 0;
}
