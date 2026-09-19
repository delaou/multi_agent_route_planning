#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// 列车相对于线路有序采样点的运行方向。FORWARD 表示 route_index 递增，
// REVERSE 表示 route_index 递减；它与某个共线段的 FORWARD/REVERSE 不是一回事。
enum class RouteTravelDirection { FORWARD, REVERSE };

// 列车从普通监控到接近、等待、待确认和资源内的服务器阶段。
enum class TrainDispatchPhase {
    MONITORED,
    APPROACHING,
    WAITING_PERMISSION,
    RELEASE_PENDING,
    INSIDE_RESOURCE
};

// 游戏 Mod 上报的一辆列车的最新遥测。simulation_time_ms 用于调度计时，
// received_at_wall_ms 用于网络数据过期判断，两种时钟不能混用。
struct TrainTelemetry {
    std::string train_id;
    std::string line_id;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double speed = 0.0;
    size_t route_index = 0;
    // 连续线路位置；旧 Mod 未上传时回退为 route_index。
    double route_position = 0.0;
    RouteTravelDirection travel_direction = RouteTravelDirection::FORWARD;
    uint64_t simulation_time_ms = 0;
    uint64_t received_at_wall_ms = 0;
};

// 最新遥测与服务器推导调度阶段的组合视图。
struct MonitoredTrain {
    TrainTelemetry telemetry;
    TrainDispatchPhase phase = TrainDispatchPhase::MONITORED;
    std::string controlled_resource_id;
};

// 批量遥测接收、命令生成和持久化结果摘要。
struct TrainBatchUpdateResult {
    size_t accepted = 0;
    size_t ignored_out_of_order = 0;
    size_t invalid = 0;
    size_t issued_commands = 0;
    bool persistence_ok = true;
    std::string persistence_error;
};

// 保存地图上所有列车的最新状态。该类只负责监控事实，不负责判断共线段，
// 从而让“全量监控”和“局部规划”保持清晰分层。
class TrainMonitor {
public:
    // 批量新增或更新列车；旧于已保存墙上时间的乱序包会被忽略。
    // 返回成功、乱序和非法记录的数量，单条失败不会中断整批更新。
    TrainBatchUpdateResult upsertBatch(const std::vector<TrainTelemetry>& batch);
    // 按 train_id 返回一份线程安全快照；不存在时返回 nullopt。
    std::optional<MonitoredTrain> get(const std::string& train_id) const;
    // 返回全部列车快照，并按 train_id 排序以保证日志和测试结果稳定。
    std::vector<MonitoredTrain> snapshot() const;
    // 更新服务器维护的调度阶段；不会覆盖同一列车的最新遥测。
    bool setPhase(const std::string& train_id,
                  TrainDispatchPhase phase,
                  const std::string& controlled_resource_id = "");

    // 返回被删除的 train_id，调用方可据此取消尚未 ACK 的调度命令。
    std::vector<std::string> removeStale(uint64_t wall_now_ms,
                                         uint64_t stale_after_ms);
    // 清空全部监控状态，主要用于服务器启动时重新配置或测试隔离。
    void clear();
    // 返回当前监控列车数量。
    size_t size() const;

private:
    std::unordered_map<std::string, MonitoredTrain> trains_;
    mutable std::mutex mutex_;
};
