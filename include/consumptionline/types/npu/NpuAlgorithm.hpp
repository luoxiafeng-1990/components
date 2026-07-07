/**
 * @file NpuAlgorithm.hpp
 * @brief NPU 推理算法枚举与字符串互转
 */

#ifndef CONSUMPTIONLINE_TYPES_NPU_NPU_ALGORITHM_HPP
#define CONSUMPTIONLINE_TYPES_NPU_NPU_ALGORITHM_HPP

#include <algorithm>
#include <string>
#include <vector>

namespace consumer {

enum class NpuAlgorithm {
    UNKNOWN = 0,
    YOLOV5_DET,
    YOLOV8_DET,
    YOLO11_DET,
    YOLOV12_DET,
};

/// MR-1：CLI 校验与 initialize() 仅允许此列表内的算法
inline const std::vector<std::string>& supportedNpuAlgorithmNames() {
    static const std::vector<std::string> kNames = {
        "yolov5_det",
        "yolov8_det",
        "yolo11_det",
        "yolov12_det",
    };
    return kNames;
}

inline std::string npuAlgorithmToString(NpuAlgorithm algorithm) {
    switch (algorithm) {
    case NpuAlgorithm::YOLOV5_DET:  return "yolov5_det";
    case NpuAlgorithm::YOLOV8_DET:  return "yolov8_det";
    case NpuAlgorithm::YOLO11_DET:  return "yolo11_det";
    case NpuAlgorithm::YOLOV12_DET: return "yolov12_det";
    case NpuAlgorithm::UNKNOWN:
    default:
        return "unknown";
    }
}

inline bool isNpuAlgorithmSupported(NpuAlgorithm algorithm) {
    if (algorithm == NpuAlgorithm::UNKNOWN) {
        return false;
    }
    const std::string name = npuAlgorithmToString(algorithm);
    const auto& supported = supportedNpuAlgorithmNames();
    return std::find(supported.begin(), supported.end(), name) != supported.end();
}

inline NpuAlgorithm parseNpuAlgorithm(const std::string& name) {
    if (name == "yolov5_det")  return NpuAlgorithm::YOLOV5_DET;
    if (name == "yolov8_det")  return NpuAlgorithm::YOLOV8_DET;
    if (name == "yolo11_det")  return NpuAlgorithm::YOLO11_DET;
    if (name == "yolov12_det") return NpuAlgorithm::YOLOV12_DET;
    return NpuAlgorithm::UNKNOWN;
}

} // namespace consumer

#endif // CONSUMPTIONLINE_TYPES_NPU_NPU_ALGORITHM_HPP
