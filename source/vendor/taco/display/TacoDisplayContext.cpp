#include "vendor/taco/display/TacoDisplayContext.hpp"
#include "common/ImageMeta.hpp"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <errno.h>

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

    int ch_w = screen_width_ / grid_cols_;
    int ch_h = screen_height_ / grid_rows_;

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
        LOG4CPLUS_INFO_FMT(logger_,
            "TacoDisplayContext: initialized (%dx%d, %d channels, %d fps, grid %dx%d)",
            screen_width_, screen_height_,
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
        frames_release(ch);
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
    // 硬件探测交给 taco-vo（ta_vo_dev_set_attr 内找 fb）
    dev_ctx_ = ta_vo_dev_create(TA_VO_DEV_IDS);
    if (!dev_ctx_) {
        LOG4CPLUS_ERROR(logger_, "ta_vo_dev_create(TA_VO_DEV_IDS) failed");
        return false;
    }

    ta_vo_dev_attr dev_attr;
    int ret = ta_vo_dev_get_attr(dev_ctx_, &dev_attr);
    if (ret != 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "ta_vo_dev_get_attr failed: %d", ret);
        return false;
    }
    // 对齐 ta_vo_test.c::nchn_sample 中对 video layer0 的配置
    dev_attr.fps = config_.target_fps;
    dev_attr.layer[0].format = TA_AV_PIX_FMT_NV12;
    dev_attr.layer[0].on = 1;
    dev_attr.layer[1].on = 0;

    ret = ta_vo_dev_set_attr(dev_ctx_, &dev_attr);
    if (ret != 0) {
        LOG4CPLUS_ERROR_FMT(logger_,
            "ta_vo_dev_set_attr failed: %d (check VO device / taco-vo logs)", ret);
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

    ta_vo_dev_attr dev_attr;
    int ret = ta_vo_dev_get_attr(dev_ctx_, &dev_attr);
    if (ret != 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "initLayer: ta_vo_dev_get_attr failed: %d", ret);
        return false;
    }
    const int fb_w = dev_attr.layer[0].width;
    const int fb_h = dev_attr.layer[0].height;
    if (fb_w <= 0 || fb_h <= 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "initLayer: invalid FB size %dx%d", fb_w, fb_h);
        return false;
    }
    screen_width_ = fb_w;
    screen_height_ = fb_h;

    ta_vo_layer_attr layer_attr;
    std::memset(&layer_attr, 0, sizeof(layer_attr));
    layer_attr.width = screen_width_;
    layer_attr.height = screen_height_;
    layer_attr.format = config_.frame_format;
    layer_attr.use_av_frame = 1;
    layer_attr.fps = config_.target_fps;

    ret = ta_vo_layer_bind_to_dev(layer_ctx_, dev_ctx_);
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
    // 对齐 ta_vo_test.c：ta_vo_chn_create → set_attr → bind_to_layer
    ta_vo_chn_ctx* chn_ctx = ta_vo_chn_create(static_cast<ta_vo_chn>(index));
    if (!chn_ctx) {
        LOG4CPLUS_ERROR_FMT(logger_, "ta_vo_chn_create(%d) failed", index);
        return false;
    }

    ta_vo_chn_attr chn_attr;
    std::memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.x = ch_x;
    chn_attr.y = ch_y;
    chn_attr.width = ch_w;
    chn_attr.height = ch_h;
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

    LOG4CPLUS_INFO_FMT(logger_, "Channel %d allocated", ch);
    return ch;
}

void TacoDisplayContext::releaseChannel(int channel_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (channel_id < 0 || channel_id >= static_cast<int>(channels_.size())) {
        return;
    }
    LOG4CPLUS_DEBUG_FMT(logger_, "Channel %d marked for release (deferred to destructor)",
                        channel_id);
}

/*
 * 移植自 packages/taco-vo/test/ta_vo_test.c::frame_init
 *
 * DMA：taco_sys_* 对应 test 的 DmabufHeapAlloc / GetPhysAddr / mmap(blk_id)
 */
int TacoDisplayContext::frame_init(ChannelState& ch, int idx, unsigned int width,
                                   unsigned int height, int format) {
    if (idx < 0 || idx >= ch.frame_count) {
        return -EINVAL;
    }

    ch.frame[idx] = static_cast<ta_vo_frame*>(std::malloc(sizeof(ta_vo_frame)));
    if (!ch.frame[idx]) {
        LOG4CPLUS_ERROR(logger_, "malloc failed for frame");
        return -ENOMEM;
    }
    std::memset(ch.frame[idx], 0, sizeof(ta_vo_frame));

    ch.frame[idx]->av_frame = static_cast<ta_avframe_t*>(std::malloc(sizeof(ta_avframe_t)));
    if (!ch.frame[idx]->av_frame) {
        LOG4CPLUS_ERROR(logger_, "malloc failed for frame");
        return -ENOMEM;
    }
    std::memset(ch.frame[idx]->av_frame, 0, sizeof(ta_avframe_t));

    ch.frame_info[idx] = static_cast<frame_info_t*>(std::malloc(sizeof(frame_info_t)));
    if (!ch.frame_info[idx]) {
        LOG4CPLUS_ERROR(logger_, "malloc failed for frame_info");
        return -ENOMEM;
    }
    std::memset(ch.frame_info[idx], 0, sizeof(frame_info_t));

    if (format == TA_AV_PIX_FMT_NV12) {
        ch.frame_info[idx]->size = static_cast<unsigned long>(width) * height * 3 / 2;
    } else {
        ch.frame_info[idx]->size = static_cast<unsigned long>(width) * height * 4;
    }

    ch.frame_info[idx]->blk_id = taco_sys_get_block(
        TACO_INVALID_POOLID, ch.frame_info[idx]->size, "taco_vo_ctx");
    if (ch.frame_info[idx]->blk_id == 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "taco sys get block size[%lu] failed.",
                            ch.frame_info[idx]->size);
        return -ENOMEM;
    }

    ch.frame_info[idx]->phys_addr = static_cast<unsigned long>(
        taco_sys_handle2_phys_addr(ch.frame_info[idx]->blk_id));

    ch.frame_info[idx]->virt_addr = static_cast<unsigned char*>(
        taco_sys_mmap_noncache(ch.frame_info[idx]->phys_addr,
                               static_cast<uint32_t>(ch.frame_info[idx]->size)));
    if (!ch.frame_info[idx]->virt_addr) {
        LOG4CPLUS_ERROR(logger_, "taco_sys_mmap_noncache failed");
        taco_sys_release_block(ch.frame_info[idx]->blk_id);
        ch.frame_info[idx]->blk_id = 0;
        return -ENOMEM;
    }

    std::snprintf(ch.frame_info[idx]->str_blk_id, sizeof(ch.frame_info[idx]->str_blk_id),
                  "%u", ch.frame_info[idx]->blk_id);
    ch.frame_info[idx]->elems.value = ch.frame_info[idx]->str_blk_id;
    ch.frame_info[idx]->metadata.elems = &ch.frame_info[idx]->elems;

    ch.frame[idx]->av_frame->data[0] = ch.frame_info[idx]->virt_addr;
    ch.frame[idx]->av_frame->metadata = &ch.frame_info[idx]->metadata;
    ch.frame[idx]->av_frame->width = static_cast<int>(width);
    ch.frame[idx]->av_frame->height = static_cast<int>(height);
    ch.frame[idx]->av_frame->format = format;
    return 0;
}

void TacoDisplayContext::frame_release(ChannelState& ch, int idx) {
    if (idx < 0 || idx >= static_cast<int>(ch.frame_info.size())) {
        return;
    }

    if (ch.frame_info[idx]) {
        if (ch.frame_info[idx]->virt_addr) {
            taco_sys_munmap(ch.frame_info[idx]->virt_addr,
                            static_cast<uint32_t>(ch.frame_info[idx]->size));
            ch.frame_info[idx]->virt_addr = nullptr;
        }
        if (ch.frame_info[idx]->blk_id != 0) {
            taco_sys_release_block(ch.frame_info[idx]->blk_id);
            ch.frame_info[idx]->blk_id = 0;
        }
        std::free(ch.frame_info[idx]);
        ch.frame_info[idx] = nullptr;
    }

    if (idx < static_cast<int>(ch.frame.size()) && ch.frame[idx]) {
        std::free(ch.frame[idx]->av_frame);
        ch.frame[idx]->av_frame = nullptr;
        std::free(ch.frame[idx]);
        ch.frame[idx] = nullptr;
    }
}

void TacoDisplayContext::frames_release(ChannelState& ch) {
    for (int i = 0; i < ch.frame_count; i++) {
        frame_release(ch, i);
    }
    ch.frame.clear();
    ch.frame_info.clear();
    ch.frame_count = 0;
    ch.frame_idx = 0;
    ch.frame_width = 0;
    ch.frame_height = 0;
}

int TacoDisplayContext::frames_prepare(ChannelState& ch, unsigned int width,
                                       unsigned int height, int format) {
    if (ch.frame_count > 0 &&
        ch.frame_width == static_cast<int>(width) &&
        ch.frame_height == static_cast<int>(height)) {
        return 0;
    }

    frames_release(ch);

    // 对齐 test：for (i = 0; i < TEST_FRAME_COUNT; i++) frame_init(i, w, h, fmt);
    ch.frame_count = config_.frame_pool_size > 0 ? config_.frame_pool_size : 4;
    ch.frame.assign(static_cast<size_t>(ch.frame_count), nullptr);
    ch.frame_info.assign(static_cast<size_t>(ch.frame_count), nullptr);
    ch.frame_idx = 0;
    ch.frame_width = static_cast<int>(width);
    ch.frame_height = static_cast<int>(height);

    for (int i = 0; i < ch.frame_count; i++) {
        int ret = frame_init(ch, i, width, height, format);
        if (ret != 0) {
            LOG4CPLUS_ERROR_FMT(logger_, "ch[%d] construct frame[%d] failed.",
                                ch.channel_id, i);
            frames_release(ch);
            return ret;
        }
    }
    return 0;
}

bool TacoDisplayContext::sendFrame(int channel_id, Buffer* buffer) {
    if (channel_id < 0 || channel_id >= static_cast<int>(channels_.size())) {
        return false;
    }
    if (!buffer) {
        return false;
    }

    auto& ch = channels_[channel_id];
    if (!ch.active || !ch.chn_ctx) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    ImageMeta meta = ImageMeta::fromBuffer(buffer);
    if (!meta.isValid()) {
        return false;
    }

    if (frames_prepare(ch,
                       static_cast<unsigned int>(meta.width()),
                       static_cast<unsigned int>(meta.height()),
                       config_.frame_format) != 0) {
        return false;
    }

    // 对齐 video_frame_send_thread：轮转 frame[i]
    int i = ch.frame_idx % ch.frame_count;
    ch.frame_idx++;

    if (!ch.frame[i] || !ch.frame_info[i] || !ch.frame_info[i]->virt_addr) {
        return false;
    }

    void* src = buffer->getVirtualAddress();
    if (!src) {
        return false;
    }

    // 对齐 load_test_data：把像素写入 frame_info[i]->virt_addr
    size_t copy_size = std::min(buffer->size(),
                                static_cast<size_t>(ch.frame_info[i]->size));
    std::memcpy(ch.frame_info[i]->virt_addr, src, copy_size);

    ch.frame[i]->av_frame->width = meta.width();
    ch.frame[i]->av_frame->height = meta.height();

    // 对齐 ta_vo_test.c：ta_vo_chn_send_frame(chn_ctx, frame[i])
    return ta_vo_chn_send_frame(ch.chn_ctx, ch.frame[i]) == 0;
}
