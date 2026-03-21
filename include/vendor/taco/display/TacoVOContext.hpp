#ifndef TACO_VO_CONTEXT_HPP
#define TACO_VO_CONTEXT_HPP

#include "productionline/worker/WorkerConfig.hpp"
#include "buffer/bufferpool/Buffer.hpp"

#include <mutex>
#include <vector>
#include <memory>
#include <cstdint>

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

extern "C" {
#include "ta_vo_common.h"
#include "ta_vo_dev.h"
#include "ta_vo_layer.h"
#include "ta_vo_channel.h"
#include "taco_sys_api.h"
}

/**
 * TacoVOContext - taco-vo 共享资源的 RAII 容器
 *
 * 管理 taco-vo 的全局共享资源（device、layer），
 * 构造时初始化，析构时自动清理。
 *
 * 架构层级：Device → Layer → Channel
 * 每个通道的帧池和通道分配也由此类管理。
 * TacoVODisplayDevice 通过 shared_ptr 持有此对象以保活。
 *
 * 线程安全：所有公共方法内部加锁。
 */
class TacoVOContext {
public:
    using TacoVOConfig = WorkerConfig::ConsumerTypeConfig::DisplayType::TacoVOConfig;

    explicit TacoVOContext(const TacoVOConfig& config);
    ~TacoVOContext();

    TacoVOContext(const TacoVOContext&) = delete;
    TacoVOContext& operator=(const TacoVOContext&) = delete;

    /**
     * 分配一个通道（创建 channel + frame pool，绑定到 layer）
     * @return channel_id (0-based)，失败返回 -1
     */
    int allocateChannel();

    /**
     * 释放一个通道的帧池资源
     */
    void releaseChannel(int channel_id);

    /**
     * 向指定通道发送一帧（从 Buffer 拷贝到 DMA 内存后发送）
     * 线程安全：多个通道可并发调用
     */
    bool sendFrame(int channel_id, Buffer* buffer);

    int getScreenWidth() const { return config_.screen_width; }
    int getScreenHeight() const { return config_.screen_height; }

private:
    struct FrameSlot {
        ta_vo_frame* vo_frame = nullptr;
        ta_avframe_t* av_frame = nullptr;
        uint32_t blk_id = 0;
        uint64_t phys_addr = 0;
        uint8_t* virt_addr = nullptr;
        uint64_t size = 0;
        bool ever_sent = false;
        char str_blk_id[16] = {};
        TA_AVDictionaryEntry dict_entry = {};
        TA_AVDictionary dict = {};
    };

    struct ChannelState {
        int channel_id = -1;
        ta_vo_chn_ctx* chn_ctx = nullptr;
        bool active = false;
        std::unique_ptr<FrameSlot[]> frame_pool;
        size_t pool_size = 0;
    };

    bool initDevice();
    bool initLayer();
    bool createChannel(int index, int ch_x, int ch_y, int ch_w, int ch_h);
    bool allocateFramePool(ChannelState& ch);
    void freeFramePool(ChannelState& ch);
    FrameSlot* acquireFrameSlot(ChannelState& ch);

    TacoVOConfig config_;
    std::mutex mutex_;
    int next_channel_ = 0;

    ta_vo_dev_ctx* dev_ctx_ = nullptr;
    ta_vo_layer_ctx* layer_ctx_ = nullptr;
    bool layer_enabled_ = false;

    std::vector<ChannelState> channels_;

    int grid_cols_ = 3;
    int grid_rows_ = 3;

    log4cplus::Logger logger_;
};

#endif // TACO_VO_CONTEXT_HPP
