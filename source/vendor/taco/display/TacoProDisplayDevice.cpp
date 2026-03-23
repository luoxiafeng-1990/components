#include "vendor/taco/display/TacoProDisplayDevice.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

TacoProDisplayDevice::TacoProDisplayDevice(const TacoProDisplayExtension& config)
    : config_(config)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Display.TacoPro")))
{
}

TacoProDisplayDevice::~TacoProDisplayDevice() {
    cleanup();
}

const char* TacoProDisplayDevice::findDeviceNode(int /*device_index*/) {
    return "tacopro-display";
}

bool TacoProDisplayDevice::initialize(int /*device_index*/) {
    if (initialized_) {
        return true;
    }

    context_ = TacoProDisplayContext::acquire(config_);
    if (!context_) {
        LOG4CPLUS_ERROR(logger_, "Failed to acquire TacoProDisplayContext");
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
        "TacoProDisplayDevice initialized: channel=%d", channel_id_);

    return true;
}

void TacoProDisplayDevice::cleanup() {
    if (!initialized_) {
        return;
    }

    if (context_ && channel_id_ >= 0) {
        context_->unregisterChannel(channel_id_);
        channel_id_ = -1;
    }

    context_.reset();
    initialized_ = false;
    LOG4CPLUS_DEBUG(logger_, "TacoProDisplayDevice cleaned up");
}

bool TacoProDisplayDevice::displayBuffer(Buffer* buffer) {
    if (!initialized_ || !context_ || channel_id_ < 0) {
        last_display_failed_ = true;
        return false;
    }

    bool success = context_->channelWrite(channel_id_, buffer);
    last_display_failed_ = !success;
    return success;
}

bool TacoProDisplayDevice::displayBuffer(BufferPool* /*pool*/, int /*buffer_index*/) {
    return false;
}

bool TacoProDisplayDevice::waitVerticalSync() {
    return true;
}

int TacoProDisplayDevice::getWidth() const {
    return context_ ? context_->getScreenWidth() : config_.screen_width;
}

int TacoProDisplayDevice::getHeight() const {
    return context_ ? context_->getScreenHeight() : config_.screen_height;
}

int TacoProDisplayDevice::getBytesPerPixel() const {
    int bpp = context_ ? context_->getBitsPerPixel() : config_.bits_per_pixel;
    return (bpp + 7) / 8;
}

int TacoProDisplayDevice::getBitsPerPixel() const {
    return context_ ? context_->getBitsPerPixel() : config_.bits_per_pixel;
}

int TacoProDisplayDevice::getBufferCount() const {
    return config_.frame_pool_size;
}

size_t TacoProDisplayDevice::getBufferSize() const {
    int w = getWidth();
    int h = getHeight();
    int bpp = getBitsPerPixel();
    return static_cast<size_t>(w) * h * bpp / 8;
}

int TacoProDisplayDevice::getCurrentDisplayBuffer() const {
    return 0;
}
