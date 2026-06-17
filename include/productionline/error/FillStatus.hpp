#ifndef FILL_STATUS_HPP
#define FILL_STATUS_HPP

#include "productionline/worker/datasource/encodeddata/IEncodedPacketSource.hpp"

enum class FillStatus : int {
    Success = 0,
    Error = -1
};

enum class ErrorSource : int {
    None = 0,
    Acquire = 1,
    Codec = 2,
    Worker = 3
};

enum class CodecStatus : int {
    Success = 0,
    Eagain = 1,           // AVERROR(EAGAIN)      — 解码器缓冲区满，保留 packet 重试
    Eof = -1,             // AVERROR_EOF           — 数据流结束
    SendFailed = -2,      // （保留）
    InvalidState = -3,    // AVERROR(EINVAL)       — 编解码器未打开或参数错误
    DecodeError = -4,     // 未识别的解码错误（兜底）
    ReceiveError = -5,    // receive_frame 失败
    AllocFailed = -6,     // AVERROR(ENOMEM)       — 内存分配失败
    EncodeError = -7,     // 编码错误
    InvalidData = -8,     // AVERROR_INVALIDDATA   — 流数据损坏（NAL/SPS/PPS 等），可跳过
    ExternalError = -9,   // AVERROR_EXTERNAL      — 外部库错误（硬件解码器报错），可跳过
};

enum class WorkerStatus : int {
    Success = 0,
    InvalidParam = -1,
    NotOpen = -2,
    InternalError = -3
};

inline const char* fillStatusToString(FillStatus status) {
    switch (status) {
        case FillStatus::Success: return "Success";
        case FillStatus::Error:   return "Error";
        default:                  return "Unknown";
    }
}

inline const char* errorSourceToString(ErrorSource source) {
    switch (source) {
        case ErrorSource::None:    return "None";
        case ErrorSource::Acquire: return "Acquire";
        case ErrorSource::Codec:   return "Codec";
        case ErrorSource::Worker:  return "Worker";
        default:                   return "Unknown";
    }
}

inline const char* codecStatusToString(CodecStatus status) {
    switch (status) {
        case CodecStatus::Success:       return "Success";
        case CodecStatus::Eagain:        return "Eagain (AVERROR_EAGAIN)";
        case CodecStatus::Eof:           return "Eof (AVERROR_EOF)";
        case CodecStatus::SendFailed:    return "SendFailed";
        case CodecStatus::InvalidState:  return "InvalidState (AVERROR_EINVAL)";
        case CodecStatus::DecodeError:   return "DecodeError";
        case CodecStatus::ReceiveError:  return "ReceiveError";
        case CodecStatus::AllocFailed:   return "AllocFailed (AVERROR_ENOMEM)";
        case CodecStatus::EncodeError:   return "EncodeError";
        case CodecStatus::InvalidData:   return "InvalidData (AVERROR_INVALIDDATA)";
        case CodecStatus::ExternalError: return "ExternalError (AVERROR_EXTERNAL)";
        default:                         return "Unknown";
    }
}

inline const char* workerStatusToString(WorkerStatus status) {
    switch (status) {
        case WorkerStatus::Success:       return "Success";
        case WorkerStatus::InvalidParam:  return "InvalidParam";
        case WorkerStatus::NotOpen:       return "NotOpen";
        case WorkerStatus::InternalError: return "InternalError";
        default:                          return "Unknown";
    }
}

class CodecSendResult {
public:
    static CodecSendResult success() { return CodecSendResult(CodecStatus::Success); }
    static CodecSendResult eagain() { return CodecSendResult(CodecStatus::Eagain); }
    static CodecSendResult eof() { return CodecSendResult(CodecStatus::Eof); }
    static CodecSendResult sendFailed() { return CodecSendResult(CodecStatus::SendFailed); }
    static CodecSendResult invalidState() { return CodecSendResult(CodecStatus::InvalidState); }
    static CodecSendResult decodeError() { return CodecSendResult(CodecStatus::DecodeError); }
    static CodecSendResult receiveError() { return CodecSendResult(CodecStatus::ReceiveError); }
    static CodecSendResult allocFailed() { return CodecSendResult(CodecStatus::AllocFailed); }
    static CodecSendResult encodeError() { return CodecSendResult(CodecStatus::EncodeError); }
    static CodecSendResult invalidData() { return CodecSendResult(CodecStatus::InvalidData); }
    static CodecSendResult externalError() { return CodecSendResult(CodecStatus::ExternalError); }

    bool ok() const noexcept { return status_ == CodecStatus::Success; }
    bool isEoFlush() const noexcept { return status_ == CodecStatus::Eof; }
    bool isEagain() const noexcept { return status_ == CodecStatus::Eagain; }
    bool isRetryable() const noexcept {
        return status_ == CodecStatus::Eagain ||
               status_ == CodecStatus::InvalidData ||
               status_ == CodecStatus::ExternalError;
    }
    bool isTerminal() const noexcept { return !ok() && !isEoFlush() && !isRetryable(); }
    CodecStatus status() const noexcept { return status_; }
    const char* statusString() const noexcept { return codecStatusToString(status_); }
    explicit operator bool() const noexcept { return ok(); }

private:
    explicit CodecSendResult(CodecStatus status) : status_(status) {}
    CodecStatus status_;
};

/**
 * @brief Buffer 填充结果（三层错误查询 + 消费者决策接口）
 *
 * 第一层：ok() / isError()
 * 第二层：isAcquireError() / isCodecError() / isWorkerError()
 * 第三层：acquireCause() / codecCause() / workerCause()
 *
 * 消费者决策：toAction() → kSubmit / kSkip / kRetry / kTerminate
 */
class FillResult {
public:
    static FillResult success() {
        return FillResult(FillStatus::Success);
    }

    static FillResult fromAcquire(const PacketAcquireResult& result) {
        if (result.ok()) return success();
        FillResult r(FillStatus::Error);
        r.source_ = ErrorSource::Acquire;
        r.acquire_cause_ = result.status();
        return r;
    }

    static FillResult fromCodec(const CodecSendResult& result) {
        if (result.ok()) return success();
        FillResult r(FillStatus::Error);
        r.source_ = ErrorSource::Codec;
        r.codec_cause_ = result.status();
        return r;
    }

    static FillResult fromCodec(CodecStatus cause) {
        if (cause == CodecStatus::Success) return success();
        FillResult r(FillStatus::Error);
        r.source_ = ErrorSource::Codec;
        r.codec_cause_ = cause;
        return r;
    }

    static FillResult fromWorker(WorkerStatus cause) {
        if (cause == WorkerStatus::Success) return success();
        FillResult r(FillStatus::Error);
        r.source_ = ErrorSource::Worker;
        r.worker_cause_ = cause;
        return r;
    }

    static FillResult nonVideoPacket()         { return fromAcquire(PacketAcquireResult::nonVideoPacket()); }
    static FillResult invalidParam()           { return fromWorker(WorkerStatus::InvalidParam); }
    static FillResult notOpen()                { return fromWorker(WorkerStatus::NotOpen); }
    static FillResult internalError()          { return fromWorker(WorkerStatus::InternalError); }

    FillStatus status() const noexcept { return status_; }
    bool ok() const noexcept { return status_ == FillStatus::Success; }
    bool isError() const noexcept { return status_ == FillStatus::Error; }
    explicit operator bool() const noexcept { return ok(); }

    bool isEoFlush() const noexcept {
        return isCodecError() && codec_cause_ == CodecStatus::Eof;
    }

    bool isAcquireEof() const noexcept {
        return isAcquireError() && acquire_cause_ == AcquireStatus::Eof;
    }

    bool shouldContinue() const noexcept {
        if (!isError()) return false;
        if (isAcquireError()) {
            return acquire_cause_ == AcquireStatus::PacketAlreadyProcessed ||
                   acquire_cause_ == AcquireStatus::NonVideoPacket ||
                   acquire_cause_ == AcquireStatus::InvalidData;
        }
        return false;
    }

    bool shouldRetry() const noexcept {
        if (!isError()) return false;
        if (isAcquireError()) {
            return acquire_cause_ == AcquireStatus::Again ||
                   acquire_cause_ == AcquireStatus::TimedOut;
        }
        if (isCodecError()) {
            return codec_cause_ == CodecStatus::Eagain ||
                   codec_cause_ == CodecStatus::InvalidData ||
                   codec_cause_ == CodecStatus::ExternalError;
        }
        return false;
    }

    bool isTerminal() const noexcept {
        return isError() && !shouldContinue() && !shouldRetry() && !isEoFlush() && !isAcquireEof();
    }

    enum class ConsumerAction {
        kSubmit,
        kSkip,
        kRetry,
        kTerminate,
    };

    ConsumerAction toAction() const noexcept {
        if (ok())             return ConsumerAction::kSubmit;
        if (shouldContinue()) return ConsumerAction::kSkip;
        if (shouldRetry())    return ConsumerAction::kRetry;
        return                       ConsumerAction::kTerminate;
    }

    bool shouldTerminate() const noexcept {
        return !ok() && !shouldContinue() && !shouldRetry();
    }

    bool shouldBypassFrameSync() const noexcept {
        if (!isAcquireError()) return false;
        return acquire_cause_ == AcquireStatus::PacketAlreadyProcessed ||
               acquire_cause_ == AcquireStatus::NonVideoPacket;
    }

    ErrorSource source() const noexcept { return source_; }
    bool isAcquireError() const noexcept { return isError() && source_ == ErrorSource::Acquire; }
    bool isCodecError() const noexcept { return isError() && source_ == ErrorSource::Codec; }
    bool isWorkerError() const noexcept { return isError() && source_ == ErrorSource::Worker; }

    AcquireStatus acquireCause() const noexcept { return acquire_cause_; }
    CodecStatus codecCause() const noexcept { return codec_cause_; }
    WorkerStatus workerCause() const noexcept { return worker_cause_; }

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

#endif // FILL_STATUS_HPP
