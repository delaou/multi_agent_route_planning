#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// 一条线路通过某个交叉点的一种具体运动方式。
// 交叉点本身不是简单坐标，真正参与互斥的是 movement。
struct LineIntersectionMovement {
    std::string movement_id;
    std::string intersection_id;
    std::string line_id;
    std::string incoming_route_segment_id;
    std::string outgoing_route_segment_id;
    // 与当前 movement 不能同时执行的 movement ID。
    std::vector<std::string> conflict_movement_ids;
    uint64_t clearance_time_ms = 0;
};

// 已为列车锁定、尚未收到“车尾完全离开”事件的交叉点预约。
struct IntersectionReservation {
    std::string intersection_id;
    std::string movement_id;
    std::string train_id;
    uint64_t reserved_at_simulation_ms = 0;
};

class IntersectionReservationManager {
public:
    // 设置 movement 未单独指定时使用的默认交叉点清空时间。
    explicit IntersectionReservationManager(uint64_t default_clearance_ms = 2000);

    // 注册或更新一个交叉 movement，同时创建对应交叉点的运行状态容器。
    bool registerMovement(const LineIntersectionMovement& movement);
    // 只读检查一组 movement 当前能否同时预约；失败原因通过 reason 返回。
    bool canReserve(const std::vector<std::string>& movement_ids,
                    uint64_t simulation_now_ms,
                    std::string* reason = nullptr) const;
    // movement_ids 必须全部可用才会写入，防止列车只占到部分资源。
    bool tryReserveAtomically(const std::string& train_id,
                              const std::vector<std::string>& movement_ids,
                              uint64_t simulation_now_ms,
                              std::string* reason = nullptr);
    // 必须由游戏端在车尾完全离开交叉点后调用；随后仍执行清空时间。
    bool markCleared(const std::string& intersection_id,
                     const std::string& train_id,
                     uint64_t simulation_now_ms);
    // 撤销列车尚未实际进入的全部预约，不记录额外清空时间。
    void cancelTrain(const std::string& train_id);
    // 返回所有尚未收到车尾离开事件的活动预约快照。
    std::vector<IntersectionReservation> activeReservations() const;

private:
    struct IntersectionRuntime {
        // 非冲突 movement 可以在同一交叉点同时存在多条活动预约。
        std::unordered_map<std::string, IntersectionReservation> active_by_movement;
        // 记录每种 movement 的实际清空时刻，用于额外 2 秒安全间隔。
        std::unordered_map<std::string, uint64_t> last_clear_by_movement;
    };

    // canReserve 的锁内实现；调用者必须已经持有 mutex_。
    bool canReserveLocked(const std::vector<std::string>& movement_ids,
                          uint64_t simulation_now_ms,
                          std::string* reason) const;
    // 对称判断两个 movement 是否冲突；任意一方声明冲突即视为互斥。
    bool movementsConflict(const LineIntersectionMovement& lhs,
                           const LineIntersectionMovement& rhs) const;
    // 返回 movement 自定义清空时间，未指定则使用管理器默认值。
    uint64_t clearanceFor(const LineIntersectionMovement& movement) const;

    uint64_t default_clearance_ms_;
    std::unordered_map<std::string, LineIntersectionMovement> movements_;
    std::unordered_map<std::string, IntersectionRuntime> intersections_;
    mutable std::mutex mutex_;
};
