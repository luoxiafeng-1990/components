#include "common/ImageMeta.hpp"
#include "bufferpool/buffer/Buffer.hpp"
#include <opencv2/core/mat.hpp>
#include <iostream>

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

    // 根据 buffer 类型选择提取方式，避免 MatBuffer 中底层 AVFrame 的
    // width/height 与实际 Mat 尺寸不一致（如 hw NV12 场景下 avframe_->height != mat->rows）
    switch (buffer->type()) {
        case Buffer::Type::MAT: {
            cv::Mat* mat = buffer->getMat();
            return fromMat(mat);
        }
        case Buffer::Type::AVFRAME: {
            AVFrame* frame = buffer->getAVFrame();
            return fromAVFrame(frame);
        }
        default:
            AVFrame* frame = buffer->getAVFrame();
            return fromAVFrame(frame);
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
