#pragma once

#include "DispatchPolicy.h"
#include "LineRouteAnalyzer.h"
#include "TrainMonitor.h"
#include "AffectedLinePlanner.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class DBConnectionPool;
class RedisCache;
struct LineTopologySnapshot;

// 调度数据层配置。MySQL 保存可信历史，Redis 只保存可重建的实时视图和事件流。
struct DispatchPersistenceConfig {
    bool auto_migrate = true;
    bool telemetry_history_enabled = true;
    // 是否在进程启动时恢复尚未完成的 PENDING/ACCEPTED 命令。
    // 默认关闭；拓扑和下一命令序号仍然恢复。
    bool command_recovery_enabled = false;
    int realtime_ttl_ms = 15000;
    int rolling_plan_ttl_ms = 60000;
    int command_ttl_ms = 86400000;
    size_t stream_max_len = 100000;
};

// 数据层累计统计。MySQL 失败会阻止新 RELEASE 命令对外可见；Redis 失败只降级。
struct DispatchPersistenceStats {
    uint64_t mysql_writes = 0;
    uint64_t mysql_errors = 0;
    uint64_t redis_writes = 0;
    uint64_t redis_errors = 0;
    uint64_t recovered_pending_commands = 0;
    uint64_t recovered_active_commands = 0;
    bool command_recovery_enabled = false;
};

// 服务重启时需要恢复的最小安全状态。历史遥测不恢复为“新鲜状态”，因为
// received_at_wall_ms 使用 steady_clock，跨进程后不再具有可比性。
struct DispatchRecoveryState {
    uint64_t topology_revision = 0;
    uint64_t next_command_sequence = 1;
    std::vector<LineRoute> routes;
    std::vector<DispatchDecision> pending_commands;
    std::vector<DispatchDecision> active_commands;
};

// MySQL/Redis 统一数据访问层。
//
// MySQL 是调度计划、资源指令和状态事件的最终可信来源；Redis 是可丢弃并可从
// MySQL/实时遥测重建的加速层。类本身不拥有连接池或 Redis 单例，二者必须比它
// 活得更久。所有公开方法均可被不同 HTTP 工作线程调用。
class DispatchPersistence {
public:
    // 创建空数据层；调用 initialize 前所有持久化方法按“未启用”处理。
    DispatchPersistence();
    // 关闭数据层并释放内部实现。连接实际归各自连接池所有。
    ~DispatchPersistence();

    DispatchPersistence(const DispatchPersistence&) = delete;
    DispatchPersistence& operator=(const DispatchPersistence&) = delete;

    // 绑定可选 MySQL 和 Redis。pool 可为空；非空时必须已初始化。
    // auto_migrate=true 会幂等创建本项目需要的表。
    bool initialize(DBConnectionPool* pool,
                    RedisCache* redis,
                    const DispatchPersistenceConfig& config,
                    std::string* error = nullptr);

    // 从 MySQL 读取最新拓扑以及未完成指令，用于服务重启后的安全恢复。
    bool loadRecoveryState(DispatchRecoveryState* state,
                           std::string* error = nullptr);

    // 保存一次完整拓扑版本，并刷新 Redis 拓扑视图和拓扑事件流。
    bool persistTopology(const LineTopologySnapshot& snapshot,
                         uint64_t wall_now_ms,
                         std::string* error = nullptr);

    // 原子保存一次调度周期：最新列车状态、可选遥测历史、滚动计划和新指令。
    // MySQL 提交成功后才更新 Redis，保证缓存不会领先于可信状态。
    bool persistDispatchCycle(
        const std::vector<MonitoredTrain>& trains,
        const RollingPlanResult& rolling_plan,
        const std::vector<DispatchDecision>& issued_commands,
        uint64_t topology_revision,
        uint64_t simulation_now_ms,
        uint64_t wall_now_ms,
        std::string* error = nullptr);

    // 在内存状态切换前幂等记录 ACK；相同 command_id 的重复 ACK 不会重复插入事件。
    bool persistCommandAck(const DispatchDecision& command,
                           bool accepted,
                           uint64_t simulation_now_ms,
                           uint64_t wall_now_ms,
                           std::string* error = nullptr);

    // 幂等记录车尾离开事件，并把已接受指令更新为 CLEARED。
    bool persistResourceCleared(
        const DispatchDecision& command,
        const std::vector<std::string>& intersection_ids,
        uint64_t simulation_now_ms,
        uint64_t wall_now_ms,
        std::string* error = nullptr);

    // 标记过期列车不再活跃，同时删除对应 Redis 实时 Key。
    void markTrainStale(const std::string& train_id, uint64_t wall_now_ms);

    // 返回 MySQL 是否绑定且表结构初始化成功。
    bool mysqlEnabled() const;
    // 返回 Redis 是否绑定且当前客户端池可用。
    bool redisEnabled() const;
    // 返回累计统计的快照。
    DispatchPersistenceStats stats() const;
    // 解除外部依赖引用；不销毁全局 MySQL/Redis 连接池。
    void shutdown();

private:
    // PImpl 隔离 MySQL C API、hiredis 适配和 JSON 序列化细节。
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
