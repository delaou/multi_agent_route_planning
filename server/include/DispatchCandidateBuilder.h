#pragma once

#include "DispatchPolicy.h"
#include "LineTopologyRegistry.h"
#include "TrainMonitor.h"
#include "AffectedLinePlanner.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct DispatchCandidateBuilderConfig {
    // 从列车当前位置沿线路到共线入口的最大距离，单位与游戏坐标一致。
    double approach_distance = 2000.0;
    // 进入该距离后才成为可放行候选；外层 approach 区只提高监控频率。
    double control_point_distance = 1000.0;
};

struct CandidateBuildResult {
    std::unordered_map<std::string, std::vector<DispatchCandidate>>
        candidates_by_resource;
    std::unordered_map<std::string, TrainDispatchPhase> phase_by_train;
};

// 将全量列车状态裁剪为受控区域候选。首次进入控制区的 simulation_time
// 会跨批次保留，保证等待时间不会因每次遥测更新而被重置。
class DispatchCandidateBuilder {
public:
    // 校验并保存接近区、实时竞争区距离；竞争区不能大于接近区。
    explicit DispatchCandidateBuilder(DispatchCandidateBuilderConfig config = {});

    // 从全量监控中寻找每辆列车最近的未来共线入口：2000m 内更新阶段，
    // 1000m 内生成实时候选，并把全线滚动计划的软优先级附到候选上。
    CandidateBuildResult build(const std::vector<MonitoredTrain>& trains,
                               const LineTopologyRegistry& topology,
                               const RollingPlanResult* rolling_plan = nullptr);
    // 删除指定列车在所有共线资源上的首次等待时间，供过期/清空处理调用。
    void forgetTrain(const std::string& train_id);
    // 清除全部候选等待记录，通常在线路拓扑整体切换时调用。
    void clear();

    // 把共线段 ID 和方向转换为稳定资源 ID，例如 C1:FORWARD。
    static std::string resourceId(const std::string& corridor_id,
                                  CorridorDirection direction);

private:
    struct WaitingRecord {
        uint64_t arrived_at_simulation_ms = 0;
        uint64_t last_seen_wall_ms = 0;
    };

    DispatchCandidateBuilderConfig config_;
    std::unordered_map<std::string, WaitingRecord> waiting_by_train_resource_;
    mutable std::mutex mutex_;
};
