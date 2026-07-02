#ifndef TACO_PRO_STITCHER_DRIVER_HPP
#define TACO_PRO_STITCHER_DRIVER_HPP

#include "consumptionline/types/stitcher/IFrameStitcherDriver.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

extern "C" {
#include "ta_cv_api_ext_c.h"
#include <libavutil/frame.h>
#include <libavutil/dict.h>
}

/**
 * @brief Hardware-accelerated stitcher driver using TACO PP engine.
 *
 * Wraps ta_cv_image_stitch and ta_cv_image_copy_to for GPU-assisted
 * image resize / copy on TACO SoC platforms.
 */
class TacoProStitcherDriver : public IFrameStitcherDriver {
public:
    TacoProStitcherDriver();
    ~TacoProStitcherDriver() override = default;

    bool stitch(Buffer* src, Buffer* dst,
                int dst_x, int dst_y, int dst_w, int dst_h,
                int src_width, int src_height,
                int screen_width, int screen_height,
                int src_format) override;

    bool copy(Buffer* src, Buffer* dst,
              int screen_width, int screen_height) override;

    bool copyRegion(Buffer* src, Buffer* dst,
                   int x, int y, int w, int h,
                   int screen_width, int screen_height) override;

private:
    static ta_image_format_ext_t getTacoCvFormat(int av_format);
    static int getBlkIdFromAVFrame(AVFrame* avframe);

    log4cplus::Logger logger_;
};

#endif // TACO_PRO_STITCHER_DRIVER_HPP
