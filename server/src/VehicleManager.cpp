#include "VehicleManager.h"

#include "DBConnectionPool.h"

#include <mysql/mysql.h>
#include <sstream>
#include <utility>

namespace {

std::string escapeSql(MYSQL* conn, const std::string& value) {
    std::string escaped(value.size() * 2 + 1, '\0');
    unsigned long length = mysql_real_escape_string(
        conn,
        escaped.data(),
        value.c_str(),
        static_cast<unsigned long>(value.size())
    );
    escaped.resize(length);
    return escaped;
}

bool query(MYSQL* conn, const char* sql) {
    return mysql_query(conn, sql) == 0;
}

}

VehicleManager& VehicleManager::instance() {
    static VehicleManager manager;
    return manager;
}

bool VehicleManager::enablePersistence(DBConnectionPool* pool) {
    if (!pool || !pool->isInitialized()) {
        return false;
    }

    MYSQL* conn = pool->getConnection();
    const char* create_vehicles_sql =
        "CREATE TABLE IF NOT EXISTS game_service_vehicles ("
        "id VARCHAR(255) PRIMARY KEY,"
        "service VARCHAR(64) NOT NULL,"
        "prefab VARCHAR(255) NOT NULL,"
        "status VARCHAR(255) NOT NULL,"
        "x DOUBLE NOT NULL,"
        "y DOUBLE NOT NULL,"
        "z DOUBLE NOT NULL,"
        "speed DOUBLE NOT NULL DEFAULT 0,"
        "heading DOUBLE NOT NULL DEFAULT 0"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    const char* create_metadata_sql =
        "CREATE TABLE IF NOT EXISTS game_vehicle_snapshot ("
        "id TINYINT UNSIGNED PRIMARY KEY,"
        "snapshot_timestamp BIGINT UNSIGNED NOT NULL DEFAULT 0,"
        "min_x DOUBLE NOT NULL DEFAULT -8640,"
        "max_x DOUBLE NOT NULL DEFAULT 8640,"
        "min_z DOUBLE NOT NULL DEFAULT -8640,"
        "max_z DOUBLE NOT NULL DEFAULT 8640"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    if (!query(conn, create_vehicles_sql) || !query(conn, create_metadata_sql)) {
        pool->releaseConnection(conn);
        return false;
    }

    if (!query(conn,
        "SELECT id, service, prefab, status, x, y, z, speed, heading "
        "FROM game_service_vehicles")) {
        pool->releaseConnection(conn);
        return false;
    }

    MYSQL_RES* result = mysql_store_result(conn);
    if (!result) {
        pool->releaseConnection(conn);
        return false;
    }

    std::unordered_map<std::string, VehicleState> loaded;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result)) != nullptr) {
        VehicleState state;
        state.id = row[0] ? row[0] : "";
        state.service = row[1] ? row[1] : "";
        state.prefab = row[2] ? row[2] : "";
        state.status = row[3] ? row[3] : "";
        state.position.x = row[4] ? std::stod(row[4]) : 0.0;
        state.position.y = row[5] ? std::stod(row[5]) : 0.0;
        state.position.z = row[6] ? std::stod(row[6]) : 0.0;
        state.speed = row[7] ? std::stod(row[7]) : 0.0;
        state.heading = row[8] ? std::stod(row[8]) : 0.0;
        loaded[state.id] = std::move(state);
    }
    mysql_free_result(result);

    uint64_t timestamp = 0;
    GameMapBounds bounds;
    if (!query(conn,
        "SELECT snapshot_timestamp, min_x, max_x, min_z, max_z "
        "FROM game_vehicle_snapshot WHERE id=1")) {
        pool->releaseConnection(conn);
        return false;
    }

    result = mysql_store_result(conn);
    if (!result) {
        pool->releaseConnection(conn);
        return false;
    }

    row = mysql_fetch_row(result);
    if (row) {
        timestamp = row[0] ? std::stoull(row[0]) : 0;
        bounds.min_x = row[1] ? std::stod(row[1]) : bounds.min_x;
        bounds.max_x = row[2] ? std::stod(row[2]) : bounds.max_x;
        bounds.min_z = row[3] ? std::stod(row[3]) : bounds.min_z;
        bounds.max_z = row[4] ? std::stod(row[4]) : bounds.max_z;
    }
    mysql_free_result(result);
    pool->releaseConnection(conn);

    std::lock_guard<std::mutex> lock(mutex_);
    vehicles_ = std::move(loaded);
    snapshot_timestamp_ = timestamp;
    map_bounds_ = bounds;
    db_pool_ = pool;
    return true;
}

bool VehicleManager::registerVehicle(const VehicleState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    return vehicles_.emplace(state.id, state).second;
}

bool VehicleManager::updateVehicle(const VehicleState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = vehicles_.find(state.id);
    if (it == vehicles_.end()) {
        return false;
    }
    it->second = state;
    return true;
}

bool VehicleManager::removeVehicle(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return vehicles_.erase(id) != 0;
}

void VehicleManager::replaceSnapshot(VehicleSnapshot snapshot) {
    std::unordered_map<std::string, VehicleState> replacement;
    replacement.reserve(snapshot.vehicles.size());
    for (auto& vehicle : snapshot.vehicles) {
        replacement[vehicle.id] = std::move(vehicle);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    vehicles_.swap(replacement);
    snapshot_timestamp_ = snapshot.timestamp;
    map_bounds_ = snapshot.map;
}

std::optional<VehicleState> VehicleManager::getVehicle(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = vehicles_.find(id);
    if (it == vehicles_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<VehicleState> VehicleManager::getAllVehicles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VehicleState> result;
    result.reserve(vehicles_.size());
    for (const auto& pair : vehicles_) {
        result.push_back(pair.second);
    }
    return result;
}

VehicleSnapshot VehicleManager::getSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    VehicleSnapshot snapshot;
    snapshot.timestamp = snapshot_timestamp_;
    snapshot.map = map_bounds_;
    snapshot.vehicles.reserve(vehicles_.size());
    for (const auto& pair : vehicles_) {
        snapshot.vehicles.push_back(pair.second);
    }
    return snapshot;
}

bool VehicleManager::hasVehicle(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return vehicles_.find(id) != vehicles_.end();
}

size_t VehicleManager::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return vehicles_.size();
}

bool VehicleManager::saveAll() {
    DBConnectionPool* pool;
    VehicleSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pool = db_pool_;
        snapshot.timestamp = snapshot_timestamp_;
        snapshot.map = map_bounds_;
        snapshot.vehicles.reserve(vehicles_.size());
        for (const auto& pair : vehicles_) {
            snapshot.vehicles.push_back(pair.second);
        }
    }

    if (!pool) {
        return true;
    }

    MYSQL* conn = pool->getConnection();
    if (!query(conn, "START TRANSACTION") ||
        !query(conn, "DELETE FROM game_service_vehicles")) {
        query(conn, "ROLLBACK");
        pool->releaseConnection(conn);
        return false;
    }

    for (const auto& state : snapshot.vehicles) {
        std::ostringstream sql;
        sql.precision(17);
        sql << "INSERT INTO game_service_vehicles "
            << "(id, service, prefab, status, x, y, z, speed, heading) VALUES ('"
            << escapeSql(conn, state.id) << "','"
            << escapeSql(conn, state.service) << "','"
            << escapeSql(conn, state.prefab) << "','"
            << escapeSql(conn, state.status) << "',"
            << state.position.x << ","
            << state.position.y << ","
            << state.position.z << ","
            << state.speed << ","
            << state.heading << ")";

        if (mysql_query(conn, sql.str().c_str()) != 0) {
            query(conn, "ROLLBACK");
            pool->releaseConnection(conn);
            return false;
        }
    }

    std::ostringstream metadata_sql;
    metadata_sql.precision(17);
    metadata_sql << "REPLACE INTO game_vehicle_snapshot "
                 << "(id, snapshot_timestamp, min_x, max_x, min_z, max_z) VALUES (1,"
                 << snapshot.timestamp << ","
                 << snapshot.map.min_x << ","
                 << snapshot.map.max_x << ","
                 << snapshot.map.min_z << ","
                 << snapshot.map.max_z << ")";

    if (mysql_query(conn, metadata_sql.str().c_str()) != 0 ||
        !query(conn, "COMMIT")) {
        query(conn, "ROLLBACK");
        pool->releaseConnection(conn);
        return false;
    }

    pool->releaseConnection(conn);
    return true;
}
