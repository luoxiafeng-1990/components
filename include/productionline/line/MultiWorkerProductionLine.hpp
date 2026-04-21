#pragma once

#include "productionline/line/VideoProductionLine.hpp"
#include "productionline/worker/base/WorkerBase.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"
#include "productionline/line/WorkerSyncCoordinator.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include "common/Logger.hpp"
#include "common/GlobalThreadPool.hpp"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <map>
#include <queue>

extern "C" {
#include <libavcodec/avcodec.h>
}

class IEncodedPacketSource;

class MultiWorkerProductionLine : public VideoProductionLine {
public:
    MultiWorkerProductionLine(
        const MultiWorkerConfig& config,
        bool loop = false,
        int thread_count = 1,
        bool enable_monitor = false
    );
    
    ~MultiWorkerProductionLine();
    
    MultiWorkerProductionLine(const MultiWorkerProductionLine&) = delete;
    MultiWorkerProductionLine& operator=(const MultiWorkerProductionLine&) = delete;
    
    bool start();
    void stop();
    
    uint64_t getGroupConsumerBufferPoolId(size_t group_index, size_t consumer_index) const;
    int64_t getAllLineFramesProduced() const;
    int64_t getAllLineFramesFailed() const;
    int getActiveWorkerCount(size_t group_index) const;
    void printDetailedStats() const;

    static std::string getProducerNameForConsumer(
        const GroupConfig& config, const std::string& consumer_name);

private:
    struct WorkerGroupRuntime {
        std::string group_id;
        uint64_t topology_group_id{0};
        
        struct ProducerInfo {
            std::unique_ptr<VideoProductionLine> producer_line;
            uint64_t buffer_pool_id{0};
            std::weak_ptr<BufferPool> buffer_pool_weak;
        };
        std::map<std::string, ProducerInfo> producers;
        
        struct ConsumerInfo {
            std::shared_ptr<WorkerBase> worker;
            uint64_t buffer_pool_id{0};
            std::weak_ptr<BufferPool> buffer_pool_weak;
        };
        std::map<std::string, ConsumerInfo> consumers;
        
        std::map<std::string, std::shared_ptr<IEncodedPacketSource>> shared_sources;
        std::unique_ptr<WorkerSyncCoordinator> coordinator;
        const GroupConfig* config_ref = nullptr;
        
        std::thread group_thread;
        std::atomic<bool> is_running{false};
        
        struct GroupStats {
            std::atomic<int64_t> frames_produced{0};
            std::atomic<int64_t> frames_failed{0};
        };
        GroupStats stats;
        
        struct WorkerProductionStats {
            std::atomic<int64_t> worker_frames_produced{0};
            std::atomic<int64_t> worker_frames_failed{0};
            std::atomic<int64_t> consecutive_failures{0};
            std::atomic<bool> is_active{true};
        };
        
        std::map<std::string, std::unique_ptr<WorkerProductionStats>> worker_stats;
    };
    
    bool validateConfig() const;
    
    void groupThreadFunc(const std::shared_ptr<WorkerGroupRuntime>& group);
    
    void workerThreadFunc(const std::shared_ptr<WorkerGroupRuntime>& group, 
                         const std::string& consumer_name);
    
    bool performFrameSync(const std::shared_ptr<WorkerGroupRuntime>& group,
                         const std::string& consumer_name,
                         WorkerGroupRuntime::ConsumerInfo* consumer_info,
                         Buffer* buffer,
                         const FillResult& result);
    
    void setError(const std::string& error_msg) const;
    
    bool createProducersForGroup(WorkerGroupRuntime* group, const GroupConfig& group_config);
    
    bool setupSharedSources(WorkerGroupRuntime* group, const GroupConfig& group_config);
    
    bool setupCoordinator(WorkerGroupRuntime* group, const GroupConfig& group_config);
    
    bool createConsumersForGroup(WorkerGroupRuntime* group, const GroupConfig& group_config);
    
    bool startGroupThreads();
    
    MultiWorkerConfig config_;
    uint64_t multi_line_registry_id_{0};
    std::vector<std::shared_ptr<WorkerGroupRuntime>> groups_;
    mutable std::mutex error_mutex_;
    mutable std::queue<std::string> error_queue_;
    log4cplus::Logger logger_;
    std::string log_prefix_;
};
