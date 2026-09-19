#pragma once

#include "DispatchPolicy.h"

// 动态优先级策略的硬约束阈值和可调评分权重。
struct DynamicPriorityConfig {
    // 共线段同一方向两次进入之间的硬下限，使用游戏时间。
    uint64_t default_corridor_minimum_headway_ms = 5000;
    // 超过该等待时间后进入防饥饿优先选择。
    uint64_t maximum_wait_ms = 20000;
    // 网络状态新鲜度使用服务器现实时间，不受游戏暂停影响。
    uint64_t telemetry_stale_wall_ms = 1500;
    size_t max_consecutive_release = 2;

    double headway_urgency_weight = 0.50;
    double waiting_time_weight = 0.30;
    double fairness_credit_weight = 0.20;
    double global_plan_weight = 0.25;
    double consecutive_penalty_weight = 0.25;
};

// 在所有安全候选中兼顾班次、等待、公平性和滚动计划的选择策略。
class DynamicPriorityPolicy : public IDispatchPolicy {
public:
    // 保存评分权重和安全阈值；非法时间或非有限权重会抛出异常。
    explicit DynamicPriorityPolicy(DynamicPriorityConfig config = {});

    // 先检查状态新鲜度、容量、下游和 5 秒间隔等硬约束，再对可行候选
    // 计算动态分数并选择一辆 RELEASE；本函数本身不修改运行状态。
    DispatchDecision decide(const DispatchContext& context) const override;

    // 暴露只读配置，供 DispatchManager 初始化缺省共线间隔等参数。
    const DynamicPriorityConfig& config() const;

private:
    DynamicPriorityConfig config_;
};
