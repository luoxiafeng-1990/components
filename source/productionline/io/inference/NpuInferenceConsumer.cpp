/**
 * @file NpuInferenceConsumer.cpp
 * @brief NPU 推理消费者实现
 *
 * 目录: productionline/io/inference/
 * 与其他消费者策略（Display, SaveRaw 等）保持架构一致
 */

#include "productionline/io/inference/NpuInferenceConsumer.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

#include <opencv2/imgproc.hpp>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <unordered_map>

extern "C" {
#include <libavutil/pixfmt.h>
}

static log4cplus::Logger logger() {
    return log4cplus::Logger::getRoot();
}

namespace consumer {

// ============================================================
// COCO 80 类别名称 & 配色（用于 drawDetections）
// ============================================================

static const char* COCO_NAMES[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck",
    "boat", "traffic light", "fire hydrant", "stop sign", "parking meter", "bench",
    "bird", "cat", "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra",
    "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove",
    "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup",
    "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush"
};

static const cv::Scalar COCO_COLORS[] = {
    {56,0,255}, {226,255,0}, {0,94,255}, {0,37,255}, {0,255,94},
    {255,226,0}, {0,18,255}, {255,151,0}, {170,0,255}, {0,255,56},
    {255,0,75}, {0,75,255}, {0,255,169}, {255,0,207}, {75,255,0},
    {207,0,255}, {37,0,255}, {0,207,255}, {94,0,255}, {0,255,113},
    {255,18,0}, {255,0,56}, {18,0,255}, {0,255,226}, {170,255,0},
    {255,0,245}, {151,255,0}, {132,255,0}, {75,0,255}, {151,0,255},
    {0,151,255}, {132,0,255}, {0,255,245}, {255,132,0}, {226,0,255},
    {255,37,0}, {207,255,0}, {0,255,207}, {94,255,0}, {0,226,255},
    {56,255,0}, {255,94,0}, {255,113,0}, {0,132,255}, {255,0,132},
    {255,170,0}, {255,0,188}, {113,255,0}, {245,0,255}, {113,0,255},
    {255,188,0}, {0,113,255}, {255,0,0}, {0,56,255}, {255,0,113},
    {0,255,188}, {255,0,94}, {255,0,18}, {18,255,0}, {0,255,132},
    {0,188,255}, {0,245,255}, {0,169,255}, {37,255,0}, {255,0,151},
    {188,0,255}, {0,255,37}, {0,255,0}, {255,0,170}, {255,0,37},
    {255,75,0}, {0,0,255}, {255,207,0}, {255,0,226}, {255,245,0},
    {188,255,0}, {0,255,18}, {0,255,75}, {0,255,151}, {255,56,0}
};

const char* NpuInferenceConsumer::cocoClassNames(int id) {
    if (id >= 0 && id < 80) return COCO_NAMES[id];
    return "unknown";
}

cv::Scalar NpuInferenceConsumer::cocoColor(int id) {
    if (id >= 0 && id < 80) return COCO_COLORS[id];
    return cv::Scalar(0, 255, 0);
}

// ============================================================
// 构造 / 析构
// ============================================================

NpuInferenceConsumer::NpuInferenceConsumer(const NpuInferenceConfig& config,
                                           InferenceResultCallback callback)
    : config_(config)
    , callback_(std::move(callback))
{}

NpuInferenceConsumer::~NpuInferenceConsumer() {
    finalize();
}

// ============================================================
// IBufferConsumer 接口
// ============================================================

bool NpuInferenceConsumer::initialize(const std::vector<Buffer*>& first_buffers) {
    (void)first_buffers;

    if (initialized_) return true;

    if (config_.model_path.empty()) {
        LOG4CPLUS_ERROR(logger(), "NpuInferenceConsumer: model_path is empty");
        return false;
    }

    if (!loadModel()) {
        LOG4CPLUS_ERROR(logger(), "NpuInferenceConsumer: Failed to load model");
        return false;
    }

    initialized_ = true;
    LOG4CPLUS_INFO_FMT(logger(),
        "NpuInferenceConsumer: Initialized (model=%s, input=%dx%d, mode=%s, draw=%s, interval=%d)",
        config_.model_path.c_str(), model_input_w_, model_input_h_,
        config_.input_mode == NpuInferenceConfig::InputMode::PHYSICAL_ADDR
            ? "PHYSICAL_ADDR" : "VIRTUAL_ADDR",
        config_.enable_draw ? "ON" : "OFF",
        config_.inference_interval);
    return true;
}

bool NpuInferenceConsumer::consume(const std::vector<Buffer*>& buffers, int frame_index) {
    if (!initialized_ || buffers.empty() || !buffers[0]) {
        fail_count_++;
        return true;
    }

    // 帧跳过：非推理帧直接放行，buffer 不做任何修改
    if (config_.inference_interval > 1 &&
        (frame_index % config_.inference_interval) != 0) {
        return true;
    }

    Buffer* buffer = buffers[0];

    // 1. 预处理
    LetterboxParams params;
    cv::Mat bgr_full;
    if (config_.input_mode == NpuInferenceConfig::InputMode::PHYSICAL_ADDR) {
        params = preprocessPhysicalAddr(buffer);
    } else {
        params = preprocessVirtualAddr(buffer,
                                       config_.enable_draw ? &bgr_full : nullptr);
    }

    // 2. 推理
    if (!runInference()) {
        fail_count_++;
        return true;
    }

    // 3. 后处理
    std::vector<DetectionResult> results;
    postprocess(params, results);

    // 4. 画框 & 回写 buffer（如果启用）
    if (config_.enable_draw && !bgr_full.empty()) {
        drawAndWriteBack(bgr_full, buffer, results);
    }

    // 5. 保存 & 回调
    last_results_ = results;
    infer_count_++;

    if (callback_) {
        callback_(results, buffer, frame_index);
    }

    if (infer_count_ % 100 == 1) {
        LOG4CPLUS_DEBUG_FMT(logger(),
            "NpuInferenceConsumer: frame=%d, detections=%zu",
            frame_index, results.size());
    }

    return true;
}

void NpuInferenceConsumer::finalize() {
    if (!initialized_) return;

    releaseModel();
    initialized_ = false;

    LOG4CPLUS_INFO_FMT(logger(),
        "NpuInferenceConsumer: Finalized (inferred=%d, failed=%d)",
        infer_count_.load(), fail_count_.load());
}

std::string NpuInferenceConsumer::getStats() const {
    std::ostringstream oss;
    oss << "NpuInferenceConsumer: " << infer_count_.load() << " inferred, "
        << fail_count_.load() << " failed";
    return oss.str();
}

std::vector<DetectionResult> NpuInferenceConsumer::getLastResults() const {
    return last_results_;
}

// ============================================================
// 模型加载 / 释放
// ============================================================

bool NpuInferenceConsumer::loadModel() {
    int status = ta_runtime_init();
    if (status != 0) {
        LOG4CPLUS_ERROR_FMT(logger(), "ta_runtime_init failed: 0x%x", status);
        return false;
    }

    status = ta_runtime_load_model_from_file(
        &nnrt_context_, config_.model_path.c_str(), config_.npu_core_index);
    if (status != 0) {
        LOG4CPLUS_ERROR_FMT(logger(), "ta_runtime_load_model_from_file failed: 0x%x", status);
        ta_runtime_deinit();
        return false;
    }

    // 查询输入/输出数量
    taconn_input_output_num_t num{};
    ta_runtime_query(&nnrt_context_, TACONN_QUERY_IN_OUT_NUM, &num);
    input_num_  = num.input_num;
    output_num_ = num.output_num;

    LOG4CPLUS_INFO_FMT(logger(), "Model IO: %d inputs, %d outputs", input_num_, output_num_);

    // 查询输入属性
    for (int i = 0; i < input_num_; i++) {
        taconn_inout_attr_t attr{};
        attr.index = i;
        ta_runtime_query(&nnrt_context_, TACONN_QUERY_INPUT_ATTR, &attr);
        input_attrs_.push_back(attr);

        LOG4CPLUS_INFO_FMT(logger(),
            "  Input[%d]: dims=[%lu,%lu,%lu,%lu], format=%s, quant=%s",
            i, attr.dim_size[0], attr.dim_size[1],
            attr.dim_size[2], attr.dim_size[3],
            get_type_string(static_cast<taconn_data_format_t>(attr.data_format)),
            get_qnt_type_string(static_cast<taco_qnt_type_t>(attr.quant_format)));
    }

    // 从第一个输入推断模型输入尺寸 (CHW 或 HWC 布局)
    if (input_num_ > 0 && input_attrs_[0].dim_count >= 3) {
        model_input_h_ = input_attrs_[0].dim_size[1];
        model_input_w_ = input_attrs_[0].dim_size[0];
    }

    // 查询输出属性
    for (int i = 0; i < output_num_; i++) {
        taconn_inout_attr_t attr{};
        attr.index = i;
        ta_runtime_query(&nnrt_context_, TACONN_QUERY_OUTPUT_ATTR, &attr);
        output_attrs_.push_back(attr);

        LOG4CPLUS_INFO_FMT(logger(),
            "  Output[%d]: dims=[%lu,%lu,%lu,%lu], format=%s, quant=%s",
            i, attr.dim_size[0], attr.dim_size[1],
            attr.dim_size[2], attr.dim_size[3],
            get_type_string(static_cast<taconn_data_format_t>(attr.data_format)),
            get_qnt_type_string(static_cast<taco_qnt_type_t>(attr.quant_format)));
    }

    // 分配并绑定输入 buffer
    input_tensors_ = static_cast<taconn_input_t*>(
        malloc(sizeof(taconn_input_t) * input_num_));

    for (int i = 0; i < input_num_; i++) {
        auto fmt = static_cast<taconn_data_format_t>(input_attrs_[i].data_format);
        size_t buf_size = calcBufferSize(fmt, getElementNum(input_attrs_[i]));

        input_tensors_[i].index = i;
        input_tensors_[i].size  = buf_size;
        input_tensors_[i].data  = nullptr;

        if (posix_memalign(&input_tensors_[i].data, 256, buf_size) != 0) {
            LOG4CPLUS_ERROR(logger(), "posix_memalign for input buffer failed");
            return false;
        }
        memset(input_tensors_[i].data, 0, buf_size);
    }

    status = ta_runtime_set_input_cva(&nnrt_context_, input_num_, input_tensors_);
    if (status != 0) {
        LOG4CPLUS_ERROR_FMT(logger(), "ta_runtime_set_input_cva failed: 0x%x", status);
        return false;
    }

    // 分配并绑定输出 buffer
    output_buffers_ = static_cast<taconn_buffer_t*>(
        malloc(sizeof(taconn_buffer_t) * output_num_));

    for (int i = 0; i < output_num_; i++) {
        auto fmt = static_cast<taconn_data_format_t>(output_attrs_[i].data_format);
        size_t buf_size = calcBufferSize(fmt, getElementNum(output_attrs_[i]));

        status = ta_runtime_create_buffer(&nnrt_context_, buf_size, &output_buffers_[i]);
        if (status != 0) {
            LOG4CPLUS_ERROR_FMT(logger(), "ta_runtime_create_buffer[%d] failed: 0x%x", i, status);
            return false;
        }
    }

    status = ta_runtime_set_output(&nnrt_context_, output_num_, output_buffers_);
    if (status != 0) {
        LOG4CPLUS_ERROR_FMT(logger(), "ta_runtime_set_output failed: 0x%x", status);
        return false;
    }

    return true;
}

void NpuInferenceConsumer::releaseModel() {
    if (output_buffers_) {
        for (int i = 0; i < output_num_; i++) {
            ta_runtime_destroy_buffer(&nnrt_context_, &output_buffers_[i]);
        }
        free(output_buffers_);
        output_buffers_ = nullptr;
    }

    if (input_tensors_) {
        for (int i = 0; i < input_num_; i++) {
            if (input_tensors_[i].data) free(input_tensors_[i].data);
        }
        free(input_tensors_);
        input_tensors_ = nullptr;
    }

    if (nnrt_context_) {
        ta_runtime_destroy_context(&nnrt_context_);
        nnrt_context_ = 0;
    }

    ta_runtime_deinit();

    input_attrs_.clear();
    output_attrs_.clear();
}

// ============================================================
// 预处理: VIRTUAL_ADDR 模式
// ============================================================

NpuInferenceConsumer::LetterboxParams
NpuInferenceConsumer::preprocessVirtualAddr(Buffer* buffer, cv::Mat* bgr_out) {
    LetterboxParams params;
    params.src_w = buffer->getImageWidth();
    params.src_h = buffer->getImageHeight();

    // 1) NV12 → BGR (通过 OpenCV)
    cv::Mat nv12(params.src_h * 3 / 2, params.src_w, CV_8UC1,
                 buffer->getVirtualAddress());
    cv::Mat bgr;
    cv::cvtColor(nv12, bgr, cv::COLOR_YUV2BGR_NV12);

    // 如果需要画框，保留全分辨率 BGR 副本
    if (bgr_out) {
        *bgr_out = bgr.clone();
    }

    // 2) Letterbox resize (保持宽高比, 填充灰色114)
    params.ratio = std::min(
        static_cast<float>(model_input_h_) / params.src_h,
        static_cast<float>(model_input_w_) / params.src_w);

    int new_w = static_cast<int>(params.src_w * params.ratio);
    int new_h = static_cast<int>(params.src_h * params.ratio);

    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);

    params.pad_top    = (model_input_h_ - new_h) / 2;
    params.pad_bottom = model_input_h_ - new_h - params.pad_top;
    params.pad_left   = (model_input_w_ - new_w) / 2;
    params.pad_right  = model_input_w_ - new_w - params.pad_left;

    cv::Mat padded;
    cv::copyMakeBorder(resized, padded,
                       params.pad_top, params.pad_bottom,
                       params.pad_left, params.pad_right,
                       cv::BORDER_CONSTANT, cv::Scalar(114, 114, 114));

    // 3) BGR → RGB
    cv::Mat rgb;
    cv::cvtColor(padded, rgb, cv::COLOR_BGR2RGB);

    // 4) 填充到 input tensor
    for (int i = 0; i < input_num_; i++) {
        if (input_attrs_[i].quant_format == TACONN_QNT_TYPE_NONE &&
            input_attrs_[i].data_format  == TACONN_DATA_FORMAT_UINT8) {
            memcpy(input_tensors_[i].data, rgb.data, input_tensors_[i].size);
        } else {
            size_t num_pixels = model_input_w_ * model_input_h_ * 3;
            std::vector<uint8_t> chw_buf(num_pixels);
            matToTensorCHW(rgb, chw_buf.data());

            size_t num_elements = getElementNum(input_attrs_[i]);
            normalizeAndQuantize(chw_buf.data(), input_tensors_[i].data,
                                 num_elements, input_attrs_[i]);
        }
    }

    return params;
}

// ============================================================
// 预处理: PHYSICAL_ADDR 模式
// ============================================================

NpuInferenceConsumer::LetterboxParams
NpuInferenceConsumer::preprocessPhysicalAddr(Buffer* buffer) {
    LetterboxParams params;
    params.src_w = buffer->getImageWidth();
    params.src_h = buffer->getImageHeight();
    params.ratio = 1.0f;

    /*
     * 物理地址模式：解码输出 NV12 直接通过 DMA 物理地址传给 NPU。
     * 前提：模型编译时以 NV12 为输入 (如 yolov5_tracker_sample 中的 2 input 模型)。
     * TODO: 集成 tacv resize
     */

    uint64_t phy_addr = buffer->getPhysicalAddress();
    int w = params.src_w;
    int h = params.src_h;

    if (input_num_ >= 2) {
        taconn_input_phy_t phy_inputs[2];
        phy_inputs[0].physical_table[0] = phy_addr;
        phy_inputs[0].size_table[0]     = w * h;
        phy_inputs[1].physical_table[0] = phy_addr + w * h;
        phy_inputs[1].size_table[0]     = w * h / 2;

        int status = ta_runtime_set_input_pha(&nnrt_context_, input_num_, phy_inputs);
        if (status != 0) {
            LOG4CPLUS_ERROR_FMT(logger(),
                "ta_runtime_set_input_pha failed: 0x%x", status);
        }
    }

    return params;
}

// ============================================================
// 推理执行
// ============================================================

bool NpuInferenceConsumer::runInference() {
    int status = ta_runtime_run_network(&nnrt_context_);
    if (status != 0) {
        LOG4CPLUS_ERROR_FMT(logger(), "ta_runtime_run_network failed: 0x%x", status);
        return false;
    }

    status = ta_runtime_invalidate_buffer(&nnrt_context_, output_buffers_);
    if (status != 0) {
        LOG4CPLUS_ERROR_FMT(logger(), "ta_runtime_invalidate_buffer failed: 0x%x", status);
        return false;
    }

    return true;
}

// ============================================================
// 后处理 (YOLO 风格: anchor-free, DFL)
// ============================================================

void NpuInferenceConsumer::postprocess(const LetterboxParams& params,
                                       std::vector<DetectionResult>& results) {
    std::vector<DetectionResult> proposals;
    int strides[] = {8, 16, 32};

    int num_scales = std::min(output_num_, 3);

    for (int i = 0; i < num_scales; i++) {
        void* data = output_buffers_[i].data;
        uint32_t fmt = output_attrs_[i].data_format;

        int32_t zp = 0;
        float scale = 1.0f;
        if (output_attrs_[i].quant_format == TACONN_QNT_TYPE_ASYMMETRIC) {
            zp    = output_attrs_[i].quant_data.affine.tf_zero_point;
            scale = output_attrs_[i].quant_data.affine.tf_scale;
        }

        generateProposals(strides[i], data, fmt, zp, scale,
                          config_.conf_threshold, proposals);
    }

    qsortDescend(proposals);
    std::vector<int> picked;
    nmsSortedBboxes(proposals, picked, config_.nms_threshold);

    results.resize(picked.size());
    for (size_t i = 0; i < picked.size(); i++) {
        results[i] = proposals[picked[i]];
        inverseCoordinates(results[i].box, params);
    }
}

// ============================================================
// 画框 & 回写 Buffer
// ============================================================

void NpuInferenceConsumer::drawAndWriteBack(
        cv::Mat& bgr, Buffer* buffer,
        const std::vector<DetectionResult>& results) {

    const float font_scale = 0.5f;
    const int thickness = 2;

    for (const auto& det : results) {
        cv::Scalar color = cocoColor(det.class_id);

        cv::Point pt1(static_cast<int>(det.box.left),
                      static_cast<int>(det.box.top));
        cv::Point pt2(static_cast<int>(det.box.left + det.box.width),
                      static_cast<int>(det.box.top + det.box.height));
        cv::rectangle(bgr, pt1, pt2, color, thickness);

        char text[128];
        snprintf(text, sizeof(text), "%s %.0f%%",
                 cocoClassNames(det.class_id), det.score * 100.f);

        int baseline = 0;
        cv::Size label_sz = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX,
                                            font_scale, thickness, &baseline);

        int lx = std::max(0, pt1.x);
        int ly = pt1.y - label_sz.height - baseline;
        if (ly < 0) ly = 0;
        if (lx + label_sz.width > bgr.cols)
            lx = bgr.cols - label_sz.width;

        cv::rectangle(bgr,
                      cv::Rect(cv::Point(lx, ly),
                               cv::Size(label_sz.width, label_sz.height + baseline)),
                      cv::Scalar(0, 0, 0), -1);
        cv::putText(bgr, text, cv::Point(lx, ly + label_sz.height),
                    cv::FONT_HERSHEY_SIMPLEX, font_scale,
                    cv::Scalar(255, 255, 255), thickness);
    }

    // BGR → NV12, 回写到 buffer 的虚拟地址
    int h = buffer->getImageHeight();
    int w = buffer->getImageWidth();

    cv::Mat yuv_i420;
    cv::cvtColor(bgr, yuv_i420, cv::COLOR_BGR2YUV_I420);

    uint8_t* dst = static_cast<uint8_t*>(buffer->getVirtualAddress());
    int y_size = w * h;
    int uv_size = y_size / 4;

    // Y plane: 直接复制
    memcpy(dst, yuv_i420.data, y_size);

    // I420 (YYYY UU VV) → NV12 (YYYY UVUV): 交错 U/V
    const uint8_t* u_plane = yuv_i420.data + y_size;
    const uint8_t* v_plane = u_plane + uv_size;
    uint8_t* nv12_uv = dst + y_size;

    for (int i = 0; i < uv_size; i++) {
        nv12_uv[2 * i]     = u_plane[i];
        nv12_uv[2 * i + 1] = v_plane[i];
    }
}

// ============================================================
// Proposal 生成 (YOLO11/v8 anchor-free + DFL)
// ============================================================

void NpuInferenceConsumer::generateProposals(
        int stride, void* feat, uint32_t data_format,
        int32_t zp, float scale, float prob_threshold,
        std::vector<DetectionResult>& objects) {

    int feat_w = model_input_w_ / stride;
    int feat_h = model_input_h_ / stride;
    int reg_max = 16;
    int cls_num = 80;
    int channel_size = feat_h * feat_w;

    std::vector<float> dis_buf(4 * reg_max, 0.f);

    for (int h = 0; h < feat_h; h++) {
        for (int w = 0; w < feat_w; w++) {
            int spatial_offset = h * feat_w + w;

            int best_cls = 0;
            float best_score = -FLT_MAX;
            size_t cls_start = 4 * reg_max * channel_size;

            for (int s = 0; s < cls_num; s++) {
                size_t offset = cls_start + s * channel_size + spatial_offset;
                float sc = dequantizeValue(feat, offset, data_format, zp, scale);
                if (sc > best_score) {
                    best_cls = s;
                    best_score = sc;
                }
            }

            float box_prob = 1.0f / (1.0f + expf(-best_score));
            if (box_prob < prob_threshold) continue;

            float pred_ltrb[4];
            size_t elem_sz = getElementSize(data_format);
            for (int c = 0; c < 4; c++) {
                size_t box_start = c * reg_max * channel_size;
                size_t offset = box_start + spatial_offset;
                char* ptr = static_cast<char*>(feat) + offset * elem_sz;
                pred_ltrb[c] = softmaxWithStride(
                    ptr, dis_buf.data() + c * reg_max,
                    reg_max, channel_size, zp, scale, data_format) * stride;
            }

            float pb_cx = (w + 0.5f) * stride;
            float pb_cy = (h + 0.5f) * stride;

            float x0 = std::max(0.f, pb_cx - pred_ltrb[0]);
            float y0 = std::max(0.f, pb_cy - pred_ltrb[1]);
            float x1 = std::min(pb_cx + pred_ltrb[2], (float)(model_input_w_ - 1));
            float y1 = std::min(pb_cy + pred_ltrb[3], (float)(model_input_h_ - 1));

            DetectionResult obj;
            obj.box.left   = x0;
            obj.box.top    = y0;
            obj.box.width  = x1 - x0;
            obj.box.height = y1 - y0;
            obj.class_id   = best_cls;
            obj.score      = box_prob;
            objects.push_back(obj);
        }
    }
}

// ============================================================
// 反量化
// ============================================================

float NpuInferenceConsumer::dequantizeValue(
        void* data, size_t idx, uint32_t data_format,
        int32_t zp, float scale) {
    switch (data_format) {
    case TACONN_DATA_FORMAT_FP32:
        return static_cast<float*>(data)[idx];
    case TACONN_DATA_FORMAT_FP16: {
        uint16_t v = static_cast<uint16_t*>(data)[idx];
        uint32_t sign = (v >> 15) & 0x1;
        uint32_t exp  = (v >> 10) & 0x1F;
        uint32_t mant = v & 0x3FF;
        if (exp == 0 && mant == 0) {
            uint32_t f = sign << 31;
            return *reinterpret_cast<float*>(&f);
        }
        if (exp == 0) {
            uint32_t f = (sign << 31) | (103u << 23) | (mant << 13);
            return *reinterpret_cast<float*>(&f);
        }
        if (exp == 31) {
            uint32_t f = (sign << 31) | 0x7F800000 | (mant << 13);
            return *reinterpret_cast<float*>(&f);
        }
        uint32_t f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
        return *reinterpret_cast<float*>(&f);
    }
    case TACONN_DATA_FORMAT_UINT8:
        return (static_cast<float>(static_cast<uint8_t*>(data)[idx]) - zp) * scale;
    case TACONN_DATA_FORMAT_INT8:
        return (static_cast<float>(static_cast<int8_t*>(data)[idx]) - zp) * scale;
    default:
        return 0.0f;
    }
}

float NpuInferenceConsumer::softmaxWithStride(
        void* src, float* dst, int length,
        int stride, int32_t zp, float scale, uint32_t data_format) {
    float max_val = -FLT_MAX;
    for (int i = 0; i < length; i++) {
        float val = dequantizeValue(src, i * stride, data_format, zp, scale);
        if (val > max_val) max_val = val;
    }

    float sum = 0.f;
    for (int i = 0; i < length; i++) {
        float val = dequantizeValue(src, i * stride, data_format, zp, scale);
        dst[i] = expf(val - max_val);
        sum += dst[i];
    }

    float weighted_sum = 0.f;
    for (int i = 0; i < length; i++) {
        dst[i] /= sum;
        weighted_sum += i * dst[i];
    }
    return weighted_sum;
}

// ============================================================
// NMS
// ============================================================

float NpuInferenceConsumer::intersectionArea(
        const DetectionResult& a, const DetectionResult& b) {
    float x0 = std::max(a.box.left, b.box.left);
    float y0 = std::max(a.box.top,  b.box.top);
    float x1 = std::min(a.box.left + a.box.width,  b.box.left + b.box.width);
    float y1 = std::min(a.box.top  + a.box.height, b.box.top  + b.box.height);
    return std::max(0.f, x1 - x0) * std::max(0.f, y1 - y0);
}

void NpuInferenceConsumer::nmsSortedBboxes(
        const std::vector<DetectionResult>& objects,
        std::vector<int>& picked, float nms_threshold) {
    picked.clear();
    int n = objects.size();
    if (n == 0) return;

    std::vector<float> areas(n);
    for (int i = 0; i < n; i++) {
        areas[i] = objects[i].box.width * objects[i].box.height;
    }

    std::unordered_map<int, std::vector<int>> class_map;
    for (int i = 0; i < n; i++) {
        class_map[objects[i].class_id].push_back(i);
    }

    for (auto& [cls, indices] : class_map) {
        (void)cls;
        std::vector<int> cls_picked;
        for (int idx_i : indices) {
            bool keep = true;
            for (int idx_j : cls_picked) {
                float inter = intersectionArea(objects[idx_i], objects[idx_j]);
                float uni   = areas[idx_i] + areas[idx_j] - inter;
                if (inter / uni > nms_threshold) { keep = false; break; }
            }
            if (keep) cls_picked.push_back(idx_i);
        }
        picked.insert(picked.end(), cls_picked.begin(), cls_picked.end());
    }
}

void NpuInferenceConsumer::qsortDescend(
        std::vector<DetectionResult>& objects, int left, int right) {
    int i = left, j = right;
    float pivot = objects[(left + right) / 2].score;
    while (i <= j) {
        while (objects[i].score > pivot) i++;
        while (objects[j].score < pivot) j--;
        if (i <= j) { std::swap(objects[i], objects[j]); i++; j--; }
    }
    if (left < j)  qsortDescend(objects, left, j);
    if (i < right)  qsortDescend(objects, i, right);
}

void NpuInferenceConsumer::qsortDescend(std::vector<DetectionResult>& objects) {
    if (objects.empty()) return;
    qsortDescend(objects, 0, static_cast<int>(objects.size()) - 1);
}

void NpuInferenceConsumer::inverseCoordinates(
        DetectionBox& box, const LetterboxParams& params) {
    box.left -= params.pad_left;
    box.top  -= params.pad_top;

    float inv = 1.0f / params.ratio;
    box.left   *= inv;
    box.top    *= inv;
    box.width  *= inv;
    box.height *= inv;

    float x1 = box.left + box.width;
    float y1 = box.top  + box.height;

    box.left   = std::max(0.f, std::min(box.left, (float)params.src_w));
    box.top    = std::max(0.f, std::min(box.top,  (float)params.src_h));
    x1         = std::max(0.f, std::min(x1, (float)params.src_w));
    y1         = std::max(0.f, std::min(y1, (float)params.src_h));
    box.width  = std::max(0.f, x1 - box.left);
    box.height = std::max(0.f, y1 - box.top);
}

// ============================================================
// 工具函数
// ============================================================

size_t NpuInferenceConsumer::getElementNum(const taconn_inout_attr_t& attr) {
    size_t n = 1;
    for (unsigned i = 0; i < attr.dim_count; i++) n *= attr.dim_size[i];
    return n;
}

size_t NpuInferenceConsumer::getElementSize(uint32_t fmt) {
    switch (fmt) {
    case TACONN_DATA_FORMAT_FP32:  return 4;
    case TACONN_DATA_FORMAT_FP16:  return 2;
    case TACONN_DATA_FORMAT_UINT8: return 1;
    case TACONN_DATA_FORMAT_INT8:  return 1;
    default: return 1;
    }
}

size_t NpuInferenceConsumer::calcBufferSize(
        taconn_data_format_t fmt, size_t element_count) {
    switch (fmt) {
    case TACONN_DATA_FORMAT_FP32:
    case TACONN_DATA_FORMAT_INT32:
    case TACONN_DATA_FORMAT_UINT32:
        return sizeof(uint32_t) * element_count;
    case TACONN_DATA_FORMAT_FP16:
    case TACONN_DATA_FORMAT_BFP16:
    case TACONN_DATA_FORMAT_INT16:
    case TACONN_DATA_FORMAT_UINT16:
        return sizeof(uint16_t) * element_count;
    case TACONN_DATA_FORMAT_UINT8:
    case TACONN_DATA_FORMAT_INT8:
    case TACONN_DATA_FORMAT_CHAR:
    case TACONN_DATA_FORMAT_BOOL8:
        return sizeof(uint8_t) * element_count;
    case TACONN_DATA_FORMAT_FP64:
    case TACONN_DATA_FORMAT_INT64:
    case TACONN_DATA_FORMAT_UINT64:
        return sizeof(uint64_t) * element_count;
    case TACONN_DATA_FORMAT_INT4:
    case TACONN_DATA_FORMAT_UINT4:
        return (element_count + 1) / 2;
    default:
        return element_count;
    }
}

void NpuInferenceConsumer::matToTensorCHW(const cv::Mat& mat, uint8_t* tensor) {
    int total_pixels = mat.rows * mat.cols;
    int channels = mat.channels();
    std::vector<cv::Mat> ch;
    cv::split(mat, ch);
    for (int c = 0; c < channels; c++) {
        memcpy(tensor + c * total_pixels, ch[c].data, total_pixels);
    }
}

void NpuInferenceConsumer::normalizeAndQuantize(
        uint8_t* src, void* dst, size_t num_elements,
        const taconn_inout_attr_t& attr) {

    if (attr.quant_format == TACONN_QNT_TYPE_NONE) {
        if (attr.data_format == TACONN_DATA_FORMAT_FP32) {
            auto* out = static_cast<float*>(dst);
            for (size_t i = 0; i < num_elements; i++)
                out[i] = src[i] / 255.0f;
        } else if (attr.data_format == TACONN_DATA_FORMAT_FP16) {
            auto* out = static_cast<uint16_t*>(dst);
            for (size_t i = 0; i < num_elements; i++)
                out[i] = fp32ToFp16(src[i] / 255.0f);
        } else if (attr.data_format == TACONN_DATA_FORMAT_UINT8) {
            memcpy(dst, src, num_elements);
        }
    } else if (attr.quant_format == TACONN_QNT_TYPE_ASYMMETRIC) {
        float sc = attr.quant_data.affine.tf_scale;
        int32_t zp = attr.quant_data.affine.tf_zero_point;

        if (attr.data_format == TACONN_DATA_FORMAT_INT8) {
            auto* out = static_cast<int8_t*>(dst);
            for (size_t i = 0; i < num_elements; i++) {
                int32_t q = lrintf((src[i] / 255.0f) / sc) + zp;
                out[i] = static_cast<int8_t>(std::max(-128, std::min(127, q)));
            }
        } else if (attr.data_format == TACONN_DATA_FORMAT_UINT8) {
            memcpy(dst, src, num_elements);
        }
    } else if (attr.quant_format == TACONN_QNT_TYPE_DFP) {
        int fp_pos = attr.quant_data.dfp.fixed_point_pos;
        if (attr.data_format == TACONN_DATA_FORMAT_INT8) {
            auto* out = static_cast<int8_t*>(dst);
            for (size_t i = 0; i < num_elements; i++) {
                int32_t scaled = lrintf((src[i] / 255.0f) * (1 << fp_pos));
                out[i] = static_cast<int8_t>(std::max(-128, std::min(127, scaled)));
            }
        }
    }
}

uint16_t NpuInferenceConsumer::fp32ToFp16(float value) {
    uint32_t bits = *reinterpret_cast<uint32_t*>(&value);
    uint16_t sign = (bits >> 31) & 0x1;
    int exp = (bits >> 23) & 0xFF;
    uint32_t frac = bits & 0x7FFFFF;

    if (exp == 0 && frac == 0) return sign << 15;
    if (exp == 0xFF) return (sign << 15) | (frac ? 0x7E00 : 0x7C00);

    exp -= 127;
    if (exp < -14) return (sign << 15);
    if (exp > 15)  return (sign << 15) | 0x7C00;

    return (sign << 15) | ((exp + 15) << 10) | (frac >> 13);
}

} // namespace consumer
