#ifndef WORKER_BASE_HPP
#define WORKER_BASE_HPP

#include "productionline/worker/IDataSourceNavigator.hpp"
#include "productionline/worker/IEncodedPacketSource.hpp"
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include "productionline/worker/WorkerConfig.hpp"

extern "C" {
#include <libavutil/error.h>
}
#include "buffer/bufferpool/Buffer.hpp"
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include "buffer/BufferAllocatorFacade.hpp"
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include "buffer/BufferAllocatorFactory.hpp"
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include "buffer/bufferpool/BufferPool.hpp"
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <memory>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <utility>  // for std::move
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <map>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <vector>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <optional>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <string>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

// FFmpeg 头文件（用于编解码器类型检测）
extern "C" {
#include <libavcodec/avcodec.h>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
}

/**
 * @brief BufferPool 类型枚举（统一规范）
 * 
 * 定义 Worker 可以创建的所有 BufferPool 类型
 * 所有 Worker 使用此枚举标识其输出的 BufferPool
 * 
 * v2.0 设计原则：
 * - 统一规范：所有 Worker 共用此枚举
 * - 类型安全：编译期检查，避免字符串拼写错误
 * - 易于扩展：添加新类型只需在此枚举中增加
 */
enum class BufferPoolType {
    // ========== 视频相关 ==========
    DECODE_VIDEO_PRIMARY,      // 主视频解码输出（默认通道）
    DECODE_VIDEO_SECONDARY,    // 副视频解码输出（如 TACO CH1）
    DECODE_VIDEO_THUMBNAIL,    // 视频缩略图输出
    DECODE_VIDEO_PREVIEW,      // 视频预览输出（低分辨率）
    
    // ========== 音频相关 ==========
    DECODE_AUDIO_PRIMARY,      // 主音频解码输出
    DECODE_AUDIO_SECONDARY,    // 副音频解码输出（多声道）
    
    // ========== 数据包相关 ==========
    PACKET_VIDEO,              // 视频 AVPacket 缓冲池
    PACKET_AUDIO,              // 音频 AVPacket 缓冲池
    PACKET_SUBTITLE,           // 字幕 AVPacket 缓冲池
    
    // ========== 编码相关 ==========
    ENCODE_VIDEO_INPUT,        // 编码器输入 BufferPool
    ENCODE_VIDEO_OUTPUT,       // 编码器输出 BufferPool
    ENCODE_AUDIO_INPUT,        // 音频编码器输入
    ENCODE_AUDIO_OUTPUT,       // 音频编码器输出
    
    // ========== 特殊用途 ==========
    RAW_FILE_READ,             // 原始文件读取缓冲池
    FRAMEBUFFER_OUTPUT,        // Framebuffer 输出缓冲池
    NETWORK_STREAM,            // 网络流缓冲池
    
    // ========== 扩展保留 ==========
    CUSTOM_1,                  // 自定义类型 1
    CUSTOM_2,                  // 自定义类型 2
    CUSTOM_3                   // 自定义类型 3
};

// ============================================================
// FillStatus - Buffer 填充结果状态（v2.33 新增）
// ============================================================

/**
 * @brief Buffer 填充结果状态（v2.33 新增，v2.34 重构为两值 + 错误来源分层）
 * 
 * v2.34 设计变更：
 * - FillStatus 只表达"成功"或"错误"两种结果
 * - 具体错误来源由 ErrorSource 枚举表达（Acquire / Codec / Worker）
 * - 具体错误原因由各模块的 cause 枚举表达（AcquireStatus / CodecStatus / WorkerStatus）
 */
enum class FillStatus : int {
    Success = 0,       ///< 成功
    Error = -1         ///< 错误（具体看 source + cause）
};

/**
 * @brief 错误来源模块（v2.34 新增）
 */
enum class ErrorSource : int {
    None = 0,          ///< 无错误来源（Success 时）
    Acquire = 1,       ///< 来自数据获取层（File/RTSP/Buffer）
    Codec = 2,         ///< 来自编解码器层
    Worker = 3         ///< 来自 Worker 自身（状态/参数）
};

/**
 * @brief Codec 层错误状态（v2.34 新增，v2.35 重构：细分错误码 + 新增 CodecSendResult）
 * 
 * v2.35 设计变更：
 * - 新增 Eof：区分 Codec 层 flush 完成（AVERROR_EOF）和 Acquire 层数据源 EOF
 * - 新增 InvalidState：区分编解码器状态错误（EINVAL）和码流损坏（DecodeError）
 * - 新增 EncodeError：区分编码器错误和解码器错误
 * - 新增 CodecSendResult 结果类：集中 FFmpeg 错误码映射，与 PacketAcquireResult 对称
 */
enum class CodecStatus : int {
    // ===== 业务状态 =====
    Success = 0,           ///< 成功
    Eagain = 1,            ///< 解码器/编码器需要更多输入（AVERROR(EAGAIN)）
    Eof = -1,              ///< 编解码器 flush 完成（AVERROR_EOF）
    
    // ===== 解码器错误 =====
    SendFailed = -2,       ///< avcodec_send_packet EAGAIN 重试耗尽
    InvalidState = -3,     ///< 编解码器状态错误（AVERROR(EINVAL)，未正确打开等）
    DecodeError = -4,      ///< 码流损坏等解码错误（其他未识别错误码）
    ReceiveError = -5,     ///< avcodec_receive_frame 失败
    AllocFailed = -6,      ///< Codec 内部内存分配失败（AVERROR(ENOMEM)）
    
    // ===== 编码器错误 =====
    EncodeError = -7       ///< avcodec_send_frame 失败（非 EAGAIN）
};

/**
 * @brief Worker 层错误状态（v2.34 新增）
 */
enum class WorkerStatus : int {
    Success = 0,           ///< 成功
    InvalidParam = -1,     ///< 参数无效（空指针等）
    NotOpen = -2,          ///< Worker 未打开/未初始化
    InternalError = -3     ///< 内部逻辑错误（不应到达的代码路径）
};

/**
 * @brief 获取 FillStatus 的字符串描述
 */
inline const char* fillStatusToString(FillStatus status) {
    switch (status) {
        case FillStatus::Success: return "Success";
        case FillStatus::Error:   return "Error";
        default:                  return "Unknown";
    }
}

/**
 * @brief 获取 ErrorSource 的字符串描述
 */
inline const char* errorSourceToString(ErrorSource source) {
    switch (source) {
        case ErrorSource::None:    return "None";
        case ErrorSource::Acquire: return "Acquire";
        case ErrorSource::Codec:   return "Codec";
        case ErrorSource::Worker:  return "Worker";
        default:                   return "Unknown";
    }
}

/**
 * @brief 获取 CodecStatus 的字符串描述
 */
inline const char* codecStatusToString(CodecStatus status) {
    switch (status) {
        case CodecStatus::Success:      return "Success";
        case CodecStatus::Eagain:       return "Eagain";
        case CodecStatus::Eof:          return "Eof";
        case CodecStatus::SendFailed:   return "SendFailed";
        case CodecStatus::InvalidState: return "InvalidState";
        case CodecStatus::DecodeError:  return "DecodeError";
        case CodecStatus::ReceiveError: return "ReceiveError";
        case CodecStatus::AllocFailed:  return "AllocFailed";
        case CodecStatus::EncodeError:  return "EncodeError";
        default:                        return "Unknown";
    }
}

/**
 * @brief 获取 WorkerStatus 的字符串描述
 */
inline const char* workerStatusToString(WorkerStatus status) {
    switch (status) {
        case WorkerStatus::Success:       return "Success";
        case WorkerStatus::InvalidParam:  return "InvalidParam";
        case WorkerStatus::NotOpen:       return "NotOpen";
        case WorkerStatus::InternalError: return "InternalError";
        default:                          return "Unknown";
    }
}

// ============================================================
// CodecSendResult - Codec 层操作结果类（v2.35 新增）
// ============================================================

/**
 * @brief Codec 层操作结果（v2.35 新增，与 PacketAcquireResult 对称设计）
 * 
 * 封装 CodecStatus + 查询方法，FFmpeg 错误码映射由各调用点自行完成
 * （与 PacketAcquireResult 在各 EncodedPacketSource 实现类中映射的风格一致）。
 * 
 * 使用示例：
 * @code
 * // 解码器：发送 packet 后映射错误码
 * int ret = avcodec_send_packet(ctx, pkt);
 * if (ret == 0)               return FillResult::success();
 * if (ret == AVERROR_EOF)     return FillResult::fromCodec(CodecSendResult::eof());
 * if (ret == AVERROR(EINVAL)) return FillResult::fromCodec(CodecSendResult::invalidState());
 * if (ret == AVERROR(ENOMEM)) return FillResult::fromCodec(CodecSendResult::allocFailed());
 * return FillResult::fromCodec(CodecSendResult::decodeError());
 * @endcode
 */
class CodecSendResult {
public:
    // ===== 工厂方法 =====
    
    /// 成功
    static CodecSendResult success() { return CodecSendResult(CodecStatus::Success); }
    
    /// 编解码器需要更多输入/输出
    static CodecSendResult eagain() { return CodecSendResult(CodecStatus::Eagain); }
    
    /// 编解码器 flush 完成
    static CodecSendResult eof() { return CodecSendResult(CodecStatus::Eof); }
    
    /// avcodec_send_packet EAGAIN 重试耗尽
    static CodecSendResult sendFailed() { return CodecSendResult(CodecStatus::SendFailed); }
    
    /// 编解码器状态错误（EINVAL）
    static CodecSendResult invalidState() { return CodecSendResult(CodecStatus::InvalidState); }
    
    /// 码流损坏等解码错误
    static CodecSendResult decodeError() { return CodecSendResult(CodecStatus::DecodeError); }
    
    /// avcodec_receive_frame 失败
    static CodecSendResult receiveError() { return CodecSendResult(CodecStatus::ReceiveError); }
    
    /// Codec 内部内存分配失败
    static CodecSendResult allocFailed() { return CodecSendResult(CodecStatus::AllocFailed); }
    
    /// 编码器错误
    static CodecSendResult encodeError() { return CodecSendResult(CodecStatus::EncodeError); }
    
    // ===== 查询方法 =====
    
    /// 是否成功
    bool ok() const noexcept { return status_ == CodecStatus::Success; }
    
    /// codec flush pipeline 已清空（avcodec_send_packet/frame 返回 AVERROR_EOF）
    bool isEoFlush() const noexcept { return status_ == CodecStatus::Eof; }

    /// 是否 EAGAIN
    bool isEagain() const noexcept { return status_ == CodecStatus::Eagain; }
    
    /// 是否可重试（Eagain）
    bool isRetryable() const noexcept { return isEagain(); }
    
    /// 是否终止错误
    bool isTerminal() const noexcept {
        return !ok() && !isEoFlush() && !isEagain();
    }
    
    /// 获取状态码
    CodecStatus status() const noexcept { return status_; }
    
    /// 获取状态字符串
    const char* statusString() const noexcept { return codecStatusToString(status_); }
    
    /// 隐式 bool 转换
    explicit operator bool() const noexcept { return ok(); }

private:
    explicit CodecSendResult(CodecStatus status) : status_(status) {}
    CodecStatus status_;
};

// ============================================================
// FillResult - Buffer 填充结果类（v2.33 新增）
// ============================================================

/**
 * @brief Buffer 填充结果（v2.33 新增，v2.34 重构为三层错误查询，v2.36 新增消费者决策接口）
 * 
 * v2.34 设计变更：错误来源分层 + 各层携带自己的 cause
 * v2.36 设计变更：新增消费者决策接口（ConsumerAction / toAction() / shouldTerminate() / shouldBypassFrameSync()）
 * 
 * 推荐消费方式（v2.36）：
 * @code
 * FillResult result = worker->fillBuffer(index, buffer);
 * 
 * switch (result.toAction()) {
 *     case FillResult::ConsumerAction::kSubmit:
 *         // ✅ 提交 buffer
 *         break;
 *     case FillResult::ConsumerAction::kSkip:
 *         // ⏭ 跳过当前 packet（PacketAlreadyProcessed / NonVideoPacket / InvalidData）
 *         break;
 *     case FillResult::ConsumerAction::kRetry:
 *         // 🔄 重试当前操作（Again / TimedOut / CodecEagain）
 *         break;
     *     case FillResult::ConsumerAction::kTerminate:
 *         // 正常结束：codec flush 完 or 数据源到头
 *         // 真正错误：result.source() + result.statusString()
 *         if (result.isEoFlush() || worker->isAtEnd()) { ... }
 *         break;
 * }
 * @endcode
 */
class FillResult {
public:
    // ===== 核心工厂方法 =====
    
    /// 成功
    static FillResult success() {
        return FillResult(FillStatus::Success);
    }
    
    /// 从 Acquire 层结果构造
    static FillResult fromAcquire(const PacketAcquireResult& result) {
        if (result.ok()) return success();
        FillResult r(FillStatus::Error);
        r.source_ = ErrorSource::Acquire;
        r.acquire_cause_ = result.status();
        return r;
    }
    
    /// 从 Codec 层结果构造（v2.35 重构：接受 CodecSendResult）
    static FillResult fromCodec(const CodecSendResult& result) {
        if (result.ok()) return success();
        FillResult r(FillStatus::Error);
        r.source_ = ErrorSource::Codec;
        r.codec_cause_ = result.status();
        return r;
    }
    
    /// 从 Codec 层错误码直接构造（保留向后兼容）
    static FillResult fromCodec(CodecStatus cause) {
        if (cause == CodecStatus::Success) return success();
        FillResult r(FillStatus::Error);
        r.source_ = ErrorSource::Codec;
        r.codec_cause_ = cause;
        return r;
    }
    
    /// 从 Worker 层结果构造
    static FillResult fromWorker(WorkerStatus cause) {
        if (cause == WorkerStatus::Success) return success();
        FillResult r(FillStatus::Error);
        r.source_ = ErrorSource::Worker;
        r.worker_cause_ = cause;
        return r;
    }
    
    // ===== 向后兼容便捷方法 =====
    // Acquire 层便捷方法（仍在使用）
    static FillResult nonVideoPacket()         { return fromAcquire(PacketAcquireResult::nonVideoPacket()); }
    // Worker 层便捷方法（仍在使用）
    static FillResult invalidParam()           { return fromWorker(WorkerStatus::InvalidParam); }
    static FillResult notOpen()                { return fromWorker(WorkerStatus::NotOpen); }
    static FillResult internalError()          { return fromWorker(WorkerStatus::InternalError); }
    
    // ===== 第一层查询：成功还是失败 =====
    
    /// 获取状态
    FillStatus status() const noexcept { return status_; }
    
    /// 是否成功
    bool ok() const noexcept { return status_ == FillStatus::Success; }
    
    /// 是否错误
    bool isError() const noexcept { return status_ == FillStatus::Error; }
    
    /// 隐式 bool 转换
    explicit operator bool() const noexcept { return ok(); }
    
    // ===== 错误分类查询方法（v2.34 重构：拆分 shouldRetry 为 shouldContinue + shouldRetry）=====

    /**
     * @brief Codec 内部 flush pipeline 已清空（v2.36 重命名，语义收窄）
     * 
     * 仅检查 Codec 层 EOF（avcodec_send_packet/frame 返回 AVERROR_EOF），
     * 表示解码器/编码器的内部 pipeline 已完全 flush 清空。
     * 
     * @note 不代表数据源到达文件末尾。数据源是否结束请查询 worker->isAtEnd()。
     *       v2.35 旧名：isEof()，原来同时覆盖 AcquireStatus::Eof，已拆分。
     */
    bool isEoFlush() const noexcept {
        return isCodecError() && codec_cause_ == CodecStatus::Eof;
    }

    /**
     * @brief 数据获取层报告数据源已耗尽（AcquireStatus::Eof）
     * 
     * 表示 packet 获取层（文件/流/buffer）在本次 fillBuffer() 中明确报告"无更多数据"。
     * 此时 worker->isAtEnd() 通常也同时为 true（两者来自同一代码路径），
     * 但 isAtEnd() 是权威来源，消费者应优先使用 isAtEnd()。
     * 
     * @note 设计用途：在 WorkerSyncCoordinator 等无法访问 worker 对象的场景中，
     *       通过 FillResult 本身区分"干净退出"与"真正错误"。
     */
    bool isAcquireEof() const noexcept {
        return isAcquireError() && acquire_cause_ == AcquireStatus::Eof;
    }
    
    /// 是否应该 continue（跳过当前 packet，获取下一个）
    /// 适用于：当前 packet 无意义或已损坏，跳过即可
    bool shouldContinue() const noexcept {
        if (!isError()) return false;
        if (isAcquireError()) {
            return acquire_cause_ == AcquireStatus::PacketAlreadyProcessed ||
                   acquire_cause_ == AcquireStatus::NonVideoPacket ||
                   acquire_cause_ == AcquireStatus::InvalidData;
        }
        return false;
    }
    
    /// 是否应该 retry（重试当前读取/解码操作）
    /// 适用于：暂时性问题，重试同一操作可能成功
    bool shouldRetry() const noexcept {
        if (!isError()) return false;
        // Acquire 层：暂时无数据 / 网络超时 → 重试当前读取
        if (isAcquireError()) {
            return acquire_cause_ == AcquireStatus::Again ||
                   acquire_cause_ == AcquireStatus::TimedOut;
        }
        // Codec 层：解码器需要更多输入 → 重试（再送一个 packet）
        if (isCodecError()) {
            return codec_cause_ == CodecStatus::Eagain;
        }
        return false;
    }
    
    /**
     * @brief 是否是不可恢复的异常错误（排除法）
     * 
     * 定义：既不能 continue、也不能 retry、也不是 codec flush EOF、也不是 data source EOF
     * 这类错误才应计入连续失败计数。
     */
    bool isTerminal() const noexcept {
        return isError() && !shouldContinue() && !shouldRetry() && !isEoFlush() && !isAcquireEof();
    }
    
    // ===== v2.36 消费者决策接口 =====
    
    /**
     * @brief 消费者行动指令枚举
     * 
     * 将所有 FillResult 状态映射为消费者循环中的四种互斥行动。
     * 配合 toAction() 使用，让 switch 语句穷举所有 case，
     * 避免 if-else if 链遗漏分支（编译器会警告缺失的 case）。
     * 
     * 注：shouldContinue() / shouldRetry() / shouldTerminate() 仍可单独使用，
     * toAction() 是在此基础上提供的 switch 聚合入口。
     */
    enum class ConsumerAction {
        kSubmit,    ///< ok()：填充成功，提交 buffer
        kSkip,      ///< shouldContinue()：跳过当前 packet，获取下一个
        kRetry,     ///< shouldRetry()：重试当前操作
        kTerminate, ///< shouldTerminate()：终止循环（配合 isAtEnd()/isEoFlush() 区分正常结束与错误中止）
    };
    
    /**
     * @brief 将 FillResult 映射为消费者行动指令
     * 
     * 聚合 shouldContinue() / shouldRetry() / shouldTerminate()，
     * 供消费者 switch 语句使用，保证四路互斥完备。
     * 
     * 使用示例：
     * @code
     * switch (result.toAction()) {
     *     case FillResult::ConsumerAction::kSubmit:    // 提交 buffer
     *     case FillResult::ConsumerAction::kSkip:      // continue
     *     case FillResult::ConsumerAction::kRetry:     // continue（重试）
     *     case FillResult::ConsumerAction::kTerminate: // break（配合 isAtEnd()/isEoFlush() 区分结束与错误）
     * }
     * @endcode
     */
    ConsumerAction toAction() const noexcept {
        if (ok())             return ConsumerAction::kSubmit;
        if (shouldContinue()) return ConsumerAction::kSkip;
        if (shouldRetry())    return ConsumerAction::kRetry;
        return                       ConsumerAction::kTerminate;
    }
    
    /**
     * @brief 是否应该终止循环（break）
     * 
     * 覆盖所有非 ok/skip/retry 的情况：
     *   - isAcquireEof()：数据获取层报告数据源耗尽
     *   - isEoFlush()：codec flush pipeline 清空
     *   - isTerminal()：不可恢复的真正错误
     * 
     * 与 shouldContinue() / shouldRetry() 合并后，四路互斥完备（恒为 true）。
     * 消费者通过 worker->isAtEnd() 或 isEoFlush() 区分"正常结束"与"异常中止"。
     */
    bool shouldTerminate() const noexcept {
        return !ok() && !shouldContinue() && !shouldRetry();
    }
    
    /**
     * @brief 是否完全绕过帧同步点（不进入 arrive()，不调用 commit）
     * 
     * 适用于：packet 未被实际消费、帧版本号未推进的情况。
     * 此时两路 Worker 均会同时得到相同状态，无需进入同步协调器。
     * 
     * 当前适用状态：PacketAlreadyProcessed / NonVideoPacket
     * 
     * @note 与 shouldContinue() 的区别：InvalidData 属于 shouldContinue()
     *       但帧版本已推进，仍需进入同步点；而 PacketAlreadyProcessed /
     *       NonVideoPacket 帧版本未推进，直接绕过。
     */
    bool shouldBypassFrameSync() const noexcept {
        if (!isAcquireError()) return false;
        return acquire_cause_ == AcquireStatus::PacketAlreadyProcessed ||
               acquire_cause_ == AcquireStatus::NonVideoPacket;
    }
    
    // ===== 第二层查询：哪个模块的错误 =====
    
    /// 获取错误来源
    ErrorSource source() const noexcept { return source_; }
    
    /// 是否是 Acquire 层错误
    bool isAcquireError() const noexcept { return isError() && source_ == ErrorSource::Acquire; }
    
    /// 是否是 Codec 层错误
    bool isCodecError() const noexcept { return isError() && source_ == ErrorSource::Codec; }
    
    /// 是否是 Worker 层错误
    bool isWorkerError() const noexcept { return isError() && source_ == ErrorSource::Worker; }
    
    // ===== 第三层查询：具体错误类型 =====
    
    /// 获取 Acquire 层具体错误（仅当 isAcquireError() 时有意义）
    AcquireStatus acquireCause() const noexcept { return acquire_cause_; }
    
    /// 获取 Codec 层具体错误（仅当 isCodecError() 时有意义）
    CodecStatus codecCause() const noexcept { return codec_cause_; }
    
    /// 获取 Worker 层具体错误（仅当 isWorkerError() 时有意义）
    WorkerStatus workerCause() const noexcept { return worker_cause_; }
    
    /// 获取完整的状态描述
    const char* statusString() const noexcept {
        if (ok()) return "Success";
        switch (source_) {
            case ErrorSource::Acquire: return acquireStatusToString(acquire_cause_);
            case ErrorSource::Codec:   return codecStatusToString(codec_cause_);
            case ErrorSource::Worker:  return workerStatusToString(worker_cause_);
            default:                   return "Unknown";
        }
    }

private:
    explicit FillResult(FillStatus status)
        : status_(status), source_(ErrorSource::None)
        , acquire_cause_(AcquireStatus::Success)
        , codec_cause_(CodecStatus::Success)
        , worker_cause_(WorkerStatus::Success) {}
    
    FillStatus    status_;
    ErrorSource   source_;
    AcquireStatus acquire_cause_;
    CodecStatus   codec_cause_;
    WorkerStatus  worker_cause_;
};

/**
 * @brief BufferPoolType 转字符串（调试用）
 */
inline const char* bufferPoolTypeToString(BufferPoolType type) {
    switch (type) {
        case BufferPoolType::DECODE_VIDEO_PRIMARY:    return "DECODE_VIDEO_PRIMARY";
        case BufferPoolType::DECODE_VIDEO_SECONDARY:  return "DECODE_VIDEO_SECONDARY";
        case BufferPoolType::DECODE_VIDEO_THUMBNAIL:  return "DECODE_VIDEO_THUMBNAIL";
        case BufferPoolType::DECODE_VIDEO_PREVIEW:    return "DECODE_VIDEO_PREVIEW";
        case BufferPoolType::DECODE_AUDIO_PRIMARY:    return "DECODE_AUDIO_PRIMARY";
        case BufferPoolType::DECODE_AUDIO_SECONDARY:  return "DECODE_AUDIO_SECONDARY";
        case BufferPoolType::PACKET_VIDEO:            return "PACKET_VIDEO";
        case BufferPoolType::PACKET_AUDIO:            return "PACKET_AUDIO";
        case BufferPoolType::PACKET_SUBTITLE:         return "PACKET_SUBTITLE";
        case BufferPoolType::ENCODE_VIDEO_INPUT:      return "ENCODE_VIDEO_INPUT";
        case BufferPoolType::ENCODE_VIDEO_OUTPUT:     return "ENCODE_VIDEO_OUTPUT";
        case BufferPoolType::ENCODE_AUDIO_INPUT:      return "ENCODE_AUDIO_INPUT";
        case BufferPoolType::ENCODE_AUDIO_OUTPUT:     return "ENCODE_AUDIO_OUTPUT";
        case BufferPoolType::RAW_FILE_READ:           return "RAW_FILE_READ";
        case BufferPoolType::FRAMEBUFFER_OUTPUT:      return "FRAMEBUFFER_OUTPUT";
        case BufferPoolType::NETWORK_STREAM:          return "NETWORK_STREAM";
        case BufferPoolType::CUSTOM_1:                return "CUSTOM_1";
        case BufferPoolType::CUSTOM_2:                return "CUSTOM_2";
        case BufferPoolType::CUSTOM_3:                return "CUSTOM_3";
        default:                                      return "UNKNOWN";
    }
}

/**
 * @brief WorkerBase - Worker基类
 * 
 * 架构角色：抽象基类（Abstract Base Class）
 * 
 * 设计变更（v2.0）：
 * - 去除 IBufferFillingWorker 接口类
 * - 直接在 WorkerBase 中定义 Buffer 填充相关的纯虚函数
 * - 简化架构，减少不必要的抽象层
 * 
 * 设计目的：
 * - 统一所有Worker实现类的基类
 * - 定义 Buffer 填充功能（原 IBufferFillingWorker 的方法）
 * - 继承文件导航功能（IVideoFileNavigator 接口）
 * - 提供统一的Allocator和BufferPool管理（所有Worker的共同职责）
 * - 采用构造函数参数传递模式，父类统一管理Allocator创建逻辑
 * 
 * 职责：
 * - 作为所有Worker实现类的统一基类
 * - 定义 Buffer 填充功能（纯虚函数，强制子类实现）
 * - 继承文件导航功能（IVideoFileNavigator 接口）
 * - 提供统一的Allocator门面（所有Worker都需要创建BufferPool）
 * - 管理Worker创建的BufferPool（通过Allocator创建）
 * 
 * 继承关系：
 * - WorkerBase 继承 IDataSourceNavigator
 * - 所有具体Worker实现类继承 WorkerBase
 * 
 * 优势：
 * - 架构简化：减少一层接口抽象
 * - 强制实现：通过基类纯虚函数强制子类实现
 * - 易于维护：统一的基类便于扩展和维护
 * - 统一管理：所有Worker自动继承allocator_和buffer_pool_，无需每个子类重复定义
 * - 符合单一职责原则：子类关注业务逻辑，父类关注Allocator管理
 * 
 * 构造函数参数传递模式：
 * - 子类通过初始化列表向父类传递 AllocatorType
 * - 父类在构造函数中统一创建 Allocator
 * - 所有Allocator配置细节封装在Factory中
 * - 子类无需关心Allocator内部实现
 */
class WorkerBase : public IDataSourceNavigator {
public:
    // ==================== 分辨率限制常量 ====================
    
    /// 最小允许的分辨率（宽或高）
    static constexpr int MIN_RESOLUTION = 16;
    
    /// 最大允许的分辨率（宽或高），支持到 8K
    static constexpr int MAX_RESOLUTION = 8192;
    
    // ==================== 构造/析构 ====================
    
    /**
     * @brief 构造函数
     * 
     * Allocator类型选择建议：
     * - NORMAL: Raw视频文件Worker（需要内部分配内存）
     * - AVFRAME: FFmpeg解码Worker（需要动态注入AVFrame）
     * - FRAMEBUFFER: Framebuffer设备Worker（需要包装外部内存）
     * - AUTO: 默认使用NORMAL（不推荐，子类应明确指定）
     * 
     * 构造顺序：
     * 1. 父类 WorkerBase 构造（创建 allocator_facade_）
     * 2. 子类成员变量初始化
     * 3. 子类构造函数体执行
     * 
     * @param allocator_type Allocator类型（子类传递）
     * @param config Worker配置（v2.2新增）
     */
    explicit WorkerBase(
        BufferAllocatorFactory::AllocatorType allocator_type,
        const WorkerConfig& config = WorkerConfig()
    ) : allocator_facade_(allocator_type)  // 🎯 父类直接创建Allocator门面
      , buffer_pool_type_map_()  // v2.0: 初始化 BufferPool 类型映射表
      , worker_config_(config)  // 🎯 v2.2: 存储配置
      , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker")))  // 🎯 初始化 logger
    {
    }
    
    virtual ~WorkerBase() = default;
    
    // ==================== Buffer填充功能（原IBufferFillingWorker的方法）====================
    
    /**
     * @brief 填充Buffer（核心功能）
     * 
     * 纯虚函数：强制所有子类必须实现
     * 
     * @param frame_index 帧索引
     * @param buffer 输出 Buffer（从 BufferPool 获取）
     * @return FillResult 结果对象
     * 
     * v2.33 变更：返回类型从 bool 改为 FillResult
     */
    virtual FillResult fillBuffer(int frame_index, Buffer* buffer) = 0;
    
    /**
     * @brief 从AVFrame元数据中提取硬件解码器的物理内存地址
     * 
     * ⭐ 设计原则：
     * - 由 Worker 负责提取，因为 Worker 知道解码器类型和上下文
     * - 默认实现返回 false（不支持硬件地址提取）
     * - 子类重写实现特定硬件解码器的提取逻辑
     * 
     * ⚠️ 调用时机：
     * - 只在使用硬件解码器时调用（decoder_name 非空且 enable_hardware=true）
     * - 软件解码不应调用此函数
     * 
     * @param frame AVFrame 指针（需要包含 libavcodec/avcodec.h）
     * @param buffer Buffer 指针（用于存储提取的物理地址）
     * @return true 成功提取物理地址，false 提取失败或不支持
     * 
     * @note 扩展点：不同硬件解码器子类可以重写此方法
     *       - h264_taco: 从 metadata 提取 pool_blk_id
     *       - h264_cuvid: 从 CUDA 设备内存获取
     *       - h264_qsv: 从 QSV 表面获取
     */
    virtual bool extractHardwareAddressFromMetadata(struct AVFrame* frame, Buffer* buffer) {
        // 默认实现：不支持硬件地址提取
        // 子类（如 FFmpegDecodeWorker）可以重写此方法
        (void)frame;   // 避免未使用参数警告
        (void)buffer;
        return false;
    }
    
    /**
     * @brief 获取Worker类型名称（用于调试和日志）
     * 
     * 纯虚函数：强制所有子类必须实现
     * 
     * @return 类型名称（如 "FFmpegDecodeWorker"、"MmapRawVideoFileWorker"）
     */
    virtual const char* getWorkerType() const = 0;
    
    /**
     * @brief 获取指定类型的 BufferPool ID（主要接口）
     * 
     * v2.0 设计：
     * - 使用统一的 BufferPoolType 枚举标识不同用途的 BufferPool
     * - Worker 只记录 pool_id，不持有 Pool 指针
     * - 使用者通过枚举获取 pool_id，再从 Registry 获取 Pool
     * 
     * @param type BufferPool 类型枚举
     * @return uint64_t Pool ID，如果不存在返回 0
     * 
     * @note 使用示例：
     * @code
     * uint64_t video_pool_id = worker->getOutputBufferPoolId(BufferPoolType::DECODE_VIDEO_PRIMARY);
     * uint64_t packet_pool_id = worker->getOutputBufferPoolId(BufferPoolType::PACKET_VIDEO);
     * @endcode
     */
    virtual uint64_t getOutputBufferPoolId(BufferPoolType type) const {
        auto it = buffer_pool_type_map_.find(type);
        return (it != buffer_pool_type_map_.end()) ? it->second : 0;
    }
    
    /**
     * @brief 检查是否存在指定类型的 BufferPool
     * 
     * @param type BufferPool 类型
     * @return true 存在，false 不存在
     */
    virtual bool hasBufferPoolType(BufferPoolType type) const {
        return buffer_pool_type_map_.find(type) != buffer_pool_type_map_.end();
    }
    
    /**
     * @brief 获取 Worker 的主要 BufferPool 类型
     * 
     * 这是一个查询方法，告诉调用者这个 Worker 的主要输出是什么类型。
     * 子类可以重写此方法以返回正确的主要类型。
     * 
     * @return BufferPoolType 主要 BufferPool 类型
     * 
     * @note 默认返回 DECODE_VIDEO_PRIMARY（适用于视频解码 Worker）
     * @note FfmpegRecordRtspWorker 应该重写为 PACKET_VIDEO
     * 
     * @note 使用示例：
     * @code
     * // 调用者不需要硬编码类型
     * BufferPoolType type = worker->getPrimaryBufferPoolType();
     * uint64_t pool_id = worker->getOutputBufferPoolId(type);
     * @endcode
     */
    virtual BufferPoolType getPrimaryBufferPoolType() const {
        return BufferPoolType::DECODE_VIDEO_PRIMARY;  // 默认值
    }
    
    // ==================== 编解码器参数获取功能（v2.14新增）====================
    
    /**
     * @brief 获取编解码器参数（用于 BufferWriter 等场景）
     * 
     * v2.14 设计：
     * - 虚函数：子类根据实际情况重写
     * - 默认实现：返回 nullptr（不支持编解码器参数的 Worker）
     * - 适用场景：Packet录制、编码器等需要提供编解码器信息的 Worker
     * 
     * @return AVCodecParameters* 编解码器参数指针，如果不可用则返回 nullptr
     * 
     * @note 子类实现示例：
     * @code
     * // FfmpegPacketRecorderWorker 实现
     * const AVCodecParameters* getCodecParameters() const override {
     *     return packet_source_ ? packet_source_->getCodecParameters() : nullptr;
     * }
     * @endcode
     */
    virtual const AVCodecParameters* getCodecParameters() const override {
        // 默认实现：不支持编解码器参数
        return nullptr;
    }
    
    /**
     * @brief 向后兼容的别名（deprecated，请使用 getCodecParameters()）
     */
    const AVCodecParameters* getSourceCodecParameters() const {
        return getCodecParameters();
    }
    
    /**
     * @brief 设置源 BufferPool（用于 Buffer 模式）
     * 
     * 功能：在 Buffer 模式下，关联 Record Worker 的 BufferPool
     * 
     * 使用场景：
     * - MultiWorkerProductionLine 场景
     * - 消费者 Worker 从生产者 Worker 的 BufferPool 获取数据
     * 
     * 默认实现：返回 false（不支持 Buffer 模式）
     * 子类（如 FFmpegDecodeWorker）可以重写此方法
     * 
     * @param pool_weak Record Worker 的 BufferPool（weak_ptr）
     * @return true 如果成功设置，false 如果失败（不支持 Buffer 模式）
     * 
     * @note 子类实现示例：
     * @code
     * bool setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) override {
     *     auto* buffer_source = dynamic_cast<EncodedPacketSourceFromBuffer*>(packet_source_.get());
     *     if (!buffer_source) {
     *         return false;
     *     }
     *     buffer_source->setSourceBufferPool(pool_weak);
     *     return true;
     * }
     * @endcode
     */
    virtual bool setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) {
        // 默认实现：不支持 Buffer 模式
        (void)pool_weak;  // 避免未使用参数警告
        return false;
    }
    
    /**
     * @brief 获取输入数据源的原始视频宽度
     * @return 视频宽度（像素），如果不可用则返回 0
     * 
     * @note 这是输入数据源（文件/流）的原始分辨率，不是解码器输出分辨率
     */
    virtual int getSourceWidth() const {
        return 0;
    }
    
    /**
     * @brief 获取输入数据源的原始视频高度
     * @return 视频高度（像素），如果不可用则返回 0
     * 
     * @note 这是输入数据源（文件/流）的原始分辨率，不是解码器输出分辨率
     */
    virtual int getSourceHeight() const {
        return 0;
    }
    
    /**
     * @brief 获取输入数据源的原始像素格式
     * @return AVPixelFormat，如果不可用则返回 AV_PIX_FMT_NONE
     * 
     * @note 这是输入数据源的编码格式，不是解码器输出格式
     */
    virtual AVPixelFormat getSourcePixelFormat() const {
        return AV_PIX_FMT_NONE;
    }
    
    // ==================== Worker 输出属性（处理后的结果）====================
    // 这些是 Worker 处理后的输出属性，不是数据源原始属性
    
    /**
     * @brief 获取 Worker 输出的视频宽度
     * @return 输出宽度（像素），可能与数据源原始宽度不同
     * 
     * @note 这是 Worker 处理后的输出分辨率，不是数据源原始分辨率
     *       - 对于解码Worker：可能经过硬件缩放（如TACO ch1_scale）
     *       - 对于RecorderWorker：等于数据源原始分辨率（不处理）
     *       - 与 getSourceWidth() 的区别：Source是输入，Output是输出
     */
    virtual int getOutputWidth() const = 0;
    
    /**
     * @brief 获取 Worker 输出的视频高度
     * @return 输出高度（像素），可能与数据源原始高度不同
     * 
     * @note 这是 Worker 处理后的输出分辨率，不是数据源原始分辨率
     */
    virtual int getOutputHeight() const = 0;
    
    /**
     * @brief 获取 Worker 输出的每像素字节数
     * 
     * @param channel 通道编号（默认 0）
     *   - channel = 0：主通道（通常是 YUV 格式）
     *   - channel = 1：第二通道（如 TACO 的 RGB 通道）
     * 
     * @return 每像素字节数（浮点数，支持如NV12的1.5字节/像素）
     *   - 返回 0.0 表示该通道不存在或未启用
     * 
     * @note 这是 Worker 解码后输出的像素格式，不是数据源编码格式
     *       - 计算基于 Worker 的解码器输出格式（YUV420、ARGB888等）
     *       - 数据源是压缩的（H.264、H.265），没有"每像素字节数"概念
     *       - 用于计算输出帧大小：getOutputWidth() * getOutputHeight() * getOutputBytesPerPixel()
     * @note 向后兼容：不传参数时等同于 getOutputBytesPerPixel(0)
     */
    virtual double getOutputBytesPerPixel(int channel = 0) const = 0;
    
    /**
     * @brief 获取时间基（用于 BufferWriter 等场景）
     * 
     * v2.14 设计：
     * - 虚函数：子类根据实际情况重写
     * - 默认实现：返回 {1, 25}（25fps）
     * - 适用场景：Packet录制、编码器等需要提供时间基的 Worker
     * 
     * @return AVRational 时间基
     * 
     * @note 子类实现示例：
     * @code
     * // FfmpegPacketRecorderWorker 实现
     * AVRational getTimeBase() const override {
     *     return packet_source_ ? packet_source_->getTimeBase() : AVRational{1, 25};
     * }
     * @endcode
     */
    virtual struct AVRational getTimeBase() const {
        // 默认实现：返回 25fps
        return {1, 25};
    }
    
    // ==================== 解码器配置功能（v2.2新增）====================
    
    /**
     * @brief 设置解码器名称（用于FFmpeg解码Worker）
     * 
     * 默认实现：空操作（不支持解码器配置的Worker忽略此调用）
     * 子类可以重写此方法
     * 
     * @param decoder_name 解码器名称（如 "h264_taco", "h264_cuvid"）
     * 
     * @note 必须在 open() 之前调用
     * @note 只有FFmpeg类型的Worker需要重写此方法
     */
    virtual void setDecoderName(const char* decoder_name) {
        // 默认空实现：不支持解码器配置的 Worker 忽略此调用
        (void)decoder_name;
    }
    
    /**
     * @brief 启用/禁用硬件解码（用于FFmpeg解码Worker）
     * 
     * 默认实现：空操作（不支持硬件解码配置的Worker忽略此调用）
     * 子类可以重写此方法
     * 
     * @param enable true启用硬件解码，false禁用
     * 
     * @note 必须在 open() 之前调用
     * @note 只有FFmpeg类型的Worker需要重写此方法
     */
    virtual void setHardwareDecoder(bool enable) {
        // 默认空实现：不支持硬件解码配置的 Worker 忽略此调用
        (void)enable;
    }
    
    // ==================== 数据源导航功能（继承自IDataSourceNavigator）====================
    // 以下方法继承自 IDataSourceNavigator，子类必须实现
    
    /**
     * @brief 打开数据源（从 worker_config_ 读取所有参数）
     * 
     * v2.13设计：
     * - Worker 从自己的 worker_config_ 读取所有参数
     * - 默认实现：调用 open(path)，从 config 中提取路径
     * - 子类可以重写此方法以实现更复杂的初始化逻辑
     */
    virtual bool open() override {
        // 默认实现：调用 open(path)，从 config 中提取路径
        return open(worker_config_.data_source.path.c_str());
    }
    
    /**
     * @brief 打开数据源（指定路径）
     * 
     * @param path 数据源路径（可以覆盖 config 中的路径）
     * @return 成功返回true
     * 
     * @note 子类必须实现此方法
     */
    virtual bool open(const char* path) override = 0;
    virtual void close() override = 0;
    virtual bool isOpen() const override = 0;
    virtual bool seek(int frame_index) override = 0;
    virtual bool seekToBegin() override = 0;
    virtual bool seekToEnd() override = 0;
    virtual bool skip(int frame_count) override = 0;
    virtual int getTotalFrames() const override = 0;
    virtual int getCurrentFrameIndex() const override = 0;
    virtual size_t getFrameSize() const override = 0;
    virtual long getFileSize() const override = 0;
    virtual std::string getPath() const override = 0;
    virtual bool hasMoreFrames() const override = 0;
    virtual bool isAtEnd() const override = 0;
    virtual SourceType getDataSourceType() const override = 0;
    
protected:
    // ========== 编解码器类型检测工具（v2.18 新增）==========
    
    /**
     * @brief 判断解码器是否是硬件解码器（使用 FFmpeg 官方 API）
     * 
     * v2.18 设计：
     * - 使用 FFmpeg 官方 API 判断（`AV_CODEC_CAP_HARDWARE` 和 `avcodec_get_hw_config`）
     * - 替代不可靠的字符串匹配方式（strstr）
     * - 所有子类通过继承使用，无需重复实现
     * 
     * @param codec AVCodec 指针
     * @return true=硬件解码器，false=软件解码器
     * 
     * @note 判断依据：
     *       1. AVCodec->capabilities 中的 AV_CODEC_CAP_HARDWARE 标志
     *       2. AVCodec 是否有硬件配置（avcodec_get_hw_config）
     * 
     * @note 使用 virtual final 禁止子类覆盖，保证判断逻辑统一
     */
    virtual bool isHardwareDecoder(const AVCodec* codec) const final;
    
    /**
     * @brief 查找指定 codec_id 的纯软件解码器
     * 
     * v2.18 设计：
     * - 遍历所有注册的解码器，使用 isHardwareDecoder() 过滤硬件解码器
     * - 解决 FFmpeg 默认优先返回硬件解码器的问题
     * - 适用于用户明确要求软件解码（use_hardware_decoder_=false）的场景
     * 
     * @param codec_id 编解码器 ID（如 AV_CODEC_ID_H264）
     * @return 软件解码器指针，未找到返回 nullptr
     * 
     * @note 使用 virtual final 禁止子类覆盖，保证查找逻辑统一
     * @note 查找顺序：按 FFmpeg 注册顺序（av_codec_iterate）
     * 
     * @note 使用示例：
     * @code
     * // 在 initializeDecoder() 中，如果用户要求软件解码
     * if (!use_hardware_decoder_) {
     *     codec = findPureSoftwareDecoder(AV_CODEC_ID_H264);
     *     if (!codec) {
     *         return false;  // 无可用软件解码器
     *     }
     * }
     * @endcode
     */
    virtual const AVCodec* findPureSoftwareDecoder(AVCodecID codec_id) const final;
    
    /**
     * @brief Allocator门面（所有Worker子类自动继承）
     */
    BufferAllocatorFacade allocator_facade_;
    
    /**
     * @brief BufferPool 类型 → Pool ID 映射表（v2.0 新设计）
     * 
     * v2.0 设计变更：
     * - Worker 只记录 pool_id，不持有 Pool 指针
     * - 使用统一的 BufferPoolType 枚举标识不同用途的 BufferPool
     * - 使用者通过枚举获取 pool_id，再从 Registry 获取 Pool
     * - 符合中心化资源管理原则
     * 
     * @note 替代了旧的 buffer_pool_id_ 单个变量
     */
    std::map<BufferPoolType, uint64_t> buffer_pool_type_map_;
    
    /**
     * @brief 注册一个 BufferPool（供子类在 open() 中调用）
     * 
     * @param type BufferPool 类型枚举
     * @param pool_id BufferPool ID（由 allocator_facade_.allocatePoolWithBuffers() 返回）
     * @return true 注册成功，false 该类型已存在或 pool_id 无效
     * 
     * @note 同一类型只能注册一次，重复注册会返回 false
     * 
     * @note 使用示例：
     * @code
     * uint64_t pool_id = allocator_facade_.allocatePoolWithBuffers(...);
     * if (pool_id != 0) {
     *     registerBufferPool(BufferPoolType::DECODE_VIDEO_PRIMARY, pool_id);
     * }
     * @endcode
     */
    bool registerBufferPool(BufferPoolType type, uint64_t pool_id) {
        if (pool_id == 0) {
            return false;  // 无效的 pool_id
        }
        
        // 检查是否已存在
        if (buffer_pool_type_map_.find(type) != buffer_pool_type_map_.end()) {
            // 注意：不使用 LOG 宏，因为 Logger.hpp 可能未包含（避免循环依赖）
            return false;
        }
        
        buffer_pool_type_map_[type] = pool_id;
        return true;
    }
    
    /**
     * @brief 注销一个 BufferPool（供子类在 close() 中调用）
     * 
     * @param type BufferPool 类型
     */
    void unregisterBufferPool(BufferPoolType type) {
        buffer_pool_type_map_.erase(type);
    }
    
    /**
     * @brief 清空所有 BufferPool 注册（供子类在 close() 中调用）
     */
    void clearAllBufferPools() {
        buffer_pool_type_map_.clear();
    }
    
    // ========== 编解码器类型检测工具（v2.11 新增）==========
    
    /**
     * @brief 检查配置的解码器与实际编解码器是否匹配
     * @param actual_codec_id 实际的编解码器ID（从AVCodecParameters->codec_id获取）
     * @param decoder_name 配置的解码器名称（从config.decoder.name获取）
     * 
     * @note 只打印警告，不影响程序执行
     * @note 如果decoder_name为空或"auto"，则跳过检查
     * @note 如果匹配成功，不打印任何信息
     * 
     * @note 子类使用示例：
     * @code
     * // 在 open() 方法中，openMediaSource() 成功后调用
     * AVCodecParameters* codecpar = format_ctx_->streams[video_idx]->codecpar;
     * checkCodecMismatch(codecpar->codec_id, decoder_name_);
     * @endcode
     */
    void checkCodecMismatch(AVCodecID actual_codec_id, const std::string& decoder_name) const;
    
    /**
     * @brief 从解码器名称推断期望的编解码器ID
     * @param decoder_name 解码器名称（如 "h264_taco", "hevc", "vp9"）
     * @return 期望的AVCodecID，如果无法确定则返回AV_CODEC_ID_NONE
     * 
     * @note 支持常见的解码器名称映射：
     *       - "h264", "h264_taco", "h264_cuvid" → AV_CODEC_ID_H264
     *       - "h265", "hevc", "hevc_taco" → AV_CODEC_ID_HEVC
     *       - "vp8", "vp9", "av1" → 对应的 ID
     *       - "mpeg2", "mpeg4" → 对应的 ID
     * @note 名称匹配不区分大小写，使用 std::string::find()
     */
    static AVCodecID getExpectedCodecIdFromDecoderName(const std::string& decoder_name);
    
    /**
     * @brief 获取 AVCodecID 的友好名称
     * @param codec_id 编解码器ID
     * @return 友好的名称字符串（如 "H.264/AVC", "H.265/HEVC"）
     * 
     * @note 对于常见编解码器返回易读名称，其他返回 FFmpeg 原始名称
     */
    static std::string getCodecFriendlyName(AVCodecID codec_id);
    
    /**
     * @brief Worker配置（v2.2 所有Worker子类自动继承）
     * 
     * v2.2 设计变更：
     * - Worker 在构造时接收配置
     * - Worker 从配置中读取需要的参数
     * - 符合依赖注入原则
     */
    WorkerConfig worker_config_;
    
    // 日志器
    log4cplus::Logger logger_;
    
};

#endif // WORKER_BASE_HPP

