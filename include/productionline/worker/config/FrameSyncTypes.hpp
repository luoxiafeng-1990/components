#ifndef WORKER_CONFIG_FRAME_SYNC_TYPES_HPP
#define WORKER_CONFIG_FRAME_SYNC_TYPES_HPP

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <cstdint>

// 前向声明
class Buffer;

// ⭐ v2.23 新增：帧同步回调类型（前向声明）
// 完整定义在 WorkerSyncCoordinator.hpp 中
using FrameSyncCallback = std::function<bool(
    uint64_t frame_version,
    const std::map<std::string, Buffer*>& worker_buffers,
    void* context
)>;

// ⭐ v2.23 新增：回调链项
struct CallbackChainItem {
    FrameSyncCallback callback;
    void* context;
    std::string name;
    
    CallbackChainItem(FrameSyncCallback cb, void* ctx, const std::string& n)
        : callback(cb), context(ctx), name(n) {}
};

// ⭐ v2.23 新增：回调链类型
using CallbackChain = std::vector<CallbackChainItem>;

#endif // WORKER_CONFIG_FRAME_SYNC_TYPES_HPP
