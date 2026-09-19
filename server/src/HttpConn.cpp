#include "HttpConn.h"
#include <sstream>
#include <sys/stat.h>
#include <fcntl.h>
#include <initializer_list>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <iostream>
#include "VehicleManager.h"
#include "RailwayDispatchService.h"
#include "RedisCache.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <charconv>

using json = nlohmann::json;

static VehicleManager& g_vehicle_manager = VehicleManager::instance();
static RailwayDispatchService& g_railway_dispatch =
    RailwayDispatchService::instance();
static RedisCache& g_redis_cache = RedisCache::instance();

namespace {
constexpr const char* kVehiclesCache = "http:vehicles";
constexpr const char* kTrainsCache = "http:trains";
constexpr const char* kTopologyCache = "http:topology";
constexpr const char* kCommandsCache = "http:commands";
constexpr const char* kPlansCache = "http:plans";
}

static const std::string doc_root = "./files";

// ================= 工具函数 =================
bool endsWith(const std::string& str, const std::string& suffix) {
    if (str.size() < suffix.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ================= 构造 =================
HttpConn::HttpConn() {
    state_ = ParseState::REQUEST_LINE;
    keep_alive_ = false;
}

// ================= 主处理 =================
void HttpConn::process() {
    state_ = ParseState::REQUEST_LINE;
    parse();
    if (state_ == ParseState::ERROR) {
        keep_alive_ = false;
        buildJsonResponse(400, "Bad Request", R"({"error":"malformed HTTP request"})");
        return;
    }
    if(state_ != ParseState::FINISH) return;
    buildResponse();
}

static std::string lower(std::string s) {
    for (char& c : s) c = std::tolower(static_cast<unsigned char>(c));
    return s;
}

static bool hasKey(const json& obj, const char* key) {
    return obj.is_object() && obj.find(key) != obj.end();
}

static std::string getRequiredString(const json& obj, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (hasKey(obj, key)) {
            return obj.at(key).get<std::string>();
        }
    }
    throw std::invalid_argument("missing required vehicle id");
}

static double getRequiredNumber(const json& obj, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (hasKey(obj, key)) {
            return obj.at(key).get<double>();
        }
    }
    throw std::invalid_argument("missing required position field");
}

static double getOptionalNumber(const json& primary,
                                const json& fallback,
                                std::initializer_list<const char*> keys,
                                double default_value) {
    for (const char* key : keys) {
        if (hasKey(primary, key)) {
            return primary.at(key).get<double>();
        }
    }

    for (const char* key : keys) {
        if (hasKey(fallback, key)) {
            return fallback.at(key).get<double>();
        }
    }

    return default_value;
}

static std::string getOptionalString(const json& obj,
                                     const char* key,
                                     const std::string& default_value = "") {
    if (hasKey(obj, key)) {
        return obj.at(key).get<std::string>();
    }
    return default_value;
}

static VehicleState parseVehicleState(const json& obj) {
    if (!obj.is_object()) {
        throw std::invalid_argument("vehicle must be an object");
    }

    VehicleState state;
    state.id = getRequiredString(obj, {"id", "vehicleId", "vehicle_id"});
    state.service = getOptionalString(obj, "service", "unknown");
    state.prefab = getOptionalString(obj, "prefab");
    state.status = getOptionalString(obj, "status");

    if (!hasKey(obj, "position") || !obj.at("position").is_object()) {
        throw std::invalid_argument("missing vehicle position");
    }
    const json& position = obj.at("position");
    state.position.x = getRequiredNumber(position, {"x"});
    state.position.y = getOptionalNumber(position, obj, {"y"}, 0.0);
    state.position.z = getRequiredNumber(position, {"z"});
    state.speed = getOptionalNumber(obj, position, {"speed"}, 0.0);
    state.heading = getOptionalNumber(obj, position, {"heading"}, 0.0);

    if (state.id.empty()) {
        throw std::invalid_argument("empty vehicle id");
    }
    return state;
}

static json vehicleToJson(const VehicleState& state) {
    json vehicle;
    vehicle["id"] = state.id;
    vehicle["service"] = state.service;
    vehicle["prefab"] = state.prefab;
    vehicle["status"] = state.status;
    vehicle["position"] = {
        {"x", state.position.x},
        {"y", state.position.y},
        {"z", state.position.z}
    };
    vehicle["speed"] = state.speed;
    vehicle["heading"] = state.heading;
    return vehicle;
}

static uint64_t wallNowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static uint64_t getRequiredUint64(const json& obj, const char* key) {
    if (!hasKey(obj, key))
        throw std::invalid_argument(std::string("missing required field: ") + key);
    return obj.at(key).get<uint64_t>();
}

static LineRoute parseLineRoute(const json& obj) {
    if (!obj.is_object()) throw std::invalid_argument("route must be an object");
    LineRoute route;
    route.line_id = getRequiredString(obj, {"line_id", "lineId"});
    route.line_name = getOptionalString(obj, "line_name",
        getOptionalString(obj, "lineName"));
    route.transport_type = getOptionalString(obj, "transport_type", "unknown");
    route.geometry_source = getOptionalString(obj, "geometry_source", "unknown");
    route.is_loop = obj.value("is_loop", false);
    if (hasKey(obj, "target_headway_ms"))
        route.target_headway_ms = obj.at("target_headway_ms").get<uint64_t>();
    if (!hasKey(obj, "points") || !obj.at("points").is_array())
        throw std::invalid_argument("route points must be an array");
    for (const auto& item : obj.at("points")) {
        if (!item.is_object())
            throw std::invalid_argument("route point must be an object");
        LineRoutePoint point;
        point.point_id = getOptionalString(item, "point_id",
            getOptionalString(item, "id"));
        point.track_segment_id = getOptionalString(item, "track_segment_id");
        point.x = getRequiredNumber(item, {"x"});
        point.y = getOptionalNumber(item, item, {"y"}, 0.0);
        point.z = getRequiredNumber(item, {"z"});
        route.points.push_back(std::move(point));
    }
    return route;
}

static TrainTelemetry parseTrainTelemetry(const json& obj,
                                          uint64_t simulation_time_ms,
                                          uint64_t received_at_wall_ms) {
    if (!obj.is_object()) throw std::invalid_argument("train must be an object");
    TrainTelemetry train;
    train.train_id = getRequiredString(obj, {"train_id", "trainId", "id"});
    train.line_id = getRequiredString(obj, {"line_id", "lineId"});
    if (!hasKey(obj, "position") || !obj.at("position").is_object())
        throw std::invalid_argument("train position must be an object");
    const json& position = obj.at("position");
    train.x = getRequiredNumber(position, {"x"});
    train.y = getOptionalNumber(position, obj, {"y"}, 0.0);
    train.z = getRequiredNumber(position, {"z"});
    train.speed = getOptionalNumber(obj, position, {"speed"}, 0.0);
    train.route_index = static_cast<size_t>(getRequiredUint64(obj, "route_index"));
    train.route_position = getOptionalNumber(
        obj, obj, {"route_position"}, static_cast<double>(train.route_index));
    const std::string direction = lower(getOptionalString(
        obj, "travel_direction", "forward"));
    if (direction == "forward") {
        train.travel_direction = RouteTravelDirection::FORWARD;
    } else if (direction == "reverse") {
        train.travel_direction = RouteTravelDirection::REVERSE;
    } else {
        throw std::invalid_argument("travel_direction must be forward or reverse");
    }
    train.simulation_time_ms = simulation_time_ms;
    train.received_at_wall_ms = received_at_wall_ms;
    return train;
}

static const char* actionName(DispatchAction action) {
    switch (action) {
    case DispatchAction::HOLD_ALL: return "hold_all";
    case DispatchAction::RELEASE: return "release";
    default: return "no_action";
    }
}

static const char* phaseName(TrainDispatchPhase phase) {
    switch (phase) {
    case TrainDispatchPhase::APPROACHING: return "approaching";
    case TrainDispatchPhase::WAITING_PERMISSION: return "waiting_permission";
    case TrainDispatchPhase::RELEASE_PENDING: return "release_pending";
    case TrainDispatchPhase::INSIDE_RESOURCE: return "inside_resource";
    default: return "monitored";
    }
}

static json decisionToJson(const DispatchDecision& decision) {
    return json{
        {"command_id", decision.command_id},
        {"action", actionName(decision.action)},
        {"corridor_resource_id", decision.corridor_resource_id},
        {"corridor_id", decision.corridor_id},
        {"direction", decision.direction == CorridorDirection::FORWARD
            ? "forward" : "reverse"},
        {"train_id", decision.train_id},
        {"line_id", decision.line_id},
        {"topology_revision", decision.topology_revision},
        {"issued_at_simulation_ms", decision.issued_at_simulation_ms},
        {"has_route_bounds", decision.has_route_bounds},
        {"entry_route_index", decision.has_route_bounds
            ? json(decision.entry_route_index) : json(-1)},
        {"exit_route_index", decision.has_route_bounds
            ? json(decision.exit_route_index) : json(-1)},
        {"entry_route_position", decision.has_route_bounds
            ? json(decision.entry_route_position) : json(-1.0)},
        {"exit_route_position", decision.has_route_bounds
            ? json(decision.exit_route_position) : json(-1.0)},
        {"required_intersection_movement_ids",
            decision.reserved_intersection_movement_ids},
        {"reason", decision.reason},
        {"score", decision.score},
        {"score_breakdown", {
            {"headway_urgency", decision.breakdown.headway_urgency},
            {"waiting_time", decision.breakdown.waiting_time},
            {"fairness_credit", decision.breakdown.fairness_credit},
            {"global_plan_priority", decision.breakdown.global_plan_priority},
            {"consecutive_penalty", decision.breakdown.consecutive_penalty}
        }}
    };
}

static json routeToJson(const LineRoute& route) {
    json item;
    item["line_id"] = route.line_id;
    item["line_name"] = route.line_name;
    item["transport_type"] = route.transport_type;
    item["geometry_source"] = route.geometry_source;
    item["is_loop"] = route.is_loop;
    item["target_headway_ms"] = route.target_headway_ms;
    item["points"] = json::array();
    for (const LineRoutePoint& point : route.points) {
        item["points"].push_back({
            {"id", point.point_id},
            {"track_segment_id", point.track_segment_id},
            {"x", point.x},
            {"y", point.y},
            {"z", point.z}
        });
    }
    return item;
}

static const char* relationName(RouteDirectionRelation relation) {
    return relation == RouteDirectionRelation::SAME ? "same" : "opposite";
}

static json corridorToJson(const SharedCorridorDefinition& corridor) {
    json item;
    item["corridor_id"] = corridor.corridor_id;
    item["start"] = {
        {"x", corridor.start_x},
        {"y", corridor.start_y},
        {"z", corridor.start_z}
    };
    item["end"] = {
        {"x", corridor.end_x},
        {"y", corridor.end_y},
        {"z", corridor.end_z}
    };
    item["length"] = corridor.length;
    item["geometry"] = json::array();
    for (const LineRoutePoint& point : corridor.geometry) {
        item["geometry"].push_back({{"x", point.x}, {"y", point.y}, {"z", point.z}});
    }
    item["members"] = json::array();
    for (const SharedCorridorMember& member : corridor.members) {
        item["members"].push_back({
            {"line_id", member.line_id},
            {"start_segment", member.start_segment},
            {"end_segment", member.end_segment},
            {"start_position", member.start_position},
            {"end_position", member.end_position},
            {"occurrence_id", member.occurrence_id},
            {"direction_relation", relationName(member.direction_relation)}
        });
    }
    return item;
}

static json intersectionToJson(const LineIntersectionDefinition& intersection) {
    return json{
        {"intersection_id", intersection.intersection_id},
        {"position", {
            {"x", intersection.x},
            {"y", intersection.y},
            {"z", intersection.z}
        }},
        {"line_a_id", intersection.line_a_id},
        {"line_b_id", intersection.line_b_id},
        {"line_a_segment", intersection.line_a_segment},
        {"line_b_segment", intersection.line_b_segment},
        {"line_a_movement_id", intersection.line_a_movement_id},
        {"line_b_movement_id", intersection.line_b_movement_id}
    };
}

static json movementToJson(const LineIntersectionMovement& movement) {
    return json{
        {"movement_id", movement.movement_id},
        {"intersection_id", movement.intersection_id},
        {"line_id", movement.line_id},
        {"incoming_route_segment_id", movement.incoming_route_segment_id},
        {"outgoing_route_segment_id", movement.outgoing_route_segment_id},
        {"conflict_movement_ids", movement.conflict_movement_ids},
        {"clearance_time_ms", movement.clearance_time_ms}
    };
}

// ================= 解析 =================

void HttpConn::parse() {
    std::string data(read_buf_.data(), read_idx_);

    size_t pos = data.find("\r\n\r\n");
    if (pos == std::string::npos) {
        return;
    }
    std::string header_part = data.substr(0, pos);

    std::istringstream stream(header_part);
    std::string line;

    header_r_.clear();
    body_r_.clear();

    // HTTP/1.1 默认 keep-alive
    keep_alive_ = true;
    content_length_ = 0;

    while (1) {
        if(!std::getline(stream, line)){
            if(content_length_ == 0){
                state_ = ParseState::FINISH;
            }
            else{
                state_ = ParseState::BODY;
            }
            break;
        }

        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (state_ == ParseState::REQUEST_LINE) {
            if (!parseRequestLine(line)) {
                state_ = ParseState::ERROR;
                return;
            }
            state_ = ParseState::HEADERS;

            if (version_ == "HTTP/1.0") {
                keep_alive_ = false;
            }
        }
        else if (state_ == ParseState::HEADERS) {
            if (!parseHeader(line)) {
                state_ = ParseState::ERROR;
                return;
            }
        }
    }

    if(state_ == ParseState::BODY) {
        size_t body_start = pos + 4;
        if(data.size() < body_start + content_length_) {
            // TCP 不保留 HTTP 消息边界，大请求分多次到达是正常情况。读缓冲
            // 会继续保留，后续 EPOLLIN 到来后再从完整缓冲重新解析。
            return;
        }
        body_r_ = data.substr(body_start, content_length_);
    }
    state_ = ParseState::FINISH;
}

bool HttpConn::parseHeader(const std::string& line) {
    size_t pos = line.find(":");
    if (pos == std::string::npos) return false;

    std::string key = lower(line.substr(0, pos));
    std::string value = line.substr(pos + 1);

    while (!value.empty() && value[0] == ' ')
        value.erase(value.begin());

    value = lower(value);
    header_r_[key] = value;

    if (key == "connection") {
        if (value == "close") {
            keep_alive_ = false;
        } else if (value == "keep-alive") {
            keep_alive_ = true;
        }
    }

    else if(key == "content-length") {
        size_t parsed = 0;
        const char* first = value.data();
        const char* last = first + value.size();
        auto result = std::from_chars(first, last, parsed);
        if (result.ec != std::errc() || result.ptr != last ||
            parsed > MAX_READ_BUFFER_SIZE) {
            return false;
        }
        content_length_ = parsed;
    }
    return true;
}

bool HttpConn::parseRequestLine(const std::string& line) {
    std::istringstream ss(line);
    ss >> method_ >> path_ >> version_;
    std::string trailing;
    if (method_.empty() || path_.empty() ||
        (version_ != "HTTP/1.0" && version_ != "HTTP/1.1") ||
        (ss >> trailing)) {
        return false;
    }
    return true;
}

// ================= 路径安全处理 =================
std::string HttpConn::safePath(const std::string& path) {
    if (path.find("..") != std::string::npos) {
        return doc_root + "/403.html";
    }

    if (path == "/") {
        return doc_root + "/index.html";
    }

    return doc_root + path;
}

// ================= 核心：构建响应 =================

void HttpConn::buildResponse() {
    if (handleAPI()) {
        return;
    }

    if (handleGet()) {
        return;
    }

    buildFileResponse();
}

void HttpConn::buildJsonResponse(int code, const std::string& status, const std::string& body) {
    std::string header;

    header += "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n";
    header += "Content-Type: application/json\r\n";
    header += "Content-Length: " + std::to_string(body.size()) + "\r\n";

    if (keep_alive_) {
        header += "Connection: keep-alive\r\n";
    } else {
        header += "Connection: close\r\n";
    }

    header += "\r\n";

    response_ = std::make_unique<MemoryResponse>(header, body);
}

bool HttpConn::tryBuildCachedJsonResponse(const char* key) {
    if (!key) return false;
    const auto cached = g_redis_cache.get(key);
    if (!cached.has_value()) return false;
    buildJsonResponse(200, "OK", *cached);
    return true;
}

void HttpConn::cacheJsonResponse(const char* key, const std::string& body) {
    if (key) g_redis_cache.set(key, body);
}

void HttpConn::invalidateRedisCaches(
    std::initializer_list<const char*> keys) {
    g_redis_cache.remove(keys);
}

bool HttpConn::handleAPI() {
    if (method_ == "POST" && path_ == "/api/game/vehicles/snapshot") {
        return handleGameVehicleSnapshot();
    }
    if (method_ == "POST" && path_ == "/api/game/trains/snapshot")
        return handleTrainSnapshot();
    if (method_ == "POST" && path_ == "/api/game/lines/snapshot")
        return handleLineRouteSnapshot();
    if (method_ == "POST" && path_ == "/api/game/dispatch/commands/query")
        return handleDispatchCommandQuery();
    if (method_ == "POST" && path_ == "/api/game/dispatch/commands/ack")
        return handleDispatchCommandAck();
    if (method_ == "POST" && path_ == "/api/game/dispatch/resources/cleared")
        return handleDispatchResourceCleared();

    if (path_.find("/api/vehicles") != 0) {
        return false;
    }

    if (method_ == "GET") {
        return false;
    }

    if (method_ == "POST") {
        if (path_ == "/api/vehicles/register") {
            return handleVehicleRegister();
        }

        if (path_ == "/api/vehicles/update") {
            return handleVehicleUpdate();
        }
    }

    if (method_ == "DELETE") {
        return handleVehicleDelete();
    }

    buildJsonResponse(404, "Not Found", R"({"ok":false,"error":"unknown api"})");
    return true;
}

bool HttpConn::handleLineRouteSnapshot() {
    // POST /api/game/lines/snapshot
    // {"routes":[{"line_id":"A","target_headway_ms":15000,
    //              "points":[{"x":0,"y":0,"z":0}, ...]}]}
    // 这是低频全量接口：地图加载或线路改变后调用，成功后重建几何索引。
    try {
        const json req = json::parse(body_r_);
        if (!req.is_object() || !hasKey(req, "routes") ||
            !req.at("routes").is_array()) {
            throw std::invalid_argument("routes must be an array");
        }
        if (req.at("routes").size() > 4096)
            throw std::invalid_argument("too many routes");
        std::vector<LineRoute> routes;
        routes.reserve(req.at("routes").size());
        for (const auto& item : req.at("routes"))
            routes.push_back(parseLineRoute(item));

        RouteUploadResult uploaded;
        std::string reason;
        if (!g_railway_dispatch.replaceRoutes(
                routes, wallNowMs(), &uploaded, &reason)) {
            throw std::invalid_argument(reason.empty()
                ? "route topology rejected" : reason);
        }
        invalidateRedisCaches({kTopologyCache, kTrainsCache,
                               kCommandsCache, kPlansCache});
        const int status = uploaded.persistence_ok ? 200 : 503;
        buildJsonResponse(status,
            uploaded.persistence_ok ? "OK" : "Service Unavailable", json{
            {"ok", uploaded.persistence_ok}, {"route_count", uploaded.route_count},
            {"corridor_count", uploaded.corridor_count},
            {"intersection_count", uploaded.intersection_count},
            {"persistence_ok", uploaded.persistence_ok},
            {"persistence_error", uploaded.persistence_error}
        }.dump());
    } catch (const std::exception& e) {
        buildJsonResponse(400, "Bad Request",
            json{{"ok", false}, {"error", e.what()}}.dump());
    }
    return true;
}

bool HttpConn::handleTrainSnapshot() {
    // POST /api/game/trains/snapshot
    // {"simulation_time_ms":10000,"trains":[{"train_id":"T1",
    //   "line_id":"A","route_index":3,"travel_direction":"forward",
    //   "position":{"x":0,"y":0,"z":0},"speed":12.0}]}
    // received_at_wall_ms 必须由服务器落时间，避免 Mod 和服务器时钟不一致。
    try {
        const json req = json::parse(body_r_);
        if (!req.is_object() || !hasKey(req, "trains") ||
            !req.at("trains").is_array()) {
            throw std::invalid_argument("trains must be an array");
        }
        if (req.at("trains").size() > 65535)
            throw std::invalid_argument("too many trains");
        const uint64_t simulation_now_ms =
            getRequiredUint64(req, "simulation_time_ms");
        const uint64_t received_at_wall_ms = wallNowMs();
        std::vector<TrainTelemetry> trains;
        trains.reserve(req.at("trains").size());
        for (const auto& item : req.at("trains")) {
            trains.push_back(parseTrainTelemetry(
                item, simulation_now_ms, received_at_wall_ms));
        }
        const TrainBatchUpdateResult updated = g_railway_dispatch.updateTrains(
            trains, simulation_now_ms, received_at_wall_ms);
        invalidateRedisCaches(
            {kTrainsCache, kCommandsCache, kPlansCache});
        const int status = updated.persistence_ok ? 200 : 503;
        buildJsonResponse(status,
            updated.persistence_ok ? "OK" : "Service Unavailable", json{
            {"ok", updated.persistence_ok}, {"accepted", updated.accepted},
            {"ignored_out_of_order", updated.ignored_out_of_order},
            {"invalid", updated.invalid},
            {"issued_commands", updated.issued_commands},
            {"persistence_ok", updated.persistence_ok},
            {"persistence_error", updated.persistence_error},
            {"monitored_train_count", g_railway_dispatch.trains().size()}
        }.dump());
    } catch (const std::exception& e) {
        buildJsonResponse(400, "Bad Request",
            json{{"ok", false}, {"error", e.what()}}.dump());
    }
    return true;
}

bool HttpConn::handleDispatchCommandQuery() {
    // 查询是幂等的：同一 RELEASE 在 ACK 前会反复返回，游戏端可安全重试。
    try {
        g_railway_dispatch.runMaintenance(wallNowMs());
        const json req = body_r_.empty() ? json::object() : json::parse(body_r_);
        std::optional<std::string> train_id;
        if (hasKey(req, "train_id")) train_id = req.at("train_id").get<std::string>();
        const auto commands = g_railway_dispatch.pendingCommands(train_id);
        json response = {{"ok", true}, {"commands", json::array()}};
        for (const DispatchDecision& command : commands)
            response["commands"].push_back(decisionToJson(command));
        response["command_count"] = commands.size();
        buildJsonResponse(200, "OK", response.dump());
    } catch (const std::exception& e) {
        buildJsonResponse(400, "Bad Request",
            json{{"ok", false}, {"error", e.what()}}.dump());
    }
    return true;
}

bool HttpConn::handleDispatchCommandAck() {
    // accepted=true 表示列车实际开始进入；false 表示执行失败并释放预预约。
    try {
        const json req = json::parse(body_r_);
        const std::string train_id = getRequiredString(req, {"train_id"});
        const std::string resource_id =
            getRequiredString(req, {"corridor_resource_id"});
        if (!hasKey(req, "accepted"))
            throw std::invalid_argument("missing required field: accepted");
        const bool accepted = req.at("accepted").get<bool>();
        const std::string command_id = getOptionalString(req, "command_id");
        const uint64_t simulation_now_ms =
            getRequiredUint64(req, "simulation_time_ms");
        if (!g_railway_dispatch.acknowledge(
                resource_id, train_id, accepted, simulation_now_ms,
                command_id)) {
            buildJsonResponse(409, "Conflict", json{
                {"ok", false}, {"error", "command not pending or ACK mismatch"}
            }.dump());
            return true;
        }
        invalidateRedisCaches({kTrainsCache, kCommandsCache, kPlansCache});
        buildJsonResponse(200, "OK", json{{"ok", true}}.dump());
    } catch (const std::exception& e) {
        buildJsonResponse(400, "Bad Request",
            json{{"ok", false}, {"error", e.what()}}.dump());
    }
    return true;
}

bool HttpConn::handleDispatchResourceCleared() {
    // 只有车尾完全离开后才能调用。intersection_ids 可省略，服务会根据
    // ACK 时保存的 movement 自动找到对应交叉点并开始 2 秒清空计时。
    try {
        const json req = json::parse(body_r_);
        const std::string train_id = getRequiredString(req, {"train_id"});
        const std::string resource_id = getOptionalString(
            req, "corridor_resource_id");
        const uint64_t simulation_now_ms =
            getRequiredUint64(req, "simulation_time_ms");
        std::vector<std::string> intersections;
        if (hasKey(req, "intersection_ids")) {
            if (!req.at("intersection_ids").is_array())
                throw std::invalid_argument("intersection_ids must be an array");
            intersections = req.at("intersection_ids").get<std::vector<std::string>>();
        }
        if (!g_railway_dispatch.markCleared(
                train_id, resource_id, intersections, simulation_now_ms)) {
            buildJsonResponse(404, "Not Found", json{
                {"ok", false}, {"error", "no matching occupied resource"}
            }.dump());
            return true;
        }
        invalidateRedisCaches({kTrainsCache, kCommandsCache, kPlansCache});
        buildJsonResponse(200, "OK", json{{"ok", true}}.dump());
    } catch (const std::exception& e) {
        buildJsonResponse(400, "Bad Request",
            json{{"ok", false}, {"error", e.what()}}.dump());
    }
    return true;
}

bool HttpConn::handleGameVehicleSnapshot() {
    try {
        json req = json::parse(body_r_);
        if (!req.is_object()) {
            throw std::invalid_argument("snapshot must be an object");
        }
        if (!hasKey(req, "vehicles") || !req.at("vehicles").is_array()) {
            throw std::invalid_argument("vehicles must be an array");
        }

        VehicleSnapshot snapshot;
        if (hasKey(req, "timestamp")) {
            snapshot.timestamp = req.at("timestamp").get<uint64_t>();
        }

        if (hasKey(req, "map")) {
            const json& map = req.at("map");
            if (!map.is_object()) {
                throw std::invalid_argument("map must be an object");
            }
            snapshot.map.min_x = getRequiredNumber(map, {"min_x"});
            snapshot.map.max_x = getRequiredNumber(map, {"max_x"});
            snapshot.map.min_z = getRequiredNumber(map, {"min_z"});
            snapshot.map.max_z = getRequiredNumber(map, {"max_z"});
            if (snapshot.map.min_x >= snapshot.map.max_x ||
                snapshot.map.min_z >= snapshot.map.max_z) {
                throw std::invalid_argument("invalid map bounds");
            }
        }

        const json& vehicles = req.at("vehicles");
        if (vehicles.size() > 65535) {
            throw std::invalid_argument("too many vehicles");
        }
        snapshot.vehicles.reserve(vehicles.size());
        for (const auto& item : vehicles) {
            snapshot.vehicles.push_back(parseVehicleState(item));
        }

        const size_t vehicle_count = snapshot.vehicles.size();
        const uint64_t timestamp = snapshot.timestamp;
        g_vehicle_manager.replaceSnapshot(std::move(snapshot));
        invalidateRedisCaches({kVehiclesCache});

        json res;
        res["ok"] = true;
        res["vehicle_count"] = vehicle_count;
        res["timestamp"] = timestamp;
        buildJsonResponse(200, "OK", res.dump());
        return true;
    }
    catch (const std::exception& e) {
        json res;
        res["ok"] = false;
        res["error"] = e.what();
        buildJsonResponse(400, "Bad Request", res.dump());
        return true;
    }
}

bool HttpConn::handleVehicleRegister() {
    try {
        std::string body = body_r_;
        json req = json::parse(body);

        VehicleState state = parseVehicleState(req);

        if (state.id.empty()) {
            buildJsonResponse(400, "Bad Request", R"({"ok":false,"error":"empty vehicle id"})");
            return true;
        }
        if (g_vehicle_manager.hasVehicle(state.id)) {
            buildJsonResponse(409, "Conflict", R"({"ok":false,"error":"vehicle already exists"})");
            return true;
        }
        if (!g_vehicle_manager.registerVehicle(state)) {
            buildJsonResponse(409, "Conflict",
                              R"({"ok":false,"error":"vehicle already exists"})");
            return true;
        }

        invalidateRedisCaches({kVehiclesCache});

        buildJsonResponse(201, "Created", R"({"ok":true})");
        return true;
    }
    catch (const std::exception& e) {
        json res;
        res["ok"] = false;
        res["error"] = e.what();

        buildJsonResponse(400, "Bad Request", res.dump());
        return true;
    }
}

bool HttpConn::handleVehicleUpdate() {
    try {
        std::string body = body_r_;
        json req = json::parse(body);

        VehicleState state = parseVehicleState(req);

        if (state.id.empty()) {
            buildJsonResponse(400, "Bad Request", R"({"ok":false,"error":"empty vehicle id"})");
            return true;
        }

        if (!g_vehicle_manager.hasVehicle(state.id)) {
            buildJsonResponse(404, "Not Found", R"({"ok":false,"error":"vehicle not registered"})");
            return true;
        }
        if (!g_vehicle_manager.updateVehicle(state)) {
            buildJsonResponse(404, "Not Found",
                              R"({"ok":false,"error":"vehicle not registered"})");
            return true;
        }

        invalidateRedisCaches({kVehiclesCache});

        buildJsonResponse(200, "OK", R"({"ok":true})");
        return true;
    }
    catch (const std::exception& e) {
        json res;
        res["ok"] = false;
        res["error"] = e.what();

        buildJsonResponse(400, "Bad Request", res.dump());
        return true;
    }
}

bool HttpConn::handleVehicleDelete() {
    const std::string prefix = "/api/vehicles/";

    if (path_.find(prefix) != 0 || path_.size() <= prefix.size()) {
        buildJsonResponse(400, "Bad Request", R"({"ok":false,"error":"missing vehicle id"})");
        return true;
    }

    std::string id = path_.substr(prefix.size());

    if (!g_vehicle_manager.hasVehicle(id)) {
        buildJsonResponse(404, "Not Found", R"({"ok":false,"error":"vehicle not found"})");
        return true;
    }

    if (!g_vehicle_manager.removeVehicle(id)) {
        buildJsonResponse(404, "Not Found",
                          R"({"ok":false,"error":"vehicle not found"})");
        return true;
    }

    invalidateRedisCaches({kVehiclesCache});

    buildJsonResponse(200, "OK", R"({"ok":true})");
    return true;
}

bool HttpConn::handleGet() {
    if (method_ != "GET") {
        return false;
    }

    if (path_ == "/api/system/cache" || path_ == "/api/system/cache/") {
        const RedisCacheStats stats = g_redis_cache.stats();
        buildJsonResponse(200, "OK", json{
            {"ok", true},
            {"redis_compiled", g_redis_cache.isCompiledWithRedis()},
            {"redis_enabled", g_redis_cache.isEnabled()},
            {"hits", stats.hits},
            {"misses", stats.misses},
            {"writes", stats.writes},
            {"errors", stats.errors},
            {"bypasses", stats.bypasses}
        }.dump());
        return true;
    }

    // 汇总可信 MySQL 写入与可降级 Redis 写入，便于监控数据层健康度。
    if (path_ == "/api/system/storage" || path_ == "/api/system/storage/") {
        std::string last_error;
        const bool healthy =
            g_railway_dispatch.persistenceHealthy(&last_error);
        const DispatchPersistenceStats stats =
            g_railway_dispatch.persistenceStats();
        buildJsonResponse(healthy ? 200 : 503,
            healthy ? "OK" : "Service Unavailable", json{
            {"ok", healthy}, {"last_error", last_error},
            {"mysql_writes", stats.mysql_writes},
            {"mysql_errors", stats.mysql_errors},
            {"redis_writes", stats.redis_writes},
            {"redis_errors", stats.redis_errors},
            {"command_recovery_enabled", stats.command_recovery_enabled},
            {"recovered_pending_commands", stats.recovered_pending_commands},
            {"recovered_active_commands", stats.recovered_active_commands}
        }.dump());
        return true;
    }

    if (path_ == "/api/game/trains" || path_ == "/api/game/trains/") {
        if (tryBuildCachedJsonResponse(kTrainsCache)) return true;
        const auto trains = g_railway_dispatch.trains();
        json res = {{"ok", true}, {"train_count", trains.size()},
                    {"trains", json::array()}};
        for (const MonitoredTrain& monitored : trains) {
            const TrainTelemetry& train = monitored.telemetry;
            res["trains"].push_back({
                {"train_id", train.train_id}, {"line_id", train.line_id},
                {"position", {{"x", train.x}, {"y", train.y}, {"z", train.z}}},
                {"speed", train.speed}, {"route_index", train.route_index},
                {"route_position", train.route_position},
                {"travel_direction",
                    train.travel_direction == RouteTravelDirection::FORWARD
                        ? "forward" : "reverse"},
                {"simulation_time_ms", train.simulation_time_ms},
                {"phase", phaseName(monitored.phase)},
                {"controlled_resource_id", monitored.controlled_resource_id}
            });
        }
        const std::string body = res.dump();
        cacheJsonResponse(kTrainsCache, body);
        buildJsonResponse(200, "OK", body);
        return true;
    }

    if (path_ == "/api/game/status" || path_ == "/api/game/status/") {
        const RailwayRuntimeStatus status =
            g_railway_dispatch.runtimeStatus(wallNowMs());
        buildJsonResponse(200, "OK", json{
            {"ok", true},
            {"telemetry_connected", status.telemetry_connected},
            {"last_train_snapshot_wall_ms", status.last_train_snapshot_wall_ms},
            {"train_snapshot_age_ms", status.train_snapshot_age_ms},
            {"train_count", status.train_count},
            {"topology_revision", status.topology.revision},
            {"topology_algorithm_version", status.topology.algorithm_version},
            {"topology_algorithm_fingerprint", status.topology.algorithm_fingerprint},
            {"route_count", status.topology.route_count},
            {"corridor_count", status.topology.corridor_count},
            {"intersection_count", status.topology.intersection_count},
            {"movement_count", status.topology.movement_count}
        }.dump());
        return true;
    }

    if (path_ == "/api/game/topology" || path_ == "/api/game/topology/" ||
        path_ == "/api/game/lines" || path_ == "/api/game/lines/" ||
        path_ == "/api/game/resources" || path_ == "/api/game/resources/") {
        if (tryBuildCachedJsonResponse(kTopologyCache)) return true;
        const LineTopologySnapshot topology = g_railway_dispatch.topology();
        json res = {
            {"ok", true},
            {"topology_revision", topology.revision},
            {"topology_algorithm_version", topology.algorithm_version},
            {"topology_algorithm_fingerprint", topology.algorithm_fingerprint},
            {"route_count", topology.routes.size()},
            {"corridor_count", topology.analysis.shared_corridors.size()},
            {"intersection_count", topology.analysis.intersections.size()},
            {"movement_count", topology.analysis.movements.size()},
            {"routes", json::array()},
            {"corridors", json::array()},
            {"intersections", json::array()},
            {"movements", json::array()}
        };

        for (const LineRoute& route : topology.routes) {
            res["routes"].push_back(routeToJson(route));
        }
        for (const SharedCorridorDefinition& corridor :
             topology.analysis.shared_corridors) {
            res["corridors"].push_back(corridorToJson(corridor));
        }
        for (const LineIntersectionDefinition& intersection :
             topology.analysis.intersections) {
            res["intersections"].push_back(intersectionToJson(intersection));
        }
        for (const LineIntersectionMovement& movement :
             topology.analysis.movements) {
            res["movements"].push_back(movementToJson(movement));
        }

        const std::string body = res.dump();
        cacheJsonResponse(kTopologyCache, body);
        buildJsonResponse(200, "OK", body);
        return true;
    }

    if (path_ == "/api/game/dispatch/commands" ||
        path_ == "/api/game/dispatch/commands/") {
        g_railway_dispatch.runMaintenance(wallNowMs());
        if (tryBuildCachedJsonResponse(kCommandsCache)) return true;
        const auto commands = g_railway_dispatch.pendingCommands();
        json res = {{"ok", true}, {"command_count", commands.size()},
                    {"commands", json::array()}};
        for (const DispatchDecision& command : commands)
            res["commands"].push_back(decisionToJson(command));
        const std::string body = res.dump();
        cacheJsonResponse(kCommandsCache, body);
        buildJsonResponse(200, "OK", body);
        return true;
    }

    if (path_ == "/api/game/dispatch/plans" ||
        path_ == "/api/game/dispatch/plans/") {
        if (tryBuildCachedJsonResponse(kPlansCache)) return true;
        const RollingPlanResult rolling = g_railway_dispatch.rollingPlan();
        json res = {{"ok", true}, {"resource_count", rolling.plans_by_resource.size()},
                    {"plans", json::array()}};
        for (const auto& pair : rolling.plans_by_resource) {
            const CorridorRollingPlan& plan = pair.second;
            json item = {
                {"corridor_resource_id", plan.corridor_resource_id},
                {"generated_at_simulation_ms", plan.generated_at_simulation_ms},
                {"trains", json::array()}
            };
            for (const PlannedTrainArrival& train : plan.trains) {
                item["trains"].push_back({
                    {"train_id", train.train_id}, {"line_id", train.line_id},
                    {"distance_to_entry", train.distance_to_entry},
                    {"estimated_arrival_simulation_ms",
                        train.estimated_arrival_simulation_ms},
                    {"planned_entry_simulation_ms",
                        train.planned_entry_simulation_ms},
                    {"within_planning_horizon", train.within_planning_horizon},
                    {"suggested_rank", train.within_planning_horizon
                        ? json(train.suggested_rank) : json(nullptr)},
                    {"line_distribution_need", train.line_distribution_need},
                    {"global_priority", train.global_priority}
                });
            }
            res["plans"].push_back(std::move(item));
        }
        const std::string body = res.dump();
        cacheJsonResponse(kPlansCache, body);
        buildJsonResponse(200, "OK", body);
        return true;
    }

    if (path_ == "/api/vehicles" || path_ == "/api/vehicles/" ||
        path_ == "/api/game/vehicles" || path_ == "/api/game/vehicles/") {
        if (tryBuildCachedJsonResponse(kVehiclesCache)) return true;
        VehicleSnapshot snapshot = g_vehicle_manager.getSnapshot();

        json res;
        res["ok"] = true;
        res["timestamp"] = snapshot.timestamp;
        res["map"] = {
            {"min_x", snapshot.map.min_x},
            {"max_x", snapshot.map.max_x},
            {"min_z", snapshot.map.min_z},
            {"max_z", snapshot.map.max_z}
        };
        res["vehicle_count"] = snapshot.vehicles.size();
        res["vehicles"] = json::array();

        for (const auto& vehicle : snapshot.vehicles) {
            res["vehicles"].push_back(vehicleToJson(vehicle));
        }

        const std::string body = res.dump();
        cacheJsonResponse(kVehiclesCache, body);
        buildJsonResponse(200, "OK", body);
        return true;
    }

    const std::string prefix = "/api/vehicles/";
    if (path_.compare(0, prefix.size(), prefix) == 0) {
        std::string id = path_.substr(prefix.size());

        if (id.empty()) {
            buildJsonResponse(400, "Bad Request", R"({"ok":false,"error":"empty vehicle id"})");
            return true;
        }

        auto vehicle = g_vehicle_manager.getVehicle(id);
        if (!vehicle.has_value()) {
            json res;
            res["ok"] = false;
            res["error"] = "vehicle not found";
            res["id"] = id;
            buildJsonResponse(404, "Not Found", res.dump());
            return true;
        }

        json res;
        res["ok"] = true;
        res["vehicle"] = vehicleToJson(vehicle.value());
        buildJsonResponse(200, "OK", res.dump());
        return true;
    }

    return false;
}

void HttpConn::buildFileResponse() {
    std::string file_path = safePath(path_);
    struct stat st;

    // =====================
    // stat
    // =====================

    if (stat(file_path.c_str(), &st) < 0 || S_ISDIR(st.st_mode)) {
        std::string body = "404 Not Found";
        std::string header;

        header += "HTTP/1.1 404 Not Found\r\n";
        header += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        header += "Content-Type: text/plain\r\n";
        header += "\r\n";

        response_ = std::make_unique<MemoryResponse>(header, body);

        return;
    }

    // =====================
    // open
    // =====================

    int file_fd = open(file_path.c_str(), O_RDONLY);

    if (file_fd < 0) {
        std::string body = "500 Internal Error";
        std::string header;

        header += "HTTP/1.1 500 Internal Error\r\n";
        header += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        header += "\r\n";

        response_ = std::make_unique<MemoryResponse>(header, body);

        return;
    }

    // =====================
    // header
    // =====================

    std::string header;

    header += "HTTP/1.1 200 OK\r\n";
    header += "Content-Length: " + std::to_string(st.st_size) + "\r\n";
    header += "Content-Type: " + getFileType(file_path) + "\r\n";

    if (keep_alive_) {
        header += "Connection: keep-alive\r\n";
    }
    else {
        header += "Connection: close\r\n";
    }

    header += "\r\n";

    // =====================
    // create response
    // =====================

    auto file_response = std::make_unique<FileResponse>(header, file_fd, st.st_size);
    close(file_fd);

    if (!file_response->valid()) {
        std::string body = "500 Internal Error";
        std::string error_header;
        error_header += "HTTP/1.1 500 Internal Server Error\r\n";
        error_header += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        error_header += "Content-Type: text/plain\r\n";
        error_header += "Connection: close\r\n\r\n";
        keep_alive_ = false;
        response_ = std::make_unique<MemoryResponse>(error_header, body);
        return;
    }

    response_ = std::move(file_response);
}

std::string HttpConn::getFileType(const std::string& path) {
    static const std::unordered_map<std::string, std::string> mime_map = {
        {".html", "text/html"},
        {".htm",  "text/html"},

        {".css",  "text/css"},
        {".js",   "application/javascript"},

        {".json", "application/json"},

        {".png",  "image/png"},
        {".jpg",  "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif",  "image/gif"},
        {".bmp",  "image/bmp"},
        {".webp", "image/webp"},
        {".svg",  "image/svg+xml"},
        {".ico",  "image/x-icon"},

        {".txt",  "text/plain"},

        {".pdf",  "application/pdf"},

        {".mp4",  "video/mp4"},
        {".webm", "video/webm"},
        {".mp3",  "audio/mpeg"},
        {".wav",  "audio/wav"},

        {".zip",  "application/zip"},
        {".tar",  "application/x-tar"},
        {".gz",   "application/gzip"},

        {".xml",  "application/xml"}
    };

    size_t pos = path.rfind('.');

    if (pos == std::string::npos) {
        return "application/octet-stream";
    }

    std::string ext = path.substr(pos);

    auto it = mime_map.find(ext);

    if (it != mime_map.end()) {
        return it->second;
    }

    return "application/octet-stream";
}

// ================= keep-alive =================
bool HttpConn::isKeepAlive() const {
    return keep_alive_;
}

HttpConn::~HttpConn() {
    LOG_DEBUG("destroy conn fd=" + std::to_string(fd_));
}
