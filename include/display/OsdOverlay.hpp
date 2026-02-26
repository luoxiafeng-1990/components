#ifndef OSD_OVERLAY_HPP
#define OSD_OVERLAY_HPP

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

#include <ft2build.h>
#include FT_FREETYPE_H

extern "C" {
#include "taco_sys_api.h"
}

struct tpsfb_dma_info;

/**
 * OsdOverlay - 图形层 OSD 叠加显示
 *
 * 在 DSS overlay1 上渲染通道号、时间戳、帧率等文字信息。
 * 使用 ARGB8888 格式，alpha=0 的区域完全透明，不遮挡视频层。
 *
 * 前提：设备树中 overlay-1 的 resolution 需配置为与屏幕一致（如 1920x1080）。
 */
class OsdOverlay {
public:
    struct Config {
        int  refresh_fps = 1;
        std::string font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
        int  font_size = 24;
    };

    OsdOverlay(int screen_width, int screen_height, int max_channels);
    ~OsdOverlay();

    OsdOverlay(const OsdOverlay&) = delete;
    OsdOverlay& operator=(const OsdOverlay&) = delete;

    bool init(const Config& config);
    void shutdown();

    void registerChannel(int channel_id, int x, int y, int w, int h);
    void unregisterChannel(int channel_id);

    void recordFrame(int channel_id);

private:
    struct OsdChannelInfo {
        int channel_id = -1;
        int x = 0, y = 0, w = 0, h = 0;
        bool active = false;
        std::atomic<int> frame_count;
        double current_fps = 0.0;

        OsdChannelInfo() : frame_count(0) {}
        OsdChannelInfo(const OsdChannelInfo& o)
            : channel_id(o.channel_id), x(o.x), y(o.y), w(o.w), h(o.h)
            , active(o.active), frame_count(o.frame_count.load())
            , current_fps(o.current_fps) {}
        OsdChannelInfo& operator=(const OsdChannelInfo& o) {
            if (this != &o) {
                channel_id = o.channel_id; x = o.x; y = o.y; w = o.w; h = o.h;
                active = o.active; frame_count.store(o.frame_count.load());
                current_fps = o.current_fps;
            }
            return *this;
        }
    };

    bool openFbDevice();
    bool allocateDmaMemory();
    void freeDmaMemory();
    bool setupDssOverlay1();
    bool initFreeType(const Config& config);
    void cleanupFreeType();

    void refreshThreadFunc();
    void renderOsd();
    void clearBuffer();
    void drawRect(int x, int y, int w, int h, uint32_t argb_color);
    void drawText(int x, int y, const std::string& text, uint32_t color);
    void drawCharGlyph(int base_x, int base_y, FT_GlyphSlot glyph, uint32_t color);

    int screen_width_;
    int screen_height_;
    int max_channels_;

    int fb_fd_ = -1;

    struct DmaMemory {
        uint32_t blk_id = 0;
        uint64_t phys_addr = 0;
        void*    virt_addr = nullptr;
        size_t   size = 0;
    };
    DmaMemory dma_mem_;

    uint32_t* pixel_buf_ = nullptr;
    uint32_t* shadow_buf_ = nullptr;

    FT_Library ft_lib_ = nullptr;
    FT_Face    ft_face_ = nullptr;

    std::vector<OsdChannelInfo> channels_;
    std::mutex channel_mutex_;

    std::thread refresh_thread_;
    std::atomic<bool> running_{false};
    int timer_fd_ = -1;
    Config config_;

    log4cplus::Logger logger_;
};

#endif // OSD_OVERLAY_HPP
