/**
 * @file MgDatasourceProducerType.hpp
 * @brief MultiWorker group 中 datasource 生产者的构造类型（配方）
 *
 * 与 WorkerType 分离：WorkerType 描述单个 WorkerFactory 产品；
 * 本枚举描述 COMPARE/MultiWorker 组装 datasource 时的构造配方。
 * FFMPEG_DECODE_THEN_ENCODE 不是新的 WorkerType，而是 Decode + 叠 video_encode。
 */

#ifndef MG_DATASOURCE_PRODUCER_TYPE_HPP
#define MG_DATASOURCE_PRODUCER_TYPE_HPP

#include <string>
#include <string_view>

enum class MgDatasourceProducerType {
    UNSPECIFIED = 0,              ///< 未设置 → COMPARE 保持现有 PACKET_RECORDER 逻辑
    FFMPEG_PACKET_RECORDER,       ///< 单 Worker：灌包
    FFMPEG_DECODE,                ///< 单 Worker：解码出帧
    FFMPEG_ENCODE,                ///< 单 Worker：编码出包
    FFMPEG_DECODE_THEN_ENCODE,    ///< 组合：Decode + video_encode 消费 → 编码包池
};

inline const char* mgDatasourceProducerTypeToString(MgDatasourceProducerType t) {
    switch (t) {
        case MgDatasourceProducerType::UNSPECIFIED:             return "UNSPECIFIED";
        case MgDatasourceProducerType::FFMPEG_PACKET_RECORDER:  return "FFMPEG_PACKET_RECORDER";
        case MgDatasourceProducerType::FFMPEG_DECODE:           return "FFMPEG_DECODE";
        case MgDatasourceProducerType::FFMPEG_ENCODE:           return "FFMPEG_ENCODE";
        case MgDatasourceProducerType::FFMPEG_DECODE_THEN_ENCODE: return "FFMPEG_DECODE_THEN_ENCODE";
    }
    return "UNSPECIFIED";
}

/**
 * @brief 解析 CLI / 配置字符串；空串 → UNSPECIFIED；无法识别 → 仍返回 UNSPECIFIED 且 ok=false
 */
inline MgDatasourceProducerType parseMgDatasourceProducerType(
    std::string_view s, bool* ok = nullptr)
{
    auto set_ok = [&](bool v) { if (ok) *ok = v; };

    if (s.empty()) {
        set_ok(true);
        return MgDatasourceProducerType::UNSPECIFIED;
    }

    if (s == "UNSPECIFIED") {
        set_ok(true);
        return MgDatasourceProducerType::UNSPECIFIED;
    }
    if (s == "FFMPEG_PACKET_RECORDER" || s == "PACKET_RECORDER" || s == "recorder") {
        set_ok(true);
        return MgDatasourceProducerType::FFMPEG_PACKET_RECORDER;
    }
    if (s == "FFMPEG_DECODE" || s == "DECODE" || s == "decode") {
        set_ok(true);
        return MgDatasourceProducerType::FFMPEG_DECODE;
    }
    if (s == "FFMPEG_ENCODE" || s == "ENCODE" || s == "encode") {
        set_ok(true);
        return MgDatasourceProducerType::FFMPEG_ENCODE;
    }
    if (s == "FFMPEG_DECODE_THEN_ENCODE" || s == "DECODE_THEN_ENCODE" ||
        s == "decode_then_encode") {
        set_ok(true);
        return MgDatasourceProducerType::FFMPEG_DECODE_THEN_ENCODE;
    }

    set_ok(false);
    return MgDatasourceProducerType::UNSPECIFIED;
}

#endif // MG_DATASOURCE_PRODUCER_TYPE_HPP
