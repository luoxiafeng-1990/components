/**
 * @file NpuInferenceConsumer.hpp
 * @brief NPU 推理消费者 —— 将解码后的视频帧送入 NPU 进行模型推理
 *
 * 设计思路：
 *   与 DisplayConsumer 平行，NPU 推理是 Buffer 的另一种"消费方式"。
 *   解码器产出帧 → BufferPool → NpuInferenceConsumer → ta_runtime 推理 → 回调返回结果
 *
 * 数据流（enable_draw = true 时）：
 *   Buffer(NV12, 1920x1080)
 *     → [预处理] NV12→BGR→RGB, resize+letterbox, HWC→CHW, normalize/quantize
 *     → [推理]   ta_runtime_run_network
 *     → [后处理] 反量化 → proposals → NMS → 坐标逆变换
 *     → [画框]   在原始 BGR 上 cv::rectangle / cv::putText
 *     → [回写]   BGR→NV12, 写回 Buffer (供后续 DisplayConsumer 显示带框画面)
 *     → [回调]   ResultCallback(detections, buffer, frame_index)
 *
 * 使用方式：
 *   1) 单独使用：
 *      auto npu = std::make_shared<NpuInferenceConsumer>(config, callback);
 *
 *   2) 与 DisplayConsumer 组合（先推理画框，再显示）：
 *      auto multi = std::make_shared<MultiConsumer>();
 *      multi->addStrategy(npuConsumer);   // 先推理+画框
 *      multi->addStrategy(displayConsumer); // 再显示带框画面
 */

#ifndef NPU_INFERENCE_CONSUMER_HPP
#define NPU_INFERENCE_CONSUMER_HPP

#include "consumptionline/core/IBufferConsumer.hpp"
#include "buffer/bufferpool/Buffer.hpp"

#include <ta-runtime-api.h>
#include <opencv2/core.hpp>

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <cstdint>

namespace consumer {

// ============================================================
// 检测结果
// ============================================================

struct DetectionBox {
    float left;
    float top;
    float width;
    float height;
};

struct DetectionResult {
    DetectionBox box;
    int class_id;
    float score;
};

// ============================================================
// 配置
// ============================================================

struct NpuInferenceConfig {
    std::string model_path;             ///< .nb 模型文件路径

    float conf_threshold  = 0.25f;      ///< 置信度阈值
    float nms_threshold   = 0.45f;      ///< NMS IoU 阈值
    int   npu_core_index  = 0;          ///< NPU 核心索引 (多核芯片)

    /**
     * 输入模式：
     *   VIRTUAL_ADDR  — NV12→BGR→RGB via OpenCV, memcpy 到 input buffer (通用, 兼容 model-zoo 模型)
     *   PHYSICAL_ADDR — NV12 直接通过物理地址传入 (零拷贝, 需要模型支持 NV12 输入)
     */
    enum class InputMode {
        VIRTUAL_ADDR,
        PHYSICAL_ADDR,
    };
    InputMode input_mode = InputMode::VIRTUAL_ADDR;

    /**
     * 是否在推理后将检测框画回原始 Buffer:
     *   true  — 画框后 BGR→NV12 回写 buffer，后续 DisplayConsumer 看到带框画面
     *   false — 仅产出 DetectionResult 数据，不修改 buffer
     */
    bool enable_draw = false;

    /**
     * 推理帧间隔：每 N 帧执行一次完整推理 + 画框，中间帧直接放行给后续消费者。
     *   1 = 每帧都推理（默认，但 CPU 开销大）
     *   N > 1 = 每 N 帧推理一次，中间帧不修改 buffer
     * 当 enable_draw 为 true 时，建议设为 10~30 以平衡推理精度与显示流畅度。
     */
    int inference_interval = 1;
};

// ============================================================
// 结果回调
// ============================================================

/**
 * @brief 推理结果回调
 * @param detections 检测结果列表
 * @param source_buffer 原始 Buffer 指针 (可用于获取帧信息/画框)
 * @param frame_index 帧序号
 */
using InferenceResultCallback = std::function<void(
    const std::vector<DetectionResult>& detections,
    Buffer* source_buffer,
    int frame_index
)>;

// ============================================================
// NpuInferenceConsumer
// ============================================================

class NpuInferenceConsumer : public IBufferConsumer {
public:
    /**
     * @param config NPU 推理配置
     * @param callback 推理结果回调 (可为 nullptr, 则仅统计不回调)
     */
    explicit NpuInferenceConsumer(const NpuInferenceConfig& config,
                                  InferenceResultCallback callback = nullptr);
    ~NpuInferenceConsumer() override;

    // ----- IBufferConsumer 接口 -----
    bool initialize(const std::vector<Buffer*>& first_buffers) override;
    bool consume(const std::vector<Buffer*>& buffers, int frame_index) override;
    void finalize() override;
    std::string getStats() const override;

    /// 获取最近一次推理的结果 (线程安全地拷贝)
    std::vector<DetectionResult> getLastResults() const;

private:
    // ----- 配置 -----
    NpuInferenceConfig config_;
    InferenceResultCallback callback_;

    // ----- ta-runtime 上下文 -----
    ta_runtime_context nnrt_context_ = 0;
    taconn_input_t*    input_tensors_  = nullptr;
    taconn_buffer_t*   output_buffers_ = nullptr;
    int input_num_  = 0;
    int output_num_ = 0;
    std::vector<taconn_inout_attr_t> input_attrs_;
    std::vector<taconn_inout_attr_t> output_attrs_;

    // 模型输入尺寸 (从模型属性自动读取)
    int model_input_w_ = 0;
    int model_input_h_ = 0;

    // ----- 状态 -----
    bool initialized_ = false;
    std::atomic<int> infer_count_{0};
    std::atomic<int> fail_count_{0};
    std::vector<DetectionResult> last_results_;

    // ----- 预处理参数 (letterbox) -----
    struct LetterboxParams {
        float ratio = 1.0f;
        int pad_top = 0, pad_bottom = 0;
        int pad_left = 0, pad_right = 0;
        int src_w = 0, src_h = 0;
    };

    // ----- 内部方法 -----

    /// 加载模型, 查询属性, 分配 input/output buffer
    bool loadModel();
    void releaseModel();

    /// 预处理: NV12 Buffer → 填充 input_tensors_ (VIRTUAL_ADDR 模式)
    /// @param bgr_out 如果 enable_draw, 输出全分辨率 BGR 用于画框
    LetterboxParams preprocessVirtualAddr(Buffer* buffer, cv::Mat* bgr_out = nullptr);

    /// 预处理: 物理地址直传 (PHYSICAL_ADDR 模式)
    LetterboxParams preprocessPhysicalAddr(Buffer* buffer);

    /// 执行推理
    bool runInference();

    /// 后处理: 从 output_buffers_ 解析检测结果
    void postprocess(const LetterboxParams& params,
                     std::vector<DetectionResult>& results);

    /// 在 BGR 图像上画检测框, 然后 BGR→NV12 回写 buffer
    void drawAndWriteBack(cv::Mat& bgr, Buffer* buffer,
                          const std::vector<DetectionResult>& results);

    // ----- 后处理辅助 -----

    float dequantizeValue(void* data, size_t idx,
                          uint32_t data_format, int32_t zp, float scale);

    float softmaxWithStride(void* src, float* dst, int length,
                            int stride, int32_t zp, float scale,
                            uint32_t data_format);

    void generateProposals(int stride, void* feat,
                           uint32_t data_format, int32_t zp, float scale,
                           float prob_threshold,
                           std::vector<DetectionResult>& objects);

    static float intersectionArea(const DetectionResult& a, const DetectionResult& b);
    static void nmsSortedBboxes(const std::vector<DetectionResult>& objects,
                                std::vector<int>& picked, float nms_threshold);
    static void qsortDescend(std::vector<DetectionResult>& objects, int left, int right);
    static void qsortDescend(std::vector<DetectionResult>& objects);
    static void inverseCoordinates(DetectionBox& box, const LetterboxParams& params);

    // ----- 工具 -----
    size_t getElementNum(const taconn_inout_attr_t& attr);
    size_t getElementSize(uint32_t data_format);
    size_t calcBufferSize(taconn_data_format_t fmt, size_t element_count);
    void matToTensorCHW(const cv::Mat& mat, uint8_t* tensor);
    void normalizeAndQuantize(uint8_t* src, void* dst,
                              size_t num_elements, const taconn_inout_attr_t& attr);
    uint16_t fp32ToFp16(float value);

    // ----- COCO 类别名 & 配色 (用于画框) -----
    static const char* cocoClassNames(int id);
    static cv::Scalar  cocoColor(int id);
};

} // namespace consumer

#endif // NPU_INFERENCE_CONSUMER_HPP
