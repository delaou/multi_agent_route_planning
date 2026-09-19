#include "DispatchPersistence.h"

#include "DBConnectionPool.h"
#include "LineTopologyRegistry.h"
#include "RedisCache.h" 

#include <mysql/mysql.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>

using json = nlohmann::json;

namespace {

// 将字符串转义为当前 MySQL 连接可安全放入单引号字面量的内容。
std::string escapeSql(MYSQL* conn, const std::string& value) {
    std::string escaped(value.size() * 2 + 1, '\0');
    const unsigned long length = mysql_real_escape_string(
        conn, escaped.data(), value.data(),
        static_cast<unsigned long>(value.size()));
    escaped.resize(length);
    return escaped;
}

// 执行不返回结果集的 SQL；失败时把 MySQL 错误写入 error。
bool execute(MYSQL* conn, const std::string& sql, std::string* error) {
    if (mysql_query(conn, sql.c_str()) == 0) return true;
    if (error) *error = mysql_error(conn);
    return false;
}

// 把列车运行方向转换为稳定字符串，供 JSON、MySQL 和 Redis 共同使用。
const char* travelDirectionName(RouteTravelDirection direction) {
    return direction == RouteTravelDirection::FORWARD ? "forward" : "reverse";
}

// 把调度阶段转换为稳定字符串；这些值会持久化，修改时需要兼容旧数据。
const char* phaseName(TrainDispatchPhase phase) {
    switch (phase) {
    case TrainDispatchPhase::APPROACHING: return "approaching";
    case TrainDispatchPhase::WAITING_PERMISSION: return "waiting_permission";
    case TrainDispatchPhase::RELEASE_PENDING: return "release_pending";
    case TrainDispatchPhase::INSIDE_RESOURCE: return "inside_resource";
    default: return "monitored";
    }
}

// 把共线方向转换为稳定字符串。
const char* corridorDirectionName(CorridorDirection direction) {
    return direction == CorridorDirection::FORWARD ? "forward" : "reverse";
}

// 把调度动作转换为稳定字符串。
const char* actionName(DispatchAction action) {
    switch (action) {
    case DispatchAction::HOLD_ALL: return "hold_all";
    case DispatchAction::RELEASE: return "release";
    default: return "no_action";
    }
}

// 从持久化字符串恢复共线方向；未知值使用更保守的 FORWARD 默认值。
CorridorDirection corridorDirectionFromName(const std::string& value) {
    return value == "reverse" ? CorridorDirection::REVERSE
                              : CorridorDirection::FORWARD;
}

// 从持久化字符串恢复调度动作。
DispatchAction actionFromName(const std::string& value) {
    if (value == "release") return DispatchAction::RELEASE;
    if (value == "hold_all") return DispatchAction::HOLD_ALL;
    return DispatchAction::NO_ACTION;
}

// 序列化线路几何。拓扑分析结果可以确定性重建，因此恢复时只依赖原始线路。
json routeToJson(const LineRoute& route) {
    json points = json::array();
    for (const LineRoutePoint& point : route.points) {
        points.push_back({{"point_id", point.point_id},
                          {"track_segment_id", point.track_segment_id}, {"x", point.x},
                          {"y", point.y}, {"z", point.z}});
    }
    return {{"line_id", route.line_id}, {"line_name", route.line_name},
            {"transport_type", route.transport_type},
            {"geometry_source", route.geometry_source},
            {"is_loop", route.is_loop},
            {"target_headway_ms", route.target_headway_ms},
            {"points", std::move(points)}};
}

// 反序列化单条线路；JSON 缺少必需字段时由 nlohmann::json 抛出异常。
LineRoute routeFromJson(const json& value) {
    LineRoute route;
    route.line_id = value.at("line_id").get<std::string>();
    route.line_name = value.value("line_name", "");
    route.transport_type = value.value("transport_type", "unknown");
    route.geometry_source = value.value("geometry_source", "unknown");
    route.is_loop = value.value("is_loop", false);
    route.target_headway_ms = value.value("target_headway_ms", 15000ULL);
    for (const json& item : value.at("points")) {
        LineRoutePoint point;
        point.point_id = item.value("point_id", "");
        point.track_segment_id = item.value("track_segment_id", "");
        point.x = item.at("x").get<double>();
        point.y = item.value("y", 0.0);
        point.z = item.at("z").get<double>();
        route.points.push_back(std::move(point));
    }
    return route;
}

// 序列化一条调度命令，保证服务重启后能重建待确认和占用资源。
json decisionToJson(const DispatchDecision& decision) {
    return {
        {"command_id", decision.command_id},
        {"action", actionName(decision.action)},
        {"corridor_resource_id", decision.corridor_resource_id},
        {"corridor_id", decision.corridor_id},
        {"direction", corridorDirectionName(decision.direction)},
        {"train_id", decision.train_id},
        {"line_id", decision.line_id},
        {"topology_revision", decision.topology_revision},
        {"issued_at_simulation_ms", decision.issued_at_simulation_ms},
        {"entry_route_index", decision.entry_route_index},
        {"exit_route_index", decision.exit_route_index},
        {"entry_route_position", decision.entry_route_position},
        {"exit_route_position", decision.exit_route_position},
        {"has_route_bounds", decision.has_route_bounds},
        {"reserved_intersection_movement_ids",
         decision.reserved_intersection_movement_ids},
        {"reason", decision.reason},
        {"score", decision.score},
        {"breakdown", {
            {"headway_urgency", decision.breakdown.headway_urgency},
            {"waiting_time", decision.breakdown.waiting_time},
            {"fairness_credit", decision.breakdown.fairness_credit},
            {"consecutive_penalty", decision.breakdown.consecutive_penalty},
            {"global_plan_priority", decision.breakdown.global_plan_priority}
        }}
    };
}

// 从 MySQL JSON 恢复一条调度命令；用于重建安全相关的待 ACK/占用状态。
DispatchDecision decisionFromJson(const json& value) {
    DispatchDecision decision;
    decision.command_id = value.at("command_id").get<std::string>();
    decision.action = actionFromName(value.value("action", "no_action"));
    decision.corridor_resource_id =
        value.at("corridor_resource_id").get<std::string>();
    decision.corridor_id = value.value("corridor_id", "");
    decision.direction = corridorDirectionFromName(
        value.value("direction", "forward"));
    decision.train_id = value.at("train_id").get<std::string>();
    decision.line_id = value.value("line_id", "");
    decision.topology_revision = value.value("topology_revision", 0ULL);
    decision.issued_at_simulation_ms =
        value.value("issued_at_simulation_ms", 0ULL);
    decision.entry_route_index = value.value("entry_route_index", size_t{0});
    decision.exit_route_index = value.value("exit_route_index", size_t{0});
    decision.entry_route_position = value.value("entry_route_position",
        static_cast<double>(decision.entry_route_index));
    decision.exit_route_position = value.value("exit_route_position",
        static_cast<double>(decision.exit_route_index));
    decision.has_route_bounds = value.value("has_route_bounds", false);
    decision.reserved_intersection_movement_ids = value.value(
        "reserved_intersection_movement_ids", std::vector<std::string>{});
    decision.reason = value.value("reason", "");
    decision.score = value.value("score", 0.0);
    if (value.contains("breakdown")) {
        const json& breakdown = value.at("breakdown");
        decision.breakdown.headway_urgency =
            breakdown.value("headway_urgency", 0.0);
        decision.breakdown.waiting_time = breakdown.value("waiting_time", 0.0);
        decision.breakdown.fairness_credit =
            breakdown.value("fairness_credit", 0.0);
        decision.breakdown.consecutive_penalty =
            breakdown.value("consecutive_penalty", 0.0);
        decision.breakdown.global_plan_priority =
            breakdown.value("global_plan_priority", 0.0);
    }
    return decision;
}

// 序列化一辆列车的最新监控状态。
json trainToJson(const MonitoredTrain& monitored) {
    const TrainTelemetry& train = monitored.telemetry;
    return {
        {"train_id", train.train_id}, {"line_id", train.line_id},
        {"x", train.x}, {"y", train.y}, {"z", train.z},
        {"speed", train.speed}, {"route_index", train.route_index},
        {"route_position", train.route_position},
        {"travel_direction", travelDirectionName(train.travel_direction)},
        {"simulation_time_ms", train.simulation_time_ms},
        {"received_at_wall_ms", train.received_at_wall_ms},
        {"phase", phaseName(monitored.phase)},
        {"controlled_resource_id", monitored.controlled_resource_id}
    };
}

// 序列化滚动计划，并保留每个资源内的稳定列车顺序。
json rollingPlanToJson(const RollingPlanResult& rolling_plan) {
    json plans = json::array();
    std::vector<std::string> resource_ids;
    resource_ids.reserve(rolling_plan.plans_by_resource.size());
    for (const auto& pair : rolling_plan.plans_by_resource)
        resource_ids.push_back(pair.first);
    std::sort(resource_ids.begin(), resource_ids.end());

    for (const std::string& resource_id : resource_ids) {
        const CorridorRollingPlan& plan =
            rolling_plan.plans_by_resource.at(resource_id);
        json trains = json::array();
        for (const PlannedTrainArrival& train : plan.trains) {
            trains.push_back({
                {"train_id", train.train_id}, {"line_id", train.line_id},
                {"corridor_resource_id", train.corridor_resource_id},
                {"distance_to_entry", train.distance_to_entry},
                {"estimated_arrival_simulation_ms",
                 train.estimated_arrival_simulation_ms},
                {"planned_entry_simulation_ms",
                 train.planned_entry_simulation_ms},
                {"target_headway_ms", train.target_headway_ms},
                {"within_planning_horizon", train.within_planning_horizon},
                {"suggested_rank", train.suggested_rank},
                {"line_distribution_need", train.line_distribution_need},
                {"global_priority", train.global_priority}
            });
        }
        plans.push_back({
            {"corridor_resource_id", plan.corridor_resource_id},
            {"generated_at_simulation_ms", plan.generated_at_simulation_ms},
            {"trains", std::move(trains)}
        });
    }
    return {{"plans", std::move(plans)}};
}

// 从 CMD:<number> 中提取单调序号；非标准 ID 返回 0，不参与序号恢复。
uint64_t commandSequence(const std::string& command_id) {
    constexpr const char* prefix = "CMD:";
    if (command_id.rfind(prefix, 0) != 0) return 0;
    try {
        return std::stoull(command_id.substr(4));
    } catch (...) {
        return 0;
    }
}

// 用 RAII 保证所有返回路径都会把连接归还池中。
class ConnectionLease {
public:
    // 从池中借出一个连接；pool 为空时保持空租约。
    explicit ConnectionLease(DBConnectionPool* pool)
        : pool_(pool), connection_(pool ? pool->getConnection() : nullptr) {}
    // 归还连接；连接池负责回滚残留事务并验证连接健康。
    ~ConnectionLease() {
        if (pool_ && connection_) pool_->releaseConnection(connection_);
    }
    ConnectionLease(const ConnectionLease&) = delete;
    ConnectionLease& operator=(const ConnectionLease&) = delete;
    // 返回底层 MySQL C API 连接，不转移所有权。
    MYSQL* get() const { return connection_; }

private:
    DBConnectionPool* pool_ = nullptr;
    MYSQL* connection_ = nullptr;
};

} // namespace

struct DispatchPersistence::Impl {
    DBConnectionPool* pool = nullptr;
    RedisCache* redis = nullptr;
    DispatchPersistenceConfig config;
    bool mysql_enabled = false;

    mutable std::mutex state_mutex;
    std::atomic<uint64_t> mysql_writes{0};
    std::atomic<uint64_t> mysql_errors{0};
    std::atomic<uint64_t> redis_writes{0};
    std::atomic<uint64_t> redis_errors{0};
    std::atomic<uint64_t> recovered_pending_commands{0};
    std::atomic<uint64_t> recovered_active_commands{0};

    // 幂等创建调度存储表；DDL 只在启动阶段执行。
    bool createSchema(std::string* error) {
        static const std::vector<std::string> statements = {
            "CREATE TABLE IF NOT EXISTS dispatch_topology_snapshots ("
            "revision BIGINT UNSIGNED NOT NULL,"
            "received_at_wall_ms BIGINT UNSIGNED NOT NULL,"
            "route_count INT UNSIGNED NOT NULL,"
            "corridor_count INT UNSIGNED NOT NULL,"
            "intersection_count INT UNSIGNED NOT NULL,"
            "payload_json JSON NOT NULL,"
            "created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),"
            "PRIMARY KEY (revision),"
            "KEY idx_dispatch_topology_created (created_at)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

            "CREATE TABLE IF NOT EXISTS dispatch_train_state ("
            "train_id VARCHAR(128) NOT NULL,"
            "line_id VARCHAR(128) NOT NULL,"
            "x DOUBLE NOT NULL,y DOUBLE NOT NULL,z DOUBLE NOT NULL,"
            "speed DOUBLE NOT NULL,route_index BIGINT UNSIGNED NOT NULL,"
            "travel_direction VARCHAR(16) NOT NULL,"
            "simulation_time_ms BIGINT UNSIGNED NOT NULL,"
            "received_at_wall_ms BIGINT UNSIGNED NOT NULL,"
            "phase VARCHAR(32) NOT NULL,"
            "controlled_resource_id VARCHAR(255) NOT NULL,"
            "is_active TINYINT(1) NOT NULL DEFAULT 1,"
            "updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) "
            "ON UPDATE CURRENT_TIMESTAMP(6),"
            "PRIMARY KEY (train_id),"
            "KEY idx_dispatch_train_line_active (line_id,is_active),"
            "KEY idx_dispatch_train_resource (controlled_resource_id,phase)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

            "CREATE TABLE IF NOT EXISTS dispatch_telemetry_batches ("
            "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "simulation_time_ms BIGINT UNSIGNED NOT NULL,"
            "received_at_wall_ms BIGINT UNSIGNED NOT NULL,"
            "train_count INT UNSIGNED NOT NULL,"
            "payload_json JSON NOT NULL,"
            "created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),"
            "PRIMARY KEY (id),"
            "KEY idx_dispatch_telemetry_simulation (simulation_time_ms,id),"
            "KEY idx_dispatch_telemetry_created (created_at,id)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

            "CREATE TABLE IF NOT EXISTS dispatch_rolling_plans ("
            "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "topology_revision BIGINT UNSIGNED NOT NULL,"
            "generated_at_simulation_ms BIGINT UNSIGNED NOT NULL,"
            "generated_at_wall_ms BIGINT UNSIGNED NOT NULL,"
            "resource_count INT UNSIGNED NOT NULL,"
            "payload_json JSON NOT NULL,"
            "created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),"
            "PRIMARY KEY (id),"
            "KEY idx_dispatch_plan_revision_time "
            "(topology_revision,generated_at_simulation_ms,id),"
            "KEY idx_dispatch_plan_created (created_at,id)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

            "CREATE TABLE IF NOT EXISTS dispatch_commands ("
            "command_id VARCHAR(128) NOT NULL,"
            "command_sequence BIGINT UNSIGNED NOT NULL,"
            "train_id VARCHAR(128) NOT NULL,"
            "corridor_resource_id VARCHAR(255) NOT NULL,"
            "action VARCHAR(32) NOT NULL,"
            "status VARCHAR(32) NOT NULL,"
            "topology_revision BIGINT UNSIGNED NOT NULL,"
            "issued_at_simulation_ms BIGINT UNSIGNED NOT NULL,"
            "issued_at_wall_ms BIGINT UNSIGNED NOT NULL,"
            "acknowledged_at_simulation_ms BIGINT UNSIGNED NULL,"
            "cleared_at_simulation_ms BIGINT UNSIGNED NULL,"
            "payload_json JSON NOT NULL,"
            "created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),"
            "updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) "
            "ON UPDATE CURRENT_TIMESTAMP(6),"
            "PRIMARY KEY (command_id),"
            "UNIQUE KEY uk_dispatch_command_sequence (command_sequence),"
            "KEY idx_dispatch_command_train_status (train_id,status),"
            "KEY idx_dispatch_command_resource_status "
            "(corridor_resource_id,status)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

            "CREATE TABLE IF NOT EXISTS dispatch_command_events ("
            "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "event_key VARCHAR(255) NOT NULL,"
            "command_id VARCHAR(128) NOT NULL,"
            "event_type VARCHAR(32) NOT NULL,"
            "simulation_time_ms BIGINT UNSIGNED NOT NULL,"
            "received_at_wall_ms BIGINT UNSIGNED NOT NULL,"
            "payload_json JSON NOT NULL,"
            "created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),"
            "PRIMARY KEY (id),"
            "UNIQUE KEY uk_dispatch_event_key (event_key),"
            "KEY idx_dispatch_event_command (command_id,id),"
            "CONSTRAINT fk_dispatch_event_command FOREIGN KEY (command_id) "
            "REFERENCES dispatch_commands(command_id)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
        };

        ConnectionLease lease(pool);
        MYSQL* conn = lease.get();
        if (!conn) {
            if (error) *error = "no MySQL connection available";
            return false;
        }
        for (const std::string& statement : statements) {
            if (!execute(conn, statement, error)) return false;
        }
        return true;
    }

    // 记录 Redis 操作结果，不把缓存故障传播为调度事务失败。
    void countRedis(bool ok) {
        if (ok) ++redis_writes;
        else ++redis_errors;
    }
};

DispatchPersistence::DispatchPersistence()
    : impl_(std::make_unique<Impl>()) {}

DispatchPersistence::~DispatchPersistence() {
    shutdown();
}

bool DispatchPersistence::initialize(
    DBConnectionPool* pool,
    RedisCache* redis,
    const DispatchPersistenceConfig& config,
    std::string* error) {
    if (config.realtime_ttl_ms <= 0 || config.rolling_plan_ttl_ms <= 0 ||
        config.command_ttl_ms <= 0 || config.stream_max_len == 0) {
        if (error) *error = "dispatch persistence configuration is invalid";
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    impl_->pool = pool && pool->isInitialized() ? pool : nullptr;
    impl_->redis = redis;
    impl_->config = config;
    impl_->mysql_enabled = false;

    if (impl_->pool && config.auto_migrate) {
        if (!impl_->createSchema(error)) {
            ++impl_->mysql_errors;
            return false;
        }
    }
    impl_->mysql_enabled = impl_->pool != nullptr;
    return impl_->mysql_enabled || (impl_->redis && impl_->redis->isEnabled());
}

bool DispatchPersistence::loadRecoveryState(
    DispatchRecoveryState* state,
    std::string* error) {
    if (!state) {
        if (error) *error = "recovery state output is null";
        return false;
    }
    *state = {};
    state->next_command_sequence = 1;
    if (!impl_->mysql_enabled) return true;

    ConnectionLease lease(impl_->pool);
    MYSQL* conn = lease.get();
    if (!conn) {
        if (error) *error = "no MySQL connection available";
        ++impl_->mysql_errors;
        return false;
    }

    try {
        if (!execute(conn,
            "SELECT revision,payload_json FROM dispatch_topology_snapshots "
            "ORDER BY revision DESC LIMIT 1", error)) {
            ++impl_->mysql_errors;
            return false;
        }
        MYSQL_RES* result = mysql_store_result(conn);
        if (!result) {
            if (error) *error = mysql_error(conn);
            ++impl_->mysql_errors;
            return false;
        }
        MYSQL_ROW row = mysql_fetch_row(result);
        if (row) {
            state->topology_revision = row[0] ? std::stoull(row[0]) : 0;
            const json payload = json::parse(row[1] ? row[1] : "{}");
            for (const json& route : payload.value("routes", json::array()))
                state->routes.push_back(routeFromJson(route));
        }
        mysql_free_result(result);

        // 命令恢复可以独立关闭。关闭时保留数据库历史，不把旧命令重新放入
        // 运行时资源状态；命令序号仍从历史最大值继续，避免主键重复。
        if (impl_->config.command_recovery_enabled) {
            if (!execute(conn,
                "SELECT status,payload_json FROM dispatch_commands "
                "WHERE status IN ('PENDING','ACCEPTED') "
                "ORDER BY command_sequence", error)) {
                ++impl_->mysql_errors;
                return false;
            }
            result = mysql_store_result(conn);
            if (!result) {
                if (error) *error = mysql_error(conn);
                ++impl_->mysql_errors;
                return false;
            }
            while ((row = mysql_fetch_row(result)) != nullptr) {
                const std::string status = row[0] ? row[0] : "";
                DispatchDecision decision = decisionFromJson(
                    json::parse(row[1] ? row[1] : "{}"));
                if (status == "PENDING")
                    state->pending_commands.push_back(std::move(decision));
                else
                    state->active_commands.push_back(std::move(decision));
            }
            mysql_free_result(result);
        }

        if (!execute(conn,
            "SELECT COALESCE(MAX(command_sequence),0) FROM dispatch_commands",
            error)) {
            ++impl_->mysql_errors;
            return false;
        }
        result = mysql_store_result(conn);
        if (!result) {
            if (error) *error = mysql_error(conn);
            ++impl_->mysql_errors;
            return false;
        }
        row = mysql_fetch_row(result);
        if (row && row[0]) state->next_command_sequence = std::stoull(row[0]) + 1;
        mysql_free_result(result);
    } catch (const std::exception& ex) {
        if (error) *error = std::string("invalid recovery data: ") + ex.what();
        ++impl_->mysql_errors;
        return false;
    }

    impl_->recovered_pending_commands = state->pending_commands.size();
    impl_->recovered_active_commands = state->active_commands.size();
    return true;
}

bool DispatchPersistence::persistTopology(
    const LineTopologySnapshot& snapshot,
    uint64_t wall_now_ms,
    std::string* error) {
    json routes = json::array();
    for (const LineRoute& route : snapshot.routes)
        routes.push_back(routeToJson(route));
    const std::string payload = json{{"revision", snapshot.revision},
        {"algorithm_version", snapshot.algorithm_version},
        {"algorithm_fingerprint", snapshot.algorithm_fingerprint},
        {"routes", std::move(routes)}}.dump();

    bool mysql_ok = true;
    if (impl_->mysql_enabled) {
        ConnectionLease lease(impl_->pool);
        MYSQL* conn = lease.get();
        if (!conn) {
            if (error) *error = "no MySQL connection available";
            ++impl_->mysql_errors;
            return false;
        }
        std::ostringstream sql;
        sql << "INSERT INTO dispatch_topology_snapshots "
            << "(revision,received_at_wall_ms,route_count,corridor_count,"
               "intersection_count,payload_json) VALUES ("
            << snapshot.revision << ',' << wall_now_ms << ','
            << snapshot.routes.size() << ','
            << snapshot.analysis.shared_corridors.size() << ','
            << snapshot.analysis.intersections.size() << " ,'"
            << escapeSql(conn, payload) << "') ON DUPLICATE KEY UPDATE "
            << "received_at_wall_ms=VALUES(received_at_wall_ms),"
            << "route_count=VALUES(route_count),"
            << "corridor_count=VALUES(corridor_count),"
            << "intersection_count=VALUES(intersection_count),"
            << "payload_json=VALUES(payload_json)";
        mysql_ok = execute(conn, sql.str(), error);
        if (mysql_ok) ++impl_->mysql_writes;
        else ++impl_->mysql_errors;
    }
    if (!mysql_ok) return false;

    if (impl_->redis && impl_->redis->isEnabled()) {
        impl_->countRedis(impl_->redis->set(
            "realtime:topology", payload, impl_->config.command_ttl_ms));
        impl_->countRedis(impl_->redis->setPersistent(
            "realtime:topology_revision", std::to_string(snapshot.revision)));
        impl_->countRedis(impl_->redis->appendStream(
            "stream:topology", "topology.replaced",
            std::to_string(snapshot.revision), payload,
            impl_->config.stream_max_len).has_value());
    }
    return true;
}

bool DispatchPersistence::persistDispatchCycle(
    const std::vector<MonitoredTrain>& trains,
    const RollingPlanResult& rolling_plan,
    const std::vector<DispatchDecision>& issued_commands,
    uint64_t topology_revision,
    uint64_t simulation_now_ms,
    uint64_t wall_now_ms,
    std::string* error) {
    json train_array = json::array();
    for (const MonitoredTrain& train : trains)
        train_array.push_back(trainToJson(train));
    const std::string train_payload = json{
        {"simulation_time_ms", simulation_now_ms},
        {"received_at_wall_ms", wall_now_ms},
        {"trains", train_array}
    }.dump();
    const std::string plan_payload = rollingPlanToJson(rolling_plan).dump();

    if (impl_->mysql_enabled) {
        ConnectionLease lease(impl_->pool);
        MYSQL* conn = lease.get();
        if (!conn) {
            if (error) *error = "no MySQL connection available";
            ++impl_->mysql_errors;
            return false;
        }
        if (!execute(conn, "START TRANSACTION", error)) {
            ++impl_->mysql_errors;
            return false;
        }
        bool ok = execute(conn, "UPDATE dispatch_train_state SET is_active=0", error);

        if (ok && !trains.empty()) {
            std::ostringstream sql;
            sql.precision(17);
            sql << "INSERT INTO dispatch_train_state "
                << "(train_id,line_id,x,y,z,speed,route_index,travel_direction,"
                   "simulation_time_ms,received_at_wall_ms,phase,"
                   "controlled_resource_id,is_active) VALUES ";
            for (size_t i = 0; i < trains.size(); ++i) {
                if (i != 0) sql << ',';
                const MonitoredTrain& monitored = trains[i];
                const TrainTelemetry& train = monitored.telemetry;
                sql << "('" << escapeSql(conn, train.train_id) << "','"
                    << escapeSql(conn, train.line_id) << "',"
                    << train.x << ',' << train.y << ',' << train.z << ','
                    << train.speed << ',' << train.route_index << ",'"
                    << travelDirectionName(train.travel_direction) << "',"
                    << train.simulation_time_ms << ','
                    << train.received_at_wall_ms << ",'"
                    << phaseName(monitored.phase) << "','"
                    << escapeSql(conn, monitored.controlled_resource_id)
                    << "',1)";
            }
            sql << " ON DUPLICATE KEY UPDATE "
                << "line_id=VALUES(line_id),x=VALUES(x),y=VALUES(y),z=VALUES(z),"
                << "speed=VALUES(speed),route_index=VALUES(route_index),"
                << "travel_direction=VALUES(travel_direction),"
                << "simulation_time_ms=VALUES(simulation_time_ms),"
                << "received_at_wall_ms=VALUES(received_at_wall_ms),"
                << "phase=VALUES(phase),"
                << "controlled_resource_id=VALUES(controlled_resource_id),"
                << "is_active=1";
            ok = execute(conn, sql.str(), error);
        }

        if (ok && impl_->config.telemetry_history_enabled) {
            std::ostringstream sql;
            sql << "INSERT INTO dispatch_telemetry_batches "
                << "(simulation_time_ms,received_at_wall_ms,train_count,"
                   "payload_json) VALUES ("
                << simulation_now_ms << ',' << wall_now_ms << ','
                << trains.size() << ",'" << escapeSql(conn, train_payload)
                << "')";
            ok = execute(conn, sql.str(), error);
        }

        if (ok) {
            std::ostringstream sql;
            sql << "INSERT INTO dispatch_rolling_plans "
                << "(topology_revision,generated_at_simulation_ms,"
                   "generated_at_wall_ms,resource_count,payload_json) VALUES ("
                << topology_revision << ',' << simulation_now_ms << ','
                << wall_now_ms << ',' << rolling_plan.plans_by_resource.size()
                << ",'" << escapeSql(conn, plan_payload) << "')";
            ok = execute(conn, sql.str(), error);
        }

        for (const DispatchDecision& command : issued_commands) {
            if (!ok) break;
            const std::string command_payload = decisionToJson(command).dump();
            std::ostringstream sql;
            sql << "INSERT INTO dispatch_commands "
                << "(command_id,command_sequence,train_id,corridor_resource_id,"
                   "action,status,topology_revision,issued_at_simulation_ms,"
                   "issued_at_wall_ms,payload_json) VALUES ('"
                << escapeSql(conn, command.command_id) << "',"
                << commandSequence(command.command_id) << ",'"
                << escapeSql(conn, command.train_id) << "','"
                << escapeSql(conn, command.corridor_resource_id) << "','"
                << actionName(command.action) << "','PENDING',"
                << command.topology_revision << ','
                << command.issued_at_simulation_ms << ',' << wall_now_ms
                << ",'" << escapeSql(conn, command_payload)
                << "') ON DUPLICATE KEY UPDATE "
                << "payload_json=VALUES(payload_json),updated_at=CURRENT_TIMESTAMP(6)";
            ok = execute(conn, sql.str(), error);
        }

        if (ok) ok = execute(conn, "COMMIT", error);
        if (!ok) {
            std::string ignored;
            execute(conn, "ROLLBACK", &ignored);
            ++impl_->mysql_errors;
            return false;
        }
        ++impl_->mysql_writes;
    }

    if (impl_->redis && impl_->redis->isEnabled()) {
        impl_->countRedis(impl_->redis->set(
            "realtime:trains", train_payload, impl_->config.realtime_ttl_ms));
        for (size_t i = 0; i < trains.size(); ++i) {
            impl_->countRedis(impl_->redis->set(
                "realtime:train:" + trains[i].telemetry.train_id,
                train_array[i].dump(), impl_->config.realtime_ttl_ms));
        }
        impl_->countRedis(impl_->redis->set(
            "realtime:rolling_plan", plan_payload,
            impl_->config.rolling_plan_ttl_ms));
        impl_->countRedis(impl_->redis->appendStream(
            "stream:telemetry", "telemetry.batch",
            std::to_string(simulation_now_ms), train_payload,
            impl_->config.stream_max_len).has_value());

        for (const auto& pair : rolling_plan.plans_by_resource) {
            std::vector<std::pair<double, std::string>> eta;
            eta.reserve(pair.second.trains.size());
            for (const PlannedTrainArrival& train : pair.second.trains) {
                eta.emplace_back(
                    static_cast<double>(train.planned_entry_simulation_ms),
                    train.train_id);
            }
            impl_->countRedis(impl_->redis->replaceSortedSet(
                "eta:" + pair.first, eta, impl_->config.rolling_plan_ttl_ms));
        }
        for (const DispatchDecision& command : issued_commands) {
            const std::string payload = decisionToJson(command).dump();
            impl_->countRedis(impl_->redis->set(
                "realtime:command:" + command.command_id, payload,
                impl_->config.command_ttl_ms));
            impl_->countRedis(impl_->redis->appendStream(
                "stream:dispatch", "command.issued", command.command_id,
                payload, impl_->config.stream_max_len).has_value());
        }
    }
    return true;
}

bool DispatchPersistence::persistCommandAck(
    const DispatchDecision& command,
    bool accepted,
    uint64_t simulation_now_ms,
    uint64_t wall_now_ms,
    std::string* error) {
    const std::string status = accepted ? "ACCEPTED" : "REJECTED";
    const std::string event_type = accepted ? "command.accepted"
                                            : "command.rejected";
    const std::string event_key = "ACK:" + command.command_id + ':' + status;
    const std::string payload = json{
        {"command_id", command.command_id}, {"train_id", command.train_id},
        {"accepted", accepted},
        {"simulation_time_ms", simulation_now_ms}
    }.dump();

    if (impl_->mysql_enabled) {
        ConnectionLease lease(impl_->pool);
        MYSQL* conn = lease.get();
        if (!conn) {
            if (error) *error = "no MySQL connection available";
            ++impl_->mysql_errors;
            return false;
        }
        bool ok = execute(conn, "START TRANSACTION", error);
        if (ok) {
            std::ostringstream sql;
            sql << "UPDATE dispatch_commands SET status='" << status
                << "',acknowledged_at_simulation_ms=" << simulation_now_ms
                << " WHERE command_id='" << escapeSql(conn, command.command_id)
                << "' AND status='PENDING'";
            ok = execute(conn, sql.str(), error);
        }
        if (ok) {
            std::ostringstream sql;
            sql << "INSERT INTO dispatch_command_events "
                << "(event_key,command_id,event_type,simulation_time_ms,"
                   "received_at_wall_ms,payload_json) VALUES ('"
                << escapeSql(conn, event_key) << "','"
                << escapeSql(conn, command.command_id) << "','"
                << event_type << "'," << simulation_now_ms << ','
                << wall_now_ms << ",'" << escapeSql(conn, payload)
                << "') ON DUPLICATE KEY UPDATE event_key=VALUES(event_key)";
            ok = execute(conn, sql.str(), error);
        }
        if (ok) ok = execute(conn, "COMMIT", error);
        if (!ok) {
            std::string ignored;
            execute(conn, "ROLLBACK", &ignored);
            ++impl_->mysql_errors;
            return false;
        }
        ++impl_->mysql_writes;
    }

    if (impl_->redis && impl_->redis->isEnabled()) {
        impl_->countRedis(impl_->redis->set(
            "realtime:command:" + command.command_id, payload,
            impl_->config.command_ttl_ms));
        impl_->countRedis(impl_->redis->appendStream(
            "stream:dispatch", event_type, command.command_id, payload,
            impl_->config.stream_max_len).has_value());
    }
    return true;
}

bool DispatchPersistence::persistResourceCleared(
    const DispatchDecision& command,
    const std::vector<std::string>& intersection_ids,
    uint64_t simulation_now_ms,
    uint64_t wall_now_ms,
    std::string* error) {
    const std::string event_key = "CLEARED:" + command.command_id;
    const std::string payload = json{
        {"command_id", command.command_id}, {"train_id", command.train_id},
        {"corridor_resource_id", command.corridor_resource_id},
        {"intersection_ids", intersection_ids},
        {"simulation_time_ms", simulation_now_ms}
    }.dump();

    if (impl_->mysql_enabled) {
        ConnectionLease lease(impl_->pool);
        MYSQL* conn = lease.get();
        if (!conn) {
            if (error) *error = "no MySQL connection available";
            ++impl_->mysql_errors;
            return false;
        }
        bool ok = execute(conn, "START TRANSACTION", error);
        if (ok) {
            std::ostringstream sql;
            sql << "UPDATE dispatch_commands SET status='CLEARED',"
                << "cleared_at_simulation_ms=" << simulation_now_ms
                << " WHERE command_id='" << escapeSql(conn, command.command_id)
                << "' AND status IN ('ACCEPTED','CLEARED')";
            ok = execute(conn, sql.str(), error);
        }
        if (ok) {
            std::ostringstream sql;
            sql << "INSERT INTO dispatch_command_events "
                << "(event_key,command_id,event_type,simulation_time_ms,"
                   "received_at_wall_ms,payload_json) VALUES ('"
                << escapeSql(conn, event_key) << "','"
                << escapeSql(conn, command.command_id)
                << "','resource.cleared'," << simulation_now_ms << ','
                << wall_now_ms << ",'" << escapeSql(conn, payload)
                << "') ON DUPLICATE KEY UPDATE event_key=VALUES(event_key)";
            ok = execute(conn, sql.str(), error);
        }
        if (ok) ok = execute(conn, "COMMIT", error);
        if (!ok) {
            std::string ignored;
            execute(conn, "ROLLBACK", &ignored);
            ++impl_->mysql_errors;
            return false;
        }
        ++impl_->mysql_writes;
    }

    if (impl_->redis && impl_->redis->isEnabled()) {
        impl_->countRedis(impl_->redis->remove(
            "realtime:command:" + command.command_id));
        impl_->countRedis(impl_->redis->appendStream(
            "stream:dispatch", "resource.cleared", command.command_id,
            payload, impl_->config.stream_max_len).has_value());
    }
    return true;
}

void DispatchPersistence::markTrainStale(const std::string& train_id,
                                         uint64_t wall_now_ms) {
    if (impl_->mysql_enabled) {
        ConnectionLease lease(impl_->pool);
        MYSQL* conn = lease.get();
        if (conn) {
            std::string error;
            std::ostringstream sql;
            sql << "UPDATE dispatch_train_state SET is_active=0 "
                << "WHERE train_id='" << escapeSql(conn, train_id) << "'";
            if (execute(conn, sql.str(), &error)) ++impl_->mysql_writes;
            else ++impl_->mysql_errors;
        } else {
            ++impl_->mysql_errors;
        }
    }
    if (impl_->redis && impl_->redis->isEnabled()) {
        impl_->countRedis(impl_->redis->remove("realtime:train:" + train_id));
        const std::string payload = json{{"train_id", train_id},
            {"received_at_wall_ms", wall_now_ms}}.dump();
        impl_->countRedis(impl_->redis->appendStream(
            "stream:telemetry", "train.stale", train_id, payload,
            impl_->config.stream_max_len).has_value());
    }
}

bool DispatchPersistence::mysqlEnabled() const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->mysql_enabled;
}

bool DispatchPersistence::redisEnabled() const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->redis && impl_->redis->isEnabled();
}

DispatchPersistenceStats DispatchPersistence::stats() const {
    DispatchPersistenceStats result;
    result.mysql_writes = impl_->mysql_writes.load();
    result.mysql_errors = impl_->mysql_errors.load();
    result.redis_writes = impl_->redis_writes.load();
    result.redis_errors = impl_->redis_errors.load();
    result.recovered_pending_commands =
        impl_->recovered_pending_commands.load();
    result.recovered_active_commands = impl_->recovered_active_commands.load();
    result.command_recovery_enabled = impl_->config.command_recovery_enabled;
    return result;
}

void DispatchPersistence::shutdown() {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    impl_->mysql_enabled = false;
    impl_->pool = nullptr;
    impl_->redis = nullptr;
}
