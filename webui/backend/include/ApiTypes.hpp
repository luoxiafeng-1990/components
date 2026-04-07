#ifndef WEBUI_API_TYPES_HPP
#define WEBUI_API_TYPES_HPP

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include "../third_party/nlohmann/json.hpp"

namespace webui {

using json = nlohmann::json;

// ============================================================
// 通用 API 响应
// ============================================================

struct ApiResponse {
    int code = 0;
    std::string message = "success";
    json data = nullptr;

    json toJson() const {
        return {{"code", code}, {"message", message}, {"data", data}};
    }

    static ApiResponse ok(const json& d = nullptr, const std::string& msg = "success") {
        return {0, msg, d};
    }

    static ApiResponse error(int c, const std::string& msg) {
        return {c, msg, nullptr};
    }
};

// ============================================================
// 错误码
// ============================================================

namespace ErrorCode {
    constexpr int OK                      = 0;
    constexpr int PARAM_ERROR             = 1001;
    constexpr int NOT_FOUND               = 1002;
    constexpr int ALREADY_EXISTS          = 1003;
    constexpr int DATASOURCE_UNAVAILABLE  = 2001;
    constexpr int DATASOURCE_IN_USE       = 2002;
    constexpr int WORKER_START_FAILED     = 3001;
    constexpr int WORKER_STATE_INVALID    = 3002;
    constexpr int WORKER_DS_NOT_FOUND     = 3003;
    constexpr int CONSUMER_TYPE_INVALID   = 4001;
    constexpr int CONSUMER_CONFIG_INVALID = 4002;
    constexpr int PREVIEW_UNAVAILABLE     = 5001;
    constexpr int ENCODER_INIT_FAILED     = 5002;
    constexpr int INTERNAL_ERROR          = 9001;
}

// ============================================================
// 数据源类型
// ============================================================

enum class DataSourceType {
    FILE,
    RTSP,
    BUFFER
};

NLOHMANN_JSON_SERIALIZE_ENUM(DataSourceType, {
    {DataSourceType::FILE, "FILE"},
    {DataSourceType::RTSP, "RTSP"},
    {DataSourceType::BUFFER, "BUFFER"},
})

// ============================================================
// 数据源模型
// ============================================================

struct DataSourceInfo {
    std::string id;
    std::string name;
    DataSourceType type = DataSourceType::FILE;
    std::string path;
    int buffer_count = 0;
    int max_frames = -1;
    bool loop = false;
    std::string created_at;
    std::string status = "idle";   // idle | recording | in_use
};

inline void to_json(json& j, const DataSourceInfo& ds) {
    j = {
        {"id", ds.id}, {"name", ds.name}, {"type", ds.type},
        {"path", ds.path}, {"buffer_count", ds.buffer_count},
        {"max_frames", ds.max_frames}, {"loop", ds.loop},
        {"created_at", ds.created_at}, {"status", ds.status}
    };
}

inline void from_json(const json& j, DataSourceInfo& ds) {
    if (j.contains("id"))           j.at("id").get_to(ds.id);
    if (j.contains("name"))         j.at("name").get_to(ds.name);
    if (j.contains("type"))         j.at("type").get_to(ds.type);
    if (j.contains("path"))         j.at("path").get_to(ds.path);
    if (j.contains("buffer_count")) j.at("buffer_count").get_to(ds.buffer_count);
    if (j.contains("max_frames"))   j.at("max_frames").get_to(ds.max_frames);
    if (j.contains("loop"))         j.at("loop").get_to(ds.loop);
    if (j.contains("created_at"))   j.at("created_at").get_to(ds.created_at);
    if (j.contains("status"))       j.at("status").get_to(ds.status);
}

// ============================================================
// Worker 状态
// ============================================================

enum class WorkerState {
    CREATED,
    STARTING,
    RUNNING,
    STOPPING,
    STOPPED,
    ERROR
};

NLOHMANN_JSON_SERIALIZE_ENUM(WorkerState, {
    {WorkerState::CREATED, "CREATED"},
    {WorkerState::STARTING, "STARTING"},
    {WorkerState::RUNNING, "RUNNING"},
    {WorkerState::STOPPING, "STOPPING"},
    {WorkerState::STOPPED, "STOPPED"},
    {WorkerState::ERROR, "ERROR"},
})

// ============================================================
// 解码器配置 (API 层面)
// ============================================================

struct ApiDecoderConfig {
    std::optional<std::string> name;
    bool enable_hardware = true;
    int decode_threads = 0;
};

inline void to_json(json& j, const ApiDecoderConfig& c) {
    j = {
        {"name", c.name.has_value() ? json(c.name.value()) : json(nullptr)},
        {"enable_hardware", c.enable_hardware},
        {"decode_threads", c.decode_threads}
    };
}

inline void from_json(const json& j, ApiDecoderConfig& c) {
    if (j.contains("name") && !j["name"].is_null())
        c.name = j["name"].get<std::string>();
    if (j.contains("enable_hardware"))
        j.at("enable_hardware").get_to(c.enable_hardware);
    if (j.contains("decode_threads"))
        j.at("decode_threads").get_to(c.decode_threads);
}

// ============================================================
// Worker 模型
// ============================================================

struct WorkerInfo {
    std::string id;
    std::string name;
    std::string datasource_id;
    std::string datasource_name;
    WorkerState state = WorkerState::CREATED;
    std::string worker_type = "FFMPEG_DECODE";
    ApiDecoderConfig decoder;
    std::string created_at;
    std::vector<std::string> consumers;
};

inline void to_json(json& j, const WorkerInfo& w) {
    j = {
        {"id", w.id}, {"name", w.name},
        {"datasource_id", w.datasource_id},
        {"datasource_name", w.datasource_name},
        {"state", w.state}, {"worker_type", w.worker_type},
        {"decoder", w.decoder}, {"created_at", w.created_at},
        {"consumers", w.consumers}
    };
}

inline void from_json(const json& j, WorkerInfo& w) {
    if (j.contains("id"))              j.at("id").get_to(w.id);
    if (j.contains("name"))            j.at("name").get_to(w.name);
    if (j.contains("datasource_id"))   j.at("datasource_id").get_to(w.datasource_id);
    if (j.contains("worker_type"))     j.at("worker_type").get_to(w.worker_type);
    if (j.contains("decoder"))         j.at("decoder").get_to(w.decoder);
}

// ============================================================
// 消费者类型
// ============================================================

enum class ConsumerType {
    DISPLAY,
    SAVE_RAW,
    SAVE_ENCODED,
    COMPARE,
    OPENCV,
    NPU_INFERENCE,
    JPEG_PREVIEW,
    COUNT
};

NLOHMANN_JSON_SERIALIZE_ENUM(ConsumerType, {
    {ConsumerType::DISPLAY, "DISPLAY"},
    {ConsumerType::SAVE_RAW, "SAVE_RAW"},
    {ConsumerType::SAVE_ENCODED, "SAVE_ENCODED"},
    {ConsumerType::COMPARE, "COMPARE"},
    {ConsumerType::OPENCV, "OPENCV"},
    {ConsumerType::NPU_INFERENCE, "NPU_INFERENCE"},
    {ConsumerType::JPEG_PREVIEW, "JPEG_PREVIEW"},
    {ConsumerType::COUNT, "COUNT"},
})

// ============================================================
// 消费者模型
// ============================================================

struct ConsumerInfo {
    std::string id;
    ConsumerType type = ConsumerType::COUNT;
    std::string state = "inactive";
    json config = json::object();
};

inline void to_json(json& j, const ConsumerInfo& c) {
    j = {{"id", c.id}, {"type", c.type}, {"state", c.state}, {"config", c.config}};
}

inline void from_json(const json& j, ConsumerInfo& c) {
    if (j.contains("id"))     j.at("id").get_to(c.id);
    if (j.contains("type"))   j.at("type").get_to(c.type);
    if (j.contains("state"))  j.at("state").get_to(c.state);
    if (j.contains("config")) j.at("config").get_to(c.config);
}

// ============================================================
// BufferPool 状态
// ============================================================

struct BufferPoolStatus {
    int total = 0;
    int free_count = 0;
    int filled_count = 0;
};

inline void to_json(json& j, const BufferPoolStatus& b) {
    j = {{"total", b.total}, {"free", b.free_count}, {"filled", b.filled_count}};
}

// ============================================================
// Worker 实时状态
// ============================================================

struct WorkerStatus {
    std::string id;
    WorkerState state = WorkerState::CREATED;
    double fps = 0.0;
    int64_t decoded_frames = 0;
    int64_t dropped_frames = 0;
    double uptime_seconds = 0.0;
    BufferPoolStatus buffer_pool;
    std::vector<ConsumerInfo> consumers;
    std::string command_line;
    std::string output;
};

inline void to_json(json& j, const WorkerStatus& s) {
    j = {
        {"id", s.id}, {"state", s.state}, {"fps", s.fps},
        {"decoded_frames", s.decoded_frames},
        {"dropped_frames", s.dropped_frames},
        {"uptime_seconds", s.uptime_seconds},
        {"buffer_pool", s.buffer_pool},
        {"consumers", s.consumers},
        {"command_line", s.command_line},
        {"output", s.output}
    };
}

// ============================================================
// 录制模型
// ============================================================

struct RecordingInfo {
    std::string id;
    std::string datasource_id;
    std::string file_path;
    std::string format = "mp4";
    double duration_seconds = 0.0;
    int64_t file_size_bytes = 0;
    int64_t total_frames = 0;
    std::string created_at;
};

inline void to_json(json& j, const RecordingInfo& r) {
    j = {
        {"id", r.id}, {"datasource_id", r.datasource_id},
        {"file_path", r.file_path}, {"format", r.format},
        {"duration_seconds", r.duration_seconds},
        {"file_size_bytes", r.file_size_bytes},
        {"total_frames", r.total_frames},
        {"created_at", r.created_at}
    };
}

// ============================================================
// 探测结果
// ============================================================

struct ProbeResult {
    std::string format;
    std::string codec;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double duration_seconds = -1.0;
    int64_t bitrate = 0;
    std::string pixel_format;
};

inline void to_json(json& j, const ProbeResult& p) {
    j = {
        {"format", p.format}, {"codec", p.codec},
        {"width", p.width}, {"height", p.height},
        {"fps", p.fps}, {"duration_seconds", p.duration_seconds},
        {"bitrate", p.bitrate}, {"pixel_format", p.pixel_format}
    };
}

// ============================================================
// 文件浏览
// ============================================================

struct FileEntry {
    std::string name;
    std::string path;
    std::string type;      // "file" | "directory"
    int64_t size_bytes = 0;
    std::string modified_at;
    std::string extension;
};

inline void to_json(json& j, const FileEntry& f) {
    j = {
        {"name", f.name}, {"path", f.path}, {"type", f.type},
        {"size_bytes", f.size_bytes}, {"modified_at", f.modified_at},
        {"extension", f.extension}
    };
}

} // namespace webui

#endif // WEBUI_API_TYPES_HPP
