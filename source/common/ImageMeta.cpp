#include "common/ImageMeta.hpp"
#include "bufferpool/buffer/Buffer.hpp"
#include <opencv2/core/mat.hpp>

// ========== fromAVFrame ==========

ImageMeta ImageMeta::fromAVFrame(const AVFrame* frame) {
    ImageMeta meta;
    if (!frame) return meta;
    
    meta.width_ = frame->width;
    meta.height_ = frame->height;
    meta.format_ = (AVPixelFormat)frame->format;
    memcpy(meta.linesize_, frame->linesize, sizeof(meta.linesize_));
    
    meta.nb_planes_ = 0;
    for (int i = 0; i < 4; i++) {
        if (frame->data[i] != nullptr) {
            meta.nb_planes_ = i + 1;
            meta.plane_data_[i] = frame->data[i];
        }
    }
    
    return meta;
}

// ========== fromBuffer ==========

ImageMeta ImageMeta::fromBuffer(const Buffer* buffer) {
    if (!buffer) return {};
    
    // 优先从 AVFrame 提取（AVFrameBuffer 和 MatBuffer 都可能有 AVFrame）
    AVFrame* frame = buffer->getAVFrame();
    if (frame) return fromAVFrame(frame);
    
    // Mat 次之
    cv::Mat* mat = buffer->getMat();
    if (mat && !mat->empty()) {
        return fromMat(mat);
    }
    
    return {};
}

// ========== fromMat ==========

ImageMeta ImageMeta::fromMat(const cv::Mat* mat) {
    ImageMeta meta;
    if (!mat || mat->empty()) return meta;
    
    meta.width_ = mat->cols;
    meta.height_ = mat->rows;
    meta.nb_planes_ = 1;
    meta.plane_data_[0] = mat->data;
    meta.linesize_[0] = static_cast<int>(mat->step[0]);
    // format 保持 AV_PIX_FMT_NONE，通过 FormatInfo.is_mat 标识
    // channels 信息通过 linesize_[0] / width_ 推算
    return meta;
}
