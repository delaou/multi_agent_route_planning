#pragma once

#include "DynamicPriorityPolicy.h"
#include "IntersectionReservationManager.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct DispatchManagerConfig {
    DynamicPriorityConfig priority;
    uint64_t default_intersection_clearance_ms = 2000;
    size_t default_corridor_capacity = 8;
};

// 调度总控层：维护运行状态、执行硬约束过滤、调用优先级策略，
// 并原子预约线路交叉点。所有公开修改操作均由内部互斥锁保护。
class DispatchManager {
public:
    // 创建动态优先级策略和交叉点预约器，并校验默认容量/清空时间。
    explicit DispatchManager(DispatchManagerConfig config = {});

    // 将线路分析产生的 movement 注册到全局交叉点预约器。
    bool registerIntersectionMovement(const LineIntersectionMovement& movement);
    // 新增或刷新共线方向资源；保留服务器内部的待 ACK 状态和已确认进入时间。
    void upsertCorridor(const SharedCorridorState& corridor);
    // 新增或刷新某资源下某条线路的目标班次、公平信用等状态。
    void upsertLineState(const std::string& corridor_resource_id,
                         const std::string& line_id,
                         const LineDispatchState& state);
    // 只替换遥测派生的候选和时间戳，不覆盖占用量、上次进入时间等运行状态。
    bool replaceCandidates(const std::string& corridor_resource_id,
                           std::vector<DispatchCandidate> candidates,
                           uint64_t received_at_wall_ms);

    // 生成下一条命令；若返回 RELEASE，会先锁定所需交叉点并进入待确认状态。
    DispatchDecision planNext(const std::string& corridor_resource_id,
                              uint64_t simulation_now_ms,
                              uint64_t wall_now_ms);
    // 游戏端确认列车已经实际进入共线段后，才更新进入时间和公平状态。
    bool confirmEntry(const std::string& corridor_resource_id,
                      const std::string& train_id,
                      uint64_t simulation_now_ms);
    // 拒绝、超时或执行失败时撤销待 ACK 命令及其交叉点预预约。
    bool cancelPending(const std::string& corridor_resource_id);
    // 启动恢复专用：重建尚未 ACK 的预预约和资源 pending 状态。
    bool restorePending(const DispatchDecision& decision,
                        std::string* reason = nullptr);
    // 启动恢复专用：重建已 ACK 但尚未 cleared 的容量和交叉点占用。
    bool restoreActive(const DispatchDecision& decision,
                       std::string* reason = nullptr);
    // 收到车尾离开事件后将该共线方向资源的 occupancy 减一。
    bool markCorridorExited(const std::string& corridor_resource_id);
    // 通知预约器列车尾部已经离开指定交叉点并开始清空计时。
    bool markIntersectionCleared(const std::string& intersection_id,
                             const std::string& train_id,
                             uint64_t simulation_now_ms);

    // 查询一个共线方向资源的完整运行状态快照。
    std::optional<SharedCorridorState> corridorState(
        const std::string& corridor_resource_id) const;
    // 查询所有仍在占用中的交叉 movement 预约。
    std::vector<IntersectionReservation> activeIntersectionReservations() const;

private:
    // 按游戏模拟时间累计线路公平信用；调用者必须持有 mutex_。
    void updateFairnessCreditsLocked(const std::string& corridor_resource_id,
                                     uint64_t simulation_now_ms);

    DynamicPriorityPolicy policy_;
    IntersectionReservationManager reservations_;
    size_t default_corridor_capacity_;
    std::unordered_map<std::string, SharedCorridorState> corridors_;
    std::unordered_map<std::string,
        std::unordered_map<std::string, LineDispatchState>> line_states_;
    std::unordered_map<std::string, std::string> last_released_line_;
    std::unordered_map<std::string, size_t> consecutive_release_count_;
    std::unordered_map<std::string, DispatchDecision> pending_decisions_;
    mutable std::mutex mutex_;
};
