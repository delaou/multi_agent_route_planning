#pragma once

#include "DispatchPolicy.h"
#include "LineTopologyRegistry.h"
#include "TrainMonitor.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct AffectedLinePlannerConfig {
    // 滚动窗口只冻结近期建议，窗口外列车仍会被分析，但不分配确定顺序。
    uint64_t planning_horizon_ms = 300000;
    // 瞬时速度过低或停车时使用保守速度预测，避免 ETA 除零或变成无穷大。
    double minimum_prediction_speed = 5.0;
    // ETA 相差不超过该值的列车允许根据班次分布互换建议顺序。
    uint64_t reorder_tolerance_ms = 10000;
    uint64_t planned_minimum_spacing_ms = 5000;
    double same_line_consecutive_penalty = 0.25;
};

// 一辆列车针对一个未来共线资源的预测结果。同一列车可能依次经过多个
// 共线段，因此滚动结果按 train_id + corridor_resource_id 唯一标识。
struct PlannedTrainArrival {
    std::string train_id;
    std::string line_id;
    std::string corridor_resource_id;
    double distance_to_entry = 0.0;
    uint64_t estimated_arrival_simulation_ms = 0;
    uint64_t planned_entry_simulation_ms = 0;
    uint64_t target_headway_ms = 15000;
    bool within_planning_horizon = false;
    size_t suggested_rank = static_cast<size_t>(-1);

    // 正值表示应优先放走“领头车”来拉开后车，负值表示当前车贴前车太近，
    // 更适合稍作等待。范围约为 [-1, 1]。
    double line_distribution_need = 0.0;
    double global_priority = 0.0;
};

struct CorridorRollingPlan {
    std::string corridor_resource_id;
    uint64_t generated_at_simulation_ms = 0;
    std::vector<PlannedTrainArrival> trains;
};

struct RollingPlanResult {
    std::unordered_map<std::string, CorridorRollingPlan> plans_by_resource;
    std::unordered_map<std::string, PlannedTrainArrival> by_train_resource;

    // 按“列车 + 共线方向资源”查找预测结果；不存在时返回 nullopt。
    std::optional<PlannedTrainArrival> find(
        const std::string& train_id,
        const std::string& corridor_resource_id) const;
};

// 全局软规划层：扫描所有“线路拓扑中存在共线段”的列车，预测它们到达每个
// 共线入口的 ETA，并给出滚动建议顺序。它不占用资源，也不能替代入口硬约束。
class AffectedLinePlanner {
public:
    // 保存并校验滚动窗口、最低预测速度和有限换序参数。
    explicit AffectedLinePlanner(AffectedLinePlannerConfig config = {});

    // 扫描所有共线相关线路列车，计算各未来入口的距离、ETA、车群分布和
    // 建议顺序。该函数只产生软计划，不修改列车状态，也不预约运行资源。
    RollingPlanResult plan(const std::vector<MonitoredTrain>& trains,
                           const LineTopologyRegistry& topology,
                           uint64_t simulation_now_ms) const;

private:
    AffectedLinePlannerConfig config_;
};
