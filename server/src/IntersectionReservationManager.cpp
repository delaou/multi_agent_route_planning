#include "IntersectionReservationManager.h"

#include <algorithm>
#include <unordered_set>

// 保存缺省清空时间；movement 可通过 clearance_time_ms 单独覆盖。
IntersectionReservationManager::IntersectionReservationManager(
    uint64_t default_clearance_ms)
    : default_clearance_ms_(default_clearance_ms) {}

// 校验 movement 必需字段后注册定义，并确保交叉点运行容器已经创建。
bool IntersectionReservationManager::registerMovement(
    const LineIntersectionMovement& movement) {
    if (movement.movement_id.empty() || movement.intersection_id.empty() ||
        movement.line_id.empty() || movement.incoming_route_segment_id.empty() ||
        movement.outgoing_route_segment_id.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    movements_[movement.movement_id] = movement;
    intersections_.try_emplace(movement.intersection_id);
    return true;
}

// 解析 movement 最终清空时间：优先自定义值，否则回退到全局默认值。
uint64_t IntersectionReservationManager::clearanceFor(
    const LineIntersectionMovement& movement) const {
    return movement.clearance_time_ms == 0
        ? default_clearance_ms_ : movement.clearance_time_ms;
}

// 同一 movement 必然互斥；不同 movement 采用双向兼容的冲突声明判断。
bool IntersectionReservationManager::movementsConflict(
    const LineIntersectionMovement& lhs,
    const LineIntersectionMovement& rhs) const {
    if (lhs.movement_id == rhs.movement_id) return true;
    const auto contains = [](const std::vector<std::string>& values,
                             const std::string& value) {
        return std::find(values.begin(), values.end(), value) != values.end();
    };
    // 任意一侧声明冲突即视为冲突，允许游戏端只上传单向关系。
    return contains(lhs.conflict_movement_ids, rhs.movement_id) ||
           contains(rhs.conflict_movement_ids, lhs.movement_id);
}

// 在已持锁条件下完成批内冲突、活动占用和历史清空时间三层检查。
bool IntersectionReservationManager::canReserveLocked(
    const std::vector<std::string>& movement_ids,
    uint64_t simulation_now_ms,
    std::string* reason) const {
    std::unordered_set<std::string> unique_ids;
    std::vector<const LineIntersectionMovement*> requested;
    requested.reserve(movement_ids.size());
    for (const std::string& movement_id : movement_ids) {
        if (!unique_ids.insert(movement_id).second) continue;
        auto movement_it = movements_.find(movement_id);
        if (movement_it == movements_.end()) {
            if (reason) *reason = "unknown_line_intersection_movement:" + movement_id;
            return false;
        }
        requested.push_back(&movement_it->second);
    }

    // 同一次请求内部也可能包含互相冲突的 movement，必须先拒绝。
    for (size_t i = 0; i < requested.size(); ++i) {
        for (size_t j = i + 1; j < requested.size(); ++j) {
            if (requested[i]->intersection_id == requested[j]->intersection_id &&
                movementsConflict(*requested[i], *requested[j])) {
                if (reason) *reason = "requested_intersection_movements_conflict";
                return false;
            }
        }
    }

    for (const LineIntersectionMovement* movement : requested) {
        auto intersection_it = intersections_.find(movement->intersection_id);
        if (intersection_it == intersections_.end()) continue;
        const IntersectionRuntime& runtime = intersection_it->second;

        // 活动预约表示列车尚未报告车尾完全离开交叉点。
        for (const auto& pair : runtime.active_by_movement) {
            auto active_it = movements_.find(pair.first);
            if (active_it != movements_.end() &&
                movementsConflict(*movement, active_it->second)) {
                if (reason) *reason = "line_intersection_occupied:" +
                                      movement->intersection_id;
                return false;
            }
        }

        // 活动预约释放后，冲突 movement 还必须满足额外清空时间。
        for (const auto& pair : runtime.last_clear_by_movement) {
            auto cleared_it = movements_.find(pair.first);
            if (cleared_it == movements_.end() ||
                !movementsConflict(*movement, cleared_it->second)) {
                continue;
            }
            const uint64_t clearance_ms = std::max(
                clearanceFor(*movement), clearanceFor(cleared_it->second));
            if (simulation_now_ms < pair.second ||
                simulation_now_ms - pair.second < clearance_ms) {
                if (reason) *reason = "line_intersection_clearance_not_met:" +
                                      movement->intersection_id;
                return false;
            }
        }
    }
    return true;
}

// 对外只读预检查入口；加锁后复用 canReserveLocked，绝不写入预约。
bool IntersectionReservationManager::canReserve(
    const std::vector<std::string>& movement_ids,
    uint64_t simulation_now_ms,
    std::string* reason) const {
    // 只读预检查也加锁，避免遍历过程中拓扑或预约状态被并发修改。
    std::lock_guard<std::mutex> lock(mutex_);
    return canReserveLocked(movement_ids, simulation_now_ms, reason);
}

// 在同一临界区中先验证后写入全部 movement，任何一项失败都不会部分占用。
bool IntersectionReservationManager::tryReserveAtomically(
    const std::string& train_id,
    const std::vector<std::string>& movement_ids,
    uint64_t simulation_now_ms,
    std::string* reason) {
    if (train_id.empty()) {
        if (reason) *reason = "empty_train_id";
        return false;
    }
    // 校验和写入持有同一把锁，从而保证多交叉点预约的原子性。
    std::lock_guard<std::mutex> lock(mutex_);
    if (!canReserveLocked(movement_ids, simulation_now_ms, reason)) return false;

    std::unordered_set<std::string> unique_ids;
    for (const std::string& movement_id : movement_ids) {
        if (!unique_ids.insert(movement_id).second) continue;
        const LineIntersectionMovement& movement = movements_.at(movement_id);
        IntersectionReservation reservation;
        reservation.intersection_id = movement.intersection_id;
        reservation.movement_id = movement_id;
        reservation.train_id = train_id;
        reservation.reserved_at_simulation_ms = simulation_now_ms;
        intersections_[movement.intersection_id].active_by_movement[movement_id] =
            reservation;
    }
    return true;
}

// 删除指定列车在该交叉点的活动预约，并记录车尾离开模拟时间开始安全间隔。
bool IntersectionReservationManager::markCleared(
    const std::string& intersection_id,
    const std::string& train_id,
    uint64_t simulation_now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto intersection_it = intersections_.find(intersection_id);
    if (intersection_it == intersections_.end()) return false;

    bool cleared = false;
    // 同一列车可能一次预约多个 movement，这里清除指定交叉点内的全部项。
    auto& active = intersection_it->second.active_by_movement;
    for (auto it = active.begin(); it != active.end();) {
        if (it->second.train_id == train_id) {
            intersection_it->second.last_clear_by_movement[it->first] =
                simulation_now_ms;
            it = active.erase(it);
            cleared = true;
        } else {
            ++it;
        }
    }
    return cleared;
}

// 撤销命令阶段的预预约；因为列车没有实际进入，所以不写 last_clear 时间。
void IntersectionReservationManager::cancelTrain(const std::string& train_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& intersection_pair : intersections_) {
        auto& active = intersection_pair.second.active_by_movement;
        for (auto it = active.begin(); it != active.end();) {
            if (it->second.train_id == train_id) it = active.erase(it);
            else ++it;
        }
    }
}

std::vector<IntersectionReservation>
// 汇总所有交叉点中的活动预约并返回副本，供状态查询和诊断使用。
IntersectionReservationManager::activeReservations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<IntersectionReservation> result;
    for (const auto& intersection_pair : intersections_) {
        for (const auto& reservation_pair :
             intersection_pair.second.active_by_movement) {
            result.push_back(reservation_pair.second);
        }
    }
    return result;
}
