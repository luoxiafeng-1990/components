#include "vendor/taco/display/SharedFramebufferDevice.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

SharedFramebufferDevice::SharedFramebufferDevice(const TacoVOConfig& config)
    : config_(config)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Display.SharedFB")))
{
}

SharedFramebufferDevice::~SharedFramebufferDevice() {
    cleanup();
}

const char* SharedFramebufferDevice::findDeviceNode(int /*device_index*/) {
    return "shared-framebuffer";
}

bool SharedFramebufferDevice::initialize(int /*device_index*/) {
    if (initialized_) {
        return true;
    }

    context_ = SharedDisplayContext::acquire(config_);
    if (!context_) {
        LOG4CPLUS_ERROR(logger_, "Failed to acquire SharedDisplayContext");
        return false;
    }

    channel_id_ = context_->registerChannel();
    if (channel_id_ < 0) {
        LOG4CPLUS_ERROR(logger_, "Failed to register channel");
        context_.reset();
        return false;
    }

    initialized_ = true;
    LOG4CPLUS_INFO_FMT(logger_,
        "SharedFramebufferDevice initialized: channel=%d", channel_id_);

    return true;
}

void SharedFramebufferDevice::cleanup() {
    if (!initialized_) {
        return;
    }

    if (context_ && channel_id_ >= 0) {
        context_->unregisterChannel(channel_id_);
        channel_id_ = -1;
    }

    context_.reset();
    initialized_ = false;
    LOG4CPLUS_DEBUG(logger_, "SharedFramebufferDevice cleaned up");
}

bool SharedFramebufferDevice::displayBuffer(Buffer* buffer) {
    if (!initialized_ || !context_ || channel_id_ < 0) {
        last_display_failed_ = true;
        return false;
    }

    bool success = context_->channelWrite(channel_id_, buffer);
    last_display_failed_ = !success;
    return success;
}

bool SharedFramebufferDevice::displayBuffer(BufferPool* /*pool*/, int /*buffer_index*/) {
    return false;
}

bool SharedFramebufferDevice::waitVerticalSync() {
    // VSYNC 由 SharedDisplayContext 的显示线程内部处理
    return true;
}

int SharedFramebufferDevice::getWidth() const {
    return context_ ? context_->getScreenWidth() : config_.screen_width;
}

int SharedFramebufferDevice::getHeight() const {
    return context_ ? context_->getScreenHeight() : config_.screen_height;
}

int SharedFramebufferDevice::getBytesPerPixel() const {
    int bpp = context_ ? context_->getBitsPerPixel() : 32;
    return (bpp + 7) / 8;
}

int SharedFramebufferDevice::getBitsPerPixel() const {
    return context_ ? context_->getBitsPerPixel() : 32;
}

int SharedFramebufferDevice::getBufferCount() const {
    return config_.frame_pool_size;
}

size_t SharedFramebufferDevice::getBufferSize() const {
    int w = getWidth();
    int h = getHeight();
    int bpp = getBitsPerPixel();
    return static_cast<size_t>(w) * h * bpp / 8;
}

int SharedFramebufferDevice::getCurrentDisplayBuffer() const {
    return 0;
}
