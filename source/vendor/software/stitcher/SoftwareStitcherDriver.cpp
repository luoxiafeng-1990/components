#include "vendor/software/stitcher/SoftwareStitcherDriver.hpp"
#include "bufferpool/buffer/Buffer.hpp"
#include "common/ImageMeta.hpp"

#include <cstring>
#include <cstdint>
#include <algorithm>

// ============================================================
// Construction
// ============================================================

SoftwareStitcherDriver::SoftwareStitcherDriver()
    : logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.SoftwareStitcherDriver")))
{}

// ============================================================
// stitch — nearest-neighbour resize into a sub-region (NV12)
// ============================================================

bool SoftwareStitcherDriver::stitch(
    Buffer* src, Buffer* dst,
    int dst_x, int dst_y, int dst_w, int dst_h,
    int src_width, int src_height,
    int screen_width, int screen_height,
    int /*src_format*/)
{
    if (!src || !dst) return false;

    auto meta = ImageMeta::fromBuffer(src);
    if (!meta.isValid()) {
        LOG4CPLUS_WARN(logger_, "stitch: invalid source ImageMeta");
        return false;
    }

    // Source pointers
    const uint8_t* src_y_plane = meta.planeData(0);
    const uint8_t* src_uv_plane = meta.planeData(1);
    int src_y_stride  = meta.linesize(0);
    int src_uv_stride = meta.linesize(1);

    if (!src_y_plane || !src_uv_plane || src_y_stride <= 0) {
        LOG4CPLUS_WARN(logger_, "stitch: source plane data missing");
        return false;
    }

    // Destination pointers (NV12: Y then UV)
    uint8_t* dst_base = static_cast<uint8_t*>(dst->getVirtualAddress());
    if (!dst_base) return false;

    int dst_y_stride  = screen_width;
    int dst_uv_stride = screen_width;
    uint8_t* dst_y_plane  = dst_base;
    uint8_t* dst_uv_plane = dst_base + static_cast<size_t>(screen_width) * screen_height;

    // Nearest-neighbour resize: Y plane
    for (int dy = 0; dy < dst_h; ++dy) {
        int sy = dy * src_height / dst_h;
        sy = std::min(sy, src_height - 1);
        const uint8_t* src_row = src_y_plane + sy * src_y_stride;
        uint8_t* dst_row = dst_y_plane + (dst_y + dy) * dst_y_stride + dst_x;

        for (int dx = 0; dx < dst_w; ++dx) {
            int sx = dx * src_width / dst_w;
            sx = std::min(sx, src_width - 1);
            dst_row[dx] = src_row[sx];
        }
    }

    // Nearest-neighbour resize: UV plane (interleaved, half resolution)
    int dst_uv_h = dst_h / 2;
    int dst_uv_w = dst_w;         // UV width in bytes = dst_w (pairs of U,V)
    int src_uv_h = src_height / 2;
    int src_uv_w = src_width;

    for (int dy = 0; dy < dst_uv_h; ++dy) {
        int sy = dy * src_uv_h / dst_uv_h;
        sy = std::min(sy, src_uv_h - 1);
        const uint8_t* src_row = src_uv_plane + sy * src_uv_stride;
        uint8_t* dst_row = dst_uv_plane + (dst_y / 2 + dy) * dst_uv_stride + (dst_x & ~1);

        for (int dx = 0; dx < dst_uv_w / 2; ++dx) {
            int sx = dx * (src_uv_w / 2) / (dst_uv_w / 2);
            sx = std::min(sx, src_uv_w / 2 - 1);
            // Copy U and V together (interleaved NV12)
            dst_row[dx * 2]     = src_row[sx * 2];
            dst_row[dx * 2 + 1] = src_row[sx * 2 + 1];
        }
    }

    return true;
}

// ============================================================
// copy — full buffer memcpy (NV12)
// ============================================================

bool SoftwareStitcherDriver::copy(
    Buffer* src, Buffer* dst,
    int screen_width, int screen_height)
{
    if (!src || !dst) return false;

    uint8_t* src_virt = static_cast<uint8_t*>(src->getVirtualAddress());
    uint8_t* dst_virt = static_cast<uint8_t*>(dst->getVirtualAddress());
    if (!src_virt || !dst_virt) return false;

    // NV12: Y plane = W*H, UV plane = W*H/2, total = W*H*3/2
    size_t total_size = static_cast<size_t>(screen_width) * screen_height * 3 / 2;
    memcpy(dst_virt, src_virt, total_size);

    return true;
}

// ============================================================
// copyRegion — row-by-row memcpy of a rectangular region (NV12)
// ============================================================

bool SoftwareStitcherDriver::copyRegion(
    Buffer* src, Buffer* dst,
    int x, int y, int w, int h,
    int screen_width, int screen_height)
{
    if (!src || !dst) return false;

    uint8_t* src_base = static_cast<uint8_t*>(src->getVirtualAddress());
    uint8_t* dst_base = static_cast<uint8_t*>(dst->getVirtualAddress());
    if (!src_base || !dst_base) return false;

    size_t y_plane_size = static_cast<size_t>(screen_width) * screen_height;

    // Y plane: copy rows [y, y+h)
    for (int row = y; row < y + h; ++row) {
        size_t offset = static_cast<size_t>(row) * screen_width + x;
        memcpy(dst_base + offset, src_base + offset, w);
    }

    // UV plane: copy rows [y/2, y/2 + h/2)
    uint8_t* src_uv = src_base + y_plane_size;
    uint8_t* dst_uv = dst_base + y_plane_size;
    int uv_x = x & ~1;  // align to even for NV12 chroma
    int uv_w = (w + (x - uv_x) + 1) & ~1;  // round up to even
    int uv_y = y / 2;
    int uv_h = h / 2;

    for (int row = uv_y; row < uv_y + uv_h; ++row) {
        size_t offset = static_cast<size_t>(row) * screen_width + uv_x;
        memcpy(dst_uv + offset, src_uv + offset, uv_w);
    }

    return true;
}
