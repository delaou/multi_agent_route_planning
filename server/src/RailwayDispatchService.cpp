#include "RailwayDispatchService.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace {
// 返回仅在本进程内单调递增的墙上时间，用于数据新鲜度和事件接收时间。
uint64_t serviceWallNowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
// 线路 Mod 会周期性重发全量几何，以便服务器重启后自动恢复。若内容没有变化，
// 不应递增拓扑版本或清空正在执行的命令/占用。
bool sameRoute(const LineRoute& lhs, const LineRoute& rhs) {
    // line_name 只是展示元数据，玩家重命名线路不应使正在占用的区段失效。
    if (lhs.line_id != rhs.line_id ||
        lhs.transport_type != rhs.transport_type ||
        lhs.geometry_source != rhs.geometry_source ||
        lhs.is_loop != rhs.is_loop ||
        lhs.target_headway_ms != rhs.target_headway_ms ||
        lhs.points.size() != rhs.points.size()) return false;
    for (size_t i = 0; i < lhs.points.size(); ++i) {
        const LineRoutePoint& a = lhs.points[i];
        const LineRoutePoint& b = rhs.points[i];
        if (a.point_id != b.point_id || a.track_segment_id != b.track_segment_id ||
            std::abs(a.x - b.x) > 1e-6 ||
            std::abs(a.y - b.y) > 1e-6 || std::abs(a.z - b.z) > 1e-6)
            return false;
    }
    return true;
}

bool sameRoutes(std::vector<LineRoute> lhs, std::vector<LineRoute> rhs) {
    if (lhs.size() != rhs.size()) return false;
    const auto by_id = [](const LineRoute& a, const LineRoute& b) {
        return a.line_id < b.line_id;
    };
    std::sort(lhs.begin(), lhs.end(), by_id);
    std::sort(rhs.begin(), rhs.end(), by_id);
    for (size_t i = 0; i < lhs.size(); ++i)
        if (!sameRoute(lhs[i], rhs[i])) return false;
    return true;
}
} // namespace

// 使用函数局部 static 实现线程安全单例初始化，所有 HTTP 连接共享同一状态。
RailwayDispatchService& RailwayDispatchService::instance() {
    static RailwayDispatchService service;
    return service;
}

// 创建默认子模块；main 会在 WebServer 启动前用 YAML 配置重新初始化一次。
RailwayDispatchService::RailwayDispatchService() {
    topology_ = std::make_unique<LineTopologyRegistry>(config_.route_analysis);
    candidate_builder_ =
        std::make_unique<DispatchCandidateBuilder>(config_.candidate_builder);
    rolling_planner_ =
        std::make_unique<AffectedLinePlanner>(config_.rolling_planner);
    dispatch_manager_ = std::make_unique<DispatchManager>(config_.dispatch);
}

// 原子替换服务配置并清空运行态，必须在工作线程开始处理请求前调用。
void RailwayDispatchService::configure(
    const RailwayDispatchServiceConfig& config) {
    if (config.train_stale_wall_ms == 0)
        throw std::invalid_argument("train stale timeout must be positive");
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    train_monitor_.clear();
    topology_ = std::make_unique<LineTopologyRegistry>(config_.route_analysis);
    candidate_builder_ =
        std::make_unique<DispatchCandidateBuilder>(config_.candidate_builder);
    rolling_planner_ =
        std::make_unique<AffectedLinePlanner>(config_.rolling_planner);
    dispatch_manager_ = std::make_unique<DispatchManager>(config_.dispatch);
    commands_by_train_.clear();
    active_by_train_.clear();
    rolling_plan_result_ = {};
    last_train_snapshot_wall_ms_ = 0;
    next_command_id_ = 1;
    persistence_healthy_ = true;
    persistence_error_.clear();
}

// 在工作线程启动前恢复数据库中的安全关键状态。任何一条未完成命令无法
// 重建时都拒绝启动数据层，防止重启后重复放车。
bool RailwayDispatchService::enablePersistence(
    DispatchPersistence* persistence, uint64_t wall_now_ms,
    std::string* reason) {
    if (!persistence) {
        if (reason) *reason = "persistence_is_null";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    DispatchRecoveryState recovery;
    if (!persistence->loadRecoveryState(&recovery, reason)) return false;
    if (!recovery.routes.empty()) {
        if (!topology_->restoreRoutes(recovery.routes,
                                      recovery.topology_revision, reason))
            return false;
        rebuildDispatchManagerLocked(wall_now_ms);
    }
    for (const DispatchDecision& command : recovery.pending_commands) {
        if (command.topology_revision != recovery.topology_revision ||
            !dispatch_manager_->restorePending(command, reason)) return false;
        commands_by_train_[command.train_id] = command;
    }
    for (const DispatchDecision& command : recovery.active_commands) {
        if (command.topology_revision != recovery.topology_revision ||
            !dispatch_manager_->restoreActive(command, reason)) return false;
        active_by_train_[command.train_id] = command;
    }
    next_command_id_ = std::max<uint64_t>(
        next_command_id_, recovery.next_command_sequence);
    persistence_ = persistence;
    persistence_healthy_ = true;
    persistence_error_.clear();
    return true;
}

bool RailwayDispatchService::persistenceHealthy(std::string* last_error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_error) *last_error = persistence_error_;
    return persistence_healthy_;
}

DispatchPersistenceStats RailwayDispatchService::persistenceStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return persistence_ ? persistence_->stats() : DispatchPersistenceStats{};
}

// 接收完整线路快照并重建全部拓扑相关资源；失败时返回原因，成功后输出数量。
bool RailwayDispatchService::replaceRoutes(
    const std::vector<LineRoute>& routes,
    uint64_t wall_now_ms,
    RouteUploadResult* result,
    std::string* reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    const LineTopologySnapshot current = topology_->snapshot();
    if (current.revision != 0 && sameRoutes(current.routes, routes)) {
        if (result) {
            result->route_count = current.routes.size();
            result->corridor_count = current.analysis.shared_corridors.size();
            result->intersection_count = current.analysis.intersections.size();
        }
        return true;
    }
    // 已放行列车仍可能位于旧几何资源中。此时替换拓扑会丢失占用映射，
    // 因而要求游戏端先完成 ACK/cleared 闭环再上传变化后的线路。
    if (!commands_by_train_.empty() || !active_by_train_.empty()) {
        if (reason) *reason = "topology_change_blocked_by_active_commands";
        return false;
    }
    if (!topology_->replaceRoutes(routes, reason)) return false;

    // 线路快照仅应在地图载入或线路修改时上传。拓扑变化会使旧 movement ID
    // 失效，因此必须清空待执行命令并重建预约管理器。
    commands_by_train_.clear();
    active_by_train_.clear();
    rolling_plan_result_ = {};
    candidate_builder_->clear();
    for (const MonitoredTrain& train : train_monitor_.snapshot())
        train_monitor_.setPhase(train.telemetry.train_id,
                                TrainDispatchPhase::MONITORED);
    rebuildDispatchManagerLocked(wall_now_ms);

    if (persistence_) {
        std::string persistence_error;
        const bool persisted = persistence_->persistTopology(
            topology_->snapshot(), wall_now_ms, &persistence_error);
        persistence_healthy_ = persisted;
        persistence_error_ = persisted ? "" : persistence_error;
        if (result) {
            result->persistence_ok = persisted;
            result->persistence_error = persistence_error;
        }
    }

    if (result) {
        const LineTopologySnapshot snapshot = topology_->snapshot();
        result->route_count = snapshot.routes.size();
        result->corridor_count = snapshot.analysis.shared_corridors.size();
        result->intersection_count = snapshot.analysis.intersections.size();
    }
    return true;
}

// 用当前拓扑重建预约器、每个共线段的正反向资源和各线路目标班次状态。
// 调用者持有服务 mutex_，因此重建期间不会有 HTTP 请求观察到半套资源。
void RailwayDispatchService::rebuildDispatchManagerLocked(uint64_t wall_now_ms) {
    dispatch_manager_ = std::make_unique<DispatchManager>(config_.dispatch);
    const LineTopologySnapshot snapshot = topology_->snapshot();
    for (const LineIntersectionMovement& movement : snapshot.analysis.movements)
        dispatch_manager_->registerIntersectionMovement(movement);

    std::unordered_map<std::string, uint64_t> headway_by_line;
    for (const LineRoute& route : snapshot.routes)
        headway_by_line[route.line_id] = route.target_headway_ms;

    for (const SharedCorridorDefinition& corridor :
         snapshot.analysis.shared_corridors) {
        for (CorridorDirection direction :
             {CorridorDirection::FORWARD, CorridorDirection::REVERSE}) {
            SharedCorridorState state;
            state.corridor_id = corridor.corridor_id;
            state.direction = direction;
            state.corridor_resource_id =
                DispatchCandidateBuilder::resourceId(corridor.corridor_id,
                                                     direction);
            state.received_at_wall_ms = wall_now_ms;
            state.minimum_headway_ms =
                config_.dispatch.priority.default_corridor_minimum_headway_ms;
            state.capacity = config_.dispatch.default_corridor_capacity;
            dispatch_manager_->upsertCorridor(state);

            for (const SharedCorridorMember& member : corridor.members) {
                LineDispatchState line;
                auto headway = headway_by_line.find(member.line_id);
                line.target_headway_ms = headway == headway_by_line.end()
                    ? 15000 : headway->second;
                dispatch_manager_->upsertLineState(
                    state.corridor_resource_id, member.line_id, line);
            }
        }
    }
}

// 清除过期监控、滚动预测和未 ACK 命令；已 ACK 占用必须等明确 cleared，
// 这是为了在网络中断时保持“宁可等待，也不错误放行”的安全原则。
void RailwayDispatchService::cleanupStaleLocked(uint64_t wall_now_ms) {
    const std::vector<std::string> removed = train_monitor_.removeStale(
        wall_now_ms, config_.train_stale_wall_ms);
    for (const std::string& train_id : removed) {
        if (persistence_) persistence_->markTrainStale(train_id, wall_now_ms);
        candidate_builder_->forgetTrain(train_id);
        // 同步从最近一次滚动结果删除，避免没有新遥测时查询到过期 ETA。
        for (auto& plan_pair : rolling_plan_result_.plans_by_resource) {
            auto& planned_trains = plan_pair.second.trains;
            planned_trains.erase(std::remove_if(
                planned_trains.begin(), planned_trains.end(),
                [&](const PlannedTrainArrival& planned) {
                    return planned.train_id == train_id;
                }), planned_trains.end());
        }
        for (auto it = rolling_plan_result_.by_train_resource.begin();
             it != rolling_plan_result_.by_train_resource.end();) {
            if (it->second.train_id == train_id)
                it = rolling_plan_result_.by_train_resource.erase(it);
            else
                ++it;
        }
        auto command = commands_by_train_.find(train_id);
        if (command != commands_by_train_.end()) {
            dispatch_manager_->cancelPending(
                command->second.corridor_resource_id);
            commands_by_train_.erase(command);
        }
        // 已 ACK、可能仍在区段内的列车不能仅因断网就自动释放占用；否则
        // 服务器可能放入冲突列车。该状态必须等待游戏端明确发送 cleared。
    }
}

// 一轮完整规划流水线：全线滚动软规划 -> 入口候选裁剪 -> 更新资源候选
// -> 对各个共线方向执行实时硬约束与 RELEASE 决策。
void RailwayDispatchService::refreshCandidatesAndPlanLocked(
    uint64_t simulation_now_ms, uint64_t wall_now_ms,
    std::vector<DispatchDecision>* issued) {
    const std::vector<MonitoredTrain> train_snapshot = train_monitor_.snapshot();
    // 第一层先扫描所有共线相关线路列车，生成未来数分钟的滚动软计划；
    // 第二层再从其中裁剪入口 1000m 内列车，进入实时优先级竞争。
    rolling_plan_result_ = rolling_planner_->plan(
        train_snapshot, *topology_, simulation_now_ms);
    const CandidateBuildResult built = candidate_builder_->build(
        train_snapshot, *topology_, &rolling_plan_result_);

    // 每轮都替换所有资源的候选，包括空数组；否则已经离开的列车会残留。
    const LineTopologySnapshot topology_snapshot = topology_->snapshot();
    for (const SharedCorridorDefinition& corridor :
         topology_snapshot.analysis.shared_corridors) {
        for (CorridorDirection direction :
             {CorridorDirection::FORWARD, CorridorDirection::REVERSE}) {
            const std::string resource_id =
                DispatchCandidateBuilder::resourceId(corridor.corridor_id,
                                                     direction);
            auto candidates = built.candidates_by_resource.find(resource_id);
            dispatch_manager_->replaceCandidates(
                resource_id,
                candidates == built.candidates_by_resource.end()
                    ? std::vector<DispatchCandidate>{} : candidates->second,
                wall_now_ms);
        }
    }

    for (const auto& phase : built.phase_by_train)
        train_monitor_.setPhase(phase.first, phase.second);

    for (const SharedCorridorDefinition& corridor :
         topology_snapshot.analysis.shared_corridors) {
        for (CorridorDirection direction :
             {CorridorDirection::FORWARD, CorridorDirection::REVERSE}) {
            const std::string resource_id =
                DispatchCandidateBuilder::resourceId(corridor.corridor_id,
                                                     direction);
            DispatchDecision decision = dispatch_manager_->planNext(
                resource_id, simulation_now_ms, wall_now_ms);
            if (decision.action == DispatchAction::RELEASE &&
                commands_by_train_.count(decision.train_id) == 0) {
                decision.command_id = "CMD:" +
                    std::to_string(next_command_id_++);
                decision.topology_revision = topology_snapshot.revision;
                decision.issued_at_simulation_ms = simulation_now_ms;

                // 把几何资源转换成该列车自己的线路索引范围，游戏端无需
                // 解析服务器内部 corridor_id 即可建立入口门控和驶离检测。
                const auto monitored = train_monitor_.get(decision.train_id);
                if (monitored.has_value()) {
                    for (const SharedCorridorMember& member : corridor.members) {
                        if (member.line_id != decision.line_id) continue;
                        const bool increasing =
                            monitored->telemetry.travel_direction ==
                            RouteTravelDirection::FORWARD;
                        decision.entry_route_index = increasing
                            ? member.start_segment : member.end_segment;
                        decision.exit_route_index = increasing
                            ? member.end_segment : member.start_segment;
                        decision.entry_route_position = increasing
                            ? member.start_position : member.end_position;
                        decision.exit_route_position = increasing
                            ? member.end_position : member.start_position;
                        decision.has_route_bounds = true;
                        break;
                    }
                }
                commands_by_train_[decision.train_id] = decision;
                if (issued) issued->push_back(decision);
                train_monitor_.setPhase(decision.train_id,
                    TrainDispatchPhase::RELEASE_PENDING, resource_id);
            }
        }
    }
}

// 保存本批全部列车后立即触发清理和重规划；simulation_now_ms 用于业务时间，
// wall_now_ms 只用于判断网络数据新鲜度。
TrainBatchUpdateResult RailwayDispatchService::updateTrains(
    const std::vector<TrainTelemetry>& trains,
    uint64_t simulation_now_ms,
    uint64_t wall_now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_train_snapshot_wall_ms_ = wall_now_ms;
    TrainBatchUpdateResult result = train_monitor_.upsertBatch(trains);
    cleanupStaleLocked(wall_now_ms);
    std::vector<DispatchDecision> issued;
    refreshCandidatesAndPlanLocked(simulation_now_ms, wall_now_ms, &issued);
    result.issued_commands = issued.size();
    if (persistence_) {
        std::string persistence_error;
        const bool persisted = persistence_->persistDispatchCycle(
            train_monitor_.snapshot(), rolling_plan_result_, issued,
            topology_->snapshot().revision, simulation_now_ms, wall_now_ms,
            &persistence_error);
        persistence_healthy_ = persisted;
        persistence_error_ = persisted ? "" : persistence_error;
        result.persistence_ok = persisted;
        result.persistence_error = persistence_error;
        if (!persisted) {
            // MySQL 未提交的新命令不能暴露给游戏端，撤销对应预预约。
            for (const DispatchDecision& command : issued) {
                dispatch_manager_->cancelPending(command.corridor_resource_id);
                commands_by_train_.erase(command.train_id);
            }
            result.issued_commands = 0;
        }
    }
    return result;
}

// 返回全部或指定列车的待 ACK 命令副本；查询不消费命令，允许游戏端重试。
std::vector<DispatchDecision> RailwayDispatchService::pendingCommands(
    const std::optional<std::string>& train_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DispatchDecision> result;
    if (train_id.has_value()) {
        auto it = commands_by_train_.find(*train_id);
        if (it != commands_by_train_.end()) result.push_back(it->second);
        return result;
    }
    for (const auto& pair : commands_by_train_) result.push_back(pair.second);
    std::sort(result.begin(), result.end(),
              [](const DispatchDecision& lhs, const DispatchDecision& rhs) {
                  return lhs.train_id < rhs.train_id;
              });
    return result;
}

// 在没有新遥测时提供显式清理入口，通常由命令轮询或周期任务触发。
void RailwayDispatchService::runMaintenance(uint64_t wall_now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    cleanupStaleLocked(wall_now_ms);
}

// 校验 ACK 与待命令一致后：accepted=true 确认进入并保存活动通行，
// accepted=false 取消命令和预预约；重复或不匹配 ACK 返回 false。
bool RailwayDispatchService::acknowledge(
    const std::string& corridor_resource_id,
    const std::string& train_id,
    bool accepted,
    uint64_t simulation_now_ms,
    const std::string& command_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto command = commands_by_train_.find(train_id);
    if (command == commands_by_train_.end() ||
        command->second.corridor_resource_id != corridor_resource_id ||
        (!command_id.empty() && command->second.command_id != command_id)) {
        return false;
    }
    if (persistence_) {
        std::string persistence_error;
        if (!persistence_->persistCommandAck(
                command->second, accepted, simulation_now_ms,
                serviceWallNowMs(), &persistence_error)) {
            persistence_healthy_ = false;
            persistence_error_ = persistence_error;
            return false;
        }
        persistence_healthy_ = true;
        persistence_error_.clear();
    }
    const bool ok = accepted
        ? dispatch_manager_->confirmEntry(corridor_resource_id, train_id,
                                          simulation_now_ms)
        : dispatch_manager_->cancelPending(corridor_resource_id);
    if (!ok) return false;

    if (accepted) active_by_train_[train_id] = command->second;
    train_monitor_.setPhase(train_id,
        accepted ? TrainDispatchPhase::INSIDE_RESOURCE
                 : TrainDispatchPhase::MONITORED,
        accepted ? corridor_resource_id : "");
    commands_by_train_.erase(command);
    return true;
}

// 处理车尾离开事件；缺省资源/交叉点信息可从 ACK 后保存的活动决策推导，
// 成功后把列车恢复为普通监控并删除旧候选等待计时。
bool RailwayDispatchService::markCleared(
    const std::string& train_id,
    const std::string& corridor_resource_id,
    const std::vector<std::string>& intersection_ids,
    uint64_t simulation_now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string effective_resource_id = corridor_resource_id;
    std::vector<std::string> effective_intersections = intersection_ids;
    auto active = active_by_train_.find(train_id);
    if (active != active_by_train_.end()) {
        if (effective_resource_id.empty())
            effective_resource_id = active->second.corridor_resource_id;
        if (effective_intersections.empty()) {
            // movement_id 属于几何拓扑；转换成 intersection_id 后通知预约管理器。
            const LineTopologySnapshot snapshot = topology_->snapshot();
            for (const std::string& movement_id :
                 active->second.reserved_intersection_movement_ids) {
                for (const LineIntersectionMovement& movement :
                     snapshot.analysis.movements) {
                    if (movement.movement_id == movement_id &&
                        std::find(effective_intersections.begin(),
                                  effective_intersections.end(),
                                  movement.intersection_id) ==
                            effective_intersections.end()) {
                        effective_intersections.push_back(
                            movement.intersection_id);
                    }
                }
            }
        }
    }
    if (persistence_ && active != active_by_train_.end()) {
        std::string persistence_error;
        if (!persistence_->persistResourceCleared(
                active->second, effective_intersections, simulation_now_ms,
                serviceWallNowMs(), &persistence_error)) {
            persistence_healthy_ = false;
            persistence_error_ = persistence_error;
            return false;
        }
        persistence_healthy_ = true;
        persistence_error_.clear();
    }
    bool changed = false;
    if (!effective_resource_id.empty())
        changed = dispatch_manager_->markCorridorExited(effective_resource_id);
    for (const std::string& intersection_id : effective_intersections) {
        changed = dispatch_manager_->markIntersectionCleared(
            intersection_id, train_id, simulation_now_ms) || changed;
    }
    if (changed) {
        train_monitor_.setPhase(train_id, TrainDispatchPhase::MONITORED);
        candidate_builder_->forgetTrain(train_id);
        active_by_train_.erase(train_id);
    }
    return changed;
}

// 对外返回全量监控快照，不暴露内部 TrainMonitor 容器。
std::vector<MonitoredTrain> RailwayDispatchService::trains() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return train_monitor_.snapshot();
}

// 对外返回当前生效拓扑的完整副本。
LineTopologySnapshot RailwayDispatchService::topology() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return topology_->snapshot();
}

RailwayRuntimeStatus RailwayDispatchService::runtimeStatus(
    uint64_t wall_now_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    RailwayRuntimeStatus result;
    result.topology = topology_->summary();
    result.train_count = train_monitor_.snapshot().size();
    result.last_train_snapshot_wall_ms = last_train_snapshot_wall_ms_;
    if (last_train_snapshot_wall_ms_ != 0 &&
        wall_now_ms >= last_train_snapshot_wall_ms_) {
        result.train_snapshot_age_ms = wall_now_ms - last_train_snapshot_wall_ms_;
        result.telemetry_connected =
            result.train_snapshot_age_ms <= config_.train_stale_wall_ms;
    }
    return result;
}

// 对外返回最近一次遥测生成的滚动计划副本，供 HTTP 展示和调试。
RollingPlanResult RailwayDispatchService::rollingPlan() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rolling_plan_result_;
}
