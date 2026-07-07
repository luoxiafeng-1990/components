#ifndef SOFTWARE_STITCHER_DRIVER_HPP
#define SOFTWARE_STITCHER_DRIVER_HPP

#include "consumptionline/types/stitcher/IFrameStitcherDriver.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

/**
 * @brief Pure-software (CPU) stitcher driver for NV12 frames.
 *
 * Uses nearest-neighbour resize for stitch() and row-by-row memcpy
 * for copy / copyRegion. Intended as a fallback when hardware
 * acceleration is unavailable.
 */
class SoftwareStitcherDriver : public IFrameStitcherDriver {
public:
    SoftwareStitcherDriver();
    ~SoftwareStitcherDriver() override = default;

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
    log4cplus::Logger logger_;
};

#endif // SOFTWARE_STITCHER_DRIVER_HPP
