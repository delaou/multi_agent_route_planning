#ifndef CONFIG_H
#define CONFIG_H

#include "DispatchManager.h"
#include "LineRouteAnalyzer.h"
#include "RailwayDispatchService.h"

#include <string>

/** @brief 保存默认配置，并支持 YAML 与命令行覆盖。 */
class Config {
public:
    /** @brief 初始化一套可运行的默认值。 */
    Config();

    // 读取 yaml
    bool loadYaml(const std::string& path);

    // 命令行覆盖配置
    void parseArgs(int argc, char* argv[]);
    /** @brief 组装底层实时调度器配置。 */
    DispatchManagerConfig dispatchManagerConfig() const;
    /** @brief 组装线路几何分析配置。 */
    LineRouteAnalyzerConfig lineRouteAnalyzerConfig() const;
    /** @brief 组装铁路调度编排服务配置。 */
    RailwayDispatchServiceConfig railwayDispatchServiceConfig() const;
    /** @brief 组装统一调度数据层配置。 */
    DispatchPersistenceConfig dispatchPersistenceConfig() const;

public:
    // server
    int port;
    int thread_num;

    int trig_mode;      // 0 LT 1 ET
    int actor_model;    // 0 Reactor 1 Proactor

    int timeout_ms;

    bool daemon;

    // log
    std::string log_level;
    std::string log_path;
    bool log_async;

    // mysql
    std::string mysql_host;
    int mysql_port;
    std::string mysql_user;
    std::string mysql_password;
    std::string mysql_db;
    bool mysql_enabled;
    int mysql_pool_size;
    bool mysql_auto_migrate;
    bool mysql_telemetry_history_enabled;
    bool mysql_command_recovery_enabled;

    // redis response cache (optional and fail-open)
    bool redis_enabled;
    std::string redis_host;
    int redis_port;
    std::string redis_password;
    int redis_database;
    int redis_pool_size;
    int redis_connect_timeout_ms;
    int redis_command_timeout_ms;
    int redis_response_cache_ttl_ms;
    std::string redis_key_prefix;
    int redis_realtime_ttl_ms;
    int redis_rolling_plan_ttl_ms;
    int redis_command_ttl_ms;
    int redis_stream_max_len;

    // train dispatch (simulation time unless explicitly named wall time)
    int dispatch_shared_min_headway_ms;
    int dispatch_intersection_clearance_ms;
    int dispatch_shared_corridor_capacity;
    int dispatch_maximum_wait_ms;
    int dispatch_telemetry_stale_wall_ms;
    int dispatch_max_consecutive_release;
    double dispatch_headway_urgency_weight;
    double dispatch_waiting_time_weight;
    double dispatch_fairness_credit_weight;
    double dispatch_global_plan_weight;
    double dispatch_consecutive_penalty_weight;

    double route_point_merge_tolerance;
    double route_shared_track_tolerance;
    double route_vertical_separation_tolerance;
    double route_shared_corridor_min_length;
    double route_intersection_min_angle_degrees;
    int train_stale_wall_ms;
    double train_approach_distance;
    double train_control_point_distance;
    int rolling_planning_horizon_ms;
    double rolling_minimum_prediction_speed;
    int rolling_reorder_tolerance_ms;
    double rolling_same_line_consecutive_penalty;
};

#endif
