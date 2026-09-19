#pragma once

#include "DispatchCandidateBuilder.h"
#include "DispatchManager.h"
#include "AffectedLinePlanner.h"
#include "DispatchPersistence.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct RailwayDispatchServiceConfig {
    DispatchManagerConfig dispatch;
    LineRouteAnalyzerConfig route_analysis;
    DispatchCandidateBuilderConfig candidate_builder;
    AffectedLinePlannerConfig rolling_planner;
    uint64_t train_stale_wall_ms = 5000;
};

struct RouteUploadResult {
    size_t route_count = 0;
    size_t corridor_count = 0;
    size_t intersection_count = 0;
    bool persistence_ok = true;
    std::string persistence_error;
};

struct RailwayRuntimeStatus {
    LineTopologySummary topology;
    size_t train_count = 0;
    uint64_t last_train_snapshot_wall_ms = 0;
    uint64_t train_snapshot_age_ms = 0;
    bool telemetry_connected = false;
};

// HTTP 层使用的统一编排门面。它把全量监控、拓扑索引、候选生成、策略决策
// 和命令 ACK 串成完整生命周期，并用一把服务锁保证跨组件操作的一致性。
class RailwayDispatchService {
public:
    // 返回进程内唯一服务实例，供所有 HTTP 工作线程共享调度状态。
    static RailwayDispatchService& instance();

    // 仅在服务器开始接收请求前调用。重新配置会清空运行时调度状态。
    void configure(const RailwayDispatchServiceConfig& config);
    // 绑定数据层并恢复拓扑和命令序号；命令与占用是否恢复由数据层配置决定。
    bool enablePersistence(DispatchPersistence* persistence,
                           uint64_t wall_now_ms,
                           std::string* reason = nullptr);
    // 返回数据层健康状态和最近一次错误，供运维接口展示。
    bool persistenceHealthy(std::string* last_error = nullptr) const;
    // 返回 MySQL/Redis 数据层累计统计。
    DispatchPersistenceStats persistenceStats() const;
    // 接收线路全量快照，分析成功后清理旧计划并重建共线/交叉调度资源。
    bool replaceRoutes(const std::vector<LineRoute>& routes,
                       uint64_t wall_now_ms,
                       RouteUploadResult* result,
                       std::string* reason = nullptr);
    // 批量更新全部列车，随后执行过期清理、全线滚动规划和入口实时决策。
    TrainBatchUpdateResult updateTrains(const std::vector<TrainTelemetry>& trains,
                                        uint64_t simulation_now_ms,
                                        uint64_t wall_now_ms);

    // 命令在 ACK 前会持续可查询，避免一次 HTTP 响应丢失导致永久漏执行。
    std::vector<DispatchDecision> pendingCommands(
        const std::optional<std::string>& train_id = std::nullopt) const;
    // 可由命令轮询或定时器调用；即使本轮没有新遥测也能清除过期监控项。
    void runMaintenance(uint64_t wall_now_ms);
    // 处理 RELEASE 的游戏端 ACK：接受则确认进入，拒绝则取消预预约。
    bool acknowledge(const std::string& corridor_resource_id,
                     const std::string& train_id,
                     bool accepted,
                     uint64_t simulation_now_ms,
                     const std::string& command_id = "");
    // 处理车尾离开事件，释放共线容量和交叉点预约；交叉点列表可自动推导。
    bool markCleared(const std::string& train_id,
                     const std::string& corridor_resource_id,
                     const std::vector<std::string>& intersection_ids,
                     uint64_t simulation_now_ms);

    // 返回当前全量列车监控快照。
    std::vector<MonitoredTrain> trains() const;
    // 返回当前生效的线路拓扑快照。
    LineTopologySnapshot topology() const;
    // 返回轻量运行状态；用于前端高频轮询，不复制完整拓扑。
    RailwayRuntimeStatus runtimeStatus(uint64_t wall_now_ms) const;
    // 返回最近一批遥测生成的全线滚动计划快照。
    RollingPlanResult rollingPlan() const;

private:
    // 使用默认配置创建各子模块；外部应通过 instance() 获取实例。
    RailwayDispatchService();
    // 根据当前拓扑重新创建 DispatchManager、movement、双向共线资源和线路状态。
    void rebuildDispatchManagerLocked(uint64_t wall_now_ms);
    // 依次执行全线软规划、1000m 候选裁剪和入口实时放行决策。
    void refreshCandidatesAndPlanLocked(uint64_t simulation_now_ms,
                                        uint64_t wall_now_ms,
                                        std::vector<DispatchDecision>* issued);
    // 删除墙上时间过期列车、旧计划和未 ACK 命令；已进入资源不会冒险自动释放。
    void cleanupStaleLocked(uint64_t wall_now_ms);

    RailwayDispatchServiceConfig config_;
    TrainMonitor train_monitor_;
    std::unique_ptr<LineTopologyRegistry> topology_;
    std::unique_ptr<DispatchCandidateBuilder> candidate_builder_;
    std::unique_ptr<AffectedLinePlanner> rolling_planner_;
    std::unique_ptr<DispatchManager> dispatch_manager_;
    std::unordered_map<std::string, DispatchDecision> commands_by_train_;
    // ACK 成功后保留本次通行信息，便于 clear 事件省略交叉点列表时自动释放。
    std::unordered_map<std::string, DispatchDecision> active_by_train_;
    RollingPlanResult rolling_plan_result_;
    uint64_t last_train_snapshot_wall_ms_ = 0;
    uint64_t next_command_id_ = 1;
    DispatchPersistence* persistence_ = nullptr;
    bool persistence_healthy_ = true;
    std::string persistence_error_;
    mutable std::mutex mutex_;
};
