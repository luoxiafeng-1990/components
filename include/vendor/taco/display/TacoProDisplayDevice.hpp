#ifndef TACO_PRO_DISPLAY_DEVICE_HPP
#define TACO_PRO_DISPLAY_DEVICE_HPP

#include "vendor/contracts/IDisplayDevice.hpp"
#include "vendor/taco/display/TacoProDisplayExtension.hpp"
#include "bufferpool/buffer/Buffer.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"
#include "bufferpool/pool/base/IBufferPoolBuilder.hpp"
#include "bufferpool/pool/builder/BufferPoolBuilderFactory.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include "consumptionline/types/stitcher/FrameStitcherService.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <cstdint>

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

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
    struct SharedState {
        TacoProDisplayExtension config;
        int fd = -1;
        int fb_index = 0;
        int screen_width = 1920;
        int screen_height = 1080;
        int bits_per_pixel = 32;
        size_t buffer_size = 0;
        int buffer_count = 4;

        std::unique_ptr<IBufferPoolBuilder> builder;
        uint64_t fb_pool_id = 0;
        uint32_t template_blk_id = 0;

        std::shared_ptr<FrameStitcherService> stitcher;
        std::unique_ptr<class TacoProOsdOverlay> osd;

        log4cplus::Logger logger;

        explicit SharedState(const TacoProDisplayExtension& cfg);
        ~SharedState();

        bool open();
        void close();
        bool openDevice();
        bool createBufferPool();
        std::shared_ptr<BufferPool> getBufferPool();

        int registerChannel();
        int registerChannel(const ChannelLayout& layout);
        void unregisterChannel(int channel_id);
        bool channelWrite(int channel_id, Buffer* decoded);
    };

    TacoProDisplayExtension config_;
    int channel_id_ = -1;
    bool initialized_ = false;
    bool last_display_failed_ = false;

    std::shared_ptr<SharedState> state_;

    static std::mutex s_state_mutex;
    static std::weak_ptr<SharedState> s_shared_state;

    log4cplus::Logger logger_;

public:
    static std::shared_ptr<FrameStitcherService> getStitcher();
};

#endif // TACO_PRO_DISPLAY_DEVICE_HPP
