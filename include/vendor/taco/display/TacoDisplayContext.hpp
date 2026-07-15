#ifndef TACO_DISPLAY_CONTEXT_HPP
#define TACO_DISPLAY_CONTEXT_HPP

#include "vendor/taco/display/TacoDisplayExtension.hpp"
#include "bufferpool/buffer/Buffer.hpp"

#include <mutex>
#include <vector>
#include <cstdint>

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

extern "C" {
#include "ta_vo_common.h"
#include "ta_vo_dev.h"
#include "ta_vo_layer.h"
#include "ta_vo_channel.h"
#include "ta_sys_api.h"
}

/**
 * TacoDisplayContext — 按 taco-vo 送显链路组织资源
 *
 * 链路对齐 packages/taco-vo/test/ta_vo_test.c::nchn_sample：
 *   ta_vo_dev_* → ta_vo_layer_* → ta_vo_chn_* →
 *   frame_init(idx) → ta_vo_chn_send_frame(chn, frame[idx])
 *
 * 每通道一套 frame[] / frame_info[]（test 里是全局共享；业务多路不同内容故按通道持有）。
 */
class TacoDisplayContext {
public:
    explicit TacoDisplayContext(const TacoDisplayExtension& config);
    ~TacoDisplayContext();

    TacoDisplayContext(const TacoDisplayContext&) = delete;
    TacoDisplayContext& operator=(const TacoDisplayContext&) = delete;

    int allocateChannel();
    void releaseChannel(int channel_id);

    /** Buffer → DMA virt → ta_vo_chn_send_frame（对齐 test 送显） */
    bool sendFrame(int channel_id, Buffer* buffer);

    int getScreenWidth() const { return screen_width_; }
    int getScreenHeight() const { return screen_height_; }

private:
    /** 对齐 taco-vo/test/ta_vo_test.c::frame_info_t */
    struct frame_info_t {
        unsigned long size = 0;
        unsigned int blk_id = 0;
        unsigned long phys_addr = 0;
        unsigned char* virt_addr = nullptr;
        TA_AVDictionaryEntry elems = {};
        TA_AVDictionary metadata = {};
        char str_blk_id[16] = {};
    };

    struct ChannelState {
        int channel_id = -1;
        ta_vo_chn_ctx* chn_ctx = nullptr;
        bool active = false;

        // 对齐 ta_vo_test.c：frame[idx] / frame_info[idx]
        std::vector<ta_vo_frame*> frame;
        std::vector<frame_info_t*> frame_info;
        int frame_count = 0;
        int frame_idx = 0;
        int frame_width = 0;
        int frame_height = 0;
    };

    bool initDevice();
    bool initLayer();
    bool createChannel(int index, int ch_x, int ch_y, int ch_w, int ch_h);

    /**
     * 移植自 packages/taco-vo/test/ta_vo_test.c::frame_init
     * @return 0 成功，负值失败
     */
    int frame_init(ChannelState& ch, int idx, unsigned int width,
                   unsigned int height, int format);

    /** 对齐 test release_res_handler 中对 frame_info[i]->blk_id 的释放 */
    void frame_release(ChannelState& ch, int idx);
    void frames_release(ChannelState& ch);

    /**
     * 对齐 test：for (i = 0; i < N; i++) frame_init(i, w, h, fmt);
     * 分辨率变化时重建（test 分辨率固定，业务侧多此一步）。
     */
    int frames_prepare(ChannelState& ch, unsigned int width, unsigned int height,
                       int format);

    TacoDisplayExtension config_;
    std::mutex mutex_;
    int next_channel_ = 0;

    ta_vo_dev_ctx* dev_ctx_ = nullptr;
    ta_vo_layer_ctx* layer_ctx_ = nullptr;
    bool layer_enabled_ = false;

    std::vector<ChannelState> channels_;

    int grid_cols_ = 4;  ///< TA_VO_CHN_MAX=16 → 默认 4x4
    int grid_rows_ = 4;
    int screen_width_ = 0;
    int screen_height_ = 0;

    log4cplus::Logger logger_;
};

#endif // TACO_DISPLAY_CONTEXT_HPP
