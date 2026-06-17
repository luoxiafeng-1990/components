#ifndef WORKER_CONFIG_MULTI_WORKER_CONFIG_HPP
#define WORKER_CONFIG_MULTI_WORKER_CONFIG_HPP

#include <string>
#include <vector>
#include <map>
#include "productionline/worker/config/FrameSyncTypes.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"

struct GroupConfig {
    enum class Mode {
        ONE_TO_ONE,
        ONE_TO_MANY,
        MANY_TO_ONE,
        MANY_TO_MANY
    };

    std::string group_id;

    std::map<std::string, WorkerConfig> producers;
    std::map<std::string, WorkerConfig> consumers;

    Mode mode = Mode::ONE_TO_ONE;
    bool enable_frame_sync = false;
    CallbackChain callback_chain;

    GroupConfig() = default;
    explicit GroupConfig(const std::string& id) : group_id(id) {}
};

struct MultiWorkerConfig {
    std::vector<GroupConfig> groups;
    int thread_pool_size = 64;
    MultiWorkerConfig() = default;
};

#endif // WORKER_CONFIG_MULTI_WORKER_CONFIG_HPP
