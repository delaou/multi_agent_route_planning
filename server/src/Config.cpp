#include "Config.h"

#include <yaml-cpp/yaml.h>

#include <iostream>
#include <stdexcept>
#include <unistd.h>

Config::Config() {

    // 默认配置

    port = 8080;
    thread_num = 8;

    trig_mode = 1;
    actor_model = 0;

    timeout_ms = 60000;

    daemon = false;

    log_level = "INFO";
    log_path = "./logs";
    log_async = true;

    mysql_host = "localhost";
    mysql_port = 3306;
    mysql_user = "root";
    mysql_password = "";
    mysql_db = "webserver";
    // 找不到配置文件时保持内存模式，避免意外使用 root + 空密码连接数据库。
    mysql_enabled = false;
    mysql_pool_size = 8;
    mysql_auto_migrate = true;
    mysql_telemetry_history_enabled = true;
    mysql_command_recovery_enabled = false;

    redis_enabled = false;
    redis_host = "127.0.0.1";
    redis_port = 6379;
    redis_password = "";
    redis_database = 0;
    redis_pool_size = 4;
    redis_connect_timeout_ms = 100;
    redis_command_timeout_ms = 20;
    redis_response_cache_ttl_ms = 250;
    redis_key_prefix = "multi_agent:";
    redis_realtime_ttl_ms = 15000;
    redis_rolling_plan_ttl_ms = 60000;
    redis_command_ttl_ms = 86400000;
    redis_stream_max_len = 100000;

    dispatch_shared_min_headway_ms = 5000;
    dispatch_intersection_clearance_ms = 2000;
    dispatch_shared_corridor_capacity = 8;
    dispatch_maximum_wait_ms = 20000;
    dispatch_telemetry_stale_wall_ms = 1500;
    dispatch_max_consecutive_release = 2;
    dispatch_headway_urgency_weight = 0.50;
    dispatch_waiting_time_weight = 0.30;
    dispatch_fairness_credit_weight = 0.20;
    dispatch_global_plan_weight = 0.25;
    dispatch_consecutive_penalty_weight = 0.25;
    route_point_merge_tolerance = 5.0;
    route_shared_track_tolerance = 1.25;
    route_vertical_separation_tolerance = 3.0;
    route_shared_corridor_min_length = 60.0;
    route_intersection_min_angle_degrees = 15.0;
    train_stale_wall_ms = 5000;
    train_approach_distance = 2000.0;
    train_control_point_distance = 1000.0;
    rolling_planning_horizon_ms = 300000;
    rolling_minimum_prediction_speed = 5.0;
    rolling_reorder_tolerance_ms = 10000;
    rolling_same_line_consecutive_penalty = 0.25;
}

bool Config::loadYaml(const std::string& path) {

    try {

        YAML::Node config = YAML::LoadFile(path);

        // server
        if(config["server"]) {

            auto server = config["server"];

            if(server["port"])
                port = server["port"].as<int>();

            if(server["thread_num"])
                thread_num = server["thread_num"].as<int>();

            if(server["trig_mode"])
                trig_mode = server["trig_mode"].as<int>();

            if(server["actor_model"])
                actor_model = server["actor_model"].as<int>();

            if(server["timeout_ms"])
                timeout_ms = server["timeout_ms"].as<int>();

            if(server["daemon"])
                daemon = server["daemon"].as<bool>();
        }

        // log
        if(config["log"]) {

            auto log = config["log"];

            if(log["level"])
                log_level = log["level"].as<std::string>();

            if(log["path"])
                log_path = log["path"].as<std::string>();

            if(log["async"])
                log_async = log["async"].as<bool>();
        }

        // mysql
        if(config["mysql"]) {

            auto mysql = config["mysql"];

            if(mysql["host"])
                mysql_host = mysql["host"].as<std::string>();

            if(mysql["port"])
                mysql_port = mysql["port"].as<int>();

            if(mysql["user"])
                mysql_user = mysql["user"].as<std::string>();

            if(mysql["password"])
                mysql_password = mysql["password"].as<std::string>();

            if(mysql["database"])
                mysql_db = mysql["database"].as<std::string>();

            if(mysql["enabled"])
                mysql_enabled = mysql["enabled"].as<bool>();

            if(mysql["pool_size"])
                mysql_pool_size = mysql["pool_size"].as<int>();
            if(mysql["auto_migrate"])
                mysql_auto_migrate = mysql["auto_migrate"].as<bool>();
            if(mysql["telemetry_history_enabled"])
                mysql_telemetry_history_enabled =
                    mysql["telemetry_history_enabled"].as<bool>();
            if(mysql["command_recovery_enabled"])
                mysql_command_recovery_enabled =
                    mysql["command_recovery_enabled"].as<bool>();
        }

        if(config["redis"]) {
            auto redis = config["redis"];
            if(redis["enabled"])
                redis_enabled = redis["enabled"].as<bool>();
            if(redis["host"])
                redis_host = redis["host"].as<std::string>();
            if(redis["port"])
                redis_port = redis["port"].as<int>();
            if(redis["password"])
                redis_password = redis["password"].as<std::string>();
            if(redis["database"])
                redis_database = redis["database"].as<int>();
            if(redis["pool_size"])
                redis_pool_size = redis["pool_size"].as<int>();
            if(redis["connect_timeout_ms"])
                redis_connect_timeout_ms =
                    redis["connect_timeout_ms"].as<int>();
            if(redis["command_timeout_ms"])
                redis_command_timeout_ms =
                    redis["command_timeout_ms"].as<int>();
            if(redis["response_cache_ttl_ms"])
                redis_response_cache_ttl_ms =
                    redis["response_cache_ttl_ms"].as<int>();
            if(redis["key_prefix"])
                redis_key_prefix = redis["key_prefix"].as<std::string>();
            if(redis["realtime_ttl_ms"])
                redis_realtime_ttl_ms = redis["realtime_ttl_ms"].as<int>();
            if(redis["rolling_plan_ttl_ms"])
                redis_rolling_plan_ttl_ms = redis["rolling_plan_ttl_ms"].as<int>();
            if(redis["command_ttl_ms"])
                redis_command_ttl_ms = redis["command_ttl_ms"].as<int>();
            if(redis["stream_max_len"])
                redis_stream_max_len = redis["stream_max_len"].as<int>();

            if(redis_host.empty() || redis_port <= 0 || redis_port > 65535 ||
               redis_database < 0 || redis_pool_size <= 0 ||
               redis_connect_timeout_ms <= 0 ||
               redis_command_timeout_ms <= 0 ||
               redis_response_cache_ttl_ms <= 0 || redis_realtime_ttl_ms <= 0 ||
               redis_rolling_plan_ttl_ms <= 0 || redis_command_ttl_ms <= 0 ||
               redis_stream_max_len <= 0) {
                throw std::invalid_argument("redis configuration is invalid");
            }
        }

        if(config["dispatch"]) {
            auto dispatch = config["dispatch"];
            if(dispatch["shared_min_headway_ms"])
                dispatch_shared_min_headway_ms = dispatch["shared_min_headway_ms"].as<int>();
            if(dispatch["intersection_clearance_ms"])
                dispatch_intersection_clearance_ms =
                    dispatch["intersection_clearance_ms"].as<int>();
            if(dispatch["shared_corridor_capacity"])
                dispatch_shared_corridor_capacity =
                    dispatch["shared_corridor_capacity"].as<int>();
            if(dispatch["maximum_wait_ms"])
                dispatch_maximum_wait_ms = dispatch["maximum_wait_ms"].as<int>();
            if(dispatch["telemetry_stale_wall_ms"])
                dispatch_telemetry_stale_wall_ms = dispatch["telemetry_stale_wall_ms"].as<int>();
            if(dispatch["max_consecutive_release"])
                dispatch_max_consecutive_release = dispatch["max_consecutive_release"].as<int>();
            if(dispatch["weights"]) {
                auto weights = dispatch["weights"];
                if(weights["headway_urgency"])
                    dispatch_headway_urgency_weight = weights["headway_urgency"].as<double>();
                if(weights["waiting_time"])
                    dispatch_waiting_time_weight = weights["waiting_time"].as<double>();
                if(weights["fairness_credit"])
                    dispatch_fairness_credit_weight = weights["fairness_credit"].as<double>();
                if(weights["global_plan"])
                    dispatch_global_plan_weight = weights["global_plan"].as<double>();
                if(weights["consecutive_penalty"])
                    dispatch_consecutive_penalty_weight = weights["consecutive_penalty"].as<double>();
            }

            if(dispatch_shared_min_headway_ms <= 0 ||
               dispatch_intersection_clearance_ms <= 0 ||
               dispatch_shared_corridor_capacity <= 0 ||
               dispatch_maximum_wait_ms <= 0 ||
               dispatch_telemetry_stale_wall_ms <= 0 ||
               dispatch_max_consecutive_release < 0) {
                throw std::invalid_argument("dispatch time and count values are invalid");
            }
        }

        if(config["route_analysis"]) {
            auto analysis = config["route_analysis"];
            if(analysis["point_merge_tolerance"])
                route_point_merge_tolerance =
                    analysis["point_merge_tolerance"].as<double>();
            if(analysis["shared_track_tolerance"])
                route_shared_track_tolerance =
                    analysis["shared_track_tolerance"].as<double>();
            if(analysis["shared_corridor_min_length"])
                route_shared_corridor_min_length =
                    analysis["shared_corridor_min_length"].as<double>();
            if(analysis["vertical_separation_tolerance"])
                route_vertical_separation_tolerance =
                    analysis["vertical_separation_tolerance"].as<double>();
            if(analysis["intersection_min_angle_degrees"])
                route_intersection_min_angle_degrees =
                    analysis["intersection_min_angle_degrees"].as<double>();
            if(route_point_merge_tolerance <= 0.0 ||
               route_shared_track_tolerance <= 0.0 ||
               route_shared_track_tolerance > route_point_merge_tolerance ||
               route_vertical_separation_tolerance <= 0.0 ||
               route_shared_corridor_min_length <= 0.0 ||
               route_intersection_min_angle_degrees <= 0.0 ||
               route_intersection_min_angle_degrees > 90.0) {
                throw std::invalid_argument("route analysis values are invalid");
            }
        }

        if(config["train_monitor"]) {
            auto monitor = config["train_monitor"];
            if(monitor["stale_wall_ms"])
                train_stale_wall_ms = monitor["stale_wall_ms"].as<int>();
            if(monitor["approach_distance"])
                train_approach_distance = monitor["approach_distance"].as<double>();
            if(monitor["control_point_distance"])
                train_control_point_distance =
                    monitor["control_point_distance"].as<double>();
            if(train_stale_wall_ms <= 0 || train_approach_distance <= 0.0 ||
               train_control_point_distance < 0.0 ||
               train_control_point_distance > train_approach_distance) {
                throw std::invalid_argument("train monitor values are invalid");
            }
        }

        if(config["rolling_planner"]) {
            auto planner = config["rolling_planner"];
            if(planner["planning_horizon_ms"])
                rolling_planning_horizon_ms =
                    planner["planning_horizon_ms"].as<int>();
            if(planner["minimum_prediction_speed"])
                rolling_minimum_prediction_speed =
                    planner["minimum_prediction_speed"].as<double>();
            if(planner["reorder_tolerance_ms"])
                rolling_reorder_tolerance_ms =
                    planner["reorder_tolerance_ms"].as<int>();
            if(planner["same_line_consecutive_penalty"])
                rolling_same_line_consecutive_penalty =
                    planner["same_line_consecutive_penalty"].as<double>();
            if(rolling_planning_horizon_ms <= 0 ||
               rolling_minimum_prediction_speed <= 0.0 ||
               rolling_reorder_tolerance_ms <= 0 ||
               rolling_same_line_consecutive_penalty < 0.0) {
                throw std::invalid_argument("rolling planner values are invalid");
            }
        }

    } catch(const std::exception& e) {

        std::cerr << "yaml load error: "
                  << e.what()
                  << std::endl;

        return false;
    }

    return true;
}

DispatchManagerConfig Config::dispatchManagerConfig() const {
    DispatchManagerConfig result;
    result.default_intersection_clearance_ms =
        static_cast<uint64_t>(dispatch_intersection_clearance_ms);
    result.default_corridor_capacity =
        static_cast<size_t>(dispatch_shared_corridor_capacity);
    result.priority.default_corridor_minimum_headway_ms =
        static_cast<uint64_t>(dispatch_shared_min_headway_ms);
    result.priority.maximum_wait_ms =
        static_cast<uint64_t>(dispatch_maximum_wait_ms);
    result.priority.telemetry_stale_wall_ms =
        static_cast<uint64_t>(dispatch_telemetry_stale_wall_ms);
    result.priority.max_consecutive_release =
        static_cast<size_t>(dispatch_max_consecutive_release);
    result.priority.headway_urgency_weight = dispatch_headway_urgency_weight;
    result.priority.waiting_time_weight = dispatch_waiting_time_weight;
    result.priority.fairness_credit_weight = dispatch_fairness_credit_weight;
    result.priority.global_plan_weight = dispatch_global_plan_weight;
    result.priority.consecutive_penalty_weight = dispatch_consecutive_penalty_weight;
    return result;
}

LineRouteAnalyzerConfig Config::lineRouteAnalyzerConfig() const {
    LineRouteAnalyzerConfig result;
    result.point_merge_tolerance = route_point_merge_tolerance;
    result.shared_track_tolerance = route_shared_track_tolerance;
    result.vertical_separation_tolerance =
        route_vertical_separation_tolerance;
    result.shared_corridor_min_length = route_shared_corridor_min_length;
    result.intersection_min_angle_degrees =
        route_intersection_min_angle_degrees;
    return result;
}

RailwayDispatchServiceConfig Config::railwayDispatchServiceConfig() const {
    RailwayDispatchServiceConfig result;
    result.dispatch = dispatchManagerConfig();
    result.route_analysis = lineRouteAnalyzerConfig();
    result.train_stale_wall_ms = static_cast<uint64_t>(train_stale_wall_ms);
    result.candidate_builder.approach_distance = train_approach_distance;
    result.candidate_builder.control_point_distance =
        train_control_point_distance;
    result.rolling_planner.planning_horizon_ms =
        static_cast<uint64_t>(rolling_planning_horizon_ms);
    result.rolling_planner.minimum_prediction_speed =
        rolling_minimum_prediction_speed;
    result.rolling_planner.reorder_tolerance_ms =
        static_cast<uint64_t>(rolling_reorder_tolerance_ms);
    result.rolling_planner.planned_minimum_spacing_ms =
        static_cast<uint64_t>(dispatch_shared_min_headway_ms);
    result.rolling_planner.same_line_consecutive_penalty =
        rolling_same_line_consecutive_penalty;
    return result;
}

DispatchPersistenceConfig Config::dispatchPersistenceConfig() const {
    DispatchPersistenceConfig result;
    result.auto_migrate = mysql_auto_migrate;
    result.telemetry_history_enabled = mysql_telemetry_history_enabled;
    result.command_recovery_enabled = mysql_command_recovery_enabled;
    result.realtime_ttl_ms = redis_realtime_ttl_ms;
    result.rolling_plan_ttl_ms = redis_rolling_plan_ttl_ms;
    result.command_ttl_ms = redis_command_ttl_ms;
    result.stream_max_len = static_cast<size_t>(redis_stream_max_len);
    return result;
}

void Config::parseArgs(int argc, char* argv[]) {

    int opt;

    while((opt = getopt(argc, argv, "p:t:m:a:o:d")) != -1) {

        switch(opt) {

        case 'p':
            port = atoi(optarg);
            break;

        case 't':
            thread_num = atoi(optarg);
            break;

        case 'm':
            trig_mode = atoi(optarg);
            break;

        case 'a':
            actor_model = atoi(optarg);
            break;

        case 'o':
            timeout_ms = atoi(optarg);
            break;

        case 'd':
            daemon = true;
            break;

        default:

            std::cout
                << "Usage:\n"
                << "-p port\n"
                << "-t thread_num\n"
                << "-m trig_mode\n"
                << "-a actor_model\n"
                << "-o timeout_ms\n"
                << "-d daemon\n";
        }
    }
}
