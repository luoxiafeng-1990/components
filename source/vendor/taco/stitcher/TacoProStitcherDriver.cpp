#include "vendor/taco/stitcher/TacoProStitcherDriver.hpp"
#include "bufferpool/buffer/Buffer.hpp"

#include <cstring>

extern "C" {
#include <libavutil/pixfmt.h>
}

// ============================================================
// Construction
// ============================================================

TacoProStitcherDriver::TacoProStitcherDriver()
    : logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.TacoProStitcherDriver")))
{}

// ============================================================
// Helpers
// ============================================================

ta_image_format_ext_t TacoProStitcherDriver::getTacoCvFormat(int av_format) {
    switch (av_format) {
        case 23: return FORMAT_NV12;       // AV_PIX_FMT_NV12
        case 24: return FORMAT_NV21;       // AV_PIX_FMT_NV21
        case 0:  return FORMAT_YUV420P;    // AV_PIX_FMT_YUV420P
        case 2:  return FORMAT_RGB_PACKED; // AV_PIX_FMT_RGB24
        case 3:  return FORMAT_BGR_PACKED; // AV_PIX_FMT_BGR24
        default: return FORMAT_NV12;
    }
}

int TacoProStitcherDriver::getBlkIdFromAVFrame(AVFrame* avframe) {
    if (avframe && avframe->metadata) {
        auto meta = reinterpret_cast<TA_AVDictionary*>(avframe->metadata);
        if (meta && meta->count > 0 && meta->elems) {
            for (int i = 0; i < meta->count; ++i) {
                if (meta->elems[i].key && strcmp(meta->elems[i].key, "pool_blk_id") == 0) {
                    int val = std::atoi(meta->elems[i].value);
                    if (val > 0) return val;
                }
            }
            int val = std::atoi(meta->elems[0].value);
            if (val > 0) return val;
        }
    }
    // Don't use a fallback: Buffer's internal ID is not a valid DMA block ID;
    // passing it to ta_cv_image_create_ext would cause libmm to print ERROR
    return -1;
}

// ============================================================
// stitch (resize into sub-region)
// ============================================================

bool TacoProStitcherDriver::stitch(
    Buffer* src, Buffer* dst,
    int dst_x, int dst_y, int dst_w, int dst_h,
    int src_width, int src_height,
    int screen_width, int screen_height,
    int src_format)
{
    (void)src_format; // format is read from AVFrame directly

    AVFrame* avframe_in = src->getAVFrame();
    if (!avframe_in) {
        LOG4CPLUS_WARN(logger_, "stitch: decoded buffer has no AVFrame");
        return false;
    }

    int src_blk_id = getBlkIdFromAVFrame(avframe_in);
    if (src_blk_id <= 0) {
        LOG4CPLUS_WARN(logger_, "stitch: could not resolve src dmabuf blk_id");
        return false;
    }

    ta_image_t image_in = {};
    ta_image_t image_out = {};

    LOG4CPLUS_DEBUG_FMT(logger_,
        "stitch: creating input image: %dx%d fmt=%d blk_id=%d",
        avframe_in->width, avframe_in->height,
        avframe_in->format, src_blk_id);

    tacv_status_t ret = ta_cv_image_create_ext(
        avframe_in->height,
        avframe_in->width,
        getTacoCvFormat(avframe_in->format),
        &image_in,
        src_blk_id
    );
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_, "stitch: ta_cv_image_create_ext(input) failed: %d", ret);
        return false;
    }

    LOG4CPLUS_DEBUG_FMT(logger_,
        "stitch: creating output image: %dx%d fmt=NV12 blk_id=%u",
        screen_width, screen_height, dst->id());

    ret = ta_cv_image_create_ext(
        screen_height,
        screen_width,
        FORMAT_NV12,
        &image_out,
        dst->id()
    );
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_, "stitch: ta_cv_image_create_ext(output) failed: %d", ret);
        ta_cv_image_destroy_ext(&image_in);
        return false;
    }

    ta_cv_rect_t src_crop = {};
    src_crop.start_x = 0;
    src_crop.start_y = 0;
    src_crop.crop_w  = src_width;
    src_crop.crop_h  = src_height;

    ta_cv_rect_t dst_crop = {};
    dst_crop.start_x = dst_x;
    dst_crop.start_y = dst_y;
    dst_crop.crop_w  = dst_w;
    dst_crop.crop_h  = dst_h;

    ret = ta_cv_image_stitch(1, &image_in, image_out,
                             &dst_crop, &src_crop, TA_CV_INTER_LINEAR);
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_,
            "stitch: ta_cv_image_stitch FAILED: ret=%d dst=(%d,%d,%d,%d) blk=%u",
            ret, dst_x, dst_y, dst_w, dst_h, dst->id());
    }

    ta_cv_image_destroy_ext(&image_in);
    ta_cv_image_destroy_ext(&image_out);

    return (ret == 0);
}

// ============================================================
// copy (full buffer)
// ============================================================

bool TacoProStitcherDriver::copy(
    Buffer* src, Buffer* dst,
    int screen_width, int screen_height)
{
    if (!src || !dst) {
        LOG4CPLUS_ERROR_FMT(logger_, "copy: null buffer src=%p dst=%p",
            (void*)src, (void*)dst);
        return false;
    }

    ta_image_t image_in = {};
    ta_image_t image_out = {};

    tacv_status_t ret = ta_cv_image_create_ext(
        screen_height,
        screen_width,
        FORMAT_NV12,
        &image_in,
        src->id()
    );
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_, "copy: ta_cv_image_create_ext(input) failed: %d", ret);
        return false;
    }

    ret = ta_cv_image_create_ext(
        screen_height,
        screen_width,
        FORMAT_NV12,
        &image_out,
        dst->id()
    );
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_, "copy: ta_cv_image_create_ext(output) failed: %d", ret);
        ta_cv_image_destroy_ext(&image_in);
        return false;
    }

    ta_cv_copy_to_t copy_attr = {};
    copy_attr.start_x = 0;
    copy_attr.start_y = 0;
    ret = ta_cv_image_copy_to(image_in, image_out, copy_attr);
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_, "copy: ta_cv_image_copy_to FAILED: ret=%d", ret);
    }

    ta_cv_image_destroy_ext(&image_in);
    ta_cv_image_destroy_ext(&image_out);

    return (ret == 0);
}

// ============================================================
// copyRegion (rectangular region, same position src → dst)
// ============================================================

bool TacoProStitcherDriver::copyRegion(
    Buffer* src, Buffer* dst,
    int x, int y, int w, int h,
    int screen_width, int screen_height)
{
    if (!src || !dst) return false;

    ta_image_t image_in = {}, image_out = {};
    tacv_status_t ret = ta_cv_image_create_ext(
        screen_height,
        screen_width,
        FORMAT_NV12,
        &image_in,
        src->id()
    );
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_,
            "copyRegion: ta_cv_image_create_ext(input) failed: %d", ret);
        return false;
    }

    ret = ta_cv_image_create_ext(
        screen_height,
        screen_width,
        FORMAT_NV12,
        &image_out,
        dst->id()
    );
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_,
            "copyRegion: ta_cv_image_create_ext(output) failed: %d", ret);
        ta_cv_image_destroy_ext(&image_in);
        return false;
    }

    ta_cv_rect_t src_crop = {};
    src_crop.start_x = x;
    src_crop.start_y = y;
    src_crop.crop_w  = w;
    src_crop.crop_h  = h;

    ta_cv_rect_t dst_crop = {};
    dst_crop.start_x = x;
    dst_crop.start_y = y;
    dst_crop.crop_w  = w;
    dst_crop.crop_h  = h;

    ret = ta_cv_image_stitch(1, &image_in, image_out,
                             &dst_crop, &src_crop, TA_CV_INTER_NONE);
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_,
            "copyRegion: ta_cv_image_stitch failed: ret=%d (region %d,%d %dx%d)",
            ret, x, y, w, h);
    }

    ta_cv_image_destroy_ext(&image_in);
    ta_cv_image_destroy_ext(&image_out);

    return (ret == 0);
}
