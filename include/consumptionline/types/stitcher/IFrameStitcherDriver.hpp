#ifndef IFRAME_STITCHER_DRIVER_HPP
#define IFRAME_STITCHER_DRIVER_HPP

class Buffer;

/**
 * @brief Abstract interface for frame stitching / copy operations.
 *
 * Implementations may use hardware acceleration (e.g. TACO PP engine)
 * or a pure-software fallback (CPU memcpy for NV12).
 */
class IFrameStitcherDriver {
public:
    virtual ~IFrameStitcherDriver() = default;

    /**
     * Stitch (resize) a source buffer into a rectangular region of the
     * destination buffer.
     *
     * @param src          Source buffer (decoded frame with AVFrame payload)
     * @param dst          Destination buffer (render target, full screen NV12)
     * @param dst_x        Destination region X offset
     * @param dst_y        Destination region Y offset
     * @param dst_w        Destination region width
     * @param dst_h        Destination region height
     * @param src_width    Source image width
     * @param src_height   Source image height
     * @param screen_width Full destination image width (for ta_cv_image_create_ext)
     * @param screen_height Full destination image height
     * @param src_format   Source pixel format (AV_PIX_FMT_xxx integer)
     * @return true on success
     */
    virtual bool stitch(Buffer* src, Buffer* dst,
                        int dst_x, int dst_y, int dst_w, int dst_h,
                        int src_width, int src_height,
                        int screen_width, int screen_height,
                        int src_format) = 0;

    /**
     * Full buffer copy (same dimensions, same format).
     *
     * @param src          Source buffer
     * @param dst          Destination buffer
     * @param screen_width Full image width
     * @param screen_height Full image height
     * @return true on success
     */
    virtual bool copy(Buffer* src, Buffer* dst,
                      int screen_width, int screen_height) = 0;

    /**
     * Copy a rectangular region from src to the same position in dst.
     * Used for inheriting template content in missed-frame regions.
     *
     * @param src          Source buffer (template)
     * @param dst          Destination buffer
     * @param x            Region X offset
     * @param y            Region Y offset
     * @param w            Region width
     * @param h            Region height
     * @param screen_width Full image width
     * @param screen_height Full image height
     * @return true on success
     */
    virtual bool copyRegion(Buffer* src, Buffer* dst,
                           int x, int y, int w, int h,
                           int screen_width, int screen_height) = 0;
};

#endif // IFRAME_STITCHER_DRIVER_HPP
