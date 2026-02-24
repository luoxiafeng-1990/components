#ifndef IENCODED_PACKET_SOURCE_HPP
#define IENCODED_PACKET_SOURCE_HPP

#include "productionline/worker/IDataSourceNavigator.hpp"

// FFmpeg 前向声明
struct AVPacket;

// ============================================================
// AcquireStatus - Packet 获取结果状态（v2.32 从 EncodedPacketSourceFromBuffer 移至此处）
// ============================================================

/**
 * @brief Packet 获取结果状态
 * 
 * v2.31 新增：用于 acquireEncodedPacket 的返回值
 * v2.32 移动：从 EncodedPacketSourceFromBuffer.hpp 移至接口文件
 * v2.34 新增：av_read_frame 错误码细分（InvalidData ~ UnknownError）
 */
enum class AcquireStatus : int {
    // ===== 业务状态 =====
    Success = 0,                    ///< 成功获取
    Eof = -1,                       ///< 数据流正常结束（对应 AVERROR_EOF）
    PacketAlreadyProcessed = -2,    ///< 当前 packet 已被处理过（Buffer 共享模式已获取过当前版本）
    InvalidMode = -3,               ///< 非共享模式 / 参数错误
    InternalError = -4,             ///< 内部逻辑异常（不应到达的代码路径）
    
    // ===== v2.34 新增：av_read_frame FFmpeg 错误码映射 =====
    InvalidData = -5,               ///< 数据损坏（对应 AVERROR_INVALIDDATA）
    IoError = -6,                   ///< IO 错误 / 网络断连（对应 AVERROR(EIO)）
    TimedOut = -7,                  ///< 网络超时（对应 AVERROR(ETIMEDOUT)）
    OutOfMemory = -8,               ///< 内存不足（对应 AVERROR(ENOMEM)）
    Interrupted = -9,               ///< 请求退出（对应 AVERROR_EXIT）
    Again = -10,                    ///< 暂时无数据可用（对应 AVERROR(EAGAIN)）
    NonVideoPacket = -11,           ///< 读到非视频流 packet（音频/字幕等），已跳过
    UnknownError = -99              ///< 未识别的 FFmpeg 错误码
};

/**
 * @brief 获取状态的字符串描述
 */
inline const char* acquireStatusToString(AcquireStatus status) {
    switch (status) {
        case AcquireStatus::Success:                return "Success";
        case AcquireStatus::Eof:                    return "EOF (AVERROR_EOF)";
        case AcquireStatus::PacketAlreadyProcessed: return "PacketAlreadyProcessed";
        case AcquireStatus::InvalidMode:            return "InvalidMode";
        case AcquireStatus::InternalError:          return "InternalError";
        case AcquireStatus::InvalidData:            return "InvalidData (AVERROR_INVALIDDATA)";
        case AcquireStatus::IoError:                return "IoError (AVERROR(EIO))";
        case AcquireStatus::TimedOut:               return "TimedOut (AVERROR(ETIMEDOUT))";
        case AcquireStatus::OutOfMemory:            return "OutOfMemory (AVERROR(ENOMEM))";
        case AcquireStatus::Interrupted:            return "Interrupted (AVERROR_EXIT)";
        case AcquireStatus::Again:                  return "Again (AVERROR(EAGAIN))";
        case AcquireStatus::NonVideoPacket:          return "NonVideoPacket";
        case AcquireStatus::UnknownError:           return "UnknownError";
        default:                                    return "Unknown";
    }
}

// ============================================================
// PacketAcquireResult - Packet 获取结果类（v2.32 从 EncodedPacketSourceFromBuffer 移至此处）
// ============================================================

/**
 * @brief Packet 获取结果
 * 
 * v2.31 新增：封装状态和 AVPacket 指针
 * v2.32 移动：从 EncodedPacketSourceFromBuffer.hpp 移至接口文件
 * v2.34 新增：FFmpeg 错误码对应的工厂方法和查询方法
 * 
 * 设计原则（参考 Google StatusOr + Rust Result）：
 * - 明确的成功/失败状态
 * - 只有成功时才能访问 packet
 * - 零拷贝，零额外分配
 * 
 * 使用示例：
 * @code
 * auto result = ps->acquireEncodedPacket(packet, this);
 * 
 * if (result.ok()) {
 *     AVPacket* pkt = result.packet();
 * } else if (result.isEof()) {
 *     // 正常结束
 * } else if (result.isPacketAlreadyProcessed()) {
 *     // 当前 packet 已处理过（非视频流已跳过或已获取过当前版本）
 * }
 * @endcode
 */
class PacketAcquireResult {
public:
    // ===== 工厂方法 =====
    
    /// 成功结果
    static PacketAcquireResult success(AVPacket* packet) {
        return PacketAcquireResult(AcquireStatus::Success, packet);
    }
    
    /// EOF 结果
    static PacketAcquireResult eof() {
        return PacketAcquireResult(AcquireStatus::Eof, nullptr);
    }
    
    /// 当前 packet 已处理过
    static PacketAcquireResult packetAlreadyProcessed() {
        return PacketAcquireResult(AcquireStatus::PacketAlreadyProcessed, nullptr);
    }
    
    /// 非共享模式 / 参数错误 / 未打开
    static PacketAcquireResult invalidMode() {
        return PacketAcquireResult(AcquireStatus::InvalidMode, nullptr);
    }
    
    /// 内部逻辑异常（不应到达的代码路径）
    static PacketAcquireResult internalError() {
        return PacketAcquireResult(AcquireStatus::InternalError, nullptr);
    }
    
    // ===== v2.34 新增：FFmpeg 错误码对应的工厂方法 =====
    
    /// 数据损坏（AVERROR_INVALIDDATA）
    static PacketAcquireResult invalidData() {
        return PacketAcquireResult(AcquireStatus::InvalidData, nullptr);
    }
    
    /// IO 错误 / 网络断连（AVERROR(EIO)）
    static PacketAcquireResult ioError() {
        return PacketAcquireResult(AcquireStatus::IoError, nullptr);
    }
    
    /// 网络超时（AVERROR(ETIMEDOUT)）
    static PacketAcquireResult timedOut() {
        return PacketAcquireResult(AcquireStatus::TimedOut, nullptr);
    }
    
    /// 内存不足（AVERROR(ENOMEM)）
    static PacketAcquireResult outOfMemory() {
        return PacketAcquireResult(AcquireStatus::OutOfMemory, nullptr);
    }
    
    /// 请求退出（AVERROR_EXIT）
    static PacketAcquireResult interrupted() {
        return PacketAcquireResult(AcquireStatus::Interrupted, nullptr);
    }
    
    /// 暂时无数据可用（AVERROR(EAGAIN)）
    static PacketAcquireResult again() {
        return PacketAcquireResult(AcquireStatus::Again, nullptr);
    }
    
    /// 未识别的 FFmpeg 错误码
    static PacketAcquireResult unknownError() {
        return PacketAcquireResult(AcquireStatus::UnknownError, nullptr);
    }
    
    /// 读到非视频流 packet（音频/字幕等），已跳过
    static PacketAcquireResult nonVideoPacket() {
        return PacketAcquireResult(AcquireStatus::NonVideoPacket, nullptr);
    }
    
    // ===== 状态查询 =====
    
    /// 获取状态
    AcquireStatus status() const noexcept { return status_; }
    
    /// 是否成功
    bool ok() const noexcept { return status_ == AcquireStatus::Success; }
    
    /// 是否到达 EOF（正常结束）
    bool isEof() const noexcept { return status_ == AcquireStatus::Eof; }
    
    /// 当前 packet 是否已被处理过（已获取过当前版本或非视频流已跳过）
    bool isPacketAlreadyProcessed() const noexcept { return status_ == AcquireStatus::PacketAlreadyProcessed; }
    
    /// 是否是错误（非 Success 且非 EOF 且非 PacketAlreadyProcessed）
    bool isError() const noexcept {
        return status_ != AcquireStatus::Success && 
               status_ != AcquireStatus::Eof &&
               status_ != AcquireStatus::PacketAlreadyProcessed;
    }
    
    // ===== v2.34 新增：FFmpeg 错误码对应的查询方法 =====
    
    /// 是否数据损坏（AVERROR_INVALIDDATA）
    bool isInvalidData() const noexcept { return status_ == AcquireStatus::InvalidData; }
    
    /// 是否 IO 错误 / 网络断连（AVERROR(EIO)）
    bool isIoError() const noexcept { return status_ == AcquireStatus::IoError; }
    
    /// 是否网络超时（AVERROR(ETIMEDOUT)）
    bool isTimedOut() const noexcept { return status_ == AcquireStatus::TimedOut; }
    
    /// 是否内存不足（AVERROR(ENOMEM)）
    bool isOutOfMemory() const noexcept { return status_ == AcquireStatus::OutOfMemory; }
    
    /// 是否请求退出（AVERROR_EXIT）
    bool isInterrupted() const noexcept { return status_ == AcquireStatus::Interrupted; }
    
    /// 是否暂时无数据可用（AVERROR(EAGAIN)）
    bool isAgain() const noexcept { return status_ == AcquireStatus::Again; }
    
    /// 是否未识别的错误
    bool isUnknownError() const noexcept { return status_ == AcquireStatus::UnknownError; }
    
    /// 是否非视频流 packet
    bool isNonVideoPacket() const noexcept { return status_ == AcquireStatus::NonVideoPacket; }
    
    /// 是否可重试（Again / TimedOut — 重试当前读取操作）
    bool isRetryable() const noexcept {
        return status_ == AcquireStatus::Again ||
               status_ == AcquireStatus::TimedOut;
    }
    
    /// 是否应该 continue（跳过当前 packet，获取下一个）
    /// InvalidData: 数据损坏，跳过即可
    /// （注意：PacketAlreadyProcessed / NonVideoPacket 不在此处判断，
    ///  它们在 FillResult::shouldContinue() 中统一处理）
    bool shouldContinue() const noexcept {
        return status_ == AcquireStatus::InvalidData;
    }
    
    /// 隐式 bool 转换（方便条件判断）
    explicit operator bool() const noexcept { return ok(); }
    
    // ===== 数据访问 =====
    
    /**
     * @brief 获取 AVPacket 指针
     * 
     * @warning 仅当 ok() 为 true 时有效！
     */
    AVPacket* packet() const noexcept { return packet_; }
    
    /**
     * @brief 箭头运算符（便捷访问 AVPacket 成员）
     */
    AVPacket* operator->() const noexcept { return packet_; }
    
    /**
     * @brief 获取状态描述
     */
    const char* statusString() const noexcept { return acquireStatusToString(status_); }

private:
    PacketAcquireResult(AcquireStatus status, AVPacket* packet)
        : status_(status), packet_(packet) {}
    
    AcquireStatus status_;
    AVPacket* packet_;
};

// ============================================================
// IEncodedPacketSource - 编码数据源接口
// ============================================================

/**
 * @brief IEncodedPacketSource - 编码数据源抽象接口
 * 
 * 设计模式：策略模式（Strategy Pattern）
 * 
 * 继承关系：
 * - 继承 IDataSourceNavigator，获得所有数据源操作接口（20个方法）
 * - 添加底层特有方法：acquireEncodedPacket、commitEncodedPacket、cancelEncodedPacket、getVideoStreamIndex
 * 
 * 职责：
 * - 继承：数据源操作（open/close/seek/状态查询/属性查询等）
 * - 特有：提供编码后的 packet 获取接口（acquireEncodedPacket）
 * - 特有：提供 packet 生命周期管理（commit/cancel）
 * - 特有：提供视频流索引查询（getVideoStreamIndex）
 * 
 * v2.32 重构：
 * - 统一接口名：所有数据源都使用 acquireEncodedPacket（替代 readEncodedPacket）
 * - 统一接口支持两种语义：
 *   - File/RTSP 模式：往 out_packet 填充数据（零拷贝）
 *   - Buffer 共享模式：返回借用指针，忽略 out_packet
 * - 新增 commitEncodedPacket / cancelEncodedPacket 用于共享模式
 * 
 * 设计理念：
 * - 符合 SOLID 原则（依赖倒置、开闭原则、里氏替换）
 * - 支持多种数据源（文件、Buffer、网络流等）
 * - 易于扩展和测试
 * - 零拷贝：File/RTSP 模式直接填充调用者的 AVPacket
 * 
 * 使用场景：
 * - 文件模式：从本地文件读取编码后的 packet
 * - Buffer 模式：从 BufferPool 获取编码后的 packet（MultiWorkerProductionLine）
 * - 网络流模式：从网络流读取编码后的 packet（RTSP等）
 * 
 * 实现类：
 * - EncodedPacketSourceFromFile：本地文件实现
 * - EncodedPacketSourceFromRtsp：RTSP流实现
 * - EncodedPacketSourceFromBuffer：BufferPool实现
 */
class IEncodedPacketSource : public IDataSourceNavigator {
public:
    virtual ~IEncodedPacketSource() = default;
    
    // ============ IEncodedPacketSource 特有方法 ============
    
    /**
     * @brief 获取编码后的 packet（v2.32 统一接口）
     * @param out_packet 输出的 packet（File/RTSP 模式必须提供，Buffer 共享模式可传 nullptr）
     * @param worker_id Worker 的唯一标识（Buffer 共享模式需要，File/RTSP 可传 nullptr）
     * @return PacketAcquireResult 统一的结果类型
     * 
     * 两种语义：
     * - File/RTSP 模式：往 out_packet 填充数据，result.packet() 返回 out_packet
     * - Buffer 共享模式：忽略 out_packet，result.packet() 返回借用指针
     * 
     * 返回值状态：
     * - Success：成功获取
     * - Eof：数据流正常结束
     * - PacketAlreadyProcessed：当前 packet 已被处理过
     * - 其他状态：各种错误
     */
    virtual PacketAcquireResult acquireEncodedPacket(AVPacket* out_packet, void* worker_id = nullptr) = 0;
    
    /**
     * @brief 提交释放编码后的 AVPacket（共享模式使用）
     * @param worker_id Worker 的唯一标识
     * @return true=成功提交, false=失败
     * 
     * 说明：
     * - 共享模式（Buffer）：递减订阅者计数，最后一个订阅者触发 Buffer 释放
     * - 非共享模式（File/RTSP）：默认返回 true，无需操作
     */
    virtual bool commitEncodedPacket(void* worker_id) { 
        (void)worker_id; 
        return true; 
    }
    
    /**
     * @brief 取消当前获取（共享模式使用）
     * @param worker_id Worker 的唯一标识
     * 
     * 说明：
     * - 共享模式（Buffer）：重置 Worker 状态，允许重新获取当前 buffer
     * - 非共享模式（File/RTSP）：默认空实现
     */
    virtual void cancelEncodedPacket(void* worker_id) { 
        (void)worker_id; 
    }
    
    /**
     * @brief 获取视频流索引
     * @return 视频流索引，如果不可用则返回 -1
     * 
     * @note 用于 FFmpeg 解码时判断 packet 属于哪个流
     */
    virtual int getVideoStreamIndex() const = 0;
};

#endif // IENCODED_PACKET_SOURCE_HPP
