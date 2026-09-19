#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// 方向是相对于共线段的标准起点/终点，而不是某条线路自己的“上/下行”。
// 这样 A 线下行与 B 线上行实际同向时，仍会进入同一个方向资源。
enum class CorridorDirection { FORWARD, REVERSE };
// 调度器本轮可能返回的不动作、全部保持或放行。
enum class DispatchAction { NO_ACTION, HOLD_ALL, RELEASE };

// 已经到达共线段控制区、等待参与本轮决策的列车。
struct DispatchCandidate {
    std::string train_id;
    std::string line_id;
    CorridorDirection corridor_direction = CorridorDirection::FORWARD;
    std::string corridor_resource_id;
    // 放行前必须一次性预约成功的所有线路交叉 movement。
    std::vector<std::string> required_intersection_movement_ids;
    // 列车在线路有序路径中的位置，用于游戏端和服务器稳定对齐。
    size_t route_index = 0;
    double route_position = 0.0;
    // 调度等待时间使用游戏时间；状态新鲜度使用服务器现实时间。
    uint64_t arrived_at_simulation_ms = 0;
    uint64_t received_at_wall_ms = 0;
    // 来自全线滚动规划的软建议。入口实时策略只把它作为加分项，不能绕过硬约束。
    uint64_t predicted_arrival_simulation_ms = 0;
    size_t rolling_plan_rank = static_cast<size_t>(-1);
    double global_plan_priority = 0.0;
};

// 某条运营线路在一个具体共线方向资源中的调度状态。
struct LineDispatchState {
    uint64_t target_headway_ms = 15000;
    uint64_t last_release_simulation_ms = 0;
    uint64_t last_credit_update_simulation_ms = 0;
    // 长时间未得到放行的线路会积累信用，放行后扣减。
    double fairness_credit = 0.0;
};

// 一个共线段的单方向运行资源；FORWARD/REVERSE 各维护一份。
struct SharedCorridorState {
    std::string corridor_resource_id;
    std::string corridor_id;
    CorridorDirection direction = CorridorDirection::FORWARD;
    uint64_t received_at_wall_ms = 0;
    uint64_t last_entry_simulation_ms = 0;
    uint64_t minimum_headway_ms = 5000;
    size_t occupancy = 0;
    // capacity 是同时处于共线段内的列车上限；最小进入间隔由
    // minimum_headway_ms 单独控制。
    size_t capacity = 8;
    bool downstream_available = true;
    // 已下发 RELEASE 但未收到游戏确认时，不再生成新的放行命令。
    bool command_pending = false;
    std::vector<DispatchCandidate> candidates;
};

// 评分策略一次决策所需的不可变资源、线路历史和双时钟上下文。
struct DispatchContext {
    SharedCorridorState corridor;
    std::unordered_map<std::string, LineDispatchState> lines;
    std::string last_released_line_id;
    size_t consecutive_release_count = 0;
    uint64_t simulation_now_ms = 0; // 随游戏暂停、加速而变化。
    uint64_t wall_now_ms = 0;       // 只用于网络数据超时判断。
};

// 决策总分的各项贡献，供解释、调参和离线评估使用。
struct DispatchScoreBreakdown {
    double headway_urgency = 0.0;
    double waiting_time = 0.0;
    double fairness_credit = 0.0;
    double consecutive_penalty = 0.0;
    double global_plan_priority = 0.0;
};

// 一次调度决策及其可持久化、可幂等确认的完整命令信息。
struct DispatchDecision {
    DispatchAction action = DispatchAction::NO_ACTION;
    // 同一条命令在 ACK 前重复查询时保持不变，用于游戏端幂等去重。
    std::string command_id;
    std::string corridor_resource_id;
    std::string corridor_id;
    CorridorDirection direction = CorridorDirection::FORWARD;
    std::string train_id;
    std::string line_id;
    uint64_t topology_revision = 0;
    uint64_t issued_at_simulation_ms = 0;
    // 入口/出口均使用该列车自身线路的 segment 索引；方向字段决定增减方向。
    size_t entry_route_index = 0;
    size_t exit_route_index = 0;
    double entry_route_position = 0.0;
    double exit_route_position = 0.0;
    bool has_route_bounds = false;
    std::vector<std::string> reserved_intersection_movement_ids;
    std::string reason;
    double score = 0.0;
    // 保留评分拆解，便于日志、前端展示和调参。
    DispatchScoreBreakdown breakdown;
};

// 策略接口只负责“从安全候选中选谁”，不直接修改运行状态。
class IDispatchPolicy {
public:
    // 通过虚析构保证以后经策略基类指针销毁具体策略时行为正确。
    virtual ~IDispatchPolicy() = default;
    // 根据一份只读调度上下文返回决策；具体策略不得在此直接修改资源状态。
    virtual DispatchDecision decide(const DispatchContext& context) const = 0;
};
