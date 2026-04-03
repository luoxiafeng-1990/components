#include "vendor/taco/display/TacoDisplayContext.hpp"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdlib>

TacoDisplayContext::TacoDisplayContext(const TacoDisplayExtension& config)
    : config_(config)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Display.TacoVO")))
{
    int max_ch = 9;
    grid_cols_ = static_cast<int>(std::ceil(std::sqrt(max_ch)));
    grid_rows_ = static_cast<int>(std::ceil(static_cast<double>(max_ch) / grid_cols_));
    channels_.resize(max_ch);

    if (!initDevice()) {
        LOG4CPLUS_ERROR(logger_, "TacoDisplayContext: device init failed");
        return;
    }

    if (!initLayer()) {
        LOG4CPLUS_ERROR(logger_, "TacoDisplayContext: layer init failed");
        return;
    }

    int ch_w = config_.screen_width / grid_cols_;
    int ch_h = config_.screen_height / grid_rows_;

    for (int i = 0; i < max_ch; i++) {
        int cx = (i % grid_cols_) * ch_w;
        int cy = (i / grid_cols_) * ch_h;
        if (!createChannel(i, cx, cy, ch_w, ch_h)) {
            LOG4CPLUS_ERROR_FMT(logger_, "TacoDisplayContext: failed to create channel %d", i);
        }
    }

    int ret = ta_vo_layer_enable(layer_ctx_);
    if (ret != 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "TacoDisplayContext: ta_vo_layer_enable failed: %d", ret);
    } else {
        layer_enabled_ = true;
        LOG4CPLUS_INFO_FMT(logger_, "TacoDisplayContext: initialized (%dx%d, %d channels, %d fps, grid %dx%d)",
                           config_.screen_width, config_.screen_height,
                           max_ch, config_.target_fps, grid_cols_, grid_rows_);
    }
}

TacoDisplayContext::~TacoDisplayContext() {
    if (layer_enabled_ && layer_ctx_) {
        ta_vo_layer_disable(layer_ctx_);
        layer_enabled_ = false;
    }

    for (auto& ch : channels_) {
        if (ch.chn_ctx) {
            ta_vo_chn_unbind_from_layer(ch.chn_ctx, layer_ctx_);
            ta_vo_chn_destroy(ch.chn_ctx);
            ch.chn_ctx = nullptr;
            ch.active = false;
        }
        freeFramePool(ch);
    }

    if (layer_ctx_) {
        ta_vo_layer_destroy(layer_ctx_);
        layer_ctx_ = nullptr;
    }

    if (dev_ctx_) {
        ta_vo_dev_destroy(dev_ctx_);
        dev_ctx_ = nullptr;
    }

    LOG4CPLUS_INFO(logger_, "TacoDisplayContext: destroyed");
}

bool TacoDisplayContext::initDevice() {
    dev_ctx_ = ta_vo_dev_create(TA_VO_DEV_IDS);
    if (!dev_ctx_) {
        LOG4CPLUS_ERROR(logger_, "ta_vo_dev_create(TA_VO_DEV_IDS) failed");
        return false;
    }

    ta_vo_dev_attr dev_attr;
    std::memset(&dev_attr, 0, sizeof(dev_attr));
    dev_attr.fps = config_.target_fps;

    int ret = ta_vo_dev_set_attr(dev_ctx_, &dev_attr);
    if (ret != 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "ta_vo_dev_set_attr failed: %d", ret);
        return false;
    }

    return true;
}

bool TacoDisplayContext::initLayer() {
    layer_ctx_ = ta_vo_layer_create(TA_VO_LAYER_VIDEO_0);
    if (!layer_ctx_) {
        LOG4CPLUS_ERROR(logger_, "ta_vo_layer_create(TA_VO_LAYER_VIDEO_0) failed");
        return false;
    }

    ta_vo_layer_attr layer_attr;
    std::memset(&layer_attr, 0, sizeof(layer_attr));
    layer_attr.priority = 0;
    layer_attr.rect.pos.x = 0;
    layer_attr.rect.pos.y = 0;
    layer_attr.rect.size.width = static_cast<unsigned int>(config_.screen_width);
    layer_attr.rect.size.height = static_cast<unsigned int>(config_.screen_height);
    layer_attr.format = config_.frame_format;
    layer_attr.use_av_frame = 1;

    int ret = ta_vo_layer_bind_to_dev(layer_ctx_, dev_ctx_);
    if (ret != 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "ta_vo_layer_bind_to_dev failed: %d", ret);
        return false;
    }

    ret = ta_vo_layer_set_attr(layer_ctx_, &layer_attr);
    if (ret != 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "ta_vo_layer_set_attr failed: %d", ret);
        return false;
    }

    return true;
}

bool TacoDisplayContext::createChannel(int index, int ch_x, int ch_y, int ch_w, int ch_h) {
    ta_vo_chn_ctx* chn_ctx = ta_vo_chn_create(static_cast<ta_vo_chn>(index));
    if (!chn_ctx) {
        LOG4CPLUS_ERROR_FMT(logger_, "ta_vo_chn_create(%d) failed", index);
        return false;
    }

    ta_vo_chn_attr chn_attr;
    std::memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.rect.pos.x = static_cast<unsigned int>(ch_x);
    chn_attr.rect.pos.y = static_cast<unsigned int>(ch_y);
    chn_attr.rect.size.width = static_cast<unsigned int>(ch_w);
    chn_attr.rect.size.height = static_cast<unsigned int>(ch_h);
    chn_attr.fps = config_.target_fps;

    int ret = ta_vo_chn_set_attr(chn_ctx, &chn_attr);
    if (ret != 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "ta_vo_chn_set_attr(%d) failed: %d", index, ret);
        ta_vo_chn_destroy(chn_ctx);
        return false;
    }

    ret = ta_vo_chn_bind_to_layer(chn_ctx, layer_ctx_);
    if (ret != 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "ta_vo_chn_bind_to_layer(%d) failed: %d", index, ret);
        ta_vo_chn_destroy(chn_ctx);
        return false;
    }

    channels_[index].channel_id = index;
    channels_[index].chn_ctx = chn_ctx;
    channels_[index].active = true;

    return true;
}

int TacoDisplayContext::allocateChannel() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (next_channel_ >= static_cast<int>(channels_.size())) {
        LOG4CPLUS_ERROR_FMT(logger_, "No more channels available (max=%zu)", channels_.size());
        return -1;
    }

    int ch = next_channel_++;
    auto& state = channels_[ch];

    if (!state.active || !state.chn_ctx) {
        LOG4CPLUS_ERROR_FMT(logger_, "Channel %d is not active", ch);
        return -1;
    }

    if (!allocateFramePool(state)) {
        LOG4CPLUS_ERROR_FMT(logger_, "Failed to allocate frame pool for channel %d", ch);
        return -1;
    }

    LOG4CPLUS_INFO_FMT(logger_, "Channel %d allocated (pool_size=%d)", ch, config_.frame_pool_size);
    return ch;
}

void TacoDisplayContext::releaseChannel(int channel_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (channel_id < 0 || channel_id >= static_cast<int>(channels_.size())) {
        return;
    }

    LOG4CPLUS_DEBUG_FMT(logger_, "Channel %d marked for release (deferred to destructor)", channel_id);
}

bool TacoDisplayContext::allocateFramePool(ChannelState& ch) {
    size_t pool_size = static_cast<size_t>(config_.frame_pool_size);
    ch.frame_pool = std::make_unique<FrameSlot[]>(pool_size);
    ch.pool_size = pool_size;

    uint64_t frame_size;
    if (config_.frame_format == TA_AV_PIX_FMT_NV12) {
        frame_size = static_cast<uint64_t>(config_.frame_width) * config_.frame_height * 3 / 2;
    } else {
        frame_size = static_cast<uint64_t>(config_.frame_width) * config_.frame_height * 4;
    }

    for (size_t i = 0; i < pool_size; i++) {
        auto& slot = ch.frame_pool[i];
        slot.size = frame_size;
        slot.ever_sent = false;

        slot.vo_frame = static_cast<ta_vo_frame*>(std::calloc(1, sizeof(ta_vo_frame)));
        slot.av_frame = static_cast<ta_avframe_t*>(std::calloc(1, sizeof(ta_avframe_t)));
        if (!slot.vo_frame || !slot.av_frame) {
            LOG4CPLUS_ERROR_FMT(logger_, "malloc failed for ch%d slot%zu", ch.channel_id, i);
            return false;
        }

        slot.blk_id = taco_sys_get_block(TACO_INVALID_POOLID, frame_size, "taco_vo_ctx");
        if (slot.blk_id == 0) {
            LOG4CPLUS_ERROR_FMT(logger_, "taco_sys_get_block failed for ch%d slot%zu (size=%llu)",
                                ch.channel_id, i, (unsigned long long)frame_size);
            return false;
        }

        slot.phys_addr = taco_sys_handle2_phys_addr(slot.blk_id);
        slot.virt_addr = static_cast<uint8_t*>(
            taco_sys_mmap_noncache(slot.phys_addr, static_cast<uint32_t>(frame_size)));

        if (!slot.virt_addr) {
            LOG4CPLUS_ERROR_FMT(logger_, "taco_sys_mmap_noncache failed for ch%d slot%zu", ch.channel_id, i);
            taco_sys_release_block(slot.blk_id);
            slot.blk_id = 0;
            return false;
        }

        snprintf(slot.str_blk_id, sizeof(slot.str_blk_id), "%u", slot.blk_id);
        slot.dict_entry.value = slot.str_blk_id;
        slot.dict.elems = &slot.dict_entry;

        slot.av_frame->data[0] = slot.virt_addr;
        slot.av_frame->metadata = &slot.dict;
        slot.av_frame->width = config_.frame_width;
        slot.av_frame->height = config_.frame_height;
        slot.av_frame->format = config_.frame_format;

        slot.vo_frame->av_frame = slot.av_frame;
        slot.vo_frame->frame = nullptr;
    }

    return true;
}

void TacoDisplayContext::freeFramePool(ChannelState& ch) {
    for (size_t i = 0; i < ch.pool_size; i++) {
        auto& slot = ch.frame_pool[i];
        if (slot.virt_addr) {
            taco_sys_munmap(slot.virt_addr, static_cast<uint32_t>(slot.size));
            slot.virt_addr = nullptr;
        }
        if (slot.blk_id != 0) {
            taco_sys_release_block(slot.blk_id);
            slot.blk_id = 0;
        }
        std::free(slot.av_frame);
        slot.av_frame = nullptr;
        std::free(slot.vo_frame);
        slot.vo_frame = nullptr;
    }
    ch.frame_pool.reset();
    ch.pool_size = 0;
}

TacoDisplayContext::FrameSlot* TacoDisplayContext::acquireFrameSlot(ChannelState& ch) {
    for (size_t i = 0; i < ch.pool_size; i++) {
        auto& slot = ch.frame_pool[i];
        if (!slot.ever_sent) {
            return &slot;
        }
        if (ta_vo_chn_get_frame_status(ch.chn_ctx, slot.vo_frame) == TA_VO_CHN_FRAME_STATUS_IDLE) {
            return &slot;
        }
    }
    return nullptr;
}

bool TacoDisplayContext::sendFrame(int channel_id, Buffer* buffer) {
    if (channel_id < 0 || channel_id >= static_cast<int>(channels_.size())) {
        return false;
    }
    if (!buffer) {
        return false;
    }

    auto& ch = channels_[channel_id];
    if (!ch.active || ch.pool_size == 0 || !ch.chn_ctx) {
        return false;
    }

    FrameSlot* slot = acquireFrameSlot(ch);
    if (!slot) {
        return false;
    }

    void* src = buffer->getVirtualAddress();
    if (!src) {
        return false;
    }

    size_t copy_size = std::min(buffer->size(), slot->size);
    std::memcpy(slot->virt_addr, src, copy_size);

    int ret = ta_vo_chn_send_frame(ch.chn_ctx, slot->vo_frame);
    if (ret != 0) {
        return false;
    }

    slot->ever_sent = true;
    return true;
}
