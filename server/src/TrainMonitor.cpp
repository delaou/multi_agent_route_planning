#include "TrainMonitor.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
// 检查主键、服务器接收时间和数值字段，防止非法遥测污染全局监控表。
bool validTelemetry(const TrainTelemetry& telemetry) {
    return !telemetry.train_id.empty() && !telemetry.line_id.empty() &&
           telemetry.received_at_wall_ms != 0 &&
           std::isfinite(telemetry.x) && std::isfinite(telemetry.y) &&
           std::isfinite(telemetry.z) && std::isfinite(telemetry.speed) &&
           std::isfinite(telemetry.route_position) && telemetry.route_position >= 0.0;
}
} // namespace

// 在一把锁内处理整批遥测；不同列车独立计数，乱序数据只忽略而不报致命错误。
TrainBatchUpdateResult TrainMonitor::upsertBatch(
    const std::vector<TrainTelemetry>& batch) {
    TrainBatchUpdateResult result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const TrainTelemetry& telemetry : batch) {
        if (!validTelemetry(telemetry)) {
            ++result.invalid;
            continue;
        }

        auto existing = trains_.find(telemetry.train_id);
        if (existing != trains_.end() &&
            telemetry.received_at_wall_ms <
                existing->second.telemetry.received_at_wall_ms) {
            // 并发 HTTP 请求可能乱序到达，旧包不能把新位置覆盖回去。
            ++result.ignored_out_of_order;
            continue;
        }

        if (existing == trains_.end()) {
            MonitoredTrain monitored;
            monitored.telemetry = telemetry;
            trains_.emplace(telemetry.train_id, std::move(monitored));
        } else {
            // phase 和 controlled_resource_id 是服务器状态，不能被遥测覆盖。
            existing->second.telemetry = telemetry;
        }
        ++result.accepted;
    }
    return result;
}

// 返回指定列车的值副本，使调用者离开函数后无需继续持有监控器的锁。
std::optional<MonitoredTrain> TrainMonitor::get(
    const std::string& train_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = trains_.find(train_id);
    if (it == trains_.end()) return std::nullopt;
    return it->second;
}

// 复制全部状态并稳定排序，便于滚动规划、HTTP 输出和可重复测试。
std::vector<MonitoredTrain> TrainMonitor::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MonitoredTrain> result;
    result.reserve(trains_.size());
    for (const auto& pair : trains_) result.push_back(pair.second);
    std::sort(result.begin(), result.end(), [](const MonitoredTrain& lhs,
                                               const MonitoredTrain& rhs) {
        return lhs.telemetry.train_id < rhs.telemetry.train_id;
    });
    return result;
}

// 只修改服务器调度阶段；RELEASE_PENDING/INSIDE_RESOURCE 在未给新资源 ID 时
// 保留原 controlled_resource_id，避免普通遥测把 ACK 生命周期信息清掉。
bool TrainMonitor::setPhase(const std::string& train_id,
                            TrainDispatchPhase phase,
                            const std::string& controlled_resource_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = trains_.find(train_id);
    if (it == trains_.end()) return false;
    it->second.phase = phase;
    if (!controlled_resource_id.empty() ||
        (phase != TrainDispatchPhase::RELEASE_PENDING &&
         phase != TrainDispatchPhase::INSIDE_RESOURCE)) {
        it->second.controlled_resource_id = controlled_resource_id;
    }
    return true;
}

// 用服务器墙上时间删除断联列车，返回 ID 供上层同步撤销候选和待执行命令。
std::vector<std::string> TrainMonitor::removeStale(
    uint64_t wall_now_ms, uint64_t stale_after_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> removed;
    for (auto it = trains_.begin(); it != trains_.end();) {
        const uint64_t received = it->second.telemetry.received_at_wall_ms;
        const bool stale = received == 0 || received > wall_now_ms ||
            wall_now_ms - received > stale_after_ms;
        if (stale) {
            removed.push_back(it->first);
            it = trains_.erase(it);
        } else {
            ++it;
        }
    }
    return removed;
}

// 线程安全返回当前监控规模。
size_t TrainMonitor::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return trains_.size();
}

// 清空全部监控状态；不承担外部调度预约清理，调用方必须统一编排。
void TrainMonitor::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    trains_.clear();
}
